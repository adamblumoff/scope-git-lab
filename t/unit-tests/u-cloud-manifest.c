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
	original.gc_token = xstrdup("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	original.gc_created_at = 123455;
	cl_must_pass(odb_cloud_manifest_add_pending(
		&original, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 123456,
		ODB_CLOUD_PENDING_PUBLISHED, "run/objects/pending.gsg", 12,
		"run/objects/pending.gsi", 34));
	odb_cloud_manifest_write(&original, &serialized);
	cl_must_pass(odb_cloud_manifest_parse(&parsed, serialized.buf,
		serialized.len, hash_algo));
	cl_assert_equal_u(parsed.generation, 7);
	cl_assert_equal_u(parsed.artifacts_nr, 2);
	cl_assert_equal_s(parsed.artifacts[1].data_key,
			  "run/objects/two.gsg");
	cl_assert_equal_u(parsed.artifacts[1].index_bytes, 90);
	cl_assert_equal_s(parsed.gc_token, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	cl_assert_equal_u(parsed.gc_created_at, 123455);
	cl_assert_equal_u(parsed.pending_nr, 1);
	cl_assert_equal_s(parsed.pending[0].token,
			  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	cl_assert_equal_u(parsed.pending[0].created_at, 123456);
	cl_assert_equal_i(parsed.pending[0].state,
			  ODB_CLOUD_PENDING_PUBLISHED);
	cl_must_pass(odb_cloud_manifest_remove_artifact(
		&parsed, "run/objects/one.gsg", "run/objects/one.gsi"));
	cl_assert_equal_u(parsed.artifacts_nr, 1);
	cl_must_pass(odb_cloud_manifest_remove_pending(
		&parsed, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
	cl_assert_equal_u(parsed.pending_nr, 0);
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

void test_cloud_manifest__rejects_embedded_nul(void)
{
	static const char embedded_nul[] =
		"git-cloud-odb-manifest 1\n"
		"layout group\n"
		"hash sha1\n"
		"generation 1\n"
		"\0artifact data 1 index 1\n";
	struct odb_cloud_manifest manifest = ODB_CLOUD_MANIFEST_INIT;

	cl_must_fail(odb_cloud_manifest_parse(&manifest, embedded_nul,
		sizeof(embedded_nul) - 1, hash_algo));
	cl_assert_equal_u(manifest.artifacts_nr, 0);
	odb_cloud_manifest_release(&manifest);
}

void test_cloud_manifest__validates_content_addressed_artifact_keys(void)
{
	static const char hash[] =
		"0123456789abcdef0123456789abcdef"
		"0123456789abcdef0123456789abcdef";
	struct strbuf key = STRBUF_INIT;

	strbuf_addf(&key, "run/group/objects/%s.gsg", hash);
	cl_assert(odb_cloud_manifest_key_is_artifact(
		key.buf, "run/group", "gsg"));
	cl_assert(!odb_cloud_manifest_key_is_artifact(
		key.buf, "another/group", "gsg"));
	cl_assert(!odb_cloud_manifest_key_is_artifact(
		key.buf, "run/group", "gsi"));
	strbuf_reset(&key);
	strbuf_addf(&key, "run/group/objects/%s.gsg/extra", hash);
	cl_assert(!odb_cloud_manifest_key_is_artifact(
		key.buf, "run/group", "gsg"));
	strbuf_reset(&key);
	strbuf_addf(&key, "run/group/objects/%s.gsg", hash);
	key.buf[key.len - 5] = 'A';
	cl_assert(!odb_cloud_manifest_key_is_artifact(
		key.buf, "run/group", "gsg"));
	strbuf_reset(&key);
	strbuf_addf(&key,
		    "run/group/transactions/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/objects/%s.gsi",
		    hash);
	cl_assert(odb_cloud_manifest_key_is_transaction_artifact(
		key.buf, "run/group", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "gsi"));
	cl_assert(odb_cloud_manifest_key_is_scoped_artifact(
		key.buf, "run/group", "gsi"));
	cl_assert(!odb_cloud_manifest_key_is_transaction_artifact(
		key.buf, "run/group", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "gsi"));
	strbuf_release(&key);
}
