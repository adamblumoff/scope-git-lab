#ifndef ODB_CLOUD_MANIFEST_H
#define ODB_CLOUD_MANIFEST_H

#include "hash.h"
#include "strbuf.h"

struct odb_cloud_artifact {
	char *data_key;
	char *index_key;
	uint64_t data_bytes;
	uint64_t index_bytes;
};

enum odb_cloud_pending_state {
	ODB_CLOUD_PENDING_ACTIVE = 0,
	ODB_CLOUD_PENDING_DELETING,
};

struct odb_cloud_pending_artifact {
	char *token;
	uint64_t created_at;
	enum odb_cloud_pending_state state;
	char *data_key;
	char *index_key;
	uint64_t data_bytes;
	uint64_t index_bytes;
};

struct odb_cloud_manifest {
	const struct git_hash_algo *hash_algo;
	uint64_t generation;
	char *gc_token;
	struct odb_cloud_artifact *artifacts;
	size_t artifacts_nr;
	size_t artifacts_alloc;
	struct odb_cloud_pending_artifact *pending;
	size_t pending_nr;
	size_t pending_alloc;
};

#define ODB_CLOUD_MANIFEST_INIT { 0 }

void odb_cloud_manifest_init(struct odb_cloud_manifest *manifest,
			     const struct git_hash_algo *hash_algo);
void odb_cloud_manifest_release(struct odb_cloud_manifest *manifest);
int odb_cloud_manifest_add(struct odb_cloud_manifest *manifest,
			   const char *data_key, uint64_t data_bytes,
			   const char *index_key, uint64_t index_bytes);
int odb_cloud_manifest_add_pending(
	struct odb_cloud_manifest *manifest, const char *token,
	uint64_t created_at, enum odb_cloud_pending_state state,
	const char *data_key, uint64_t data_bytes,
	const char *index_key, uint64_t index_bytes);
struct odb_cloud_pending_artifact *odb_cloud_manifest_find_pending(
	struct odb_cloud_manifest *manifest, const char *token);
int odb_cloud_manifest_remove_pending(struct odb_cloud_manifest *manifest,
				      const char *token);
int odb_cloud_manifest_key_is_artifact(const char *key, const char *prefix,
				       const char *suffix);
int odb_cloud_manifest_parse(struct odb_cloud_manifest *manifest,
			     const void *data, size_t size,
			     const struct git_hash_algo *expected_hash_algo);
void odb_cloud_manifest_write(const struct odb_cloud_manifest *manifest,
			      struct strbuf *out);

#endif /* ODB_CLOUD_MANIFEST_H */
