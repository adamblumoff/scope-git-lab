#include "git-compat-util.h"
#include "git-zlib.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "odb/segment-group.h"
#include "strbuf.h"

#define GROUP_DATA_MAGIC "GSGG"
#define GROUP_INDEX_MAGIC "GSGX"
#define GROUP_VERSION 1
#define GROUP_DATA_HEADER_SIZE 32
#define GROUP_INDEX_HEADER_SIZE 48
#define GROUP_TABLE_ENTRY_SIZE 24
#define GROUP_OBJECT_TRAILER_SIZE 32

struct group_span {
	uint32_t group_nr;
	uint64_t offset;
	uint64_t size;
};

static int valid_object_type(enum object_type type)
{
	return type == OBJ_COMMIT || type == OBJ_TREE ||
		type == OBJ_BLOB || type == OBJ_TAG;
}

static void hash_group_object(const struct git_hash_algo *hash_algo,
			      enum object_type type,
			      const void *data, size_t size,
			      struct object_id *oid)
{
	struct git_hash_ctx ctx;
	char header[32];
	int header_len;

	header_len = xsnprintf(header, sizeof(header), "%s %"PRIuMAX,
			       type_name(type), (uintmax_t)size) + 1;
	git_hash_init(&ctx, hash_algo);
	git_hash_update(&ctx, header, header_len);
	git_hash_update(&ctx, data, size);
	git_hash_final_oid(oid, &ctx);
}

static int close_fd(int *fd)
{
	int ret = 0;

	if (*fd >= 0) {
		ret = close(*fd);
		*fd = -1;
	}
	return ret;
}

static int stat_file(int fd, const char *path, uint64_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return error_errno(_("unable to stat group segment file '%s'"),
				   path);
	if (!S_ISREG(st.st_mode) || st.st_size < 0)
		return error(_("group segment file '%s' is not a regular file"),
			     path);
	*size = st.st_size;
	return 0;
}

static int read_exact(int fd, void *buf, size_t size, off_t offset,
		      const char *path)
{
	ssize_t result = pread_in_full(fd, buf, size, offset);

	if (result < 0)
		return error_errno(_("unable to read group segment file '%s'"),
				   path);
	if ((size_t)result != size)
		return error(_("truncated group segment file '%s'"), path);
	return 0;
}

static size_t object_entry_size(const struct git_hash_algo *hash_algo)
{
	return hash_algo->rawsz + GROUP_OBJECT_TRAILER_SIZE;
}

static int span_cmp(const void *va, const void *vb)
{
	const struct group_span *a = va;
	const struct group_span *b = vb;

	if (a->group_nr != b->group_nr)
		return a->group_nr < b->group_nr ? -1 : 1;
	if (a->offset != b->offset)
		return a->offset < b->offset ? -1 : 1;
	if (a->size != b->size)
		return a->size < b->size ? -1 : 1;
	return 0;
}

static int validate_object_coverage(const struct odb_segment_group *segment)
{
	struct group_span *spans;
	size_t span_nr = 0;
	size_t i;

	CALLOC_ARRAY(spans, segment->entries_nr);
	for (i = 0; i < segment->entries_nr; i++) {
		const struct odb_segment_group_entry *entry = &segment->entries[i];

		spans[i].group_nr = entry->group_nr;
		spans[i].offset = entry->group_offset;
		spans[i].size = entry->size;
	}
	QSORT(spans, segment->entries_nr, span_cmp);

	for (i = 0; i < segment->groups_nr; i++) {
		uint64_t offset = 0;
		size_t first = span_nr;

		while (span_nr < segment->entries_nr &&
		       spans[span_nr].group_nr == i) {
			if (spans[span_nr].offset != offset ||
			    spans[span_nr].size > UINT64_MAX - offset) {
				free(spans);
				return error(_("group segment objects overlap or leave a gap"));
			}
			offset += spans[span_nr].size;
			span_nr++;
		}
		if (span_nr == first || offset != segment->groups[i].size) {
			free(spans);
			return error(_("group segment has inconsistent object coverage"));
		}
	}
	free(spans);
	return span_nr == segment->entries_nr ? 0 :
		error(_("group segment object has an invalid group"));
}

