#include "unit-test.h"
#include "odb/cloud-manifest.h"

static const struct git_hash_algo *hash_algo = &hash_algos[GIT_HASH_SHA1];

void test_cloud_manifest__round_trip(void)
{
	struct odb_cloud_manifest original = ODB_CLOUD_MANIFEST_INIT;
	struct odb_cloud_manifest parsed = ODB_CLOUD_MANIFEST_INIT;
	struct strbuf serialized = STRBUF_INIT;

	odb_cloud_manifest_init(&original, hash_algo);
	original.generation = 7;
	cl_must_pass(odb_cloud_manifest_add(&original,
		"run/objects/one.gsg", 123, "run/objects/one.gsi", 45));
	cl_must_pass(odb_cloud_manifest_add(&original,
		"run/objects/two.gsg", 678, "run/objects/two.gsi", 90));
	odb_cloud_manifest_write(&original, &serialized);
	cl_must_pass(odb_cloud_manifest_parse(&parsed, serialized.buf,
		serialized.len, hash_algo));
	cl_assert_equal_u(parsed.generation, 7);
	cl_assert_equal_u(parsed.artifacts_nr, 2);
	cl_assert_equal_s(parsed.artifacts[1].data_key,
			  "run/objects/two.gsg");
	cl_assert_equal_u(parsed.artifacts[1].index_bytes, 90);
	odb_cloud_manifest_release(&parsed);
	odb_cloud_manifest_release(&original);
	strbuf_release(&serialized);
}

void test_cloud_manifest__rejects_layout_and_unsafe_keys(void)
{
	static const char wrong_layout[] =
		"git-cloud-odb-manifest 1\n"
		"layout pack\n"
		"hash sha1\n"
		"generation 1\n";
	struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;

	cl_must_fail(odb_cloud_manifest_parse(&manifest, wrong_layout,
		sizeof(wrong_layout) - 1, hash_algo));
	odb_cloud_manifest_init(&manifest, hash_algo);
	cl_must_fail(odb_cloud_manifest_add(&manifest, "../data", 1,
					    "safe/index", 1));
	cl_must_fail(odb_cloud_manifest_add(&manifest, "bad data", 1,
					    "safe/index", 1));
	odb_cloud_manifest_release(&manifest);
}
