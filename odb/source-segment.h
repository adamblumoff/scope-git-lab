#ifndef ODB_SOURCE_SEGMENT_H
#define ODB_SOURCE_SEGMENT_H

#include "odb/segment.h"
#include "odb/source.h"

struct odb_source_files;

struct odb_source_segment {
	struct odb_source base;
	struct odb_source_files *fallback;
	struct odb_segment_manifest manifest;
	struct odb_segment *segments;
	size_t segments_nr;
};

struct odb_source_segment *odb_source_segment_new(struct object_database *odb,
						  const char *path,
						  bool local);

static inline struct odb_source_segment *odb_source_segment_downcast(struct odb_source *source)
{
	if (source->type != ODB_SOURCE_SEGMENT)
		BUG("trying to downcast source of type '%d' to segment", source->type);
	return container_of(source, struct odb_source_segment, base);
}

#endif /* ODB_SOURCE_SEGMENT_H */
