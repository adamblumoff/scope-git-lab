#include "git-compat-util.h"
#include "git-zlib.h"
#include "dir.h"
#include "gettext.h"
#include "hex.h"
#include "odb/segment.h"
#include "path.h"
#include "strbuf.h"

#define SEGMENT_DATA_MAGIC "GSGD"
#define SEGMENT_INDEX_MAGIC "GSGI"
#define SEGMENT_VERSION 1
#define SEGMENT_DATA_HEADER_SIZE 16
#define SEGMENT_INDEX_HEADER_SIZE 32
#define SEGMENT_INDEX_ENTRY_TRAILER_SIZE 32
#define SEGMENT_MANIFEST_HEADER "git-segment-manifest 1"

static int valid_object_type(enum object_type type)
{
	return type == OBJ_COMMIT || type == OBJ_TREE ||
		type == OBJ_BLOB || type == OBJ_TAG;
}

static void hash_segment_object(const struct git_hash_algo *hash_algo,
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

static int read_header(int fd, unsigned char *header, size_t size,
		       const char *path)
{
	ssize_t read_result = pread_in_full(fd, header, size, 0);

	if (read_result < 0)
		return error_errno(_("unable to read segment header from '%s'"), path);
	if ((size_t)read_result != size)
		return error(_("truncated segment header in '%s'"), path);
	return 0;
}

static int stat_file(int fd, const char *path, uint64_t *size)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return error_errno(_("unable to stat segment file '%s'"), path);
	if (!S_ISREG(st.st_mode) || st.st_size < 0)
		return error(_("segment file '%s' is not a regular file"), path);
	*size = st.st_size;
	return 0;
}

static size_t index_entry_size(const struct git_hash_algo *hash_algo)
{
	return hash_algo->rawsz + SEGMENT_INDEX_ENTRY_TRAILER_SIZE;
}

static int validate_index_entries(struct odb_segment *segment,
				  const unsigned char *raw_index)
{
	size_t entry_size = index_entry_size(segment->hash_algo);
	size_t i;

	CALLOC_ARRAY(segment->entries, segment->entries_nr);
	for (i = 0; i < segment->entries_nr; i++) {
		const unsigned char *raw = raw_index + st_mult(i, entry_size);
		struct odb_segment_index_entry *entry = &segment->entries[i];
		uint32_t type;

		oidread(&entry->oid, raw, segment->hash_algo);
		raw += segment->hash_algo->rawsz;
		entry->offset = get_be64(raw);
		entry->size = get_be64(raw + 8);
		entry->disk_size = get_be64(raw + 16);
		type = get_be32(raw + 24);
		if (get_be32(raw + 28))
			return error(_("segment index has non-zero reserved data"));
		entry->type = type;

		if (!valid_object_type(entry->type))
			return error(_("segment index has invalid object type %"PRIu32),
				     type);
		if (entry->offset < SEGMENT_DATA_HEADER_SIZE ||
		    entry->offset > segment->data_bytes ||
		    entry->disk_size > segment->data_bytes - entry->offset)
			return error(_("segment index contains an out-of-bounds object"));
		if (i && oidcmp(&segment->entries[i - 1].oid, &entry->oid) >= 0)
			return error(_("segment index is not strictly sorted"));
	}

	return 0;
}

