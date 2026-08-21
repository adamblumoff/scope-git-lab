#ifndef ODB_SOURCE_CLOUD_H
#define ODB_SOURCE_CLOUD_H

#include "odb/cloud-manifest.h"
#include "odb/source.h"
#include "odb/s3-client.h"
#include "oidmap.h"

struct odb_segment_group;
struct odb_source_files;

struct odb_source_cloud {
	struct odb_source base;
	struct odb_source_files *fallback;
	struct odb_segment_group *groups;
	size_t readers_nr;
	size_t cached_reader_nr;
	struct oidmap object_index;
	struct odb_cloud_manifest manifest;
	struct s3_client client;
	struct strbuf prefix;
	struct strbuf manifest_key;
};

struct odb_source_cloud *odb_source_cloud_new(struct object_database *odb,
					      const char *path, bool local);

/* The caller must finish repository setup before reconciling against refs. */
int odb_source_cloud_recover(struct odb_source_cloud *source);

#endif /* ODB_SOURCE_CLOUD_H */
