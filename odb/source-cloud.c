#include "git-compat-util.h"
#include "dir.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "loose.h"
#include "object-file.h"
#include "odb/cloud-manifest.h"
#include "odb/segment-group.h"
#include "odb/source-cloud.h"
#include "odb/source-files.h"
#include "odb/streaming.h"
#include "oidset.h"
#include "parse.h"
#include "refs.h"
#include "repository.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "tmp-objdir.h"
#include "wrapper.h"

#define CLOUD_MARKER_HEADER "git-cloud-odb 2"
#define CLOUD_GROUP_HEADER_SIZE 32
#define CLOUD_CAS_ATTEMPTS 64
#define CLOUD_GROUP_TARGET (256 * 1024)
#define CLOUD_MAX_OBJECT_BYTES (16 * 1024 * 1024)
#define CLOUD_MAX_TRANSACTION_BYTES (32 * 1024 * 1024)
#define CLOUD_MAX_ARTIFACT_BYTES (64 * 1024 * 1024)
#define CLOUD_MAX_TRANSACTION_OBJECTS 65536
#define CLOUD_MAX_MANIFEST_ARTIFACTS 1024
#define CLOUD_MAX_PENDING_ARTIFACTS 1024
#define CLOUD_MAX_MANIFEST_INDEX_BYTES CLOUD_MAX_ARTIFACT_BYTES
#define CLOUD_PENDING_GRACE_SECONDS 3600
#define CLOUD_GC_LEASE_SECONDS 3600
#define CLOUD_MARKER_MAX_BYTES 1024

struct cloud_range {
	struct odb_source_cloud *source;
	char *key;
	uint64_t size;
};

struct cloud_transaction {
	struct odb_transaction base;
	struct tmp_objdir *objdir;
	uint64_t canonical_bytes;
	struct oidset streamed_objects;
};

struct cloud_stream {
	struct odb_read_stream base;
	unsigned char *data;
	size_t offset;
};

struct cloud_lookup_entry {
	struct oidmap_entry ent;
	size_t reader_nr;
	size_t entry_nr;
};

static const char *metrics_run_id(void)
{
	const unsigned char *value =
		(const unsigned char *)getenv("GIT_CLOUD_ODB_METRICS_RUN");
	const unsigned char *p;

	if (!value || strlen((const char *)value) != 32)
		return NULL;
	for (p = value; *p; p++)
		if (!isxdigit(*p))
			return NULL;
	return (const char *)value;
}

