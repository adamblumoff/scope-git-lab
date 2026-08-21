#include "git-compat-util.h"
#include "gettext.h"
#include "odb/cloud-manifest.h"
#include "string-list.h"

#define CLOUD_MANIFEST_HEADER "git-cloud-odb-manifest 1"

void odb_cloud_manifest_init(struct odb_cloud_manifest *manifest,
			     const struct git_hash_algo *hash_algo)
{
	*manifest = (struct odb_cloud_manifest)ODB_CLOUD_MANIFEST_INIT;
	manifest->hash_algo = hash_algo;
}

void odb_cloud_manifest_release(struct odb_cloud_manifest *manifest)
{
	for (size_t i = 0; i < manifest->artifacts_nr; i++) {
		free(manifest->artifacts[i].data_key);
		free(manifest->artifacts[i].index_key);
	}
	free(manifest->artifacts);
	*manifest = (struct odb_cloud_manifest)ODB_CLOUD_MANIFEST_INIT;
}

static int valid_key(const char *key)
{
	const unsigned char *p = (const unsigned char *)key;

	if (!*p || *p == '/' || strstr(key, ".."))
		return 0;
	for (; *p; p++)
		if (isspace(*p) || iscntrl(*p))
			return 0;
	return 1;
}

int odb_cloud_manifest_add(struct odb_cloud_manifest *manifest,
			   const char *data_key, uint64_t data_bytes,
			   const char *index_key, uint64_t index_bytes)
{
	struct odb_cloud_artifact *artifact;

	if (!valid_key(data_key) || !valid_key(index_key) ||
	    !data_bytes || !index_bytes)
		return error(_("cloud ODB manifest has an invalid artifact"));
	ALLOC_GROW(manifest->artifacts, manifest->artifacts_nr + 1,
		   manifest->artifacts_alloc);
	artifact = &manifest->artifacts[manifest->artifacts_nr++];
	artifact->data_key = xstrdup(data_key);
	artifact->index_key = xstrdup(index_key);
	artifact->data_bytes = data_bytes;
	artifact->index_bytes = index_bytes;
	return 0;
}

static int parse_u64(const char *value, uint64_t *out)
{
	uintmax_t parsed;
	char *end;

	errno = 0;
	parsed = strtoumax(value, &end, 10);
	if (errno || !*value || *end || parsed > UINT64_MAX)
		return -1;
	*out = parsed;
	return 0;
}

int odb_cloud_manifest_parse(struct odb_cloud_manifest *manifest,
			     const void *data, size_t size,
			     const struct git_hash_algo *expected_hash_algo)
{
	struct string_list lines = STRING_LIST_INIT_DUP;
	struct string_list fields = STRING_LIST_INIT_DUP;
	struct strbuf input = STRBUF_INIT;
	uint64_t generation;
	const char *value;
	int ret = -1;

	odb_cloud_manifest_release(manifest);
	if (size && memchr(data, '\0', size)) {
		error(_("cloud ODB manifest contains an embedded NUL byte"));
		goto out;
	}
	strbuf_add(&input, data, size);
	string_list_split(&lines, input.buf, "\n", -1);
	if (lines.nr && !*lines.items[lines.nr - 1].string) {
		free(lines.items[lines.nr - 1].string);
		lines.nr--;
	}
	if (lines.nr < 4 || strcmp(lines.items[0].string, CLOUD_MANIFEST_HEADER) ||
	    strcmp(lines.items[1].string, "layout group") ||
	    !skip_prefix(lines.items[2].string, "hash ", &value) ||
	    strcmp(value, expected_hash_algo->name) ||
	    !skip_prefix(lines.items[3].string, "generation ", &value) ||
	    parse_u64(value, &generation)) {
		error(_("cloud ODB manifest has an invalid header"));
		goto out;
	}
	odb_cloud_manifest_init(manifest, expected_hash_algo);
	manifest->generation = generation;
	for (size_t i = 4; i < lines.nr; i++) {
		uint64_t data_bytes, index_bytes;

		string_list_clear(&fields, 0);
		string_list_split(&fields, lines.items[i].string, " ", -1);
		if (fields.nr != 5 || strcmp(fields.items[0].string, "artifact") ||
		    parse_u64(fields.items[2].string, &data_bytes) ||
		    parse_u64(fields.items[4].string, &index_bytes) ||
		    odb_cloud_manifest_add(manifest, fields.items[1].string,
					   data_bytes, fields.items[3].string,
					   index_bytes))
			goto invalid;
	}
	ret = 0;
	goto out;

invalid:
	error(_("cloud ODB manifest has an invalid artifact line"));
	odb_cloud_manifest_release(manifest);
out:
	string_list_clear(&fields, 0);
	string_list_clear(&lines, 0);
	strbuf_release(&input);
	return ret;
}

void odb_cloud_manifest_write(const struct odb_cloud_manifest *manifest,
			      struct strbuf *out)
{
	strbuf_reset(out);
	strbuf_addf(out, "%s\nlayout group\nhash %s\ngeneration %"PRIu64"\n",
		    CLOUD_MANIFEST_HEADER,
		    manifest->hash_algo->name, manifest->generation);
	for (size_t i = 0; i < manifest->artifacts_nr; i++) {
		const struct odb_cloud_artifact *artifact = &manifest->artifacts[i];

		strbuf_addf(out, "artifact %s %"PRIu64" %s %"PRIu64"\n",
			    artifact->data_key, artifact->data_bytes,
			    artifact->index_key, artifact->index_bytes);
	}
}
