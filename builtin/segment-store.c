#define USE_THE_REPOSITORY_VARIABLE
#include "builtin.h"
#include "abspath.h"
#include "dir.h"
#include "environment.h"
#include "gettext.h"
#include "hex.h"
#include "lockfile.h"
#include "object-file.h"
#include "odb.h"
#include "odb/segment.h"
#include "oidset.h"
#include "parse-options.h"
#include "path.h"
#include "repository.h"
#include "strbuf.h"

static const char * const segment_store_usage[] = {
	N_("git segment-store import"),
	N_("git segment-store compact"),
	N_("git segment-store stats"),
	NULL,
};

static int collect_oid(const struct object_id *oid,
		       struct object_info *oi UNUSED,
		       void *cb_data)
{
	struct oidset *objects = cb_data;

	oidset_insert(objects, oid);
	return 0;
}

static int ensure_segment_directory(struct repository *repo,
				    struct strbuf *directory)
{
	strbuf_addf(directory, "%s/segments", repo_get_object_directory(repo));
	if (!mkdir(directory->buf, 0777))
		return adjust_shared_perm(repo, directory->buf);
	if (errno == EEXIST && is_directory(directory->buf))
		return 0;
	return error_errno(_("unable to create segment directory '%s'"),
			   directory->buf);
}

static int segment_store_import(struct repository *repo)
{
	struct odb_segment_writer writer = ODB_SEGMENT_WRITER_INIT;
	struct odb_segment_manifest manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct odb_segment_stats stats;
	struct oidset objects = OIDSET_INIT;
	struct oidset_iter iter;
	struct object_id *oid;
	struct lock_file manifest_lock = LOCK_INIT;
	struct strbuf directory = STRBUF_INIT;
	struct strbuf manifest_path = STRBUF_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	unsigned int number;
	int lock_fd = -1;
	int ret = -1;

	strbuf_addf(&manifest_path, "%s/segments/manifest",
		    repo_get_object_directory(repo));
	if (file_exists(manifest_path.buf)) {
		error(_("segment store is already active"));
		goto out;
	}
	if (ensure_segment_directory(repo, &directory) ||
	    odb_segment_next_paths(directory.buf, &number, &data_path, &index_path))
		goto out;
	if (odb_for_each_object(repo->objects, NULL, collect_oid, &objects,
				ODB_FOR_EACH_OBJECT_LOCAL_ONLY)) {
		error(_("unable to enumerate objects for segment import"));
		goto out;
	}
	if (odb_segment_writer_open(&writer, data_path.buf, index_path.buf,
				    repo->hash_algo))
		goto out;
	for (oid = oidset_iter_first(&objects, &iter); oid;
	     oid = oidset_iter_next(&iter)) {
		enum object_type type;
		size_t size;
		void *data = odb_read_object(repo->objects, oid, &type, &size);

		if (!data) {
			error(_("unable to read object %s during segment import"),
			      oid_to_hex(oid));
			goto out;
		}
		if (odb_segment_writer_add(&writer, oid, type, data, size)) {
			free(data);
			goto out;
		}
		free(data);
	}
	if (odb_segment_writer_finish(&writer, &stats) ||
	    odb_segment_adjust_shared_perm(repo, data_path.buf, index_path.buf))
		goto out;
	if (odb_segment_fsync_directory(directory.buf))
		goto out;

	odb_segment_manifest_init(&manifest, repo->hash_algo);
	if (odb_segment_manifest_add(&manifest,
				     strrchr(data_path.buf, '/') + 1,
				     strrchr(index_path.buf, '/') + 1))
		goto out;
	lock_fd = repo_hold_lock_file_for_update(repo, &manifest_lock,
						 manifest_path.buf,
						 LOCK_DIE_ON_ERROR);
	if (odb_segment_manifest_write_fd(&manifest, lock_fd))
		goto out;
	if (fsync(lock_fd) < 0) {
		error_errno(_("unable to fsync segment manifest"));
		goto out;
	}
	odb_segment_test_failpoint("before-manifest-rename");
	if (commit_lock_file(&manifest_lock)) {
		error_errno(_("unable to publish segment manifest"));
		goto out;
	}
	if (odb_segment_fsync_directory(directory.buf))
		goto out;
	odb_segment_test_failpoint("after-manifest-rename");
	printf("imported %"PRIu64" objects into %08u\n", stats.objects, number);
	ret = 0;
out:
	if (ret)
		odb_segment_writer_abort(&writer);
	rollback_lock_file(&manifest_lock);
	oidset_clear(&objects);
	odb_segment_manifest_release(&manifest);
	strbuf_release(&directory);
	strbuf_release(&manifest_path);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
	return ret;
}

static int segment_store_stats(struct repository *repo)
{
	struct odb_segment_manifest manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct strbuf directory = STRBUF_INIT;
	struct strbuf manifest_path = STRBUF_INIT;
	uint64_t objects = 0, data_bytes = 0, index_bytes = 0;
	size_t i;
	int ret = -1;

	strbuf_addf(&directory, "%s/segments", repo_get_object_directory(repo));
	strbuf_addf(&manifest_path, "%s/manifest", directory.buf);
	if (odb_segment_manifest_read(&manifest, manifest_path.buf,
				      repo->hash_algo))
		goto out;
	for (i = 0; i < manifest.entries_nr; i++) {
		struct odb_segment segment = ODB_SEGMENT_INIT;
		struct strbuf data_path = STRBUF_INIT;
		struct strbuf index_path = STRBUF_INIT;

		strbuf_addf(&data_path, "%s/%s", directory.buf,
			    manifest.entries[i].data_name);
		strbuf_addf(&index_path, "%s/%s", directory.buf,
			    manifest.entries[i].index_name);
		if (odb_segment_open(&segment, data_path.buf, index_path.buf,
				     repo->hash_algo)) {
			strbuf_release(&data_path);
			strbuf_release(&index_path);
			goto out;
		}
		objects += segment.entries_nr;
		data_bytes += segment.data_bytes;
		index_bytes += segment.index_bytes;
		odb_segment_close(&segment);
		strbuf_release(&data_path);
		strbuf_release(&index_path);
	}
	printf("segments %"PRIuMAX"\n", (uintmax_t)manifest.entries_nr);
	printf("objects %"PRIu64"\n", objects);
	printf("data_bytes %"PRIu64"\n", data_bytes);
	printf("index_bytes %"PRIu64"\n", index_bytes);
	ret = 0;
out:
	odb_segment_manifest_release(&manifest);
	strbuf_release(&directory);
	strbuf_release(&manifest_path);
	return ret;
}