static int parse_groups(struct odb_segment_group *segment,
			const unsigned char *raw)
{
	uint64_t next_offset = GROUP_DATA_HEADER_SIZE;
	size_t i;

	CALLOC_ARRAY(segment->groups, segment->groups_nr);
	for (i = 0; i < segment->groups_nr; i++) {
		struct odb_segment_group_info *group = &segment->groups[i];

		group->offset = get_be64(raw);
		group->compressed_size = get_be64(raw + 8);
		group->size = get_be64(raw + 16);
		raw += GROUP_TABLE_ENTRY_SIZE;

		if (group->offset != next_offset ||
		    group->offset > segment->data_bytes ||
		    !group->compressed_size ||
		    group->compressed_size > segment->data_bytes - group->offset)
			return error(_("group segment has invalid group bounds"));
		next_offset += group->compressed_size;
	}
	if (next_offset != segment->data_bytes)
		return error(_("group segment data has trailing bytes"));
	return 0;
}

static int parse_entries(struct odb_segment_group *segment,
			 const unsigned char *raw)
{
	size_t i;

	CALLOC_ARRAY(segment->entries, segment->entries_nr);
	for (i = 0; i < segment->entries_nr; i++) {
		struct odb_segment_group_entry *entry = &segment->entries[i];
		const struct odb_segment_group_info *group;
		uint32_t type;

		oidread(&entry->oid, raw, segment->hash_algo);
		raw += segment->hash_algo->rawsz;
		entry->group_nr = get_be32(raw);
		type = get_be32(raw + 4);
		entry->group_offset = get_be64(raw + 8);
		entry->size = get_be64(raw + 16);
		if (get_be64(raw + 24))
			return error(_("group segment index has non-zero reserved data"));
		entry->type = type;
		raw += GROUP_OBJECT_TRAILER_SIZE;

		if (!valid_object_type(entry->type))
			return error(_("group segment index has invalid object type %"PRIu32),
				     type);
		if (entry->group_nr >= segment->groups_nr)
			return error(_("group segment index has an invalid group number"));
		group = &segment->groups[entry->group_nr];
		if (entry->group_offset > group->size ||
		    entry->size > group->size - entry->group_offset)
			return error(_("group segment index has an out-of-bounds object"));
		if (i && oidcmp(&segment->entries[i - 1].oid, &entry->oid) >= 0)
			return error(_("group segment index is not strictly sorted"));
	}
	return validate_object_coverage(segment);
}

