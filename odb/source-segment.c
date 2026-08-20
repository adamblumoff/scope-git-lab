#include "git-compat-util.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "lockfile.h"
#include "object-file.h"
#include "odb.h"
#include "odb/segment.h"
#include "odb/source-files.h"
#include "odb/source-segment.h"
#include "odb/streaming.h"
#include "oidset.h"
#include "repository.h"
#include "strbuf.h"
#include "strvec.h"
#include "tmp-objdir.h"

static void segment_source_clear(struct odb_source_segment *source)
{
	size_t i;

	for (i = 0; i < source->segments_nr; i++)
		odb_segment_close(&source->segments[i]);
	free(source->segments);
	source->segments = NULL;
	source->segments_nr = 0;
	odb_segment_manifest_release(&source->manifest);
}

static int segment_source_open_manifest(struct odb_source_segment *source,
					struct odb_segment_manifest *manifest)
{
	size_t i;
	int ret = -1;

	CALLOC_ARRAY(source->segments, manifest->entries_nr);
	for (i = 0; i < manifest->entries_nr; i++) {
		struct odb_segment_manifest_entry *entry = &manifest->entries[i];
		struct strbuf data_path = STRBUF_INIT;
		struct strbuf index_path = STRBUF_INIT;

		strbuf_addf(&data_path, "%s/segments/%s", source->base.path,
			    entry->data_name);
		strbuf_addf(&index_path, "%s/segments/%s", source->base.path,
			    entry->index_name);
		if (odb_segment_open(&source->segments[i], data_path.buf,
				     index_path.buf,
				     source->base.odb->repo->hash_algo)) {
			strbuf_release(&data_path);
			strbuf_release(&index_path);
			goto out;
		}
		source->segments_nr++;
		strbuf_release(&data_path);
		strbuf_release(&index_path);
	}
	source->manifest = *manifest;
	*manifest = (struct odb_segment_manifest)ODB_SEGMENT_MANIFEST_INIT;
	ret = 0;
out:
	if (ret)
		segment_source_clear(source);
	return ret;
}

static int segment_source_load(struct odb_source_segment *source)
{
	struct odb_segment_manifest manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct strbuf manifest_path = STRBUF_INIT;
	int ret = -1;

	segment_source_clear(source);
	strbuf_addf(&manifest_path, "%s/segments/manifest", source->base.path);
	if (odb_segment_manifest_read(&manifest, manifest_path.buf,
				      source->base.odb->repo->hash_algo))
		goto out;
	ret = segment_source_open_manifest(source, &manifest);
out:
	odb_segment_manifest_release(&manifest);
	strbuf_release(&manifest_path);
	return ret;
}

static int segment_source_lookup(struct odb_source_segment *source,
				 const struct object_id *oid,
				 struct odb_segment **segment_out,
				 const struct odb_segment_index_entry **entry_out)
{
	size_t i = source->segments_nr;

	/* Later manifest entries shadow earlier entries. */
	while (i--) {
		int ret = odb_segment_lookup(&source->segments[i], oid, entry_out);

		if (ret < 0)
			return -1;
		if (!ret) {
			*segment_out = &source->segments[i];
			return 0;
		}
	}
	return 1;
}

static int segment_read_entry_info(struct odb_source *base,
				   struct odb_segment *segment,
				   const struct odb_segment_index_entry *entry,
				   struct object_info *oi)
{
	void *data;

	if (odb_segment_read(segment, entry, &data))
		return -1;
	if (!oi) {
		free(data);
		return 0;
	}
	if (oi->typep)
		*oi->typep = entry->type;
	if (oi->sizep)
		*oi->sizep = entry->size;
	if (oi->disk_sizep)
		*oi->disk_sizep = entry->disk_size;
	if (oi->delta_base_oid)
		oidclr(oi->delta_base_oid, base->odb->repo->hash_algo);
	if (oi->contentp)
		*oi->contentp = data;
	else
		free(data);
	if (oi->mtimep)
		*oi->mtimep = 0;
	if (oi->source_infop)
		oi->source_infop->source = base;
	return 0;
}

