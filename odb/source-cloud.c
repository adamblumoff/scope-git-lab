#include "git-compat-util.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "object-file.h"
#include "odb/cloud-manifest.h"
#include "odb/segment-group.h"
#include "odb/source-cloud.h"
#include "odb/source-files.h"
#include "odb/streaming.h"
#include "oidset.h"
#include "repository.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "tmp-objdir.h"

#define CLOUD_MARKER_HEADER "git-cloud-odb 1"
#define CLOUD_GROUP_HEADER_SIZE 32
#define CLOUD_CAS_ATTEMPTS 64
#define CLOUD_GROUP_TARGET (256 * 1024)
#define CLOUD_MAX_OBJECT_BYTES (16 * 1024 * 1024)
#define CLOUD_MAX_TRANSACTION_BYTES (32 * 1024 * 1024)
#define CLOUD_MAX_ARTIFACT_BYTES (64 * 1024 * 1024)

struct cloud_range {
	struct odb_source_cloud *source;
	char *key;
	uint64_t size;
};

struct cloud_transaction {
	struct odb_transaction base;
	struct tmp_objdir *objdir;
	uint64_t canonical_bytes;
};

struct cloud_stream {
	struct odb_read_stream base;
	unsigned char *data;
	size_t offset;
};

static void cloud_metric_event(struct odb_source_cloud *source UNUSED,
			       uint64_t object_reads, uint64_t useful_bytes,
			       uint64_t group_cache_hits,
			       uint64_t cas_retries, uint64_t publishes)
{
	const char *path = getenv("GIT_CLOUD_ODB_METRICS_PATH");
	struct strbuf line = STRBUF_INIT;
	int fd;

	if (!path || !*path)
		return;
	strbuf_addf(&line,
		    "{\"schema\":\"git-cloud-odb-logical/v1\","
		    "\"pid\":%"PRIuMAX",\"layout\":\"group\","
		    "\"objectReads\":%"PRIu64",\"usefulBytes\":%"PRIu64","
		    "\"groupCacheHits\":%"PRIu64",\"casRetries\":%"PRIu64","
		    "\"publishes\":%"PRIu64"}\n",
		    (uintmax_t)getpid(), object_reads, useful_bytes,
		    group_cache_hits, cas_retries,
		    publishes);
	fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd >= 0) {
		write_in_full(fd, line.buf, line.len);
		close(fd);
	}
	strbuf_release(&line);
}

static int successful_status(long status)
{
	return status >= 200 && status < 300;
}

static int conflict_status(long status)
{
	return status == 409 || status == 412;
}

static int valid_prefix(const char *prefix)
{
	const unsigned char *p = (const unsigned char *)prefix;

	if (!*p || *p == '/' || strstr(prefix, ".."))
		return 0;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '/' && *p != '-' && *p != '_' &&
		    *p != '.')
			return 0;
	return 1;
}

static int read_marker(const char *objects_path, struct strbuf *prefix)
{
	struct strbuf path = STRBUF_INIT;
	struct strbuf contents = STRBUF_INIT;
	struct string_list lines = STRING_LIST_INIT_DUP;
	const char *value;
	int ret = -1;

	strbuf_addf(&path, "%s/cloud-odb", objects_path);
	if (strbuf_read_file(&contents, path.buf, 1024) < 0) {
		error_errno(_("unable to read cloud ODB marker '%s'"), path.buf);
		goto out;
	}
	string_list_split(&lines, contents.buf, "\n", -1);
	if (lines.nr && !*lines.items[lines.nr - 1].string) {
		free(lines.items[lines.nr - 1].string);
		lines.nr--;
	}
	if (lines.nr != 3 || strcmp(lines.items[0].string, CLOUD_MARKER_HEADER) ||
	    strcmp(lines.items[1].string, "layout group") ||
	    !skip_prefix(lines.items[2].string, "prefix ", &value) ||
	    !valid_prefix(value)) {
		error(_("cloud ODB marker '%s' is invalid"), path.buf);
		goto out;
	}
	strbuf_addstr(prefix, value);
	ret = 0;
out:
	string_list_clear(&lines, 0);
	strbuf_release(&contents);
	strbuf_release(&path);
	return ret;
}

