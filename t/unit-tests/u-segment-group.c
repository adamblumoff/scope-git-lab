#include "unit-test.h"
#include "dir.h"
#include "object-file.h"
#include "odb/segment-group.h"
#include "strbuf.h"

static char test_dir[PATH_MAX];
static const struct git_hash_algo *hash_algo = &hash_algos[GIT_HASH_SHA1];

static void make_path(struct strbuf *path, const char *name)
{
	strbuf_addf(path, "%s/%s", test_dir, name);
}

static void hash_blob(const char *data, struct object_id *oid)
{
	hash_object_file(hash_algo, data, strlen(data), OBJ_BLOB, oid);
}

static void write_sample(const char *data_name, const char *index_name,
			 struct object_id oids[4],
			 struct odb_segment_group_stats *stats)
{
	struct odb_segment_group_writer writer = ODB_SEGMENT_GROUP_WRITER_INIT;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	const char *contents[] = { "one", "two", "three", "" };

	make_path(&data_path, data_name);
	make_path(&index_path, index_name);
	cl_must_pass(odb_segment_group_writer_open(&writer, data_path.buf,
						    index_path.buf,
						    hash_algo, 6));
	for (size_t i = 0; i < ARRAY_SIZE(contents); i++) {
		hash_blob(contents[i], &oids[i]);
		cl_must_pass(odb_segment_group_writer_add(&writer, &oids[i],
						      OBJ_BLOB, contents[i],
						      strlen(contents[i])));
	}
	cl_must_pass(odb_segment_group_writer_finish(&writer, stats));
	strbuf_release(&data_path);
	strbuf_release(&index_path);
}

void test_segment_group__initialize(void)
{
	const char *tmp = getenv("TMPDIR");

	xsnprintf(test_dir, sizeof(test_dir), "%s/segment-group-XXXXXX",
		  tmp ? tmp : "/tmp");
	cl_assert(mkdtemp(test_dir) != NULL);
}

void test_segment_group__cleanup(void)
{
	DIR *dir = opendir(test_dir);
	struct dirent *de;

	cl_assert(dir != NULL);
	while ((de = readdir_skip_dot_and_dotdot(dir)) != NULL) {
		struct strbuf path = STRBUF_INIT;

		make_path(&path, de->d_name);
		cl_must_pass(unlink(path.buf));
		strbuf_release(&path);
	}
	closedir(dir);
	cl_must_pass(rmdir(test_dir));
}

void test_segment_group__round_trip_and_group_boundaries(void)
{
	struct odb_segment_group segment = ODB_SEGMENT_GROUP_INIT;
	struct odb_segment_group_stats stats;
	struct object_id oids[4];
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	const char *contents[] = { "one", "two", "three", "" };

	write_sample("round-trip.gsg", "round-trip.gsi", oids, &stats);
	cl_assert_equal_u(stats.objects, 4);
	cl_assert_equal_u(stats.groups, 2);
	make_path(&data_path, "round-trip.gsg");
	make_path(&index_path, "round-trip.gsi");
	cl_must_pass(odb_segment_group_open(&segment, data_path.buf,
					    index_path.buf, hash_algo));
	cl_assert_equal_u(segment.entries_nr, 4);
	cl_assert_equal_u(segment.groups_nr, 2);
	cl_assert_equal_u(segment.group_target, 6);

	for (size_t i = 0; i < ARRAY_SIZE(contents); i++) {
		const struct odb_segment_group_entry *entry;
		void *data;

		cl_must_pass(odb_segment_group_lookup(&segment, &oids[i],
						      &entry));
		cl_assert_equal_u(entry->group_nr, i < 2 ? 0 : 1);
		cl_must_pass(odb_segment_group_read(&segment, entry, &data));
		cl_assert_equal_s(data, contents[i]);
		free(data);
	}
	cl_assert(segment.cached_group != NULL);
	odb_segment_group_clear_cache(&segment);
	cl_assert(segment.cached_group == NULL);
	cl_assert_equal_u(segment.cached_group_nr, SIZE_MAX);
	odb_segment_group_close(&segment);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
}

void test_segment_group__same_input_produces_same_files(void)
{
	struct odb_segment_group_stats stats;
	struct object_id first_oids[4], second_oids[4];
	struct strbuf first = STRBUF_INIT;
	struct strbuf second = STRBUF_INIT;
	struct strbuf first_path = STRBUF_INIT;
	struct strbuf second_path = STRBUF_INIT;

	write_sample("deterministic-a.gsg", "deterministic-a.gsi",
		     first_oids, &stats);
	write_sample("deterministic-b.gsg", "deterministic-b.gsi",
		     second_oids, &stats);
	make_path(&first_path, "deterministic-a.gsg");
	make_path(&second_path, "deterministic-b.gsg");
	cl_assert(strbuf_read_file(&first, first_path.buf, 0) >= 0);
	cl_assert(strbuf_read_file(&second, second_path.buf, 0) >= 0);
	cl_assert_equal_u(first.len, second.len);
	cl_assert(!memcmp(first.buf, second.buf, first.len));
	strbuf_reset(&first);
	strbuf_reset(&second);
	strbuf_reset(&first_path);
	strbuf_reset(&second_path);
	make_path(&first_path, "deterministic-a.gsi");
	make_path(&second_path, "deterministic-b.gsi");
	cl_assert(strbuf_read_file(&first, first_path.buf, 0) >= 0);
	cl_assert(strbuf_read_file(&second, second_path.buf, 0) >= 0);
	cl_assert_equal_u(first.len, second.len);
	cl_assert(!memcmp(first.buf, second.buf, first.len));
	strbuf_release(&first);
	strbuf_release(&second);
	strbuf_release(&first_path);
	strbuf_release(&second_path);
}

void test_segment_group__corrupt_group_fails_object_read(void)
{
	struct odb_segment_group segment = ODB_SEGMENT_GROUP_INIT;
	struct odb_segment_group_stats stats;
	struct object_id oids[4];
	const struct odb_segment_group_entry *entry;
	struct strbuf data_path = STRBUF_INIT;
	struct strbuf index_path = STRBUF_INIT;
	unsigned char byte;
	void *data;
	int fd;

	write_sample("corrupt.gsg", "corrupt.gsi", oids, &stats);
	make_path(&data_path, "corrupt.gsg");
	make_path(&index_path, "corrupt.gsi");
	cl_must_pass(chmod(data_path.buf, 0644));
	fd = open(data_path.buf, O_RDWR);
	cl_assert(fd >= 0);
	cl_assert_equal_i(pread(fd, &byte, 1, 32), 1);
	byte ^= 0xff;
	cl_assert_equal_i(pwrite(fd, &byte, 1, 32), 1);
	cl_must_pass(close(fd));

	cl_must_pass(odb_segment_group_open(&segment, data_path.buf,
					    index_path.buf, hash_algo));
	cl_must_pass(odb_segment_group_lookup(&segment, &oids[0], &entry));
	cl_must_fail(odb_segment_group_read(&segment, entry, &data));
	odb_segment_group_close(&segment);
	strbuf_release(&data_path);
	strbuf_release(&index_path);
}
