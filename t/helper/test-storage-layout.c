#define USE_THE_REPOSITORY_VARIABLE

#include "test-tool.h"
#include "abspath.h"
#include "csum-file.h"
#include "environment.h"
#include "git-zlib.h"
#include "hash.h"
#include "hex.h"
#include "json-writer.h"
#include "object.h"
#include "odb/segment-group.h"
#include "odb/segment.h"
#include "pack.h"
#include "repository.h"
#include "strbuf.h"
#include "write-or-die.h"

struct corpus_object {
	struct object_id oid;
	enum object_type type;
	unsigned char *data;
	size_t size;
	uint64_t pack_offset;
	uint64_t pack_length;
};

struct artifact {
	const char *role;
	const char *name;
	uint64_t bytes;
	struct object_id checksum;
};

static const size_t corpus_sizes[] = {
	0, 17, 127, 255, 511, 1023, 2049, 8193,
};

static void hash_object(const struct git_hash_algo *algo,
			enum object_type type, const void *data, size_t size,
			struct object_id *oid)
{
	struct git_hash_ctx ctx;
	char header[32];
	int header_len;

	header_len = xsnprintf(header, sizeof(header), "%s %"PRIuMAX,
			       type_name(type), (uintmax_t)size) + 1;
	git_hash_init(&ctx, algo);
	git_hash_update(&ctx, header, header_len);
	git_hash_update(&ctx, data, size);
	git_hash_final_oid(oid, &ctx);
}

static void fill_corpus_object(struct corpus_object *object, size_t nr,
			       const struct git_hash_algo *algo)
{
	size_t i;

	object->type = OBJ_BLOB;
	object->size = corpus_sizes[nr];
	object->data = xmallocz(object->size);
	for (i = 0; i < object->size; i++) {
		switch (nr % 4) {
		case 0:
			object->data[i] = nr;
			break;
		case 1:
			object->data[i] = i % 61 ? 'a' + (i % 5) : nr;
			break;
		case 2:
			object->data[i] = (i * 31 + nr * 17) & 0xff;
			break;
		default:
			object->data[i] = "scope-git-layout-v1\n"[i % 20] ^ nr;
			break;
		}
	}
	hash_object(algo, object->type, object->data, object->size,
		    &object->oid);
}

static void corpus_identity(const struct corpus_object *objects, size_t nr,
			    const struct git_hash_algo *algo,
			    struct object_id *identity)
{
	struct git_hash_ctx ctx;
	size_t i;

	git_hash_init(&ctx, algo);
	for (i = 0; i < nr; i++) {
		unsigned char metadata[12];

		put_be32(metadata, objects[i].type);
		put_be64(metadata + 4, objects[i].size);
		git_hash_update(&ctx, objects[i].oid.hash, algo->rawsz);
		git_hash_update(&ctx, metadata, sizeof(metadata));
	}
	git_hash_final_oid(identity, &ctx);
}

static int path_size_and_checksum(const char *path,
				  const struct git_hash_algo *algo,
				  struct artifact *artifact)
{
	struct git_hash_ctx ctx;
	unsigned char buf[8192];
	struct stat st;
	ssize_t read_result;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return error_errno("unable to open benchmark artifact '%s'", path);
	if (fstat(fd, &st)) {
		close(fd);
		return error_errno("unable to stat benchmark artifact '%s'", path);
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0) {
		close(fd);
		return error("benchmark artifact is not a regular file: '%s'", path);
	}
	git_hash_init(&ctx, algo);
	while ((read_result = xread(fd, buf, sizeof(buf))) > 0)
		git_hash_update(&ctx, buf, read_result);
	if (read_result < 0 || close(fd)) {
		git_hash_discard(&ctx);
		return error_errno("unable to read benchmark artifact '%s'", path);
	}
	git_hash_final_oid(&artifact->checksum, &ctx);
	artifact->bytes = st.st_size;
	return 0;
}

static int compress_object(const void *data, size_t size,
			   unsigned char **compressed, size_t *compressed_size)
{
	git_zstream stream = { 0 };
	unsigned long bound;
	int status;

	if (size > ULONG_MAX)
		return error("benchmark corpus object is too large");
	git_deflate_init(&stream, Z_DEFAULT_COMPRESSION);
	bound = git_deflate_bound(&stream, size);
	*compressed = xmalloc(bound);
	stream.next_in = (void *)data;
	stream.avail_in = size;
	stream.next_out = *compressed;
	stream.avail_out = bound;
	status = git_deflate(&stream, Z_FINISH);
	if (git_deflate_end_gently(&stream) != Z_OK)
		status = Z_DATA_ERROR;
	if (status != Z_STREAM_END) {
		free(*compressed);
		return error("unable to compress benchmark pack object");
	}
	*compressed_size = stream.total_out;
	return 0;
}