static void cloud_range_release(void *data)
{
	struct cloud_range *range = data;

	if (!range)
		return;
	free(range->key);
	free(range);
}

static int cloud_range_read(void *data, uint64_t offset, size_t length,
			    void *buffer)
{
	struct cloud_range *range = data;

	return s3_client_read_range(&range->source->client, range->key,
				    range->size, offset, length, buffer);
}

static struct cloud_range *cloud_range_new(struct odb_source_cloud *source,
					   const char *key, uint64_t size)
{
	struct cloud_range *range;

	CALLOC_ARRAY(range, 1);
	range->source = source;
	range->key = xstrdup(key);
	range->size = size;
	return range;
}

static void cloud_clear_readers(struct odb_source_cloud *source)
{
	for (size_t i = 0; i < source->readers_nr; i++)
		odb_segment_group_close(&source->groups[i]);
	free(source->groups);
	source->groups = NULL;
	source->readers_nr = 0;
	odb_cloud_manifest_release(&source->manifest);
}

static int cloud_get_manifest(struct odb_source_cloud *source,
			      struct odb_cloud_manifest *manifest,
			      struct strbuf *etag)
{
	struct s3_response response = S3_RESPONSE_INIT;
	int ret = -1;

	if (s3_client_get(&source->client, source->manifest_key.buf, &response))
		goto out;
	if (response.http_status == 404) {
		odb_cloud_manifest_init(manifest,
					source->base.odb->repo->hash_algo);
		strbuf_reset(etag);
		ret = 0;
		goto out;
	}
	if (response.http_status != 200 || !response.etag.len ||
	    odb_cloud_manifest_parse(manifest, response.body.buf,
				     response.body.len,
				     source->base.odb->repo->hash_algo))
		goto out;
	strbuf_reset(etag);
	strbuf_addbuf(etag, &response.etag);
	ret = 0;
out:
	s3_response_release(&response);
	return ret;
}

static int cloud_get_index(struct odb_source_cloud *source,
			   const struct odb_cloud_artifact *artifact,
			   struct s3_response *response)
{
	if (artifact->index_bytes > SIZE_MAX ||
	    s3_client_get(&source->client, artifact->index_key, response) ||
	    response->http_status != 200 ||
	    response->body.len != artifact->index_bytes)
		return error(_("unable to read cloud ODB index"));
	return 0;
}

static int cloud_open_group(struct odb_source_cloud *source,
			    const struct odb_cloud_artifact *artifact,
			    size_t ordinal)
{
	struct s3_response index = S3_RESPONSE_INIT;
	struct cloud_range *range = NULL;
	struct strbuf label = STRBUF_INIT;
	unsigned char header[CLOUD_GROUP_HEADER_SIZE];
	int ret = -1;

	if (cloud_get_index(source, artifact, &index) ||
	    s3_client_read_range(&source->client, artifact->data_key,
				 artifact->data_bytes, 0,
				 CLOUD_GROUP_HEADER_SIZE, header))
		goto out;
	range = cloud_range_new(source, artifact->data_key, artifact->data_bytes);
	strbuf_addf(&label, "cloud/%zu", ordinal);
	if (odb_segment_group_open_reader(
			   &source->groups[ordinal], label.buf, artifact->data_bytes,
			   header, CLOUD_GROUP_HEADER_SIZE, index.body.buf,
			   index.body.len,
			   source->base.odb->repo->hash_algo, cloud_range_read,
			   cloud_range_release, range)) {
		goto out;
	}
	range = NULL;
	{
		uint64_t canonical_bytes = 0;

		for (size_t i = 0; i < source->groups[ordinal].entries_nr; i++) {
			uint64_t object_bytes = source->groups[ordinal].entries[i].size;

			if (object_bytes > CLOUD_MAX_OBJECT_BYTES ||
			    object_bytes > CLOUD_MAX_TRANSACTION_BYTES - canonical_bytes) {
				error(_("cloud ODB artifact exceeds the bounded-object policy"));
				odb_segment_group_close(&source->groups[ordinal]);
				goto out;
			}
			canonical_bytes += object_bytes;
		}
	}
	ret = 0;
out:
	cloud_range_release(range);
	strbuf_release(&label);
	s3_response_release(&index);
	return ret;
}