static int segment_read_object_info(struct odb_source *base,
				    const struct object_id *oid,
				    struct object_info *oi,
				    enum object_info_flags flags)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);
	struct odb_segment *segment;
	const struct odb_segment_index_entry *entry;
	int ret;

	ret = segment_source_lookup(source, oid, &segment, &entry);
	if (ret < 0)
		return -1;
	if (ret > 0)
		return odb_source_read_object_info(&source->fallback->base, oid, oi,
						   flags);
	return segment_read_entry_info(base, segment, entry, oi);
}

struct segment_read_stream {
	struct odb_read_stream base;
	unsigned char *data;
	size_t offset;
};

static ssize_t segment_stream_read(struct odb_read_stream *base,
				   char *buf, size_t len)
{
	struct segment_read_stream *stream =
		container_of(base, struct segment_read_stream, base);
	size_t remaining = base->size - stream->offset;
	size_t bytes = len < remaining ? len : remaining;

	memcpy(buf, stream->data + stream->offset, bytes);
	stream->offset += bytes;
	return bytes;
}

static int segment_stream_close(struct odb_read_stream *base)
{
	struct segment_read_stream *stream =
		container_of(base, struct segment_read_stream, base);

	free(stream->data);
	return 0;
}

static int segment_read_object_stream(struct odb_read_stream **out,
				      struct odb_source *base,
				      const struct object_id *oid)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);
	struct odb_segment *segment;
	const struct odb_segment_index_entry *entry;
	struct segment_read_stream *stream;
	int ret;

	ret = segment_source_lookup(source, oid, &segment, &entry);
	if (ret < 0)
		return -1;
	if (ret > 0)
		return odb_source_read_object_stream(out, &source->fallback->base, oid);
	CALLOC_ARRAY(stream, 1);
	if (odb_segment_read(segment, entry, (void **)&stream->data)) {
		free(stream);
		return -1;
	}
	stream->base.read = segment_stream_read;
	stream->base.close = segment_stream_close;
	stream->base.type = entry->type;
	stream->base.size = entry->size;
	*out = &stream->base;
	return 0;
}

struct segment_each_data {
	struct odb_source *source;
	struct odb_segment *segment;
	const struct object_info *request;
	odb_for_each_object_cb cb;
	void *cb_data;
	const struct odb_for_each_object_options *opts;
};

static int segment_match_hash(size_t len, const unsigned char *a,
			      const unsigned char *b)
{
	while (len > 1) {
		if (*a++ != *b++)
			return 0;
		len -= 2;
	}
	return !len || !((*a ^ *b) & 0xf0);
}

static int segment_each_entry(const struct odb_segment_index_entry *entry,
			      void *cb_data)
{
	struct segment_each_data *data = cb_data;
	struct object_info oi;

	if (data->opts->prefix &&
	    !segment_match_hash(data->opts->prefix_hex_len,
				data->opts->prefix->hash, entry->oid.hash))
		return 0;
	if (!data->request) {
		if (segment_read_entry_info(data->source, data->segment, entry, NULL))
			return -1;
		return data->cb(&entry->oid, NULL, data->cb_data);
	}

	oi = *data->request;
	if (segment_read_entry_info(data->source, data->segment, entry, &oi))
		return -1;
	return data->cb(&entry->oid, &oi, data->cb_data);
}

static int segment_for_each_object(struct odb_source *base,
				   const struct object_info *request,
				   odb_for_each_object_cb cb,
				   void *cb_data,
				   const struct odb_for_each_object_options *opts)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);
	struct segment_each_data data = {
		.source = base,
		.request = request,
		.cb = cb,
		.cb_data = cb_data,
		.opts = opts,
	};
	size_t i;

	if ((opts->flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY) ||
	    ((opts->flags & ODB_FOR_EACH_OBJECT_LOCAL_ONLY) && !base->local))
		return 0;
	for (i = 0; i < source->segments_nr; i++) {
		int ret;

		data.segment = &source->segments[i];
		ret = odb_segment_for_each(&source->segments[i],
					   segment_each_entry, &data);
		if (ret)
			return ret;
	}
	return 0;
}