int odb_segment_open(struct odb_segment *segment,
		     const char *data_path, const char *index_path,
		     const struct git_hash_algo *hash_algo)
{
	unsigned char data_header[SEGMENT_DATA_HEADER_SIZE];
	unsigned char index_header[SEGMENT_INDEX_HEADER_SIZE];
	unsigned char *raw_index = NULL;
	uint64_t stored_data_bytes, entries_nr, expected_index_bytes;
	uint32_t data_algo, index_algo;
	int data_fd = -1, index_fd = -1;
	int ret = -1;

	*segment = (struct odb_segment)ODB_SEGMENT_INIT;
	data_fd = open(data_path, O_RDONLY);
	if (data_fd < 0) {
		error_errno(_("unable to open segment data '%s'"), data_path);
		goto out;
	}
	index_fd = open(index_path, O_RDONLY);
	if (index_fd < 0) {
		error_errno(_("unable to open segment index '%s'"), index_path);
		goto out;
	}
	if (stat_file(data_fd, data_path, &segment->data_bytes) ||
	    stat_file(index_fd, index_path, &segment->index_bytes) ||
	    segment->data_bytes < sizeof(data_header) ||
	    segment->index_bytes < sizeof(index_header)) {
		if (segment->data_bytes < sizeof(data_header) ||
		    segment->index_bytes < sizeof(index_header))
			error(_("segment pair has a truncated header"));
		goto out;
	}
	if (read_header(data_fd, data_header, sizeof(data_header), data_path) ||
	    read_header(index_fd, index_header, sizeof(index_header),
			index_path))
		goto out;
	if (memcmp(data_header, SEGMENT_DATA_MAGIC, 4) ||
	    memcmp(index_header, SEGMENT_INDEX_MAGIC, 4) ||
	    get_be32(data_header + 4) != SEGMENT_VERSION ||
	    get_be32(index_header + 4) != SEGMENT_VERSION) {
		error(_("segment pair has an unsupported format"));
		goto out;
	}
	data_algo = hash_algo_by_id(get_be32(data_header + 8));
	index_algo = hash_algo_by_id(get_be32(index_header + 8));
	if (!data_algo || data_algo != index_algo ||
	    (hash_algo && data_algo != hash_algo_by_ptr(hash_algo))) {
		error(_("segment pair uses an incompatible object format"));
		goto out;
	}
	if (get_be32(data_header + 12) || get_be32(index_header + 12)) {
		error(_("segment pair has non-zero reserved header data"));
		goto out;
	}
	segment->hash_algo = &hash_algos[data_algo];
	entries_nr = get_be64(index_header + 16);
	stored_data_bytes = get_be64(index_header + 24);
	if (stored_data_bytes != segment->data_bytes) {
		error(_("segment pair has inconsistent file sizes"));
		goto out;
	}
	if (entries_nr > SIZE_MAX / sizeof(*segment->entries)) {
		error(_("segment index has too many entries"));
		goto out;
	}
	if (entries_nr > (SIZE_MAX - SEGMENT_INDEX_HEADER_SIZE) /
			 index_entry_size(segment->hash_algo)) {
		error(_("segment index is too large"));
		goto out;
	}
	expected_index_bytes = SEGMENT_INDEX_HEADER_SIZE +
		entries_nr * index_entry_size(segment->hash_algo);
	if (expected_index_bytes != segment->index_bytes) {
		error(_("segment index has an invalid size"));
		goto out;
	}
	segment->entries_nr = entries_nr;
	if (entries_nr) {
		size_t raw_size = st_mult(segment->entries_nr,
					  index_entry_size(segment->hash_algo));
		ssize_t read_result;

		raw_index = xmalloc(raw_size);
		read_result = pread_in_full(index_fd, raw_index, raw_size,
					    SEGMENT_INDEX_HEADER_SIZE);
		if (read_result < 0) {
			error_errno(_("unable to read segment index '%s'"), index_path);
			goto out;
		}
		if ((size_t)read_result != raw_size) {
			error(_("truncated segment index '%s'"), index_path);
			goto out;
		}
		if (validate_index_entries(segment, raw_index))
			goto out;
	}
	segment->data_path = xstrdup(data_path);
	ret = 0;
out:
	free(raw_index);
	close_fd(&data_fd);
	close_fd(&index_fd);
	if (ret)
		odb_segment_close(segment);
	return ret;
}

void odb_segment_close(struct odb_segment *segment)
{
	free(segment->data_path);
	free(segment->entries);
	*segment = (struct odb_segment)ODB_SEGMENT_INIT;
}

int odb_segment_lookup(const struct odb_segment *segment,
		       const struct object_id *oid,
		       const struct odb_segment_index_entry **entry)
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

static int validate_inflated_size(const void *compressed,
				  size_t compressed_size,
				  size_t expected_size)
{
	unsigned char scratch[8192];
	git_zstream stream = { 0 };
	int status;

	git_inflate_init(&stream);
	stream.next_in = (unsigned char *)compressed;
	stream.avail_in = compressed_size;
	do {
		size_t total_in = stream.total_in;
		size_t total_out = stream.total_out;

		stream.next_out = scratch;
		stream.avail_out = sizeof(scratch);
		status = git_inflate(&stream, Z_FINISH);
		if (stream.total_out > expected_size)
			break;
		if (status != Z_OK && status != Z_BUF_ERROR)
			break;
		if (stream.total_in == total_in && stream.total_out == total_out)
			break;
	} while (status == Z_OK || status == Z_BUF_ERROR);
	git_inflate_end(&stream);

	if (status != Z_STREAM_END || stream.total_in != compressed_size ||
	    stream.total_out != expected_size)
		return error(_("unable to inflate object from segment"));
	return 0;
}