static int cloud_artifact_equal(const struct odb_cloud_artifact *a,
				const struct odb_cloud_artifact *b)
{
	return a->data_bytes == b->data_bytes &&
		a->index_bytes == b->index_bytes &&
		!strcmp(a->data_key, b->data_key) &&
		!strcmp(a->index_key, b->index_key);
}

static int cloud_load(struct odb_source_cloud *source)
{
	struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;
	struct strbuf etag = STRBUF_INIT;
	size_t old_readers = source->readers_nr;
	int fetched = 0;
	int ret = -1;

	if (cloud_get_manifest(source, &manifest, &etag))
		goto out;
	if (manifest.artifacts_nr < old_readers ||
	    source->manifest.artifacts_nr < old_readers)
		goto out;
	for (size_t i = 0; i < old_readers; i++)
		if (!cloud_artifact_equal(&source->manifest.artifacts[i],
					  &manifest.artifacts[i]))
			goto out;
	fetched = 1;
	if (manifest.artifacts_nr > old_readers)
		REALLOC_ARRAY(source->groups, manifest.artifacts_nr);
	for (size_t i = old_readers; i < manifest.artifacts_nr; i++) {
		if (cloud_open_group(source, &manifest.artifacts[i], i))
			goto out;
		source->readers_nr++;
	}
	ret = 0;
out:
	if (fetched) {
		odb_cloud_manifest_release(&source->manifest);
		source->manifest = manifest;
		manifest = (struct odb_cloud_manifest)ODB_CLOUD_MANIFEST_INIT;
	}
	odb_cloud_manifest_release(&manifest);
	strbuf_release(&etag);
	return ret;
}

static int cloud_lookup_segment(struct odb_source_cloud *source,
				const struct object_id *oid, size_t *reader_nr,
				const void **entry)
{
	size_t i = source->readers_nr;

	while (i--) {
		int ret = odb_segment_group_lookup(
			&source->groups[i], oid,
			(const struct odb_segment_group_entry **)entry);
		if (ret < 0)
			return -1;
		if (!ret) {
			*reader_nr = i;
			return 0;
		}
	}
	return 1;
}

static int cloud_read_data(struct odb_source_cloud *source,
			   const struct object_id *oid, enum object_type *type,
			   size_t *size, size_t *disk_size, void **data)
{
	const void *entry;
	size_t nr;
	int group_cache_hit = 0;
	int ret = cloud_lookup_segment(source, oid, &nr, &entry);

	if (ret)
		return ret;
	{
		const struct odb_segment_group_entry *e = entry;
		const struct odb_segment_group_info *group =
			&source->groups[nr].groups[e->group_nr];

		*type = e->type;
		*size = e->size;
		*disk_size = group->compressed_size;
		if (data) {
			group_cache_hit = source->groups[nr].cached_group_nr == e->group_nr;
			if (odb_segment_group_read(&source->groups[nr], e, data))
				return -1;
		}
	}
	if (data) {
		cloud_metric_event(source, 1, *size, group_cache_hit, 0, 0);
	}
	return 0;
}

static int cloud_read_object_info(struct odb_source *base,
				  const struct object_id *oid,
				  struct object_info *oi,
				  enum object_info_flags flags)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);
	enum object_type type;
	size_t size, disk_size;
	void *data = NULL;
	int ret;

	ret = cloud_read_data(source, oid, &type, &size, &disk_size,
			      oi && oi->contentp ? &data : NULL);
	if (ret > 0 && (flags & OBJECT_INFO_SECOND_READ)) {
		if (cloud_load(source))
			return -1;
		ret = cloud_read_data(source, oid, &type, &size, &disk_size,
				      oi && oi->contentp ? &data : NULL);
	}
	if (ret > 0)
		return odb_source_read_object_info(&source->fallback->base, oid, oi,
						   flags);
	if (ret < 0)
		return -1;
	if (!oi)
		return 0;
	if (oi->typep)
		*oi->typep = type;
	if (oi->sizep)
		*oi->sizep = size;
	if (oi->disk_sizep)
		*oi->disk_sizep = disk_size;
	if (oi->delta_base_oid)
		oidclr(oi->delta_base_oid, base->odb->repo->hash_algo);
	if (oi->contentp)
		*oi->contentp = data;
	if (oi->mtimep)
		*oi->mtimep = 0;
	if (oi->source_infop)
		oi->source_infop->source = base;
	return 0;
}