int odb_segment_group_open(struct odb_segment_group *segment,
			   const char *data_path, const char *index_path,
			   const struct git_hash_algo *hash_algo)
{
	unsigned char data_header[GROUP_DATA_HEADER_SIZE];
	unsigned char index_header[GROUP_INDEX_HEADER_SIZE];
	unsigned char *raw_groups = NULL;
	unsigned char *raw_entries = NULL;
	uint64_t data_groups, data_objects, groups_nr, entries_nr;
	uint64_t stored_data_bytes, expected_index_bytes;
	uint32_t data_algo, index_algo;
	size_t groups_size, entries_size, entry_size;
	int data_fd = -1, index_fd = -1;
	int ret = -1;

	*segment = (struct odb_segment_group)ODB_SEGMENT_GROUP_INIT;
	data_fd = open(data_path, O_RDONLY);
	if (data_fd < 0) {
		error_errno(_("unable to open group segment data '%s'"), data_path);
		goto out;
	}
	index_fd = open(index_path, O_RDONLY);
	if (index_fd < 0) {
		error_errno(_("unable to open group segment index '%s'"), index_path);
		goto out;
	}
	if (stat_file(data_fd, data_path, &segment->data_bytes) ||
	    stat_file(index_fd, index_path, &segment->index_bytes) ||
	    segment->data_bytes < sizeof(data_header) ||
	    segment->index_bytes < sizeof(index_header)) {
		if (segment->data_bytes < sizeof(data_header) ||
		    segment->index_bytes < sizeof(index_header))
			error(_("group segment pair has a truncated header"));
		goto out;
	}
	if (read_exact(data_fd, data_header, sizeof(data_header), 0, data_path) ||
	    read_exact(index_fd, index_header, sizeof(index_header), 0,
		       index_path))
		goto out;
	if (memcmp(data_header, GROUP_DATA_MAGIC, 4) ||
	    memcmp(index_header, GROUP_INDEX_MAGIC, 4) ||
	    get_be32(data_header + 4) != GROUP_VERSION ||
	    get_be32(index_header + 4) != GROUP_VERSION) {
		error(_("group segment pair has an unsupported format"));
		goto out;
	}
	data_algo = hash_algo_by_id(get_be32(data_header + 8));
	index_algo = hash_algo_by_id(get_be32(index_header + 8));
	if (!data_algo || data_algo != index_algo ||
	    (hash_algo && data_algo != hash_algo_by_ptr(hash_algo))) {
		error(_("group segment pair uses an incompatible object format"));
		goto out;
	}
	if (get_be32(data_header + 12) || get_be32(index_header + 12)) {
		error(_("group segment pair has non-zero reserved header data"));
		goto out;
	}
	segment->hash_algo = &hash_algos[data_algo];
	data_groups = get_be64(data_header + 16);
	data_objects = get_be64(data_header + 24);
	entries_nr = get_be64(index_header + 16);
	groups_nr = get_be64(index_header + 24);
	stored_data_bytes = get_be64(index_header + 32);
	segment->group_target = get_be64(index_header + 40);
	if (data_groups != groups_nr || data_objects != entries_nr ||
	    stored_data_bytes != segment->data_bytes || !segment->group_target) {
		error(_("group segment pair has inconsistent headers"));
		goto out;
	}
	if (!!entries_nr != !!groups_nr) {
		error(_("group segment pair has inconsistent empty tables"));
		goto out;
	}
	if (entries_nr > SIZE_MAX / sizeof(*segment->entries) ||
	    groups_nr > SIZE_MAX / sizeof(*segment->groups) ||
	    groups_nr > UINT32_MAX) {
		error(_("group segment index has too many entries"));
		goto out;
	}
	segment->entries_nr = entries_nr;
	segment->groups_nr = groups_nr;
	entry_size = object_entry_size(segment->hash_algo);
	if (segment->groups_nr >
	    (SIZE_MAX - GROUP_INDEX_HEADER_SIZE) / GROUP_TABLE_ENTRY_SIZE) {
		error(_("group segment group table is too large"));
		goto out;
	}
	groups_size = st_mult(segment->groups_nr, GROUP_TABLE_ENTRY_SIZE);
	if (segment->entries_nr >
	    (SIZE_MAX - GROUP_INDEX_HEADER_SIZE - groups_size) / entry_size) {
		error(_("group segment object table is too large"));
		goto out;
	}
	entries_size = st_mult(segment->entries_nr, entry_size);
	expected_index_bytes = GROUP_INDEX_HEADER_SIZE + groups_size + entries_size;
	if (expected_index_bytes != segment->index_bytes) {
		error(_("group segment index has an invalid size"));
		goto out;
	}
	if (groups_size) {
		raw_groups = xmalloc(groups_size);
		if (read_exact(index_fd, raw_groups, groups_size,
			       GROUP_INDEX_HEADER_SIZE, index_path) ||
		    parse_groups(segment, raw_groups))
			goto out;
	}
	if (entries_size) {
		raw_entries = xmalloc(entries_size);
		if (read_exact(index_fd, raw_entries, entries_size,
			       GROUP_INDEX_HEADER_SIZE + groups_size,
			       index_path) ||
		    parse_entries(segment, raw_entries))
			goto out;
	}
	segment->data_path = xstrdup(data_path);
	ret = 0;
out:
	free(raw_groups);
	free(raw_entries);
	close_fd(&data_fd);
	close_fd(&index_fd);
	if (ret)
		odb_segment_group_close(segment);
	return ret;
}