static int segment_count_objects(struct odb_source *base,
				 enum odb_count_objects_flags flags UNUSED,
				 unsigned long *out)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);
	uint64_t total = 0;
	size_t i;

	for (i = 0; i < source->segments_nr; i++) {
		struct odb_segment *segment = &source->segments[i];
		size_t j;

		for (j = 0; j < segment->entries_nr; j++) {
			if (segment_read_entry_info(base, segment,
						    &segment->entries[j], NULL))
				return -1;
			total++;
		}
	}
	if (total > ULONG_MAX)
		return error(_("segment object count exceeds platform limit"));
	*out = total;
	return 0;
}

struct segment_abbrev_data {
	const struct object_id *oid;
	unsigned len;
};

static int segment_abbrev_cb(const struct object_id *oid,
			     struct object_info *oi UNUSED,
			     void *cb_data)
{
	struct segment_abbrev_data *data = cb_data;
	unsigned common = oid_common_prefix_hexlen(oid, data->oid);

	if (common != hash_algos[oid->algo].hexsz && common >= data->len)
		data->len = common + 1;
	return 0;
}

static int segment_find_abbrev_len(struct odb_source *base,
				   const struct object_id *oid,
				   unsigned min_len,
				   unsigned *out)
{
	struct odb_for_each_object_options opts = {
		.prefix = oid,
		.prefix_hex_len = min_len,
	};
	struct segment_abbrev_data data = { .oid = oid, .len = min_len };
	int ret = segment_for_each_object(base, NULL, segment_abbrev_cb, &data,
					  &opts);

	*out = data.len;
	return ret;
}

static int segment_freshen_object(struct odb_source *base,
				  const struct object_id *oid,
				  const time_t *mtime)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);
	struct odb_segment *segment;
	const struct odb_segment_index_entry *entry;
	int ret;

	ret = segment_source_lookup(source, oid, &segment, &entry);
	if (ret < 0)
		return 0;
	if (ret > 0)
		return odb_source_freshen_object(&source->fallback->base, oid, mtime);
	return !segment_read_entry_info(base, segment, entry, NULL);
}

static int segment_unsupported_write(struct odb_source *source UNUSED,
				     const void *buf UNUSED,
				     size_t len UNUSED,
				     enum object_type type UNUSED,
				     const struct object_id *oid UNUSED,
				     const struct object_id *compat_oid UNUSED,
				     const time_t *mtime UNUSED,
				     enum odb_write_object_flags flags UNUSED)
{
	return error(_("direct writes to the experimental segment store are unsupported"));
}

static int segment_unsupported_stream(struct odb_source *source UNUSED,
				      struct odb_write_stream *stream UNUSED,
				      size_t len UNUSED,
				      struct object_id *oid UNUSED)
{
	return error(_("direct streamed writes to the experimental segment store are unsupported"));
}

struct segment_transaction {
	struct odb_transaction base;
	struct tmp_objdir *objdir;
};

static int transaction_collect_oid(const struct object_id *oid,
				   struct object_info *oi UNUSED,
				   void *cb_data)
{
	oidset_insert(cb_data, oid);
	return 0;
}