static ssize_t cloud_stream_read(struct odb_read_stream *base, char *buf,
				 size_t len)
{
	struct cloud_stream *stream = container_of(base, struct cloud_stream, base);
	size_t remaining = base->size - stream->offset;
	size_t bytes = len < remaining ? len : remaining;

	memcpy(buf, stream->data + stream->offset, bytes);
	stream->offset += bytes;
	return bytes;
}

static int cloud_stream_close(struct odb_read_stream *base)
{
	struct cloud_stream *stream = container_of(base, struct cloud_stream, base);

	free(stream->data);
	return 0;
}

static int cloud_read_object_stream(struct odb_read_stream **out,
				    struct odb_source *base,
				    const struct object_id *oid)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);
	struct cloud_stream *stream;
	enum object_type type;
	size_t size, disk_size;

	CALLOC_ARRAY(stream, 1);
	if (cloud_read_data(source, oid, &type, &size, &disk_size,
			    (void **)&stream->data)) {
		free(stream);
		if (!cloud_load(source)) {
			CALLOC_ARRAY(stream, 1);
			if (!cloud_read_data(source, oid, &type, &size, &disk_size,
					     (void **)&stream->data))
				goto found;
			free(stream);
		}
		return odb_source_read_object_stream(out, &source->fallback->base,
						     oid);
	}
found:
	stream->base.read = cloud_stream_read;
	stream->base.close = cloud_stream_close;
	stream->base.type = type;
	stream->base.size = size;
	*out = &stream->base;
	return 0;
}

struct cloud_each_data {
	struct odb_source_cloud *source;
	const struct object_info *request;
	odb_for_each_object_cb cb;
	void *cb_data;
	const struct odb_for_each_object_options *opts;
};

static int cloud_match_hash(size_t len, const unsigned char *a,
			    const unsigned char *b)
{
	while (len > 1) {
		if (*a++ != *b++)
			return 0;
		len -= 2;
	}
	return !len || !((*a ^ *b) & 0xf0);
}

static int cloud_each_oid(struct cloud_each_data *data,
			  const struct object_id *oid)
{
	struct object_info oi;

	if (data->opts->prefix &&
	    !cloud_match_hash(data->opts->prefix_hex_len,
			      data->opts->prefix->hash, oid->hash))
		return 0;
	if (!data->request)
		return data->cb(oid, NULL, data->cb_data);
	oi = *data->request;
	if (cloud_read_object_info(&data->source->base, oid, &oi, 0))
		return -1;
	return data->cb(oid, &oi, data->cb_data);
}

static int cloud_each_group(const struct odb_segment_group_entry *entry,
			    void *cb_data)
{
	return cloud_each_oid(cb_data, &entry->oid);
}

static int cloud_for_each_object(struct odb_source *base,
				 const struct object_info *request,
				 odb_for_each_object_cb cb, void *cb_data,
				 const struct odb_for_each_object_options *opts)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);
	struct cloud_each_data data = {
		.source = source,
		.request = request,
		.cb = cb,
		.cb_data = cb_data,
		.opts = opts,
	};

	if (!(opts->flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY) &&
	    !((opts->flags & ODB_FOR_EACH_OBJECT_LOCAL_ONLY) && !base->local)) {
		for (size_t i = 0; i < source->readers_nr; i++) {
			int ret = odb_segment_group_for_each(&source->groups[i],
							     cloud_each_group, &data);

			if (ret)
				return ret;
		}
	}

	return odb_source_for_each_object(&source->fallback->base, request, cb,
					  cb_data, opts);
}

static int cloud_count_objects(struct odb_source *base,
			       enum odb_count_objects_flags flags,
			       unsigned long *out)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);
	uint64_t total = 0;
	unsigned long fallback_count;

	for (size_t i = 0; i < source->readers_nr; i++) {
		uint64_t nr = source->groups[i].entries_nr;

		if (nr > UINT64_MAX - total)
			return error(_("cloud ODB object count exceeds internal limit"));
		total += nr;
	}
	if (odb_source_count_objects(&source->fallback->base, flags,
				     &fallback_count))
		return -1;
	if (fallback_count > UINT64_MAX - total)
		return error(_("cloud ODB object count exceeds internal limit"));
	total += fallback_count;
	if (total > ULONG_MAX)
		return error(_("cloud ODB object count exceeds platform limit"));
	*out = total;
	return 0;
}

