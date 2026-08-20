#ifndef ODB_SEGMENT_H
#define ODB_SEGMENT_H

#include "object.h"

struct strbuf;
struct repository;

/*
 * Version 1 stores full, independently zlib-compressed object payloads in the
 * data file. Its 16-byte header is `GSGD`, version, hash format ID, and a
 * reserved word.
 *
 * The index starts with `GSGI`, version, hash format ID, a reserved word,
 * object count, and data-file size. Each object ID is followed by its data
 * offset, canonical size, stored size, type, and a reserved word. Integers are
 * big-endian and index entries are strictly ordered by object ID.
 */
struct odb_segment_index_entry {
	struct object_id oid;
	enum object_type type;
	uint64_t size;
	uint64_t disk_size;
	uint64_t offset;
};

struct odb_segment {
	char *data_path;
	const struct git_hash_algo *hash_algo;
	struct odb_segment_index_entry *entries;
	size_t entries_nr;
	uint64_t data_bytes;
	uint64_t index_bytes;
};

#define ODB_SEGMENT_INIT { 0 }

struct odb_segment_stats {
	uint64_t objects;
	uint64_t data_bytes;
	uint64_t index_bytes;
};

/*
 * Open a matching segment and index pair. If `hash_algo` is non-NULL, reject
 * files written for a different object format.
 */
int odb_segment_open(struct odb_segment *segment,
		     const char *data_path, const char *index_path,
		     const struct git_hash_algo *hash_algo);
void odb_segment_close(struct odb_segment *segment);

/*
 * Return 0 when found, 1 when absent, and -1 if the segment cannot be read.
 * The returned entry belongs to `segment` and remains valid until close.
 */
int odb_segment_lookup(const struct odb_segment *segment,
		       const struct object_id *oid,
		       const struct odb_segment_index_entry **entry);

/* Read and verify the full canonical object. The caller owns `*data`. */
int odb_segment_read(const struct odb_segment *segment,
		     const struct odb_segment_index_entry *entry,
		     void **data);

typedef int odb_segment_each_fn(const struct odb_segment_index_entry *entry,
				void *cb_data);
int odb_segment_for_each(const struct odb_segment *segment,
			 odb_segment_each_fn cb, void *cb_data);

struct odb_segment_writer {
	int data_fd;
	int index_fd;
	char *data_path;
	char *index_path;
	const struct git_hash_algo *hash_algo;
	struct odb_segment_index_entry *entries;
	size_t entries_nr;
	size_t entries_alloc;
	uint64_t data_offset;
};

#define ODB_SEGMENT_WRITER_INIT { \
	.data_fd = -1, \
	.index_fd = -1, \
}

/* Create a new immutable pair. Existing paths are never overwritten. */
int odb_segment_writer_open(struct odb_segment_writer *writer,
			    const char *data_path, const char *index_path,
			    const struct git_hash_algo *hash_algo);
int odb_segment_writer_add(struct odb_segment_writer *writer,
			   const struct object_id *oid,
			   enum object_type type,
			   const void *data, size_t size);
int odb_segment_writer_finish(struct odb_segment_writer *writer,
			      struct odb_segment_stats *stats);
/* Close and remove files created by an unfinished writer. */
void odb_segment_writer_abort(struct odb_segment_writer *writer);

struct odb_segment_manifest_entry {
	char *data_name;
	char *index_name;
};

struct odb_segment_manifest {
	const struct git_hash_algo *hash_algo;
	struct odb_segment_manifest_entry *entries;
	size_t entries_nr;
	size_t entries_alloc;
};

#define ODB_SEGMENT_MANIFEST_INIT { 0 }

/*
 * The text format starts with:
 *
 *   git-segment-manifest 1
 *   hash <algorithm>
 *
 * Each following line is `segment <data-name> <index-name>`. Names are
 * relative basenames. Entry order is preserved by both reader and writer.
 */
void odb_segment_manifest_init(struct odb_segment_manifest *manifest,
			       const struct git_hash_algo *hash_algo);
void odb_segment_manifest_release(struct odb_segment_manifest *manifest);
int odb_segment_manifest_add(struct odb_segment_manifest *manifest,
			     const char *data_name, const char *index_name);
int odb_segment_manifest_read(struct odb_segment_manifest *manifest,
			      const char *path,
			      const struct git_hash_algo *hash_algo);
/* Write to a caller-owned descriptor so publication can use a lockfile. */
int odb_segment_manifest_write_fd(const struct odb_segment_manifest *manifest,
				  int fd);

int odb_segment_next_paths(const char *directory, unsigned int *number,
			   struct strbuf *data_path,
			   struct strbuf *index_path);
int odb_segment_fsync_directory(const char *directory);
int odb_segment_adjust_shared_perm(struct repository *repo,
				   const char *data_path,
				   const char *index_path);
void odb_segment_test_failpoint(const char *name);

#endif /* ODB_SEGMENT_H */