static void cloud_metric_event(struct odb_source_cloud *source UNUSED,
			       uint64_t object_reads, uint64_t useful_bytes,
			       uint64_t group_cache_hits,
			       uint64_t cas_retries, uint64_t publishes)
{
	const char *run_id = metrics_run_id();
	struct strbuf line = STRBUF_INIT;

	if (!run_id)
		return;
	strbuf_addf(&line,
		    "{\"schema\":\"git-cloud-odb-logical/v1\","
		    "\"pid\":%"PRIuMAX,
		    (uintmax_t)getpid());
	strbuf_addf(&line, ",\"runId\":\"%s\"", run_id);
	strbuf_addf(&line,
		    ",\"layout\":\"group\","
		    "\"objectReads\":%"PRIu64",\"usefulBytes\":%"PRIu64","
		    "\"groupCacheHits\":%"PRIu64",\"casRetries\":%"PRIu64","
		    "\"publishes\":%"PRIu64"}\n",
		    object_reads, useful_bytes,
		    group_cache_hits, cas_retries,
		    publishes);
	s3_metrics_append(&line);
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

static int cloud_staging_owner(const char *name, pid_t *owner)
{
	const char *value;
	char *end;
	uintmax_t parsed;

	if (!skip_prefix(name, "cloud-write-", &value))
		return -1;
	errno = 0;
	parsed = strtoumax(value, &end, 10);
	if (errno || end == value || parsed == 0 ||
	    parsed > maximum_signed_value_of_type(pid_t) || *end++ != '-' ||
	    strlen(end) != 6)
		return -1;
	for (const unsigned char *p = (const unsigned char *)end; *p; p++)
		if (!isalnum(*p))
			return -1;
	*owner = (pid_t)parsed;
	return 0;
}

static void cloud_cleanup_local_staging(const char *objects_path)
{
	struct dirent *entry;
	DIR *dir = opendir(objects_path);

	if (!dir)
		return;
	while ((entry = readdir(dir)) != NULL) {
		struct strbuf path = STRBUF_INIT;
		struct stat st;
		size_t directory_len;
		pid_t owner;

		if (cloud_staging_owner(entry->d_name, &owner))
			continue;
		if (kill(owner, 0) == 0 || errno == EPERM)
			continue;
		if (errno != ESRCH)
			continue;
		strbuf_addf(&path, "%s/%s", objects_path, entry->d_name);
		if (lstat(path.buf, &st) || !S_ISDIR(st.st_mode)) {
			strbuf_release(&path);
			continue;
		}
		directory_len = path.len;
		strbuf_addstr(&path, "/data.gsg");
		unlink(path.buf);
		strbuf_setlen(&path, directory_len);
		strbuf_addstr(&path, "/index.gsi");
		unlink(path.buf);
		strbuf_setlen(&path, directory_len);
		rmdir(path.buf);
		strbuf_release(&path);
	}
	closedir(dir);
}

static int read_marker(const char *objects_path, struct strbuf *prefix,
		       const char *storage_id)
{
	struct strbuf path = STRBUF_INIT;
	struct strbuf contents = STRBUF_INIT;
	struct string_list lines = STRING_LIST_INIT_DUP;
	const char *prefix_value;
	const char *storage_value;
	ssize_t bytes;
	int fd = -1;
	int ret = -1;

	strbuf_addf(&path, "%s/cloud-odb", objects_path);
	fd = open(path.buf, O_RDONLY);
	if (fd < 0) {
		error_errno(_("unable to read cloud ODB marker '%s'"), path.buf);
		goto out;
	}
	strbuf_grow(&contents, CLOUD_MARKER_MAX_BYTES + 1);
	bytes = read_in_full(fd, contents.buf, CLOUD_MARKER_MAX_BYTES + 1);
	if (bytes < 0) {
		error_errno(_("unable to read cloud ODB marker '%s'"), path.buf);
		goto out;
	}
	if (bytes > CLOUD_MARKER_MAX_BYTES) {
		error(_("cloud ODB marker '%s' exceeds the size limit"), path.buf);
		goto out;
	}
	strbuf_setlen(&contents, bytes);
	string_list_split(&lines, contents.buf, "\n", -1);
	if (lines.nr && !*lines.items[lines.nr - 1].string) {
		free(lines.items[lines.nr - 1].string);
		lines.nr--;
	}
	if (lines.nr != 4 || strcmp(lines.items[0].string, CLOUD_MARKER_HEADER) ||
	    strcmp(lines.items[1].string, "layout group") ||
	    !skip_prefix(lines.items[2].string, "prefix ", &prefix_value) ||
	    !valid_prefix(prefix_value) ||
	    !skip_prefix(lines.items[3].string, "storage ", &storage_value) ||
	    strcmp(storage_value, storage_id)) {
		error(_("cloud ODB marker '%s' is invalid"), path.buf);
		goto out;
	}
	strbuf_addstr(prefix, prefix_value);
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
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
	source->cached_reader_nr = SIZE_MAX;
	oidmap_clear(&source->object_index, 1);
	odb_cloud_manifest_release(&source->manifest);
}

static void cloud_rebuild_object_index(struct odb_source_cloud *source)
{
	struct oidmap replacement = OIDMAP_INIT;

	for (size_t reader_nr = 0; reader_nr < source->readers_nr; reader_nr++) {
		struct odb_segment_group *group = &source->groups[reader_nr];

		for (size_t entry_nr = 0; entry_nr < group->entries_nr; entry_nr++) {
			struct cloud_lookup_entry *entry, *replaced;

			CALLOC_ARRAY(entry, 1);
			oidcpy(&entry->ent.oid, &group->entries[entry_nr].oid);
			entry->reader_nr = reader_nr;
			entry->entry_nr = entry_nr;
			replaced = oidmap_put(&replacement, entry);
			free(replaced);
		}
	}
	oidmap_clear(&source->object_index, 1);
	source->object_index = replacement;
}

static int cloud_get_manifest(struct odb_source_cloud *source,
			      struct odb_cloud_manifest *manifest,
			      struct strbuf *etag)
{
	struct s3_response response = S3_RESPONSE_INIT;
	int ret = -1;

	if (s3_client_get_limited(&source->client, source->manifest_key.buf,
				  CLOUD_MAX_ARTIFACT_BYTES, &response))
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
	if (artifact->index_bytes > CLOUD_MAX_ARTIFACT_BYTES ||
	    artifact->index_bytes > SIZE_MAX ||
	    s3_client_get_limited(&source->client, artifact->index_key,
				  (size_t)artifact->index_bytes, response) ||
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

	if (artifact->data_bytes > CLOUD_MAX_ARTIFACT_BYTES ||
	    cloud_get_index(source, artifact, &index) ||
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

		for (size_t i = 0; i < source->groups[ordinal].groups_nr; i++) {
			const struct odb_segment_group_info *group =
				&source->groups[ordinal].groups[i];

			if (group->compressed_size > CLOUD_MAX_ARTIFACT_BYTES ||
			    group->size > CLOUD_MAX_TRANSACTION_BYTES) {
				error(_("cloud ODB group exceeds the bounded-artifact policy"));
				odb_segment_group_close(&source->groups[ordinal]);
				goto out;
			}
		}

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

static int cloud_manifest_within_limits(struct odb_source_cloud *source,
					const struct odb_cloud_manifest *manifest)
{
	uint64_t index_bytes = 0;

	if (manifest->artifacts_nr > CLOUD_MAX_MANIFEST_ARTIFACTS ||
	    manifest->pending_nr > CLOUD_MAX_PENDING_ARTIFACTS)
		return error(_("cloud manifest has too many artifacts"));
	for (size_t i = 0; i < manifest->artifacts_nr; i++) {
		uint64_t artifact_bytes = manifest->artifacts[i].index_bytes;

		if (!odb_cloud_manifest_key_is_scoped_artifact(
			    manifest->artifacts[i].data_key, source->prefix.buf, "gsg") ||
		    !odb_cloud_manifest_key_is_scoped_artifact(
			    manifest->artifacts[i].index_key, source->prefix.buf, "gsi"))
			return error(_("cloud manifest contains an artifact outside its storage prefix"));
		if (artifact_bytes > CLOUD_MAX_ARTIFACT_BYTES ||
		    artifact_bytes > CLOUD_MAX_MANIFEST_INDEX_BYTES - index_bytes)
			return error(_("cloud manifest indexes exceed the size limit"));
		index_bytes += artifact_bytes;
	}
	for (size_t i = 0; i < manifest->pending_nr; i++)
		if (!odb_cloud_manifest_key_is_transaction_artifact(
			    manifest->pending[i].data_key, source->prefix.buf,
			    manifest->pending[i].token, "gsg") ||
		    !odb_cloud_manifest_key_is_transaction_artifact(
			    manifest->pending[i].index_key, source->prefix.buf,
			    manifest->pending[i].token, "gsi"))
			return error(_("cloud manifest contains a pending artifact outside its storage prefix"));
		else if (manifest->pending[i].data_bytes > CLOUD_MAX_ARTIFACT_BYTES ||
			 manifest->pending[i].index_bytes > CLOUD_MAX_ARTIFACT_BYTES)
			return error(_("cloud manifest has an oversized pending artifact"));
	return 0;
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
	if (cloud_manifest_within_limits(source, &manifest))
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
	if (source->readers_nr != old_readers)
		cloud_rebuild_object_index(source);
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
				const struct odb_segment_group_entry **entry)
{
	struct cloud_lookup_entry *found = oidmap_get(&source->object_index, oid);

	if (!found)
		return 1;
	if (found->reader_nr >= source->readers_nr ||
	    found->entry_nr >= source->groups[found->reader_nr].entries_nr)
		BUG("cloud ODB object index points outside its readers");
	*reader_nr = found->reader_nr;
	*entry = &source->groups[found->reader_nr].entries[found->entry_nr];
	return 0;
}

static int cloud_read_entry_data(
	struct odb_source_cloud *source, size_t nr,
	const struct odb_segment_group_entry *entry, enum object_type *type,
	size_t *size, size_t *disk_size, void **data);

static int cloud_read_data(struct odb_source_cloud *source,
			   const struct object_id *oid, enum object_type *type,
			   size_t *size, size_t *disk_size, void **data)
{
	const struct odb_segment_group_entry *entry;
	size_t nr;
	int ret = cloud_lookup_segment(source, oid, &nr, &entry);

	if (ret)
		return ret;
	return cloud_read_entry_data(source, nr, entry, type, size,
				     disk_size, data);
}

static int cloud_read_entry_data(
	struct odb_source_cloud *source, size_t nr,
	const struct odb_segment_group_entry *entry, enum object_type *type,
	size_t *size, size_t *disk_size, void **data)
{
	int group_cache_hit = 0;

	{
		const struct odb_segment_group_info *group =
			&source->groups[nr].groups[entry->group_nr];

		*type = entry->type;
		*size = entry->size;
		*disk_size = group->compressed_size;
		if (data) {
			group_cache_hit = source->cached_reader_nr == nr &&
				source->groups[nr].cached_group_nr == entry->group_nr;
			if (source->cached_reader_nr != SIZE_MAX &&
			    source->cached_reader_nr != nr)
				odb_segment_group_clear_cache(
					&source->groups[source->cached_reader_nr]);
			if (odb_segment_group_read(&source->groups[nr], entry, data))
				return -1;
			source->cached_reader_nr = nr;
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
	size_t reader_nr;
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

static int cloud_each_entry(struct cloud_each_data *data,
			    const struct odb_segment_group_entry *entry)
{
	struct object_info oi;
	enum object_type type;
	size_t size, disk_size;
	void *content = NULL;
	const struct object_id *oid = &entry->oid;

	if (data->opts->prefix &&
	    !cloud_match_hash(data->opts->prefix_hex_len,
			      data->opts->prefix->hash, oid->hash))
		return 0;
	if (!data->request)
		return data->cb(oid, NULL, data->cb_data);
	oi = *data->request;
	if (cloud_read_entry_data(data->source, data->reader_nr, entry,
				  &type, &size, &disk_size,
				  oi.contentp ? &content : NULL))
		return -1;
	if (oi.typep)
		*oi.typep = type;
	if (oi.sizep)
		*oi.sizep = size;
	if (oi.disk_sizep)
		*oi.disk_sizep = disk_size;
	if (oi.delta_base_oid)
		oidclr(oi.delta_base_oid,
		       data->source->base.odb->repo->hash_algo);
	if (oi.contentp)
		*oi.contentp = content;
	if (oi.mtimep)
		*oi.mtimep = 0;
	if (oi.source_infop)
		oi.source_infop->source = &data->source->base;
	return data->cb(oid, &oi, data->cb_data);
}

static int cloud_each_group(const struct odb_segment_group_entry *entry,
			    void *cb_data)
{
	return cloud_each_entry(cb_data, entry);
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
			data.reader_nr = i;
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
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);
	const struct odb_segment_group_entry *entry;
	size_t reader_nr;

	if (!cloud_lookup_segment(source, oid, &reader_nr, &entry))
		return 1;
	return odb_source_freshen_object(&source->fallback->base, oid, mtime);
}

static int cloud_transaction_abort(struct odb_transaction *base);

struct cloud_collect_data {
	struct oidset *objects;
};

static int collect_oid(const struct object_id *oid,
		       struct object_info *oi UNUSED, void *cb_data)
{
	struct cloud_collect_data *data = cb_data;

	if (oidset_contains(data->objects, oid))
		return 0;
	if (oidset_size(data->objects) >= CLOUD_MAX_TRANSACTION_OBJECTS)
		return error(_("cloud ODB transaction exceeds the object-count limit"));
	oidset_insert(data->objects, oid);
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

static int artifact_file_key(struct odb_source_cloud *source, const char *path,
			     const char *token, const char *suffix,
			     struct strbuf *contents,
			     struct strbuf *key, uint64_t *bytes)
{
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
	strbuf_reset(contents);
	if (strbuf_read_file(contents, path, st.st_size) < 0)
		goto out;
	sha256_hex(contents->buf, contents->len, hash);
	strbuf_reset(key);
	strbuf_addf(key, "%s/transactions/%s/objects/%s.%s",
		    source->prefix.buf, token, hash, suffix);
	*bytes = contents->len;
	ret = 0;
out:
	return ret;
}

static int upload_immutable(struct odb_source_cloud *source, const char *path,
			    const char *token, const char *suffix,
			    struct strbuf *key,
			    uint64_t *bytes)
{
	struct s3_response response = S3_RESPONSE_INIT;
	struct strbuf contents = STRBUF_INIT;
	int ret = -1;

	if (artifact_file_key(source, path, token, suffix, &contents, key, bytes))
		goto out;
	if (s3_client_put(&source->client, key->buf, contents.buf, contents.len,
			  NULL, 1, &response))
		goto out;
	if (conflict_status(response.http_status)) {
		if (s3_client_get_limited(&source->client, key->buf,
					  contents.len, &response) ||
		    response.http_status != 200 ||
		    response.body.len != contents.len ||
		    memcmp(response.body.buf, contents.buf, contents.len))
			goto out;
	} else if (!successful_status(response.http_status)) {
		goto out;
	}
	ret = 0;
out:
	strbuf_release(&contents);
	s3_response_release(&response);
	return ret;
}

static int manifest_references_key(const struct odb_cloud_manifest *manifest,
				   const char *key)
{
	for (size_t i = 0; i < manifest->artifacts_nr; i++)
		if (!strcmp(manifest->artifacts[i].data_key, key) ||
		    !strcmp(manifest->artifacts[i].index_key, key))
			return 1;
	return 0;
}

static int delete_staging_artifact(struct odb_source_cloud *source,
				   const char *key)
{
	struct s3_response response = S3_RESPONSE_INIT;
	int ret = -1;

	if (s3_client_delete(&source->client, key, &response) ||
	    (!successful_status(response.http_status) &&
	     response.http_status != 404))
		goto out;
	ret = 0;
out:
	s3_response_release(&response);
	return ret;
}

static int make_cloud_token(char token[33])
{
	static const char hex[] = "0123456789abcdef";
	unsigned char random[16];

	if (csprng_bytes(random, sizeof(random), 0) < 0)
		return error(_("unable to generate cloud ODB transaction token"));
	for (size_t i = 0; i < ARRAY_SIZE(random); i++) {
		token[2 * i] = hex[random[i] >> 4];
		token[2 * i + 1] = hex[random[i] & 0xf];
	}
	token[32] = '\0';
	return 0;
}

static int cloud_cas_manifest(struct odb_source_cloud *source,
			      struct odb_cloud_manifest *manifest,
			      const struct strbuf *etag)
{
	struct s3_response response = S3_RESPONSE_INIT;
	struct strbuf serialized = STRBUF_INIT;
	int ret = -1;

	if (cloud_manifest_within_limits(source, manifest) ||
	    manifest->generation == UINT64_MAX)
		goto out;
	manifest->generation++;
	odb_cloud_manifest_write(manifest, &serialized);
	if (serialized.len > CLOUD_MAX_ARTIFACT_BYTES) {
		error(_("cloud ODB manifest exceeds the bounded-artifact policy"));
		goto out;
	}
	if (s3_client_put(&source->client, source->manifest_key.buf,
			  serialized.buf, serialized.len,
			  etag->len ? etag->buf : NULL, !etag->len,
			  &response))
		goto out;
	if (successful_status(response.http_status))
		ret = 0;
	else if (conflict_status(response.http_status))
		ret = 1;
out:
	strbuf_release(&serialized);
	s3_response_release(&response);
	return ret;
}

static int pending_matches(const struct odb_cloud_pending_artifact *pending,
			   const char *data_key, uint64_t data_bytes,
			   const char *index_key, uint64_t index_bytes)
{
	return pending->state == ODB_CLOUD_PENDING_ACTIVE &&
		pending->data_bytes == data_bytes &&
		pending->index_bytes == index_bytes &&
		!strcmp(pending->data_key, data_key) &&
		!strcmp(pending->index_key, index_key);
}

static int register_pending_artifact(struct odb_source_cloud *source,
				     const char *token, uint64_t created_at,
				     const char *data_key, uint64_t data_bytes,
				     const char *index_key, uint64_t index_bytes)
{
	for (unsigned attempt = 0; attempt < CLOUD_CAS_ATTEMPTS; attempt++) {
		struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;
		struct odb_cloud_pending_artifact *pending;
		struct strbuf etag = STRBUF_INIT;
		int ret = -1;

		if (cloud_get_manifest(source, &manifest, &etag) ||
		    cloud_manifest_within_limits(source, &manifest))
			goto attempt_out;
		pending = odb_cloud_manifest_find_pending(&manifest, token);
		if (pending) {
			ret = pending_matches(pending, data_key, data_bytes,
					      index_key, index_bytes) ? 0 : -1;
			goto attempt_out;
		}
		if (manifest.gc_token) {
			ret = 1;
			goto attempt_out;
		}
		if (odb_cloud_manifest_add_pending(
			    &manifest, token, created_at, ODB_CLOUD_PENDING_ACTIVE,
			    data_key, data_bytes, index_key, index_bytes))
			goto attempt_out;
		ret = cloud_cas_manifest(source, &manifest, &etag);
attempt_out:
		strbuf_release(&etag);
		odb_cloud_manifest_release(&manifest);
		if (!ret)
			return 0;
		if (ret < 0)
			return -1;
		sleep_millisec(5 + (getpid() + attempt * 17) % 46);
	}
	return error(_("cloud ODB pending-artifact CAS retry limit exceeded"));
}

static int pending_is_expired(const struct odb_cloud_pending_artifact *pending,
			      uint64_t now, uint64_t grace)
{
	return pending->created_at <= now && now - pending->created_at >= grace;
}

static int manifest_protects_key(const struct odb_cloud_manifest *manifest,
				 const char *key)
{
	if (manifest_references_key(manifest, key))
		return 1;
	for (size_t i = 0; i < manifest->pending_nr; i++)
		if (manifest->pending[i].state != ODB_CLOUD_PENDING_DELETING &&
		    (!strcmp(manifest->pending[i].data_key, key) ||
		     !strcmp(manifest->pending[i].index_key, key)))
			return 1;
	return 0;
}

static int collect_ref_oid(const struct reference *ref, void *cb_data)
{
	struct oidset *oids = cb_data;

	if (!is_null_oid(ref->oid))
		oidset_insert(oids, ref->oid);
	return 0;
}

static int collect_reflog_oid(const char *refname UNUSED,
			      struct object_id *old_oid,
			      struct object_id *new_oid,
			      const char *committer UNUSED,
			      timestamp_t timestamp UNUSED, int tz UNUSED,
			      const char *msg UNUSED, void *cb_data)
{
	struct oidset *oids = cb_data;

	if (!is_null_oid(old_oid))
		oidset_insert(oids, old_oid);
	if (!is_null_oid(new_oid))
		oidset_insert(oids, new_oid);
	return 0;
}

struct collect_reflog_data {
	struct ref_store *refs;
	struct oidset *oids;
};

static int collect_reflog(const char *refname, void *cb_data)
{
	struct collect_reflog_data *data = cb_data;

	return refs_for_each_reflog_ent(data->refs, refname,
					collect_reflog_oid, data->oids);
}

static int collect_ref_evidence(struct odb_source_cloud *source,
				struct oidset *oids)
{
	struct ref_store *refs = get_main_ref_store(source->base.odb->repo);
	struct collect_reflog_data data = { .refs = refs, .oids = oids };

	if (refs_for_each_ref(refs, collect_ref_oid, oids) ||
	    refs_for_each_reflog(refs, collect_reflog, &data))
		return error(_("unable to inspect refs for cloud ODB recovery"));
	return 0;
}

static int pending_artifact_has_ref(
	struct odb_source_cloud *source,
	const struct odb_cloud_pending_artifact *pending,
	const struct oidset *ref_oids, int *found)
{
	struct odb_cloud_artifact artifact = {
		.data_key = pending->data_key,
		.index_key = pending->index_key,
		.data_bytes = pending->data_bytes,
		.index_bytes = pending->index_bytes,
	};
	struct odb_segment_group reader = { 0 };
	struct s3_response index = S3_RESPONSE_INIT;
	struct cloud_range *range = NULL;
	struct strbuf label = STRBUF_INIT;
	unsigned char header[CLOUD_GROUP_HEADER_SIZE];
	int ret = -1;

	*found = 0;
	if (cloud_get_index(source, &artifact, &index) ||
	    s3_client_read_range(&source->client, artifact.data_key,
				 artifact.data_bytes, 0, CLOUD_GROUP_HEADER_SIZE,
				 header))
		goto out;
	range = cloud_range_new(source, artifact.data_key, artifact.data_bytes);
	strbuf_addf(&label, "pending/%s", pending->token);
	if (odb_segment_group_open_reader(
		    &reader, label.buf, artifact.data_bytes, header,
		    CLOUD_GROUP_HEADER_SIZE, index.body.buf, index.body.len,
		    source->base.odb->repo->hash_algo, cloud_range_read,
		    cloud_range_release, range))
		goto out;
	range = NULL;
	for (size_t i = 0; i < reader.entries_nr; i++)
		if (oidset_contains(ref_oids, &reader.entries[i].oid)) {
			*found = 1;
			break;
		}
	ret = 0;
out:
	odb_segment_group_close(&reader);
	cloud_range_release(range);
	strbuf_release(&label);
	s3_response_release(&index);
	return ret;
}

static int recover_pending_artifacts(struct odb_source_cloud *source,
				     int reconcile_published)
{
	char gc_token[33] = "";
	struct oidset ref_oids = OIDSET_INIT;
	time_t current_time = time(NULL);
	uint64_t now;
	uint64_t grace = git_env_ulong(
		"GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS",
		CLOUD_PENDING_GRACE_SECONDS);
	uint64_t lease_seconds = git_env_ulong(
		"GIT_TEST_CLOUD_ODB_GC_LEASE_SECONDS",
		CLOUD_GC_LEASE_SECONDS);
	int final_ret = -1;

	if (current_time <= 0 ||
	    (reconcile_published && collect_ref_evidence(source, &ref_oids)))
		goto out;
	now = (uint64_t)current_time;
	for (unsigned attempt = 0; attempt < CLOUD_CAS_ATTEMPTS; attempt++) {
		struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;
		struct strbuf etag = STRBUF_INIT;
		int changed = 0;
		int has_recovery = 0;
		int ret = -1;

		if (cloud_get_manifest(source, &manifest, &etag) ||
		    cloud_manifest_within_limits(source, &manifest))
			goto attempt_out;
		for (size_t i = 0; i < manifest.pending_nr;) {
			struct odb_cloud_pending_artifact *pending =
				&manifest.pending[i];
			int referenced = 0;

			if (pending->state == ODB_CLOUD_PENDING_PUBLISHED) {
				if (!reconcile_published) {
					i++;
					continue;
				}
				if (pending_artifact_has_ref(source, pending, &ref_oids,
							     &referenced))
					goto attempt_out;
				if (referenced) {
					char token[33];

					strlcpy(token, pending->token, sizeof(token));
					odb_cloud_manifest_remove_pending(&manifest, token);
					changed = 1;
					continue;
				}
				if (pending_is_expired(pending, now, grace)) {
					if (odb_cloud_manifest_remove_artifact(
						    &manifest, pending->data_key,
						    pending->index_key))
						goto attempt_out;
					pending->state = ODB_CLOUD_PENDING_DELETING;
					changed = 1;
					has_recovery = 1;
				}
			} else if (pending->state == ODB_CLOUD_PENDING_DELETING)
				has_recovery = 1;
			else if (pending_is_expired(pending, now, grace)) {
				has_recovery = 1;
				pending->state = ODB_CLOUD_PENDING_DELETING;
				changed = 1;
			}
			i++;
		}
		if (!manifest.gc_token && !has_recovery && !changed) {
			ret = 0;
			goto attempt_out;
		}
		if (has_recovery && !manifest.gc_token) {
			if (!*gc_token && make_cloud_token(gc_token))
				goto attempt_out;
			manifest.gc_token = xstrdup(gc_token);
			manifest.gc_created_at = now;
			changed = 1;
		}
		if (manifest.gc_token && strcmp(manifest.gc_token, gc_token)) {
			if (manifest.gc_created_at <= now &&
			    now - manifest.gc_created_at >= lease_seconds) {
				if (!*gc_token && make_cloud_token(gc_token))
					goto attempt_out;
				free(manifest.gc_token);
				manifest.gc_token = xstrdup(gc_token);
				manifest.gc_created_at = now;
				changed = 1;
			} else {
				ret = 0;
				goto attempt_out;
			}
		}
		if (changed) {
			ret = cloud_cas_manifest(source, &manifest, &etag);
			if (!ret) {
				if (manifest.gc_token)
					cloud_failpoint("after-gc-lease");
				ret = 2;
			}
			goto attempt_out;
		}
		for (size_t i = 0; i < manifest.pending_nr; i++) {
			const struct odb_cloud_pending_artifact *pending =
				&manifest.pending[i];

			if (pending->state != ODB_CLOUD_PENDING_DELETING)
				continue;
			if (!manifest_protects_key(&manifest, pending->data_key) &&
			    delete_staging_artifact(source, pending->data_key))
				goto attempt_out;
			if (!manifest_protects_key(&manifest, pending->index_key) &&
			    delete_staging_artifact(source, pending->index_key))
				goto attempt_out;
		}
		for (size_t i = manifest.pending_nr; i > 0; i--)
			if (manifest.pending[i - 1].state ==
			    ODB_CLOUD_PENDING_DELETING)
				odb_cloud_manifest_remove_pending(
					&manifest, manifest.pending[i - 1].token);
		FREE_AND_NULL(manifest.gc_token);
		manifest.gc_created_at = 0;
		ret = cloud_cas_manifest(source, &manifest, &etag);
attempt_out:
		strbuf_release(&etag);
		odb_cloud_manifest_release(&manifest);
		if (ret == 2)
			continue;
		if (!ret) {
			final_ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
		sleep_millisec(5 + (getpid() + attempt * 17) % 46);
	}
	error(_("cloud ODB recovery CAS retry limit exceeded"));
out:
	oidset_clear(&ref_oids);
	return final_ret;
}

int odb_source_cloud_recover(struct odb_source_cloud *source)
{
	if (recover_pending_artifacts(source, 1))
		return -1;
	/* Recovery may remove an unreferenced artifact from any ordinal. */
	cloud_clear_readers(source);
	return cloud_load(source);
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
	size_t object_count = 0;
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

		if (object_count >= CLOUD_MAX_TRANSACTION_OBJECTS) {
			error(_("cloud ODB transaction exceeds the object-count limit"));
			goto out;
		}
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
		object_count++;
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

static int publish_artifact(struct odb_source_cloud *source, const char *token,
			    const char *data_key, uint64_t data_bytes,
			    const char *index_key, uint64_t index_bytes)
{
	int ret = -1;

	for (unsigned attempt = 0; attempt < CLOUD_CAS_ATTEMPTS; attempt++) {
		struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;
		struct odb_cloud_pending_artifact *pending;
		struct strbuf etag = STRBUF_INIT;
		int attempt_ret = -1;

		if (cloud_get_manifest(source, &manifest, &etag) ||
		    cloud_manifest_within_limits(source, &manifest))
			goto attempt_out;
		pending = odb_cloud_manifest_find_pending(&manifest, token);
		if (!pending || !pending_matches(pending, data_key, data_bytes,
						 index_key, index_bytes))
			goto attempt_out;
		if (!artifact_present(&manifest, data_key, index_key) &&
		    odb_cloud_manifest_add(&manifest, data_key, data_bytes,
					   index_key, index_bytes))
			goto attempt_out;
		pending->state = ODB_CLOUD_PENDING_PUBLISHED;
		cloud_failpoint("before-cas");
		attempt_ret = cloud_cas_manifest(source, &manifest, &etag);
		if (!attempt_ret) {
			cloud_metric_event(source, 0, 0, 0, 0, 1);
		} else if (attempt_ret > 0) {
			cloud_metric_event(source, 0, 0, 0, 1, 0);
			sleep_millisec(5 + (getpid() + attempt * 17) % 46);
		}

attempt_out:
		strbuf_release(&etag);
		odb_cloud_manifest_release(&manifest);
		ret = attempt_ret;
		if (ret <= 0)
			break;
	}
	return ret ? error(_("cloud ODB manifest CAS retry limit exceeded")) : 0;
}

static int cloud_transaction_commit(struct odb_transaction *base)
{
	struct cloud_transaction *transaction =
		container_of(base, struct cloud_transaction, base);
	struct odb_source_cloud *source =
		container_of(base->source, struct odb_source_cloud, base);
	struct odb_source *incoming = base->source->odb->sources;
	struct odb_source_files *incoming_files;
	struct odb_for_each_object_options opts = { 0 };
	struct oidset objects = OIDSET_INIT;
	struct cloud_collect_data collect_data = { .objects = &objects };
	struct strbuf temporary = STRBUF_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	struct strbuf data_key = STRBUF_INIT;
	struct strbuf index_key = STRBUF_INIT;
	struct strbuf artifact_contents = STRBUF_INIT;
	char pending_token[33] = "";
	time_t created_at;
	uint64_t data_bytes = 0, index_bytes = 0;
	int ret = -1;

	if (incoming->type != ODB_SOURCE_FILES)
		BUG("cloud transaction primary source is not files");
	incoming_files = odb_source_files_downcast(incoming);
	if (odb_source_for_each_object(incoming, NULL, collect_oid, &collect_data,
				       &opts))
		goto out;
	if (!oidset_size(&objects)) {
		ret = 0;
		goto discard;
	}
	strbuf_addf(&temporary, "%s/cloud-write-%"PRIuMAX"-XXXXXX",
		    source->base.path, (uintmax_t)getpid());
	if (!mkdtemp(temporary.buf)) {
		error_errno(_("unable to create cloud ODB staging directory"));
		goto out;
	}
	cloud_failpoint("before-artifact-build");
	strbuf_addf(&data_path, "%s/data.gsg", temporary.buf);
	strbuf_addf(&index_path, "%s/index.gsi", temporary.buf);
	if (write_group_artifacts(source, &objects, data_path.buf, index_path.buf))
		goto out;
	created_at = time(NULL);
	if (created_at <= 0 || make_cloud_token(pending_token) ||
	    artifact_file_key(source, data_path.buf, pending_token, "gsg",
			      &artifact_contents,
			      &data_key, &data_bytes) ||
	    artifact_file_key(source, index_path.buf, pending_token, "gsi",
			      &artifact_contents,
			      &index_key, &index_bytes) ||
	    register_pending_artifact(source, pending_token, (uint64_t)created_at,
				      data_key.buf, data_bytes,
				      index_key.buf, index_bytes))
		goto out;
	cloud_failpoint("before-artifact-upload");
	if (upload_immutable(source, data_path.buf, pending_token, "gsg", &data_key,
			     &data_bytes) ||
	    upload_immutable(source, index_path.buf, pending_token, "gsi", &index_key,
			     &index_bytes))
		goto out;
	cloud_failpoint("after-artifact-upload");
	if (repo_migrate_loose_object_map(incoming_files->loose,
					 source->fallback->loose)) {
		error(_("unable to retain compatibility object mappings"));
		goto out;
	}
	if (publish_artifact(source, pending_token, data_key.buf, data_bytes,
			     index_key.buf, index_bytes))
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
	oidset_clear(&transaction->streamed_objects);
	oidset_clear(&objects);
	strbuf_release(&temporary);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
	strbuf_release(&data_key);
	strbuf_release(&index_key);
	strbuf_release(&artifact_contents);
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
	if (!ret) {
		transaction->canonical_bytes += len;
		if (!oidset_contains(&transaction->streamed_objects, oid)) {
			if (oidset_size(&transaction->streamed_objects) >=
			    CLOUD_MAX_TRANSACTION_OBJECTS)
				return error(_("cloud ODB transaction exceeds the object-count limit"));
			oidset_insert(&transaction->streamed_objects, oid);
		}
	}
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

static int cloud_transaction_abort(struct odb_transaction *base)
{
	struct cloud_transaction *transaction =
		container_of(base, struct cloud_transaction, base);
	int ret = 0;

	ASSERT(base == base->source->odb->transaction);
	if (transaction->objdir && tmp_objdir_destroy(transaction->objdir))
		ret = error(_("unable to remove failed cloud ODB quarantine"));
	transaction->objdir = NULL;
	oidset_clear(&transaction->streamed_objects);
	base->source->odb->transaction = NULL;
	free(transaction);
	return ret;
}

static int cloud_write_object(struct odb_source *source,
			      const void *buf, size_t len,
			      enum object_type type,
			      const struct object_id *oid,
			      const struct object_id *compat_oid,
			      const time_t *mtime,
			      enum odb_write_object_flags flags)
{
	struct odb_transaction *transaction;
	int ret;

	if (len > CLOUD_MAX_OBJECT_BYTES || len > CLOUD_MAX_TRANSACTION_BYTES)
		return error(_("cloud ODB transaction exceeds the bounded-object policy"));
	if (odb_transaction_begin(source->odb, &transaction, 0))
		return -1;
	ret = odb_source_write_object(source->odb->sources, buf, len, type, oid,
				      compat_oid, mtime, flags);
	if (ret) {
		if (cloud_transaction_abort(transaction))
			return -1;
		return ret;
	}
	return odb_transaction_commit(transaction);
}

static int cloud_write_object_stream(struct odb_source *source,
				     struct odb_write_stream *stream,
				     size_t len, struct object_id *oid)
{
	struct odb_transaction *transaction;
	int ret;

	if (len > CLOUD_MAX_OBJECT_BYTES || len > CLOUD_MAX_TRANSACTION_BYTES)
		return error(_("cloud ODB transaction exceeds the bounded-object policy"));
	if (odb_transaction_begin(source->odb, &transaction, 0))
		return -1;
	ret = odb_transaction_write_object_stream(transaction, stream, len, oid);
	if (ret) {
		if (cloud_transaction_abort(transaction))
			return -1;
		return ret;
	}
	return odb_transaction_commit(transaction);
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
	return error(_("files fallback optimization is unsupported by the cloud ODB"));
}

static bool cloud_optimize_required(struct odb_source *base,
				    const struct odb_optimize_options *opts)
{
	struct odb_source_cloud *source = container_of(base, struct odb_source_cloud,
						       base);

	return odb_source_files_optimize_required(&source->fallback->base, opts);
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
	if (odb_source_cloud_recover(source))
		warning(_("unable to reconcile cloud ODB publications"));
	else if ((flags & ODB_PREPARE_FLUSH_CACHES) && cloud_load(source))
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
	struct strbuf segment_manifest = STRBUF_INIT;
	struct strbuf storage_id = STRBUF_INIT;

	strbuf_addf(&segment_manifest, "%s/segments/manifest", path);
	if (file_exists(segment_manifest.buf))
		die(_("cloud ODB cannot hide an existing segment store at '%s'"),
		    path);
	strbuf_release(&segment_manifest);

	CALLOC_ARRAY(source, 1);
	source->cached_reader_nr = SIZE_MAX;
	source->prefix = (struct strbuf)STRBUF_INIT;
	source->manifest_key = (struct strbuf)STRBUF_INIT;
	odb_source_init(&source->base, odb, ODB_SOURCE_CLOUD, path, local);
	cloud_cleanup_local_staging(path);
	if (s3_client_init_from_env(&source->client))
		die(_("unable to configure cloud ODB at '%s'"), path);
	s3_client_storage_id(&source->client, &storage_id);
	if (read_marker(path, &source->prefix, storage_id.buf))
		die(_("unable to configure cloud ODB at '%s'"), path);
	strbuf_release(&storage_id);
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
	source->base.write_object = cloud_write_object;
	source->base.write_object_stream = cloud_write_object_stream;
	source->base.begin_transaction = cloud_begin_transaction;
	source->base.read_alternates = cloud_read_alternates;
	source->base.write_alternate = cloud_write_alternate;
	source->base.optimize = cloud_optimize;
	source->base.optimize_required = cloud_optimize_required;
	if (recover_pending_artifacts(source, 0))
		warning(_("unable to finish cloud ODB pending-artifact recovery"));
	if (cloud_load(source))
		die(_("unable to open cloud ODB at '%s'"), path);
	return source;
}