struct cloud_abbrev_data {
	const struct object_id *oid;
	unsigned len;
};

static int cloud_abbrev_cb(const struct object_id *oid,
			   struct object_info *oi UNUSED, void *cb_data)
{
	struct cloud_abbrev_data *data = cb_data;
	unsigned common = oid_common_prefix_hexlen(oid, data->oid);

	if (common != hash_algos[oid->algo].hexsz && common >= data->len)
		data->len = common + 1;
	return 0;
}

static int cloud_find_abbrev_len(struct odb_source *base,
				 const struct object_id *oid,
				 unsigned min_len, unsigned *out)
{
	struct odb_for_each_object_options opts = {
		.prefix = oid,
		.prefix_hex_len = min_len,
	};
	struct cloud_abbrev_data data = { .oid = oid, .len = min_len };
	int ret = cloud_for_each_object(base, NULL, cloud_abbrev_cb, &data, &opts);

	*out = data.len;
	return ret;
}

static int cloud_freshen_object(struct odb_source *base,
				const struct object_id *oid,
				const time_t *mtime)
{
	return !cloud_read_object_info(base, oid, NULL, 0) ||
		odb_source_freshen_object(
			&container_of(base, struct odb_source_cloud, base)->fallback->base,
			oid, mtime);
}

static int cloud_unsupported_write(struct odb_source *source UNUSED,
				   const void *buf UNUSED, size_t len UNUSED,
				   enum object_type type UNUSED,
				   const struct object_id *oid UNUSED,
				   const struct object_id *compat_oid UNUSED,
				   const time_t *mtime UNUSED,
				   enum odb_write_object_flags flags UNUSED)
{
	return error(_("direct writes to the cloud ODB are unsupported"));
}

static int cloud_unsupported_stream(struct odb_source *source UNUSED,
				    struct odb_write_stream *stream UNUSED,
				    size_t len UNUSED,
				    struct object_id *oid UNUSED)
{
	return error(_("direct streamed writes to the cloud ODB are unsupported"));
}

static int collect_oid(const struct object_id *oid,
		       struct object_info *oi UNUSED, void *cb_data)
{
	oidset_insert(cb_data, oid);
	return 0;
}

static void cloud_failpoint(const char *name)
{
	const char *value = getenv("GIT_TEST_CLOUD_ODB_FAILPOINT");

	if (value && !strcmp(value, name))
		_exit(99);
}

static void sha256_hex(const void *data, size_t size,
		       char hex[GIT_SHA256_HEXSZ + 1])
{
	static const char digits[] = "0123456789abcdef";
	git_SHA256_CTX ctx;
	unsigned char hash[GIT_SHA256_RAWSZ];

	git_SHA256_Init(&ctx);
	git_SHA256_Update(&ctx, data, size);
	git_SHA256_Final(hash, &ctx);
	for (size_t i = 0; i < sizeof(hash); i++) {
		hex[2 * i] = digits[hash[i] >> 4];
		hex[2 * i + 1] = digits[hash[i] & 0xf];
	}
	hex[GIT_SHA256_HEXSZ] = '\0';
}