void odb_segment_group_close(struct odb_segment_group *segment)
{
	free(segment->data_path);
	free(segment->entries);
	free(segment->groups);
	free(segment->cached_group);
	*segment = (struct odb_segment_group)ODB_SEGMENT_GROUP_INIT;
}

int odb_segment_group_lookup(const struct odb_segment_group *segment,
			     const struct object_id *oid,
			     const struct odb_segment_group_entry **entry)
{
	size_t lo = 0, hi = segment->entries_nr;

	if (oid->algo && oid->algo != hash_algo_by_ptr(segment->hash_algo))
		return 1;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int cmp = oidcmp(oid, &segment->entries[mid].oid);

		if (cmp < 0)
			hi = mid;
		else if (cmp > 0)
			lo = mid + 1;
		else {
			*entry = &segment->entries[mid];
			return 0;
		}
	}
	return 1;
}

static int load_group(struct odb_segment_group *segment, size_t group_nr)
{
	const struct odb_segment_group_info *group = &segment->groups[group_nr];
	git_zstream stream = { 0 };
	unsigned char *compressed;
	unsigned char *data;
	ssize_t result;
	int data_fd;
	int status;

	if (segment->cached_group_nr == group_nr)
		return 0;
	if (group->compressed_size > SIZE_MAX || group->size >= SIZE_MAX ||
	    group->compressed_size > ULONG_MAX || group->size > ULONG_MAX)
		return error(_("group segment group is too large to read"));
	compressed = xmalloc(group->compressed_size);
	data = xmallocz(group->size);
	data_fd = open(segment->data_path, O_RDONLY);
	if (data_fd < 0) {
		free(compressed);
		free(data);
		return error_errno(_("unable to open group segment data '%s'"),
				   segment->data_path);
	}
	result = pread_in_full(data_fd, compressed, group->compressed_size,
			       group->offset);
	if (close(data_fd) < 0 && result >= 0)
		result = -1;
	if (result < 0) {
		free(compressed);
		free(data);
		return error_errno(_("unable to read group segment data"));
	}
	if ((uint64_t)result != group->compressed_size) {
		free(compressed);
		free(data);
		return error(_("truncated group segment data"));
	}

	git_inflate_init(&stream);
	stream.next_in = compressed;
	stream.avail_in = group->compressed_size;
	stream.next_out = data;
	stream.avail_out = group->size ? group->size : 1;
	status = git_inflate(&stream, Z_FINISH);
	git_inflate_end(&stream);
	free(compressed);
	if (status != Z_STREAM_END ||
	    stream.total_in != group->compressed_size ||
	    stream.total_out != group->size) {
		free(data);
		return error(_("unable to inflate group segment group"));
	}
	free(segment->cached_group);
	segment->cached_group = data;
	segment->cached_group_nr = group_nr;
	return 0;
}

int odb_segment_group_read(struct odb_segment_group *segment,
			   const struct odb_segment_group_entry *entry,
			   void **data)
{
	struct object_id actual_oid;
	void *result;

	if (entry->size >= SIZE_MAX)
		return error(_("group segment object is too large to read"));
	if (load_group(segment, entry->group_nr))
		return -1;
	result = xmemdupz(segment->cached_group + entry->group_offset,
			  entry->size);
	hash_group_object(segment->hash_algo, entry->type, result,
			  entry->size, &actual_oid);
	if (!oideq(&actual_oid, &entry->oid)) {
		free(result);
		return error(_("group segment object hash mismatch for %s"),
			     oid_to_hex(&entry->oid));
	}
	*data = result;
	return 0;
}

