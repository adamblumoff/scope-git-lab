#include "git-compat-util.h"
#include "dir.h"
#include "object-file.h"
#include "odb/source-files.h"
#ifdef USE_S3
#include "odb/source-cloud.h"
#endif
#include "odb/source-segment.h"
#include "odb/source.h"
#include "packfile.h"
#include "strbuf.h"

struct odb_source *odb_source_new(struct object_database *odb,
				  const char *path,
				  bool local)
{
	struct strbuf marker = STRBUF_INIT;
	struct odb_source *source;

	strbuf_addf(&marker, "%s/cloud-odb", path);
#ifdef USE_S3
	if (file_exists(marker.buf)) {
		source = &odb_source_cloud_new(odb, path, local)->base;
		goto out;
	}
#else
	if (file_exists(marker.buf))
		die(_("repository requires a Git build with cloud ODB support"));
#endif
	strbuf_reset(&marker);
	strbuf_addf(&marker, "%s/segments/manifest", path);
	if (file_exists(marker.buf))
		source = &odb_source_segment_new(odb, path, local)->base;
	else
		source = &odb_source_files_new(odb, path, local)->base;
out:
	strbuf_release(&marker);

	return source;
}

void odb_source_init(struct odb_source *source,
		     struct object_database *odb,
		     enum odb_source_type type,
		     const char *path,
		     bool local)
{
	source->odb = odb;
	source->type = type;
	source->local = local;
	source->path = xstrdup(path);
}

void odb_source_free(struct odb_source *source)
{
	if (!source)
		return;
	source->free(source);
}

void odb_source_release(struct odb_source *source)
{
	if (!source)
		return;
	free(source->path);
}

struct odb_source_files *odb_source_files_delegate(struct odb_source *source)
{
	switch (source->type) {
	case ODB_SOURCE_FILES:
		return odb_source_files_downcast(source);
	case ODB_SOURCE_SEGMENT:
		return odb_source_segment_downcast(source)->fallback;
#ifdef USE_S3
	case ODB_SOURCE_CLOUD:
		return container_of(source, struct odb_source_cloud, base)->fallback;
#endif
	default:
		BUG("source type '%d' has no files delegate", source->type);
	}
}