static int write_pack_pair(const char *pack_path, const char *index_path,
			   struct corpus_object *objects, size_t objects_nr,
			   const struct git_hash_algo *algo)
{
	struct pack_idx_option options;
	struct pack_idx_entry *entries;
	struct pack_idx_entry **entry_ptrs;
	struct hashfile *pack;
	unsigned char pack_hash[GIT_MAX_RAWSZ];
	int fd;
	size_t i;

	if (!access(index_path, F_OK))
		return error("benchmark artifact already exists: '%s'", index_path);
	fd = open(pack_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (fd < 0)
		return error_errno("unable to create benchmark pack '%s'", pack_path);
	pack = hashfd(algo, fd, pack_path);
	write_pack_header(pack, objects_nr);
	CALLOC_ARRAY(entries, objects_nr);
	ALLOC_ARRAY(entry_ptrs, objects_nr);
	for (i = 0; i < objects_nr; i++) {
		unsigned char header[MAX_PACK_OBJECT_HEADER];
		unsigned char *compressed;
		size_t compressed_size;
		int header_size;

		if (compress_object(objects[i].data, objects[i].size,
				    &compressed, &compressed_size))
			goto error;
		objects[i].pack_offset = hashfile_total(pack);
		header_size = encode_in_pack_object_header(header, sizeof(header),
						   objects[i].type,
						   objects[i].size);
		entries[i].oid = objects[i].oid;
		entries[i].offset = objects[i].pack_offset;
		entry_ptrs[i] = &entries[i];
		crc32_begin(pack);
		hashwrite(pack, header, header_size);
		hashwrite(pack, compressed, compressed_size);
		entries[i].crc32 = crc32_end(pack);
		objects[i].pack_length = hashfile_total(pack) -
			objects[i].pack_offset;
		free(compressed);
	}
	finalize_hashfile(pack, pack_hash, FSYNC_COMPONENT_PACK,
			  CSUM_HASH_IN_STREAM | CSUM_FSYNC | CSUM_CLOSE);
	reset_pack_idx_option(&options);
	options.flags = WRITE_IDX_STRICT;
	write_idx_file(the_repository, index_path, entry_ptrs, objects_nr,
		       &options, pack_hash);
	free(entry_ptrs);
	free(entries);
	return 0;

error:
	free_hashfile(pack);
	close(fd);
	unlink(pack_path);
	free(entry_ptrs);
	free(entries);
	return -1;
}

static int write_segment_pair(const char *data_path, const char *index_path,
			      const struct corpus_object *objects,
			      size_t objects_nr,
			      const struct git_hash_algo *algo)
{
	struct odb_segment_writer writer = ODB_SEGMENT_WRITER_INIT;
	size_t i;

	if (odb_segment_writer_open(&writer, data_path, index_path, algo))
		return -1;
	for (i = 0; i < objects_nr; i++)
		if (odb_segment_writer_add(&writer, &objects[i].oid,
					   objects[i].type, objects[i].data,
					   objects[i].size)) {
			odb_segment_writer_abort(&writer);
			return -1;
		}
	return odb_segment_writer_finish(&writer, NULL);
}

static int write_group_pair(const char *data_path, const char *index_path,
			    const struct corpus_object *objects,
			    size_t objects_nr,
			    const struct git_hash_algo *algo,
			    size_t group_target)
{
	struct odb_segment_group_writer writer = ODB_SEGMENT_GROUP_WRITER_INIT;
	size_t i;

	if (odb_segment_group_writer_open(&writer, data_path, index_path, algo,
					  group_target))
		return -1;
	for (i = 0; i < objects_nr; i++)
		if (odb_segment_group_writer_add(&writer, &objects[i].oid,
						 objects[i].type,
						 objects[i].data,
						 objects[i].size)) {
			odb_segment_group_writer_abort(&writer);
			return -1;
		}
	return odb_segment_group_writer_finish(&writer, NULL);
}

static int verify_segments(const char *segment_data, const char *segment_index,
			   const char *group_data, const char *group_index,
			   const struct corpus_object *objects, size_t objects_nr,
			   const struct git_hash_algo *algo,
			   struct odb_segment *segment,
			   struct odb_segment_group *group)
{
	size_t i;

	if (odb_segment_open(segment, segment_data, segment_index, algo) ||
	    odb_segment_group_open(group, group_data, group_index, algo))
		return -1;
	for (i = 0; i < objects_nr; i++) {
		const struct odb_segment_index_entry *segment_entry;
		const struct odb_segment_group_entry *group_entry;
		void *data;

		if (odb_segment_lookup(segment, &objects[i].oid, &segment_entry) ||
		    odb_segment_read(segment, segment_entry, &data))
			return error("full-object segment lost corpus object %s",
				     oid_to_hex(&objects[i].oid));
		if (memcmp(data, objects[i].data, objects[i].size)) {
			free(data);
			return error("full-object segment changed corpus object %s",
				     oid_to_hex(&objects[i].oid));
		}
		free(data);
		if (odb_segment_group_lookup(group, &objects[i].oid, &group_entry) ||
		    odb_segment_group_read(group, group_entry, &data))
			return error("group segment lost corpus object %s",
				     oid_to_hex(&objects[i].oid));
		if (memcmp(data, objects[i].data, objects[i].size)) {
			free(data);
			return error("group segment changed corpus object %s",
				     oid_to_hex(&objects[i].oid));
		}
		free(data);
	}
	return 0;
}

static void emit_artifact(struct json_writer *json,
			  const struct artifact *artifact)
{
	jw_array_inline_begin_object(json);
	jw_object_string(json, "role", artifact->role);
	jw_object_string(json, "path", artifact->name);
	jw_object_intmax(json, "bytes", artifact->bytes);
	jw_object_string(json, "checksum", oid_to_hex(&artifact->checksum));
	jw_end(json);
}

static void emit_layout_header(struct json_writer *json, const char *id,
			       const char *format, const char *compression_unit,
			       const struct artifact artifacts[2])
{
	jw_array_inline_begin_object(json);
	jw_object_string(json, "id", id);
	jw_object_string(json, "format", format);
	jw_object_string(json, "compression", "zlib-default");
	jw_object_string(json, "compressionUnit", compression_unit);
	jw_object_intmax(json, "totalBytes",
			 artifacts[0].bytes + artifacts[1].bytes);
	jw_object_inline_begin_array(json, "artifacts");
	emit_artifact(json, &artifacts[0]);
	emit_artifact(json, &artifacts[1]);
	jw_end(json);
	jw_object_inline_begin_object(json, "metadataRange");
	jw_object_string(json, "artifact", "index");
	jw_object_intmax(json, "offset", 0);
	jw_object_intmax(json, "length", artifacts[1].bytes);
	jw_end(json);
}

static void emit_object_range(struct json_writer *json,
			      const struct object_id *oid,
			      uint64_t offset, uint64_t length)
{
	jw_array_inline_begin_object(json);
	jw_object_string(json, "oid", oid_to_hex(oid));
	jw_object_string(json, "artifact", "data");
	jw_object_intmax(json, "offset", offset);
	jw_object_intmax(json, "length", length);
	jw_end(json);
}

static void emit_json(const struct corpus_object *objects, size_t objects_nr,
		      const struct object_id *identity,
		      const struct artifact pack_artifacts[2],
		      const struct artifact segment_artifacts[2],
		      const struct artifact group_artifacts[2],
		      const struct odb_segment *segment,
		      const struct odb_segment_group *group, size_t group_target)
{
	struct json_writer json = JSON_WRITER_INIT;
	size_t i;

	jw_object_begin(&json, 1);
	jw_object_string(&json, "schema", "git-storage-layout-benchmark/v1");
	jw_object_string(&json, "measurementScope", "local-artifact-layout");
	jw_object_false(&json, "cloudMeasured");
	jw_object_string(&json, "hashAlgorithm", "sha1");
	jw_object_string(&json, "artifactChecksumAlgorithm", "sha1-file-bytes");
	jw_object_string(&json, "rangeUnits", "zero-based-offset-and-byte-length");
	jw_object_inline_begin_object(&json, "corpus");
	jw_object_string(&json, "generator", "synthetic-blobs-v1");
	jw_object_string(&json, "identity", oid_to_hex(identity));
	jw_object_string(&json, "identityAlgorithm",
			 "sha1(raw-oid || be32(type) || be64(size), corpus order)");
	jw_object_intmax(&json, "objectCount", objects_nr);
	jw_object_inline_begin_array(&json, "objects");
	for (i = 0; i < objects_nr; i++) {
		jw_array_inline_begin_object(&json);
		jw_object_intmax(&json, "ordinal", i);
		jw_object_string(&json, "oid", oid_to_hex(&objects[i].oid));
		jw_object_string(&json, "type", type_name(objects[i].type));
		jw_object_intmax(&json, "bytes", objects[i].size);
		jw_end(&json);
	}
	jw_end(&json);
	jw_end(&json);
	jw_object_inline_begin_array(&json, "layouts");

	emit_layout_header(&json, "A", "git-pack-v2-no-deltas",
			   "object", pack_artifacts);
	jw_object_inline_begin_array(&json, "objectRanges");
	for (i = 0; i < objects_nr; i++)
		emit_object_range(&json, &objects[i].oid,
				  objects[i].pack_offset,
				  objects[i].pack_length);
	jw_end(&json);
	jw_end(&json);

	emit_layout_header(&json, "B", "full-object-segment-v1",
			   "object", segment_artifacts);
	jw_object_inline_begin_array(&json, "objectRanges");
	for (i = 0; i < objects_nr; i++) {
		const struct odb_segment_index_entry *entry;

		if (odb_segment_lookup(segment, &objects[i].oid, &entry))
			BUG("verified segment object disappeared");
		emit_object_range(&json, &objects[i].oid, entry->offset,
				  entry->disk_size);
	}
	jw_end(&json);
	jw_end(&json);

	emit_layout_header(&json, "C", "bounded-group-segment-v1",
			   "bounded-group", group_artifacts);
	jw_object_intmax(&json, "groupTargetBytes", group_target);
	jw_object_intmax(&json, "groupCount", group->groups_nr);
	jw_object_inline_begin_array(&json, "objectRanges");
	for (i = 0; i < objects_nr; i++) {
		const struct odb_segment_group_entry *entry;
		const struct odb_segment_group_info *info;

		if (odb_segment_group_lookup(group, &objects[i].oid, &entry))
			BUG("verified group segment object disappeared");
		info = &group->groups[entry->group_nr];
		emit_object_range(&json, &objects[i].oid, info->offset,
				  info->compressed_size);
	}
	jw_end(&json);
	jw_end(&json);
	jw_end(&json);
	jw_end(&json);
	puts(json.json.buf);
	jw_release(&json);
}

int cmd__storage_layout(int argc, const char **argv)
{
	const struct git_hash_algo *algo = &hash_algos[GIT_HASH_SHA1];
	struct corpus_object objects[ARRAY_SIZE(corpus_sizes)] = { 0 };
	struct odb_segment segment = ODB_SEGMENT_INIT;
	struct odb_segment_group group = ODB_SEGMENT_GROUP_INIT;
	struct object_id identity;
	struct strbuf paths[6] = {
		STRBUF_INIT, STRBUF_INIT, STRBUF_INIT,
		STRBUF_INIT, STRBUF_INIT, STRBUF_INIT,
	};
	struct artifact artifacts[6] = {
		{ "data", "pack.pack" }, { "index", "pack.idx" },
		{ "data", "segment.seg" }, { "index", "segment.idx" },
		{ "data", "group.gsg" }, { "index", "group.gsi" },
	};
	size_t group_target = 1024;
	size_t i;
	char *end;
	int ret = 1;

	if (argc < 2 || argc > 3)
		die("usage: test-tool storage-layout <empty-output-directory> [group-target-bytes]");
	if (argc == 3) {
		uintmax_t parsed;

		errno = 0;
		parsed = strtoumax(argv[2], &end, 10);
		if (errno == ERANGE || !*argv[2] || *end || !parsed ||
		    parsed > SIZE_MAX)
			die("invalid group target: '%s'", argv[2]);
		group_target = parsed;
	}
	if (!is_directory(argv[1]))
		die("output is not a directory: '%s'", argv[1]);
	for (i = 0; i < ARRAY_SIZE(paths); i++)
		strbuf_addf(&paths[i], "%s/%s", argv[1], artifacts[i].name);
	for (i = 0; i < ARRAY_SIZE(objects); i++)
		fill_corpus_object(&objects[i], i, algo);
	corpus_identity(objects, ARRAY_SIZE(objects), algo, &identity);
	repo_set_hash_algo(the_repository, GIT_HASH_SHA1);

	if (write_pack_pair(paths[0].buf, paths[1].buf, objects,
			    ARRAY_SIZE(objects), algo) ||
	    write_segment_pair(paths[2].buf, paths[3].buf, objects,
			       ARRAY_SIZE(objects), algo) ||
	    write_group_pair(paths[4].buf, paths[5].buf, objects,
			     ARRAY_SIZE(objects), algo, group_target) ||
	    verify_segments(paths[2].buf, paths[3].buf,
			    paths[4].buf, paths[5].buf, objects,
			    ARRAY_SIZE(objects), algo, &segment, &group))
		goto out;
	for (i = 0; i < ARRAY_SIZE(artifacts); i++)
		if (path_size_and_checksum(paths[i].buf, algo, &artifacts[i]))
			goto out;
	emit_json(objects, ARRAY_SIZE(objects), &identity,
		  artifacts, artifacts + 2, artifacts + 4,
		  &segment, &group, group_target);
	ret = 0;
out:
	odb_segment_group_close(&group);
	odb_segment_close(&segment);
	for (i = 0; i < ARRAY_SIZE(objects); i++)
		free(objects[i].data);
	for (i = 0; i < ARRAY_SIZE(paths); i++)
		strbuf_release(&paths[i]);
	return ret;
}