static int upload_immutable(struct odb_source_cloud *source, const char *path,
			    const char *suffix, struct strbuf *key,
			    uint64_t *bytes)
{
	struct s3_response response = S3_RESPONSE_INIT;
	struct strbuf contents = STRBUF_INIT;
	struct stat st;
	char hash[GIT_SHA256_HEXSZ + 1];
	int ret = -1;

	if (stat(path, &st) < 0) {
		error_errno(_("unable to inspect cloud ODB artifact '%s'"), path);
		goto out;
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0) {
		error(_("cloud ODB artifact '%s' is not a regular file"), path);
		goto out;
	}
	if ((uintmax_t)st.st_size > CLOUD_MAX_ARTIFACT_BYTES) {
		error(_("cloud ODB artifact exceeds the %u-byte upload limit"),
		      (unsigned)CLOUD_MAX_ARTIFACT_BYTES);
		goto out;
	}
	if (strbuf_read_file(&contents, path, st.st_size) < 0)
		goto out;
	sha256_hex(contents.buf, contents.len, hash);
	strbuf_addf(key, "%s/objects/%s.%s", source->prefix.buf, hash, suffix);
	if (s3_client_put(&source->client, key->buf, contents.buf, contents.len,
			  NULL, 1, &response))
		goto out;
	if (conflict_status(response.http_status)) {
		if (s3_client_get(&source->client, key->buf, &response) ||
		    response.http_status != 200 ||
		    response.body.len != contents.len ||
		    memcmp(response.body.buf, contents.buf, contents.len))
			goto out;
	} else if (!successful_status(response.http_status)) {
		goto out;
	}
	*bytes = contents.len;
	ret = 0;
out:
	strbuf_release(&contents);
	s3_response_release(&response);
	return ret;
}

static int write_group_artifacts(struct odb_source_cloud *source,
				 struct oidset *objects,
				 const char *data_path,
				 const char *index_path)
{
	struct odb_segment_group_writer group = ODB_SEGMENT_GROUP_WRITER_INIT;
	struct oidset_iter iter;
	struct object_id *oid;
	uint64_t canonical_bytes = 0;
	int ret = -1;

	if (odb_segment_group_writer_open(
			   &group, data_path, index_path,
			   source->base.odb->repo->hash_algo, CLOUD_GROUP_TARGET)) {
		goto out;
	}
	for (oid = oidset_iter_first(objects, &iter); oid;
	     oid = oidset_iter_next(&iter)) {
		enum object_type type;
		size_t size;
		int add_ret;
		void *data;

		type = odb_read_object_info(source->base.odb, oid, &size);
		if (type < 0)
			goto out;
		if (size > CLOUD_MAX_OBJECT_BYTES ||
		    size > CLOUD_MAX_TRANSACTION_BYTES - canonical_bytes) {
			error(_("cloud ODB transaction exceeds the bounded-object policy"));
			goto out;
		}
		canonical_bytes += size;
		data = odb_read_object(source->base.odb, oid, &type, &size);

		if (!data)
			goto out;
		add_ret = odb_segment_group_writer_add(&group, oid, type, data, size);
		free(data);
		if (add_ret)
			goto out;
	}
	ret = odb_segment_group_writer_finish(&group, NULL);
out:
	if (ret)
		odb_segment_group_writer_abort(&group);
	return ret;
}

static int artifact_present(const struct odb_cloud_manifest *manifest,
			    const char *data_key, const char *index_key)
{
	for (size_t i = 0; i < manifest->artifacts_nr; i++)
		if (!strcmp(manifest->artifacts[i].data_key, data_key) &&
		    !strcmp(manifest->artifacts[i].index_key, index_key))
			return 1;
	return 0;
}

static int publish_artifact(struct odb_source_cloud *source,
			    const char *data_key, uint64_t data_bytes,
			    const char *index_key, uint64_t index_bytes)
{
	struct s3_response response = S3_RESPONSE_INIT;
	int ret = -1;

	for (unsigned attempt = 0; attempt < CLOUD_CAS_ATTEMPTS; attempt++) {
		struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;
		struct strbuf etag = STRBUF_INIT;
		struct strbuf serialized = STRBUF_INIT;
		int attempt_ret = -1;

		if (cloud_get_manifest(source, &manifest, &etag))
			goto attempt_out;
		if (artifact_present(&manifest, data_key, index_key)) {
			attempt_ret = 0;
			goto attempt_out;
		}
		if (manifest.generation == UINT64_MAX ||
		    odb_cloud_manifest_add(&manifest, data_key, data_bytes,
					   index_key, index_bytes))
			goto attempt_out;
		manifest.generation++;
		odb_cloud_manifest_write(&manifest, &serialized);
		cloud_failpoint("before-cas");
		if (s3_client_put(&source->client, source->manifest_key.buf,
				  serialized.buf, serialized.len,
				  etag.len ? etag.buf : NULL, !etag.len,
				  &response))
			goto attempt_out;
		if (successful_status(response.http_status)) {
			cloud_metric_event(source, 0, 0, 0, 0, 1);
			attempt_ret = 0;
			goto attempt_out;
		}
		if (!conflict_status(response.http_status))
			goto attempt_out;
		cloud_metric_event(source, 0, 0, 0, 1, 0);
		/* Spread competing writers before they reread the same generation. */
		sleep_millisec(5 + (getpid() + attempt * 17) % 46);
		attempt_ret = 1;

attempt_out:
		strbuf_release(&serialized);
		strbuf_release(&etag);
		odb_cloud_manifest_release(&manifest);
		ret = attempt_ret;
		if (ret <= 0)
			break;
	}
	s3_response_release(&response);
	return ret ? error(_("cloud ODB manifest CAS retry limit exceeded")) : 0;
}

