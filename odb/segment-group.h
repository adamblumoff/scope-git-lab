#ifndef ODB_SEGMENT_GROUP_H
#define ODB_SEGMENT_GROUP_H

#include "object.h"

struct strbuf;

/*
 * Group segment version 1 stores concatenated object contents in independent
 * zlib streams. A writer starts a new group before an object would make the
 * current group exceed `group_target`. A single large object forms an
 * oversized group. Objects and compression dictionaries never cross group
 * boundaries.
 *
 * The 32-byte data header contains `GSGG`, version, hash format ID, a reserved
 * word, group count, and object count. Compressed groups follow without
 * padding.
 *
 * The index starts with a 48-byte header containing `GSGX`, version, hash
 * format ID, a reserved word, object count, group count, data-file size, and
 * the writer's group target. Each group table entry contains its data offset,
 * compressed size, and canonical size. The sorted object table follows. Each
 * object ID is followed by its group number, type, offset within the
 * uncompressed group, canonical size, and a reserved word. All integers are
 * big-endian.
 */
struct odb_segment_group_entry {
	struct object_id oid;
	enum object_type type;
	uint32_t group_nr;
	uint64_t group_offset;
	uint64_t size;
};

struct odb_segment_group_info {
	uint64_t offset;
	uint64_t compressed_size;
	uint64_t size;
};

struct odb_segment_group {
	char *data_path;
	const struct git_hash_algo *hash_algo;
	struct odb_segment_group_entry *entries;
	size_t entries_nr;
	struct odb_segment_group_info *groups;
	size_t groups_nr;
	uint64_t data_bytes;
	uint64_t index_bytes;
	uint64_t group_target;
	unsigned char *cached_group;
	size_t cached_group_nr;
};

#define ODB_SEGMENT_GROUP_INIT { .cached_group_nr = SIZE_MAX }

struct odb_segment_group_stats {
	uint64_t objects;
	uint64_t groups;
	uint64_t data_bytes;
	uint64_t index_bytes;
};

int odb_segment_group_open(struct odb_segment_group *segment,
			   const char *data_path, const char *index_path,
			   const struct git_hash_algo *hash_algo);
void odb_segment_group_close(struct odb_segment_group *segment);

/* Return 0 when found and 1 when absent. */
int odb_segment_group_lookup(const struct odb_segment_group *segment,
			     const struct object_id *oid,
			     const struct odb_segment_group_entry **entry);

/* Read and hash-check one full canonical object. The caller owns `*data`. */
int odb_segment_group_read(struct odb_segment_group *segment,
			   const struct odb_segment_group_entry *entry,
			   void **data);

typedef int odb_segment_group_each_fn(
	const struct odb_segment_group_entry *entry, void *cb_data);
int odb_segment_group_for_each(const struct odb_segment_group *segment,
			       odb_segment_group_each_fn cb, void *cb_data);

struct odb_segment_group_writer {
	int data_fd;
	int index_fd;
	char *data_path;
	char *index_path;
	const struct git_hash_algo *hash_algo;
	struct odb_segment_group_entry *entries;
	size_t entries_nr;
	size_t entries_alloc;
	struct odb_segment_group_info *groups;
	size_t groups_nr;
	size_t groups_alloc;
	struct strbuf *group_data;
	size_t current_entries;
	size_t group_target;
	uint64_t data_offset;
};

#define ODB_SEGMENT_GROUP_WRITER_INIT { \
	.data_fd = -1, \
	.index_fd = -1, \
}

int odb_segment_group_writer_open(struct odb_segment_group_writer *writer,
				  const char *data_path,
				  const char *index_path,
				  const struct git_hash_algo *hash_algo,
				  size_t group_target);
int odb_segment_group_writer_add(struct odb_segment_group_writer *writer,
				 const struct object_id *oid,
				 enum object_type type,
				 const void *data, size_t size);
int odb_segment_group_writer_finish(struct odb_segment_group_writer *writer,
				    struct odb_segment_group_stats *stats);
void odb_segment_group_writer_abort(struct odb_segment_group_writer *writer);

#endif /* ODB_SEGMENT_GROUP_H */