static int segment_transaction_commit(struct odb_transaction *base)
{
	struct segment_transaction *transaction =
		container_of(base, struct segment_transaction, base);
	struct odb_source_segment *source = odb_source_segment_downcast(base->source);
	struct odb_source_segment staged = { 0 };
	struct odb_segment_writer writer = ODB_SEGMENT_WRITER_INIT;
	struct odb_segment_manifest manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct odb_segment_stats stats;
	struct oidset objects = OIDSET_INIT;
	struct oidset_iter iter;
	struct object_id *oid;
	struct lock_file manifest_lock = LOCK_INIT;
	struct strbuf directory = STRBUF_INIT;
	struct strbuf manifest_path = STRBUF_INIT;
	struct strbuf rollback_path = STRBUF_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	struct odb_for_each_object_options opts = { 0 };
	struct odb_source *incoming = base->source->odb->sources;
	unsigned int number;
	int migrate_ret;
	int lock_fd;
	int ret = -1;

	if (incoming->type != ODB_SOURCE_FILES)
		BUG("segment transaction primary source is not files");
	staged.base.odb = source->base.odb;
	staged.base.path = source->base.path;
	strbuf_addf(&directory, "%s/segments", base->source->path);
	strbuf_addf(&manifest_path, "%s/manifest", directory.buf);
	strbuf_addf(&rollback_path, "%s/manifest.rollback", directory.buf);
	lock_fd = repo_hold_lock_file_for_update(base->source->odb->repo,
						 &manifest_lock,
						 manifest_path.buf,
						 LOCK_DIE_ON_ERROR);
	if (odb_segment_manifest_read(&manifest, manifest_path.buf,
				      base->source->odb->repo->hash_algo) ||
	    odb_segment_next_paths(directory.buf, &number, &data_path,
				   &index_path))
		goto out;
	if (odb_source_for_each_object(incoming, NULL, transaction_collect_oid,
				       &objects, &opts))
		goto out;
	if (odb_segment_writer_open(&writer, data_path.buf, index_path.buf,
				    base->source->odb->repo->hash_algo))
		goto out;
	for (oid = oidset_iter_first(&objects, &iter); oid;
	     oid = oidset_iter_next(&iter)) {
		enum object_type type;
		size_t size;
		void *data = odb_read_object(base->source->odb, oid, &type, &size);

		if (!data) {
			error(_("unable to read quarantined object %s"), oid_to_hex(oid));
			goto out;
		}
		if (odb_segment_writer_add(&writer, oid, type, data, size)) {
			free(data);
			goto out;
		}
		free(data);
	}
	if (odb_segment_writer_finish(&writer, &stats) ||
	    odb_segment_adjust_shared_perm(base->source->odb->repo,
					   data_path.buf, index_path.buf) ||
	    odb_segment_fsync_directory(directory.buf) ||
	    odb_segment_manifest_add(&manifest,
				     strrchr(data_path.buf, '/') + 1,
				     strrchr(index_path.buf, '/') + 1) ||
	    odb_segment_manifest_write_fd(&manifest, lock_fd) ||
	    fsync(lock_fd) < 0 ||
	    segment_source_open_manifest(&staged, &manifest))
		goto out;
	if (unlink(rollback_path.buf) < 0 && errno != ENOENT) {
		error_errno(_("unable to remove stale segment manifest rollback"));
		goto out;
	}
	if (link(manifest_path.buf, rollback_path.buf) < 0 ||
	    odb_segment_fsync_directory(directory.buf)) {
		error_errno(_("unable to preserve segment manifest rollback"));
		goto out;
	}

	/*
	 * Keep the conventional object copy usable before publishing the segment
	 * manifest. This makes removing the marker a complete rollback even after
	 * receive-pack has accepted new objects.
	 */
	migrate_ret = tmp_objdir_migrate(transaction->objdir);
	transaction->objdir = NULL;
	if (migrate_ret) {
		error(_("unable to retain received objects in the files backend"));
		goto out;
	}
	odb_segment_test_failpoint("before-manifest-rename");
	if (commit_lock_file(&manifest_lock)) {
		error_errno(_("unable to publish segment manifest"));
		goto out;
	}
	odb_segment_test_failpoint("after-manifest-rename");
	if (odb_segment_fsync_directory(directory.buf)) {
		if (rename(rollback_path.buf, manifest_path.buf) < 0)
			die_errno(_("unable to restore segment manifest after fsync failure"));
		odb_segment_fsync_directory(directory.buf);
		goto out;
	}
	if (unlink(rollback_path.buf) < 0 && errno != ENOENT)
		warning_errno(_("unable to remove segment manifest rollback"));
	segment_source_clear(source);
	source->manifest = staged.manifest;
	source->segments = staged.segments;
	source->segments_nr = staged.segments_nr;
	staged.manifest = (struct odb_segment_manifest)ODB_SEGMENT_MANIFEST_INIT;
	staged.segments = NULL;
	staged.segments_nr = 0;
	ret = 0;
out:
	if (ret)
		odb_segment_writer_abort(&writer);
	if (ret && transaction->objdir) {
		if (tmp_objdir_destroy(transaction->objdir))
			error(_("unable to remove failed segment quarantine"));
		transaction->objdir = NULL;
	}
	rollback_lock_file(&manifest_lock);
	oidset_clear(&objects);
	odb_segment_manifest_release(&manifest);
	segment_source_clear(&staged);
	strbuf_release(&directory);
	strbuf_release(&manifest_path);
	strbuf_release(&rollback_path);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
	return ret;
}