static int cloud_transaction_commit(struct odb_transaction *base)
{
	struct cloud_transaction *transaction =
		container_of(base, struct cloud_transaction, base);
	struct odb_source_cloud *source =
		container_of(base->source, struct odb_source_cloud, base);
	struct odb_source *incoming = base->source->odb->sources;
	struct odb_for_each_object_options opts = { 0 };
	struct oidset objects = OIDSET_INIT;
	struct strbuf temporary = STRBUF_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	struct strbuf data_key = STRBUF_INIT;
	struct strbuf index_key = STRBUF_INIT;
	uint64_t data_bytes = 0, index_bytes = 0;
	int ret = -1;

	if (incoming->type != ODB_SOURCE_FILES)
		BUG("cloud transaction primary source is not files");
	if (odb_source_for_each_object(incoming, NULL, collect_oid, &objects, &opts))
		goto out;
	if (!oidset_size(&objects)) {
		ret = 0;
		goto discard;
	}
	strbuf_addf(&temporary, "%s/cloud-write-XXXXXX", base->source->path);
	if (!mkdtemp(temporary.buf)) {
		error_errno(_("unable to create cloud ODB staging directory"));
		goto out;
	}
	cloud_failpoint("before-artifact-build");
	strbuf_addf(&data_path, "%s/data.gsg", temporary.buf);
	strbuf_addf(&index_path, "%s/index.gsi", temporary.buf);
	if (write_group_artifacts(source, &objects, data_path.buf, index_path.buf))
		goto out;
	cloud_failpoint("before-artifact-upload");
	if (upload_immutable(source, data_path.buf, "gsg", &data_key,
			     &data_bytes) ||
	    upload_immutable(source, index_path.buf, "gsi", &index_key,
			     &index_bytes))
		goto out;
	cloud_failpoint("after-artifact-upload");
	if (publish_artifact(source, data_key.buf, data_bytes, index_key.buf,
			     index_bytes))
		goto out;
	cloud_failpoint("after-cas");
	if (cloud_load(source))
		goto out;
	ret = 0;

discard:
	if (tmp_objdir_destroy(transaction->objdir)) {
		error(_("unable to remove cloud ODB quarantine"));
		ret = -1;
	}
	transaction->objdir = NULL;
out:
	if (ret && transaction->objdir) {
		if (tmp_objdir_destroy(transaction->objdir))
			error(_("unable to remove failed cloud ODB quarantine"));
		transaction->objdir = NULL;
	}
	if (data_path.len)
		unlink(data_path.buf);
	if (index_path.len)
		unlink(index_path.buf);
	if (temporary.len)
		rmdir(temporary.buf);
	oidset_clear(&objects);
	strbuf_release(&temporary);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
	strbuf_release(&data_key);
	strbuf_release(&index_key);
	return ret;
}

static int cloud_transaction_write_stream(struct odb_transaction *base,
					  struct odb_write_stream *stream,
					  size_t len, struct object_id *oid)
{
	struct cloud_transaction *transaction =
		container_of(base, struct cloud_transaction, base);
	int ret;

	if (len > CLOUD_MAX_OBJECT_BYTES ||
	    len > CLOUD_MAX_TRANSACTION_BYTES - transaction->canonical_bytes)
		return error(_("cloud ODB transaction exceeds the bounded-object policy"));
	ret = odb_source_write_object_stream(base->source->odb->sources,
					     stream, len, oid);
	if (!ret)
		transaction->canonical_bytes += len;
	return ret;
}