int odb_segment_group_for_each(const struct odb_segment_group *segment,
			       odb_segment_group_each_fn cb, void *cb_data)
{
	size_t i;

	for (i = 0; i < segment->entries_nr; i++) {
		int ret = cb(&segment->entries[i], cb_data);

		if (ret)
			return ret;
	}
	return 0;
}

static int entry_cmp(const void *va, const void *vb)
{
	const struct odb_segment_group_entry *a = va;
	const struct odb_segment_group_entry *b = vb;

	return oidcmp(&a->oid, &b->oid);
}

static void writer_release(struct odb_segment_group_writer *writer)
{
	free(writer->data_path);
	free(writer->index_path);
	free(writer->entries);
	free(writer->groups);
	if (writer->group_data) {
		strbuf_release(writer->group_data);
		free(writer->group_data);
	}
	*writer = (struct odb_segment_group_writer)ODB_SEGMENT_GROUP_WRITER_INIT;
}

int odb_segment_group_writer_open(struct odb_segment_group_writer *writer,
				  const char *data_path,
				  const char *index_path,
				  const struct git_hash_algo *hash_algo,
				  size_t group_target)
{
	unsigned char header[GROUP_DATA_HEADER_SIZE] = { 0 };

	*writer = (struct odb_segment_group_writer)ODB_SEGMENT_GROUP_WRITER_INIT;
	if (!hash_algo || !hash_algo_by_ptr(hash_algo))
		return error(_("cannot write group segment with unknown object format"));
	if (!group_target)
		return error(_("cannot write group segment with zero group target"));
	writer->hash_algo = hash_algo;
	writer->group_target = group_target;
	writer->data_fd = open(data_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (writer->data_fd < 0) {
		error_errno(_("unable to create group segment data '%s'"), data_path);
		goto error;
	}
	writer->data_path = xstrdup(data_path);
	writer->index_fd = open(index_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (writer->index_fd < 0) {
		error_errno(_("unable to create group segment index '%s'"), index_path);
		goto error;
	}
	writer->index_path = xstrdup(index_path);
	writer->group_data = xmalloc(sizeof(*writer->group_data));
	strbuf_init(writer->group_data, 0);

	memcpy(header, GROUP_DATA_MAGIC, 4);
	put_be32(header + 4, GROUP_VERSION);
	put_be32(header + 8, hash_algo->format_id);
	if (write_in_full(writer->data_fd, header, sizeof(header)) < 0) {
		error_errno(_("unable to write group segment data header"));
		goto error;
	}
	writer->data_offset = sizeof(header);
	return 0;
error:
	odb_segment_group_writer_abort(writer);
	return -1;
}

static int flush_group(struct odb_segment_group_writer *writer)
{
	struct odb_segment_group_info *group;
	git_zstream stream = { 0 };
	unsigned char *compressed;
	unsigned long bound;
	int status;

	if (!writer->current_entries)
		return 0;
	if (writer->group_data->len > ULONG_MAX)
		return error(_("group segment group is too large to compress"));
	git_deflate_init(&stream, Z_DEFAULT_COMPRESSION);
	bound = git_deflate_bound(&stream, writer->group_data->len);
	compressed = xmalloc(bound);
	stream.next_in = (void *)writer->group_data->buf;
	stream.avail_in = writer->group_data->len;
	stream.next_out = compressed;
	stream.avail_out = bound;
	status = git_deflate(&stream, Z_FINISH);
	if (git_deflate_end_gently(&stream) != Z_OK)
		status = Z_DATA_ERROR;
	if (status != Z_STREAM_END) {
		free(compressed);
		return error(_("unable to compress group segment group"));
	}
	if (stream.total_out > UINT64_MAX - writer->data_offset) {
		free(compressed);
		return error(_("group segment data is too large"));
	}
	if (write_in_full(writer->data_fd, compressed, stream.total_out) < 0) {
		free(compressed);
		return error_errno(_("unable to write group segment group"));
	}
	free(compressed);

	ALLOC_GROW(writer->groups, writer->groups_nr + 1,
		   writer->groups_alloc);
	group = &writer->groups[writer->groups_nr++];
	group->offset = writer->data_offset;
	group->compressed_size = stream.total_out;
	group->size = writer->group_data->len;
	writer->data_offset += stream.total_out;
	strbuf_reset(writer->group_data);
	writer->current_entries = 0;
	return 0;
}

int odb_segment_group_writer_add(struct odb_segment_group_writer *writer,
				 const struct object_id *oid,
				 enum object_type type,
				 const void *data, size_t size)
{
	struct odb_segment_group_entry *entry;
	struct object_id actual_oid;

	if (writer->data_fd < 0 || writer->index_fd < 0)
		BUG("adding an object to an unopened group segment writer");
	if (!valid_object_type(type))
		return error(_("cannot write invalid object type %d to group segment"),
			     type);
	if (oid->algo && oid->algo != hash_algo_by_ptr(writer->hash_algo))
		return error(_("cannot mix object formats in a group segment"));
	hash_group_object(writer->hash_algo, type, data, size, &actual_oid);
	if (!oideq(&actual_oid, oid))
		return error(_("object data does not match object ID %s"),
			     oid_to_hex(oid));
	if (writer->current_entries &&
	    (size > writer->group_target ||
	     writer->group_data->len > writer->group_target - size) &&
	    flush_group(writer))
		return -1;
	if (!writer->current_entries && writer->groups_nr >= UINT32_MAX)
		return error(_("group segment has too many groups"));

	ALLOC_GROW(writer->entries, writer->entries_nr + 1,
		   writer->entries_alloc);
	entry = &writer->entries[writer->entries_nr++];
	entry->oid = *oid;
	entry->type = type;
	entry->group_nr = writer->groups_nr;
	entry->group_offset = writer->group_data->len;
	entry->size = size;
	strbuf_add(writer->group_data, data, size);
	writer->current_entries++;
	return 0;
}

static int write_data_header(struct odb_segment_group_writer *writer)
{
	unsigned char header[GROUP_DATA_HEADER_SIZE] = { 0 };

	memcpy(header, GROUP_DATA_MAGIC, 4);
	put_be32(header + 4, GROUP_VERSION);
	put_be32(header + 8, writer->hash_algo->format_id);
	put_be64(header + 16, writer->groups_nr);
	put_be64(header + 24, writer->entries_nr);
	if (lseek(writer->data_fd, 0, SEEK_SET) < 0)
		return error_errno(_("unable to seek group segment data"));
	if (write_in_full(writer->data_fd, header, sizeof(header)) < 0)
		return error_errno(_("unable to finish group segment data header"));
	return 0;
}

static int write_index(struct odb_segment_group_writer *writer,
		       uint64_t *index_bytes)
{
	unsigned char header[GROUP_INDEX_HEADER_SIZE] = { 0 };
	unsigned char raw_group[GROUP_TABLE_ENTRY_SIZE];
	unsigned char *raw_entry;
	size_t entry_size = object_entry_size(writer->hash_algo);
	size_t i;

	QSORT(writer->entries, writer->entries_nr, entry_cmp);
	for (i = 1; i < writer->entries_nr; i++) {
		if (oideq(&writer->entries[i - 1].oid, &writer->entries[i].oid))
			return error(_("cannot write duplicate object %s to group segment"),
				     oid_to_hex(&writer->entries[i].oid));
	}
	if (writer->groups_nr >
	    (SIZE_MAX - GROUP_INDEX_HEADER_SIZE) / GROUP_TABLE_ENTRY_SIZE)
		return error(_("group segment group table is too large"));
	if (writer->entries_nr >
	    (SIZE_MAX - GROUP_INDEX_HEADER_SIZE -
	     writer->groups_nr * GROUP_TABLE_ENTRY_SIZE) / entry_size)
		return error(_("group segment object table is too large"));

	memcpy(header, GROUP_INDEX_MAGIC, 4);
	put_be32(header + 4, GROUP_VERSION);
	put_be32(header + 8, writer->hash_algo->format_id);
	put_be64(header + 16, writer->entries_nr);
	put_be64(header + 24, writer->groups_nr);
	put_be64(header + 32, writer->data_offset);
	put_be64(header + 40, writer->group_target);
	if (write_in_full(writer->index_fd, header, sizeof(header)) < 0)
		return error_errno(_("unable to write group segment index header"));

	for (i = 0; i < writer->groups_nr; i++) {
		memset(raw_group, 0, sizeof(raw_group));
		put_be64(raw_group, writer->groups[i].offset);
		put_be64(raw_group + 8, writer->groups[i].compressed_size);
		put_be64(raw_group + 16, writer->groups[i].size);
		if (write_in_full(writer->index_fd, raw_group,
				  sizeof(raw_group)) < 0)
			return error_errno(_("unable to write group segment group table"));
	}

	raw_entry = xmalloc(entry_size);
	for (i = 0; i < writer->entries_nr; i++) {
		const struct odb_segment_group_entry *entry = &writer->entries[i];

		memset(raw_entry, 0, entry_size);
		hashcpy(raw_entry, entry->oid.hash, writer->hash_algo);
		put_be32(raw_entry + writer->hash_algo->rawsz, entry->group_nr);
		put_be32(raw_entry + writer->hash_algo->rawsz + 4, entry->type);
		put_be64(raw_entry + writer->hash_algo->rawsz + 8,
			 entry->group_offset);
		put_be64(raw_entry + writer->hash_algo->rawsz + 16, entry->size);
		if (write_in_full(writer->index_fd, raw_entry, entry_size) < 0) {
			free(raw_entry);
			return error_errno(_("unable to write group segment object table"));
		}
	}
	free(raw_entry);
	*index_bytes = GROUP_INDEX_HEADER_SIZE +
		writer->groups_nr * GROUP_TABLE_ENTRY_SIZE +
		writer->entries_nr * entry_size;
	return 0;
}

int odb_segment_group_writer_finish(struct odb_segment_group_writer *writer,
				    struct odb_segment_group_stats *stats)
{
	struct odb_segment_group_stats result = { 0 };
	int ret = 0;

	if (writer->data_fd < 0 || writer->index_fd < 0)
		BUG("finishing an unopened group segment writer");
	if (flush_group(writer) || write_data_header(writer))
		return -1;
	result.objects = writer->entries_nr;
	result.groups = writer->groups_nr;
	result.data_bytes = writer->data_offset;
	if (git_fsync(writer->data_fd, FSYNC_HARDWARE_FLUSH) < 0)
		return error_errno(_("unable to flush group segment data '%s'"),
				   writer->data_path);
	if (write_index(writer, &result.index_bytes))
		return -1;
	if (git_fsync(writer->index_fd, FSYNC_HARDWARE_FLUSH) < 0)
		return error_errno(_("unable to flush group segment index '%s'"),
				   writer->index_path);
	if (close_fd(&writer->data_fd) < 0) {
		error_errno(_("unable to close group segment data '%s'"),
			    writer->data_path);
		ret = -1;
	}
	if (close_fd(&writer->index_fd) < 0) {
		error_errno(_("unable to close group segment index '%s'"),
			    writer->index_path);
		ret = -1;
	}
	if (ret)
		return ret;
	if (stats)
		*stats = result;
	writer_release(writer);
	return 0;
}

void odb_segment_group_writer_abort(struct odb_segment_group_writer *writer)
{
	close_fd(&writer->data_fd);
	close_fd(&writer->index_fd);
	if (writer->data_path)
		unlink_or_warn(writer->data_path);
	if (writer->index_path)
		unlink_or_warn(writer->index_path);
	writer_release(writer);
}