static int segment_transaction_write_stream(struct odb_transaction *base,
					    struct odb_write_stream *stream,
					    size_t len,
					    struct object_id *oid)
{
	return odb_source_write_object_stream(base->source->odb->sources,
					      stream, len, oid);
}

static int segment_transaction_env(struct odb_transaction *base,
				   struct strvec *env)
{
	struct segment_transaction *transaction =
		container_of(base, struct segment_transaction, base);

	strvec_pushv(env, tmp_objdir_env(transaction->objdir));
	return 0;
}

static int segment_begin_transaction(struct odb_source *source,
				     struct odb_transaction **out,
				     enum odb_transaction_flags flags UNUSED)
{
	struct segment_transaction *transaction;

	CALLOC_ARRAY(transaction, 1);
	transaction->base.source = source;
	transaction->base.commit = segment_transaction_commit;
	transaction->base.write_object_stream = segment_transaction_write_stream;
	transaction->base.env = segment_transaction_env;
	transaction->objdir = tmp_objdir_create(source->odb->repo, "incoming");
	if (!transaction->objdir) {
		free(transaction);
		return error(_("unable to create segment quarantine"));
	}
	tmp_objdir_replace_primary_odb(transaction->objdir, 0);
	*out = &transaction->base;
	return 0;
}

static int segment_read_alternates(struct odb_source *source,
				   struct strvec *out)
{
	struct strbuf buf = STRBUF_INIT;
	char *path = xstrfmt("%s/info/alternates", source->path);

	if (strbuf_read_file(&buf, path, 1024) < 0) {
		warn_on_fopen_errors(path);
		free(path);
		return 0;
	}
	parse_alternates(buf.buf, '\n', source->path, out);
	strbuf_release(&buf);
	free(path);
	return 0;
}

static int segment_write_alternate(struct odb_source *source UNUSED,
				   const char *alternate UNUSED)
{
	return error(_("alternates are unsupported by the experimental segment store"));
}

static void segment_close(struct odb_source *base)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);

	odb_source_close(&source->fallback->base);
}

static void segment_prepare(struct odb_source *base,
			    enum odb_prepare_flags flags)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);

	odb_source_prepare(&source->fallback->base, flags);
	if ((flags & ODB_PREPARE_FLUSH_CACHES) && segment_source_load(source))
		die(_("unable to reload experimental segment store"));
}

static void segment_free(struct odb_source *base)
{
	struct odb_source_segment *source = odb_source_segment_downcast(base);

	segment_source_clear(source);
	odb_source_free(&source->fallback->base);
	odb_source_release(base);
	free(source);
}

struct odb_source_segment *odb_source_segment_new(struct object_database *odb,
						  const char *path,
						  bool local)
{
	struct odb_source_segment *source;

	CALLOC_ARRAY(source, 1);
	odb_source_init(&source->base, odb, ODB_SOURCE_SEGMENT, path, local);
	source->fallback = odb_source_files_new(odb, path, local);
	source->base.free = segment_free;
	source->base.close = segment_close;
	source->base.prepare = segment_prepare;
	source->base.read_object_info = segment_read_object_info;
	source->base.read_object_stream = segment_read_object_stream;
	source->base.for_each_object = segment_for_each_object;
	source->base.count_objects = segment_count_objects;
	source->base.find_abbrev_len = segment_find_abbrev_len;
	source->base.freshen_object = segment_freshen_object;
	source->base.write_object = segment_unsupported_write;
	source->base.write_object_stream = segment_unsupported_stream;
	source->base.begin_transaction = segment_begin_transaction;
	source->base.read_alternates = segment_read_alternates;
	source->base.write_alternate = segment_write_alternate;

	if (segment_source_load(source))
		die(_("unable to open experimental segment store at '%s'"), path);
	return source;
}