int odb_segment_read(const struct odb_segment *segment,
		     const struct odb_segment_index_entry *entry,
		     void **data)
{
	struct object_id actual_oid;
	git_zstream stream = { 0 };
	void *compressed;
	void *buf;
	ssize_t read_result;
	int data_fd;
	int status;

	if (entry->size >= SIZE_MAX || entry->disk_size > SIZE_MAX)
		return error(_("segment object is too large to read"));
	compressed = xmalloc(entry->disk_size);
	data_fd = open(segment->data_path, O_RDONLY);
	if (data_fd < 0) {
		free(compressed);
		return error_errno(_("unable to open segment data '%s'"),
				   segment->data_path);
	}
	read_result = pread_in_full(data_fd, compressed, entry->disk_size,
				    entry->offset);
	if (close(data_fd) < 0 && read_result >= 0)
		read_result = -1;
	if (read_result < 0) {
		free(compressed);
		return error_errno(_("unable to read object from segment"));
	}
	if ((uint64_t)read_result != entry->disk_size) {
		free(compressed);
		return error(_("truncated object in segment"));
	}
	if (validate_inflated_size(compressed, entry->disk_size, entry->size)) {
		free(compressed);
		return -1;
	}
	buf = xmallocz(entry->size);
	git_inflate_init(&stream);
	stream.next_in = compressed;
	stream.avail_in = entry->disk_size;
	stream.next_out = buf;
	stream.avail_out = entry->size ? entry->size : 1;
	status = git_inflate(&stream, Z_FINISH);
	git_inflate_end(&stream);
	free(compressed);
	if (status != Z_STREAM_END || stream.total_in != entry->disk_size ||
	    stream.total_out != entry->size) {
		free(buf);
		return error(_("unable to inflate object from segment"));
	}
	hash_segment_object(segment->hash_algo, entry->type, buf, entry->size,
			    &actual_oid);
	if (!oideq(&actual_oid, &entry->oid)) {
		free(buf);
		return error(_("segment object hash mismatch for %s"),
			     oid_to_hex(&entry->oid));
	}
	*data = buf;
	return 0;
}

int odb_segment_for_each(const struct odb_segment *segment,
			 odb_segment_each_fn cb, void *cb_data)
{
	size_t i;

	for (i = 0; i < segment->entries_nr; i++) {
		int ret = cb(&segment->entries[i], cb_data);

		if (ret)
			return ret;
	}
	return 0;
}

static int writer_entry_cmp(const void *va, const void *vb)
{
	const struct odb_segment_index_entry *a = va;
	const struct odb_segment_index_entry *b = vb;

	return oidcmp(&a->oid, &b->oid);
}

static void writer_release(struct odb_segment_writer *writer)
{
	free(writer->data_path);
	free(writer->index_path);
	free(writer->entries);
	*writer = (struct odb_segment_writer)ODB_SEGMENT_WRITER_INIT;
}