static int cloud_transaction_env(struct odb_transaction *base,
				 struct strvec *env)
{
	struct cloud_transaction *transaction =
		container_of(base, struct cloud_transaction, base);

	strvec_pushv(env, tmp_objdir_env(transaction->objdir));
	return 0;
}

static int cloud_begin_transaction(struct odb_source *source,
				   struct odb_transaction **out,
				   enum odb_transaction_flags flags UNUSED)
{
	struct cloud_transaction *transaction;

	CALLOC_ARRAY(transaction, 1);
	transaction->base.source = source;
	transaction->base.commit = cloud_transaction_commit;
	transaction->base.write_object_stream = cloud_transaction_write_stream;
	transaction->base.env = cloud_transaction_env;
	transaction->objdir = tmp_objdir_create(source->odb->repo, "cloud-incoming");
	if (!transaction->objdir) {
		free(transaction);
		return error(_("unable to create cloud ODB quarantine"));
	}
	tmp_objdir_replace_primary_odb(transaction->objdir, 0);
	*out = &transaction->base;
	return 0;
}

static int cloud_read_alternates(struct odb_source *source, struct strvec *out)
{
	struct odb_source_cloud *cloud = container_of(source, struct odb_source_cloud,
						      base);

	return odb_source_read_alternates(&cloud->fallback->base, out);
}

static int cloud_write_alternate(struct odb_source *source UNUSED,
				 const char *alternate UNUSED)
{
	return error(_("alternates are unsupported by the cloud ODB"));
}

static int cloud_optimize(struct odb_source *base UNUSED,
			  const struct odb_optimize_options *opts UNUSED)
{
	return 0;
}

static bool cloud_optimize_required(struct odb_source *base UNUSED,
				    const struct odb_optimize_options *opts UNUSED)
{
	return false;
}

static void cloud_close(struct odb_source *base)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);

	odb_source_close(&source->fallback->base);
}

static void cloud_prepare(struct odb_source *base, enum odb_prepare_flags flags)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);

	odb_source_prepare(&source->fallback->base, flags);
	if ((flags & ODB_PREPARE_FLUSH_CACHES) && cloud_load(source))
		die(_("unable to reload cloud ODB"));
}

static void cloud_free(struct odb_source *base)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);

	cloud_clear_readers(source);
	odb_source_free(&source->fallback->base);
	s3_client_release(&source->client);
	strbuf_release(&source->prefix);
	strbuf_release(&source->manifest_key);
	odb_source_release(base);
	free(source);
}

struct odb_source_cloud *odb_source_cloud_new(struct object_database *odb,
					      const char *path, bool local)
{
	struct odb_source_cloud *source;

	CALLOC_ARRAY(source, 1);
	source->prefix = (struct strbuf)STRBUF_INIT;
	source->manifest_key = (struct strbuf)STRBUF_INIT;
	odb_source_init(&source->base, odb, ODB_SOURCE_CLOUD, path, local);
	if (read_marker(path, &source->prefix) ||
	    s3_client_init_from_env(&source->client))
		die(_("unable to configure cloud ODB at '%s'"), path);
	strbuf_addf(&source->manifest_key, "%s/manifest", source->prefix.buf);
	source->fallback = odb_source_files_new(odb, path, local);
	source->base.free = cloud_free;
	source->base.close = cloud_close;
	source->base.prepare = cloud_prepare;
	source->base.read_object_info = cloud_read_object_info;
	source->base.read_object_stream = cloud_read_object_stream;
	source->base.for_each_object = cloud_for_each_object;
	source->base.count_objects = cloud_count_objects;
	source->base.find_abbrev_len = cloud_find_abbrev_len;
	source->base.freshen_object = cloud_freshen_object;
	source->base.write_object = cloud_unsupported_write;
	source->base.write_object_stream = cloud_unsupported_stream;
	source->base.begin_transaction = cloud_begin_transaction;
	source->base.read_alternates = cloud_read_alternates;
	source->base.write_alternate = cloud_write_alternate;
	source->base.optimize = cloud_optimize;
	source->base.optimize_required = cloud_optimize_required;
	if (cloud_load(source))
		die(_("unable to open cloud ODB at '%s'"), path);
	return source;
}