struct compact_data {
	struct odb_segment *segment;
	struct odb_segment_writer *writer;
	struct oidset *seen;
};

static int compact_one_object(const struct odb_segment_index_entry *entry,
			      void *cb_data)
{
	struct compact_data *data = cb_data;
	void *contents;
	int ret;

	if (oidset_insert(data->seen, &entry->oid))
		return 0;
	if (odb_segment_read(data->segment, entry, &contents))
		return -1;
	ret = odb_segment_writer_add(data->writer, &entry->oid, entry->type,
				     contents, entry->size);
	free(contents);
	return ret;
}

static int segment_store_compact(struct repository *repo)
{
	struct odb_segment_manifest old_manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct odb_segment_manifest new_manifest = ODB_SEGMENT_MANIFEST_INIT;
	struct odb_segment_writer writer = ODB_SEGMENT_WRITER_INIT;
	struct oidset seen = OIDSET_INIT;
	struct lock_file manifest_lock = LOCK_INIT;
	struct strbuf directory = STRBUF_INIT;
	struct strbuf manifest_path = STRBUF_INIT;
	struct strbuf rollback_path = STRBUF_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	unsigned int number;
	int lock_fd;
	int ret = -1;
	size_t i;

	strbuf_addf(&directory, "%s/segments", repo_get_object_directory(repo));
	strbuf_addf(&manifest_path, "%s/manifest", directory.buf);
	strbuf_addf(&rollback_path, "%s/manifest.rollback", directory.buf);
	lock_fd = repo_hold_lock_file_for_update(repo, &manifest_lock,
						 manifest_path.buf,
						 LOCK_DIE_ON_ERROR);
	if (odb_segment_manifest_read(&old_manifest, manifest_path.buf,
				      repo->hash_algo) ||
	    odb_segment_next_paths(directory.buf, &number, &data_path, &index_path) ||
	    odb_segment_writer_open(&writer, data_path.buf, index_path.buf,
				    repo->hash_algo))
		goto out;

	for (i = old_manifest.entries_nr; i > 0; i--) {
		struct odb_segment segment = ODB_SEGMENT_INIT;
		struct compact_data data = {
			.segment = &segment,
			.writer = &writer,
			.seen = &seen,
		};
		struct strbuf old_data = STRBUF_INIT;
		struct strbuf old_index = STRBUF_INIT;

		strbuf_addf(&old_data, "%s/%s", directory.buf,
			    old_manifest.entries[i - 1].data_name);
		strbuf_addf(&old_index, "%s/%s", directory.buf,
			    old_manifest.entries[i - 1].index_name);
		if (odb_segment_open(&segment, old_data.buf, old_index.buf,
				     repo->hash_algo) ||
		    odb_segment_for_each(&segment, compact_one_object, &data)) {
			odb_segment_close(&segment);
			strbuf_release(&old_data);
			strbuf_release(&old_index);
			goto out;
		}
		odb_segment_close(&segment);
		strbuf_release(&old_data);
		strbuf_release(&old_index);
	}
	if (odb_segment_writer_finish(&writer, NULL) ||
	    odb_segment_adjust_shared_perm(repo, data_path.buf, index_path.buf) ||
	    odb_segment_fsync_directory(directory.buf))
		goto out;
	odb_segment_manifest_init(&new_manifest, repo->hash_algo);
	if (odb_segment_manifest_add(&new_manifest,
				     strrchr(data_path.buf, '/') + 1,
				     strrchr(index_path.buf, '/') + 1) ||
	    odb_segment_manifest_write_fd(&new_manifest, lock_fd) ||
	    fsync(lock_fd) < 0)
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
	odb_segment_test_failpoint("before-manifest-rename");
	if (commit_lock_file(&manifest_lock)) {
		error_errno(_("unable to publish compacted segment manifest"));
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
	printf("compacted %"PRIuMAX" segments into %08u\n",
	       (uintmax_t)old_manifest.entries_nr, number);
	ret = 0;
out:
	if (ret)
		odb_segment_writer_abort(&writer);
	rollback_lock_file(&manifest_lock);
	oidset_clear(&seen);
	odb_segment_manifest_release(&old_manifest);
	odb_segment_manifest_release(&new_manifest);
	strbuf_release(&directory);
	strbuf_release(&manifest_path);
	strbuf_release(&rollback_path);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
	return ret;
}

int cmd_segment_store(int argc, const char **argv,
		      const char *prefix UNUSED, struct repository *repo)
{
	if (argc != 2)
		usage_with_options(segment_store_usage, NULL);
	if (!strcmp(argv[1], "import"))
		return segment_store_import(repo);
	if (!strcmp(argv[1], "compact"))
		return segment_store_compact(repo);
	if (!strcmp(argv[1], "stats"))
		return segment_store_stats(repo);
	usage_with_options(segment_store_usage, NULL);
}