int odb_segment_writer_open(struct odb_segment_writer *writer,
			    const char *data_path, const char *index_path,
			    const struct git_hash_algo *hash_algo)
{
	unsigned char header[SEGMENT_DATA_HEADER_SIZE] = { 0 };

	*writer = (struct odb_segment_writer)ODB_SEGMENT_WRITER_INIT;
	if (!hash_algo || !hash_algo_by_ptr(hash_algo))
		return error(_("cannot write segment with unknown object format"));
	writer->hash_algo = hash_algo;
	writer->data_fd = open(data_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (writer->data_fd < 0) {
		error_errno(_("unable to create segment data '%s'"), data_path);
		goto error;
	}
	writer->data_path = xstrdup(data_path);
	writer->index_fd = open(index_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (writer->index_fd < 0) {
		error_errno(_("unable to create segment index '%s'"), index_path);
		goto error;
	}
	writer->index_path = xstrdup(index_path);
	memcpy(header, SEGMENT_DATA_MAGIC, 4);
	put_be32(header + 4, SEGMENT_VERSION);
	put_be32(header + 8, hash_algo->format_id);
	if (write_in_full(writer->data_fd, header, sizeof(header)) < 0) {
		error_errno(_("unable to write segment data header"));
		goto error;
	}
	writer->data_offset = sizeof(header);
	return 0;
error:
	odb_segment_writer_abort(writer);
	return -1;
}

int odb_segment_writer_add(struct odb_segment_writer *writer,
			   const struct object_id *oid,
			   enum object_type type,
			   const void *data, size_t size)
{
	struct odb_segment_index_entry *entry;
	struct object_id actual_oid;
	git_zstream stream = { 0 };
	unsigned char *compressed;
	unsigned long bound;
	int status;

	if (writer->data_fd < 0 || writer->index_fd < 0)
		BUG("adding an object to an unopened segment writer");
	if (!valid_object_type(type))
		return error(_("cannot write invalid object type %d to segment"),
			     type);
	if (oid->algo && oid->algo != hash_algo_by_ptr(writer->hash_algo))
		return error(_("cannot mix object formats in a segment"));
	if (size > ULONG_MAX)
		return error(_("segment object is too large to compress"));
	hash_segment_object(writer->hash_algo, type, data, size, &actual_oid);
	if (!oideq(&actual_oid, oid))
		return error(_("object data does not match object ID %s"),
			     oid_to_hex(oid));

	git_deflate_init(&stream, Z_DEFAULT_COMPRESSION);
	bound = git_deflate_bound(&stream, size);
	compressed = xmalloc(bound);
	stream.next_in = (void *)data;
	stream.avail_in = size;
	stream.next_out = compressed;
	stream.avail_out = bound;
	status = git_deflate(&stream, Z_FINISH);
	if (git_deflate_end_gently(&stream) != Z_OK)
		status = Z_DATA_ERROR;
	if (status != Z_STREAM_END) {
		free(compressed);
		return error(_("unable to compress object for segment"));
	}
	if (stream.total_out > UINT64_MAX - writer->data_offset) {
		free(compressed);
		return error(_("segment data is too large"));
	}
	if (write_in_full(writer->data_fd, compressed, stream.total_out) < 0) {
		free(compressed);
		return error_errno(_("unable to write segment object"));
	}
	free(compressed);

	ALLOC_GROW(writer->entries, writer->entries_nr + 1,
		   writer->entries_alloc);
	entry = &writer->entries[writer->entries_nr++];
	entry->oid = *oid;
	entry->type = type;
	entry->size = size;
	entry->disk_size = stream.total_out;
	entry->offset = writer->data_offset;
	writer->data_offset += stream.total_out;
	return 0;
}

static int write_index(struct odb_segment_writer *writer, uint64_t *index_bytes)
{
	unsigned char header[SEGMENT_INDEX_HEADER_SIZE] = { 0 };
	size_t i, entry_size = index_entry_size(writer->hash_algo);
	unsigned char *raw;

	QSORT(writer->entries, writer->entries_nr, writer_entry_cmp);
	for (i = 1; i < writer->entries_nr; i++) {
		if (oideq(&writer->entries[i - 1].oid, &writer->entries[i].oid))
			return error(_("cannot write duplicate object %s to segment"),
				     oid_to_hex(&writer->entries[i].oid));
	}
	if (writer->entries_nr >
	    (SIZE_MAX - SEGMENT_INDEX_HEADER_SIZE) / entry_size)
		return error(_("segment index is too large"));

	memcpy(header, SEGMENT_INDEX_MAGIC, 4);
	put_be32(header + 4, SEGMENT_VERSION);
	put_be32(header + 8, writer->hash_algo->format_id);
	put_be64(header + 16, writer->entries_nr);
	put_be64(header + 24, writer->data_offset);
	if (write_in_full(writer->index_fd, header, sizeof(header)) < 0)
		return error_errno(_("unable to write segment index header"));

	raw = xmalloc(entry_size);
	for (i = 0; i < writer->entries_nr; i++) {
		const struct odb_segment_index_entry *entry = &writer->entries[i];

		memset(raw, 0, entry_size);
		hashcpy(raw, entry->oid.hash, writer->hash_algo);
		put_be64(raw + writer->hash_algo->rawsz, entry->offset);
		put_be64(raw + writer->hash_algo->rawsz + 8, entry->size);
		put_be64(raw + writer->hash_algo->rawsz + 16, entry->disk_size);
		put_be32(raw + writer->hash_algo->rawsz + 24, entry->type);
		if (write_in_full(writer->index_fd, raw, entry_size) < 0) {
			free(raw);
			return error_errno(_("unable to write segment index"));
		}
	}
	free(raw);
	*index_bytes = SEGMENT_INDEX_HEADER_SIZE +
		writer->entries_nr * entry_size;
	return 0;
}

void odb_segment_test_failpoint(const char *name)
{
	const char *failpoint = getenv("GIT_TEST_SEGMENT_FAILPOINT");

	if (failpoint && !strcmp(failpoint, name))
		_exit(99);
}

int odb_segment_writer_finish(struct odb_segment_writer *writer,
			      struct odb_segment_stats *stats)
{
	struct odb_segment_stats result = {
		.objects = writer->entries_nr,
		.data_bytes = writer->data_offset,
	};
	int ret = 0;

	if (writer->data_fd < 0 || writer->index_fd < 0)
		BUG("finishing an unopened segment writer");
	if (git_fsync(writer->data_fd, FSYNC_HARDWARE_FLUSH) < 0)
		return error_errno(_("unable to flush segment data '%s'"),
				   writer->data_path);
	odb_segment_test_failpoint("after-segment-fsync");
	if (write_index(writer, &result.index_bytes))
		return -1;
	if (git_fsync(writer->index_fd, FSYNC_HARDWARE_FLUSH) < 0)
		return error_errno(_("unable to flush segment index '%s'"),
				   writer->index_path);
	odb_segment_test_failpoint("after-index-fsync");
	if (close_fd(&writer->data_fd) < 0) {
		error_errno(_("unable to close segment data '%s'"),
			    writer->data_path);
		ret = -1;
	}
	if (close_fd(&writer->index_fd) < 0) {
		error_errno(_("unable to close segment index '%s'"),
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

void odb_segment_writer_abort(struct odb_segment_writer *writer)
{
	close_fd(&writer->data_fd);
	close_fd(&writer->index_fd);
	if (writer->data_path)
		unlink_or_warn(writer->data_path);
	if (writer->index_path)
		unlink_or_warn(writer->index_path);
	writer_release(writer);
}

static int valid_manifest_name(const char *name)
{
	const char *p;

	if (!*name || !strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	for (p = name; *p; p++) {
		if (isspace((unsigned char)*p) || *p == '/' || *p == '\\')
			return 0;
	}
	return 1;
}

void odb_segment_manifest_init(struct odb_segment_manifest *manifest,
			       const struct git_hash_algo *hash_algo)
{
	*manifest = (struct odb_segment_manifest)ODB_SEGMENT_MANIFEST_INIT;
	manifest->hash_algo = hash_algo;
}

void odb_segment_manifest_release(struct odb_segment_manifest *manifest)
{
	size_t i;

	for (i = 0; i < manifest->entries_nr; i++) {
		free(manifest->entries[i].data_name);
		free(manifest->entries[i].index_name);
	}
	free(manifest->entries);
	*manifest = (struct odb_segment_manifest)ODB_SEGMENT_MANIFEST_INIT;
}

int odb_segment_manifest_add(struct odb_segment_manifest *manifest,
			     const char *data_name, const char *index_name)
{
	struct odb_segment_manifest_entry *entry;

	if (!valid_manifest_name(data_name) || !valid_manifest_name(index_name))
		return error(_("invalid segment manifest filename"));
	ALLOC_GROW(manifest->entries, manifest->entries_nr + 1,
		   manifest->entries_alloc);
	entry = &manifest->entries[manifest->entries_nr++];
	entry->data_name = xstrdup(data_name);
	entry->index_name = xstrdup(index_name);
	return 0;
}

static int parse_manifest_segment(struct odb_segment_manifest *manifest,
				  const char *line)
{
	const char *data_name = line + strlen("segment ");
	const char *space = strchr(data_name, ' ');
	char *data;
	int ret;

	if (!space || space == data_name || !space[1] || strchr(space + 1, ' '))
		return -1;
	data = xstrndup(data_name, space - data_name);
	ret = odb_segment_manifest_add(manifest, data, space + 1);
	free(data);
	return ret;
}

int odb_segment_manifest_read(struct odb_segment_manifest *manifest,
			      const char *path,
			      const struct git_hash_algo *hash_algo)
{
	struct odb_segment_manifest parsed = ODB_SEGMENT_MANIFEST_INIT;
	struct strbuf line = STRBUF_INIT;
	FILE *file;
	const char *hash_name;
	unsigned int line_nr = 1;
	uint32_t algo;
	int ret = -1;

	file = fopen(path, "r");
	if (!file)
		return error_errno(_("unable to open segment manifest '%s'"), path);
	if (strbuf_getline_lf(&line, file) == EOF ||
	    strcmp(line.buf, SEGMENT_MANIFEST_HEADER)) {
		error(_("segment manifest '%s' has an invalid header"), path);
		goto out;
	}
	if (strbuf_getline_lf(&line, file) == EOF ||
	    !skip_prefix(line.buf, "hash ", &hash_name) ||
	    !*hash_name || strchr(hash_name, ' ')) {
		error(_("segment manifest '%s' has an invalid hash line"), path);
		goto out;
	}
	line_nr++;
	algo = hash_algo_by_name(hash_name);
	if (!algo || (hash_algo && algo != hash_algo_by_ptr(hash_algo))) {
		error(_("segment manifest '%s' uses an incompatible object format"),
		      path);
		goto out;
	}
	parsed.hash_algo = &hash_algos[algo];
	while (strbuf_getline_lf(&line, file) != EOF) {
		line_nr++;
		if (!starts_with(line.buf, "segment ") ||
		    parse_manifest_segment(&parsed, line.buf)) {
			error(_("invalid segment manifest line %u in '%s'"),
			      line_nr, path);
			goto out;
		}
	}
	if (ferror(file)) {
		error_errno(_("unable to read segment manifest '%s'"), path);
		goto out;
	}
	odb_segment_manifest_release(manifest);
	*manifest = parsed;
	parsed = (struct odb_segment_manifest)ODB_SEGMENT_MANIFEST_INIT;
	ret = 0;
out:
	odb_segment_manifest_release(&parsed);
	strbuf_release(&line);
	if (fclose(file) < 0 && !ret)
		ret = error_errno(_("unable to close segment manifest '%s'"), path);
	return ret;
}

int odb_segment_manifest_write_fd(const struct odb_segment_manifest *manifest,
				  int fd)
{
	struct strbuf buf = STRBUF_INIT;
	size_t i;
	int ret = -1;

	if (!manifest->hash_algo || !hash_algo_by_ptr(manifest->hash_algo))
		return error(_("cannot write manifest with unknown object format"));
	strbuf_addstr(&buf, SEGMENT_MANIFEST_HEADER "\n");
	strbuf_addf(&buf, "hash %s\n", manifest->hash_algo->name);
	for (i = 0; i < manifest->entries_nr; i++) {
		const struct odb_segment_manifest_entry *entry =
			&manifest->entries[i];

		if (!valid_manifest_name(entry->data_name) ||
		    !valid_manifest_name(entry->index_name)) {
			error(_("invalid segment manifest filename"));
			goto out;
		}
		strbuf_addf(&buf, "segment %s %s\n",
			    entry->data_name, entry->index_name);
	}
	if (write_in_full(fd, buf.buf, buf.len) < 0)
		error_errno(_("unable to write segment manifest"));
	else
		ret = 0;
out:
	strbuf_release(&buf);
	return ret;
}

int odb_segment_next_paths(const char *directory, unsigned int *number,
			   struct strbuf *data_path,
			   struct strbuf *index_path)
{
	for (*number = 1; *number < 100000000; (*number)++) {
		strbuf_reset(data_path);
		strbuf_reset(index_path);
		strbuf_addf(data_path, "%s/%08u.seg", directory, *number);
		strbuf_addf(index_path, "%s/%08u.idx", directory, *number);
		if (!file_exists(data_path->buf) && !file_exists(index_path->buf))
			return 0;
	}
	return error(_("no available experimental segment filename"));
}

int odb_segment_fsync_directory(const char *directory)
{
	int fd = open(directory, O_RDONLY);
	int ret;

	if (fd < 0)
		return error_errno(_("unable to open segment directory '%s'"),
				   directory);
	ret = fsync(fd);
	if (close(fd) < 0 && !ret)
		ret = -1;
	if (ret < 0)
		return error_errno(_("unable to fsync segment directory '%s'"),
				   directory);
	return 0;
}

int odb_segment_adjust_shared_perm(struct repository *repo,
				   const char *data_path,
				   const char *index_path)
{
	if (adjust_shared_perm(repo, data_path) ||
	    adjust_shared_perm(repo, index_path))
		return error_errno(_("unable to adjust shared segment permissions"));
	return 0;
}
