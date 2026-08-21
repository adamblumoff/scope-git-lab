#include "git-compat-util.h"
#include "gettext.h"
#include "json-writer.h"
#include "odb/s3-client.h"
#include "run-command.h"
#include "strbuf.h"
#include "trace2.h"
#include "wrapper.h"

static const char cloud_bench_usage[] =
	"git-cloud-bench probe --json\n"
	"git-cloud-bench validate-config [--print-storage-id]";

static const char race_manifest_one[] =
	"git-cloud-probe-manifest 1\nrace publisher one\n";
static const char race_manifest_two[] =
	"git-cloud-probe-manifest 1\nrace publisher two\n";

enum probe_step_id {
	PROBE_IMMUTABLE_PUT,
	PROBE_HEAD,
	PROBE_FULL_GET,
	PROBE_RANGE_GET,
	PROBE_IMMUTABLE_CONFLICT,
	PROBE_MANIFEST_CREATE,
	PROBE_MANIFEST_HEAD,
	PROBE_MANIFEST_CAS,
	PROBE_STALE_MANIFEST_CONFLICT,
	PROBE_MANIFEST_FINAL_GET,
	PROBE_RESTART_VISIBILITY,
	PROBE_RACE_PUBLISHER_ONE,
	PROBE_RACE_PUBLISHER_TWO,
	PROBE_RACE_FINAL_GET,
	PROBE_CLEANUP_MANIFEST,
	PROBE_CLEANUP_IMMUTABLE,
	PROBE_STEP_NR,
};

struct probe_step {
	const char *name;
	long status;
	uint64_t uploaded_bytes;
	uint64_t downloaded_bytes;
	char error_code[64];
	unsigned int attempted:1;
	unsigned int passed:1;
};

struct probe_result {
	struct probe_step steps[PROBE_STEP_NR];
	struct strbuf prefix;
	const char *failure_phase;
	int race_winner_count;
	unsigned int ok:1;
};

#define PROBE_RESULT_INIT { \
	.steps = { \
		[PROBE_IMMUTABLE_PUT] = { .name = "immutablePut" }, \
		[PROBE_HEAD] = { .name = "head" }, \
		[PROBE_FULL_GET] = { .name = "fullGet" }, \
		[PROBE_RANGE_GET] = { .name = "rangeGet" }, \
		[PROBE_IMMUTABLE_CONFLICT] = { \
			.name = "immutableConflict", \
		}, \
		[PROBE_MANIFEST_CREATE] = { .name = "manifestCreate" }, \
		[PROBE_MANIFEST_HEAD] = { .name = "manifestHead" }, \
		[PROBE_MANIFEST_CAS] = { .name = "manifestCas" }, \
		[PROBE_STALE_MANIFEST_CONFLICT] = { \
			.name = "staleManifestConflict", \
		}, \
		[PROBE_MANIFEST_FINAL_GET] = { \
			.name = "manifestFinalGet", \
		}, \
		[PROBE_RESTART_VISIBILITY] = { \
			.name = "restartVisibility", \
		}, \
		[PROBE_RACE_PUBLISHER_ONE] = { \
			.name = "racePublisherOne", \
		}, \
		[PROBE_RACE_PUBLISHER_TWO] = { \
			.name = "racePublisherTwo", \
		}, \
		[PROBE_RACE_FINAL_GET] = { .name = "raceFinalGet" }, \
		[PROBE_CLEANUP_MANIFEST] = { .name = "cleanupManifest" }, \
		[PROBE_CLEANUP_IMMUTABLE] = { .name = "cleanupImmutable" }, \
	}, \
	.prefix = STRBUF_INIT, \
	.race_winner_count = -1, \
}

struct race_request_header {
	uint32_t key_len;
	uint32_t etag_len;
};

struct race_wire_result {
	int64_t status;
	uint64_t uploaded_bytes;
	uint64_t downloaded_bytes;
	uint32_t request_error;
};

struct race_publisher_result {
	long status;
	uint64_t uploaded_bytes;
	uint64_t downloaded_bytes;
	unsigned int request_error:1;
};

static int successful_status(long status)
{
	return status >= 200 && status < 300;
}

static int conflict_status(long status)
{
	return status == 409 || status == 412;
}

static int response_equals(const struct s3_response *response,
			   const void *expected, size_t expected_size)
{
	return response->body.len == expected_size &&
		!memcmp(response->body.buf, expected, expected_size);
}

static void record_error_code(struct probe_step *step,
			      const struct s3_response *response)
{
	const char *start, *end;
	size_t len;

	if (response->http_status < 300)
		return;
	start = strstr(response->body.buf, "<Code>");
	if (!start)
		return;
	start += strlen("<Code>");
	end = strstr(start, "</Code>");
	if (!end || end == start || (size_t)(end - start) >= sizeof(step->error_code))
		return;
	len = end - start;
	for (size_t i = 0; i < len; i++)
		if (!isalnum((unsigned char)start[i]) && start[i] != '-' &&
		    start[i] != '_')
			return;
	memcpy(step->error_code, start, len);
	step->error_code[len] = '\0';
}

static int record_step(struct probe_result *result, enum probe_step_id id,
		       const struct s3_response *response, int passed)
{
	struct probe_step *step = &result->steps[id];

	step->attempted = 1;
	step->passed = !!passed;
	step->status = response->http_status;
	step->uploaded_bytes = response->uploaded_bytes;
	step->downloaded_bytes = response->downloaded_bytes;
	record_error_code(step, response);
	return step->passed;
}

static void record_publisher(struct probe_result *result,
			     enum probe_step_id id,
			     const struct race_publisher_result *publisher,
			     int passed)
{
	struct probe_step *step = &result->steps[id];

	step->attempted = 1;
	step->passed = !!passed;
	step->status = publisher->status;
	step->uploaded_bytes = publisher->uploaded_bytes;
	step->downloaded_bytes = publisher->downloaded_bytes;
}

static void close_pipe(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static int race_publisher_worker(const char *which)
{
	const char *payload;
	size_t payload_len;
	struct race_request_header request;
	struct race_wire_result result = { 0 };
	struct s3_client client = S3_CLIENT_INIT;
	struct s3_response response = S3_RESPONSE_INIT;
	char *key = NULL;
	char *etag = NULL;
	char ready;
	int ret = 1;

	if (!strcmp(which, "one")) {
		payload = race_manifest_one;
		payload_len = sizeof(race_manifest_one) - 1;
	} else if (!strcmp(which, "two")) {
		payload = race_manifest_two;
		payload_len = sizeof(race_manifest_two) - 1;
	} else {
		return error(_("invalid race publisher"));
	}

	if (s3_client_init_from_env(&client)) {
		ready = 'E';
		write_in_full(1, &ready, 1);
		goto out;
	}
	ready = 'R';
	if (write_in_full(1, &ready, 1) != 1 ||
	    read_in_full(0, &request, sizeof(request)) != sizeof(request) ||
	    !request.key_len || !request.etag_len ||
	    request.key_len > 4096 || request.etag_len > 4096)
		goto out;

	key = xmallocz(request.key_len);
	etag = xmallocz(request.etag_len);
	if (read_in_full(0, key, request.key_len) != request.key_len ||
	    read_in_full(0, etag, request.etag_len) != request.etag_len)
		goto out;
	ready = 'A';
	if (write_in_full(1, &ready, 1) != 1 ||
	    read_in_full(0, &ready, 1) != 1 || ready != 'G')
		goto out;

	result.request_error = s3_client_put(&client, key, payload, payload_len,
					     etag, 0, &response) != 0;
	result.status = response.http_status;
	result.uploaded_bytes = response.uploaded_bytes;
	result.downloaded_bytes = response.downloaded_bytes;
	if (write_in_full(1, &result, sizeof(result)) != sizeof(result))
		goto out;
	ret = 0;
out:
	free(etag);
	free(key);
	s3_response_release(&response);
	s3_client_release(&client);
	return ret;
}

static int start_race_publisher(struct child_process *publisher,
				const char *which)
{
	publisher->git_cmd = 1;
	publisher->in = -1;
	publisher->out = -1;
	strvec_pushl(&publisher->env, "GIT_TRACE2=0", "GIT_TRACE2_EVENT=0",
		     "GIT_TRACE2_PERF=0", NULL);
	strvec_pushl(&publisher->args, "cloud-bench", "--race-publisher",
		     which, NULL);
	return start_command(publisher);
}

static int send_race_request(int fd, const char *key, const char *etag)
{
	size_t key_len = strlen(key);
	size_t etag_len = strlen(etag);
	struct race_request_header request;

	if (key_len > UINT32_MAX || etag_len > UINT32_MAX)
		return -1;
	request.key_len = key_len;
	request.etag_len = etag_len;
	if (write_in_full(fd, &request, sizeof(request)) != sizeof(request) ||
	    write_in_full(fd, key, key_len) != key_len ||
	    write_in_full(fd, etag, etag_len) != etag_len)
		return -1;
	return 0;
}

static int run_manifest_race(const char *key, const char *etag,
			     struct race_publisher_result result[2])
{
	struct child_process publisher[2] = {
		CHILD_PROCESS_INIT,
		CHILD_PROCESS_INIT,
	};
	struct race_wire_result wire[2];
	const char *which[] = { "one", "two" };
	char ready[2];
	int started = 0;
	int i;
	int ret = -1;

	for (i = 0; i < 2; i++) {
		if (start_race_publisher(&publisher[i], which[i]))
			goto out;
		started++;
	}
	for (i = 0; i < 2; i++)
		if (read_in_full(publisher[i].out, &ready[i], 1) != 1 ||
		    ready[i] != 'R')
			goto out;
	for (i = 0; i < 2; i++) {
		if (send_race_request(publisher[i].in, key, etag))
			goto out;
	}
	for (i = 0; i < 2; i++)
		if (read_in_full(publisher[i].out, &ready[i], 1) != 1 ||
		    ready[i] != 'A')
			goto out;
	for (i = 0; i < 2; i++) {
		ready[i] = 'G';
		if (write_in_full(publisher[i].in, &ready[i], 1) != 1)
			goto out;
		close_pipe(&publisher[i].in);
	}
	for (i = 0; i < 2; i++) {
		if (read_in_full(publisher[i].out, &wire[i], sizeof(wire[i])) !=
		    sizeof(wire[i]))
			goto out;
		close_pipe(&publisher[i].out);
		result[i].status = wire[i].status;
		result[i].uploaded_bytes = wire[i].uploaded_bytes;
		result[i].downloaded_bytes = wire[i].downloaded_bytes;
		result[i].request_error = wire[i].request_error;
	}
	ret = 0;
out:
	for (i = 0; i < started; i++) {
		close_pipe(&publisher[i].in);
		close_pipe(&publisher[i].out);
		if (finish_command(&publisher[i]))
			ret = -1;
	}
	return ret;
}

static int valid_probe_prefix(const char *prefix)
{
	const unsigned char *p = (const unsigned char *)prefix;

	if (!starts_with(prefix, "probe/") || !prefix[sizeof("probe/") - 1])
		return 0;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '/' && *p != '-' && *p != '_' &&
		    *p != '.')
			return 0;
	return 1;
}

static int make_probe_prefix(struct strbuf *prefix)
{
	static const char hex[] = "0123456789abcdef";
	const char *test_prefix = getenv("GIT_TEST_CLOUD_BENCH_PREFIX");
	unsigned char random[12];
	size_t i;

	if (test_prefix) {
		if (!valid_probe_prefix(test_prefix))
			return error(_("invalid GIT_TEST_CLOUD_BENCH_PREFIX"));
		strbuf_addstr(prefix, test_prefix);
		return 0;
	}
	if (csprng_bytes(random, sizeof(random), 0) < 0)
		return error(_("unable to generate S3 probe prefix"));
	strbuf_addstr(prefix, "probe/");
	for (i = 0; i < ARRAY_SIZE(random); i++) {
		strbuf_addch(prefix, hex[random[i] >> 4]);
		strbuf_addch(prefix, hex[random[i] & 0xf]);
	}
	return 0;
}

static void write_probe_json(const struct probe_result *result)
{
	struct json_writer json = JSON_WRITER_INIT;
	size_t i;

	jw_object_begin(&json, 0);
	jw_object_string(&json, "schema", "git-cloud-bench-probe/v1");
	jw_object_string(&json, "measurementScope", "bucket-capability-probe");
	jw_object_true(&json, "cloudMeasured");
	jw_object_bool(&json, "ok", result->ok);
	if (result->prefix.len)
		jw_object_string(&json, "prefix", result->prefix.buf);
	else
		jw_object_null(&json, "prefix");
	if (result->failure_phase)
		jw_object_string(&json, "failurePhase", result->failure_phase);
	else
		jw_object_null(&json, "failurePhase");
	if (result->race_winner_count >= 0)
		jw_object_intmax(&json, "raceWinnerCount",
				 result->race_winner_count);
	else
		jw_object_null(&json, "raceWinnerCount");
	jw_object_inline_begin_object(&json, "steps");
	for (i = 0; i < PROBE_STEP_NR; i++) {
		const struct probe_step *step = &result->steps[i];

		jw_object_inline_begin_object(&json, step->name);
		jw_object_bool(&json, "attempted", step->attempted);
		jw_object_bool(&json, "passed", step->passed);
		if (step->attempted)
			jw_object_intmax(&json, "status", step->status);
		else
			jw_object_null(&json, "status");
		if (step->error_code[0])
			jw_object_string(&json, "errorCode", step->error_code);
		else
			jw_object_null(&json, "errorCode");
		jw_object_intmax(&json, "uploadedBytes",
				 step->uploaded_bytes);
		jw_object_intmax(&json, "downloadedBytes",
				 step->downloaded_bytes);
		jw_end(&json);
	}
	jw_end(&json);
	jw_end(&json);
	puts(json.json.buf);
	jw_release(&json);
}

static int cleanup_probe_object(struct s3_client *client, const char *key,
				struct s3_response *response,
				struct probe_result *result,
				enum probe_step_id step_id)
{
	if (!client->initialized && s3_client_init_from_env(client)) {
		result->steps[step_id].attempted = 1;
		return 0;
	}
	if (s3_client_delete(client, key, response)) {
		result->steps[step_id].attempted = 1;
		return 0;
	}
	return record_step(result, step_id, response,
			   response->http_status == 200 ||
			   response->http_status == 204 ||
			   response->http_status == 404);
}

static int run_probe(void)
{
	static const char immutable_data[] =
		"scope-git-cloud-probe-object-v1\n";
	static const char manifest_v1[] =
		"git-cloud-probe-manifest 1\nvalue one\n";
	static const char manifest_v2[] =
		"git-cloud-probe-manifest 1\nvalue two\n";
	const uint64_t range_offset = 6;
	const uint64_t range_length = 9;
	struct s3_client client = S3_CLIENT_INIT;
	struct s3_response response = S3_RESPONSE_INIT;
	struct probe_result result = PROBE_RESULT_INIT;
	struct strbuf immutable_key = STRBUF_INIT;
	struct strbuf manifest_key = STRBUF_INIT;
	struct strbuf expected_range = STRBUF_INIT;
	struct strbuf immutable_etag = STRBUF_INIT;
	struct strbuf manifest_etag = STRBUF_INIT;
	struct strbuf race_etag = STRBUF_INIT;
	struct race_publisher_result publishers[2] = { 0 };
	const char *winning_manifest = NULL;
	size_t winning_manifest_len = 0;
	int race_ok;
	int immutable_created = 0;
	int manifest_created = 0;
	size_t i;
	int ret = 1;

	if (s3_client_init_from_env(&client)) {
		result.failure_phase = "configuration";
		goto out;
	}
	if (make_probe_prefix(&result.prefix)) {
		result.failure_phase = "prefix";
		goto out;
	}
	strbuf_addf(&immutable_key, "%s/immutable", result.prefix.buf);
	strbuf_addf(&manifest_key, "%s/manifest", result.prefix.buf);

	if (s3_client_put(&client, immutable_key.buf,
			  immutable_data, sizeof(immutable_data) - 1,
			  NULL, 1, &response)) {
		result.failure_phase = "immutablePut";
		goto out;
	}
	if (!record_step(&result, PROBE_IMMUTABLE_PUT, &response,
			 successful_status(response.http_status))) {
		result.failure_phase = "immutablePut";
		goto out;
	}
	immutable_created = 1;

	if (s3_client_head(&client, immutable_key.buf, &response)) {
		result.failure_phase = "head";
		goto out;
	}
	if (!record_step(&result, PROBE_HEAD, &response,
			 successful_status(response.http_status) &&
			 response.content_length == sizeof(immutable_data) - 1 &&
			 response.etag.len)) {
		result.failure_phase = "head";
		goto out;
	}
	strbuf_addbuf(&immutable_etag, &response.etag);

	if (s3_client_get(&client, immutable_key.buf, &response)) {
		result.failure_phase = "fullGet";
		goto out;
	}
	if (!record_step(&result, PROBE_FULL_GET, &response,
			 response.http_status == 200 &&
			 response_equals(&response, immutable_data,
					 sizeof(immutable_data) - 1) &&
			 response.etag.len &&
			 !strbuf_cmp(&response.etag, &immutable_etag))) {
		result.failure_phase = "fullGet";
		goto out;
	}

	strbuf_addf(&expected_range, "bytes %"PRIu64"-%"PRIu64"/%zu",
		    range_offset, range_offset + range_length - 1,
		    sizeof(immutable_data) - 1);
	if (s3_client_get_range(&client, immutable_key.buf,
				range_offset, range_length, &response)) {
		result.failure_phase = "rangeGet";
		goto out;
	}
	if (!record_step(&result, PROBE_RANGE_GET, &response,
			 response.http_status == 206 &&
			 response_equals(&response, immutable_data + range_offset,
					 range_length) &&
			 !strbuf_cmp(&response.content_range, &expected_range) &&
			 response.etag.len &&
			 !strbuf_cmp(&response.etag, &immutable_etag))) {
		result.failure_phase = "rangeGet";
		goto out;
	}

	if (s3_client_put(&client, immutable_key.buf,
			  immutable_data, sizeof(immutable_data) - 1,
			  NULL, 1, &response)) {
		result.failure_phase = "immutableConflict";
		goto out;
	}
	if (!record_step(&result, PROBE_IMMUTABLE_CONFLICT, &response,
			 conflict_status(response.http_status))) {
		result.failure_phase = "immutableConflict";
		goto out;
	}

	if (s3_client_put(&client, manifest_key.buf,
			  manifest_v1, sizeof(manifest_v1) - 1,
			  NULL, 1, &response)) {
		result.failure_phase = "manifestCreate";
		goto out;
	}
	if (!record_step(&result, PROBE_MANIFEST_CREATE, &response,
			 successful_status(response.http_status))) {
		result.failure_phase = "manifestCreate";
		goto out;
	}
	manifest_created = 1;

	if (s3_client_head(&client, manifest_key.buf, &response)) {
		result.failure_phase = "manifestHead";
		goto out;
	}
	if (!record_step(&result, PROBE_MANIFEST_HEAD, &response,
			 successful_status(response.http_status) &&
			 response.content_length == sizeof(manifest_v1) - 1 &&
			 response.etag.len)) {
		result.failure_phase = "manifestHead";
		goto out;
	}
	strbuf_addbuf(&manifest_etag, &response.etag);
	if (!manifest_etag.len) {
		result.failure_phase = "manifestEtag";
		goto out;
	}

	if (s3_client_put(&client, manifest_key.buf,
			  manifest_v2, sizeof(manifest_v2) - 1,
			  manifest_etag.buf, 0, &response)) {
		result.failure_phase = "manifestCas";
		goto out;
	}
	if (!record_step(&result, PROBE_MANIFEST_CAS, &response,
			 successful_status(response.http_status))) {
		result.failure_phase = "manifestCas";
		goto out;
	}

	if (s3_client_put(&client, manifest_key.buf,
			  manifest_v1, sizeof(manifest_v1) - 1,
			  manifest_etag.buf, 0, &response)) {
		result.failure_phase = "staleManifestConflict";
		goto out;
	}
	if (!record_step(&result, PROBE_STALE_MANIFEST_CONFLICT, &response,
			 conflict_status(response.http_status))) {
		result.failure_phase = "staleManifestConflict";
		goto out;
	}

	if (s3_client_get(&client, manifest_key.buf, &response)) {
		result.failure_phase = "manifestFinalGet";
		goto out;
	}
	if (!record_step(&result, PROBE_MANIFEST_FINAL_GET, &response,
			 response.http_status == 200 &&
			 response_equals(&response, manifest_v2,
					 sizeof(manifest_v2) - 1) &&
			 response.etag.len)) {
		result.failure_phase = "manifestFinalGet";
		goto out;
	}
	strbuf_addbuf(&race_etag, &response.etag);
	if (!race_etag.len) {
		result.failure_phase = "raceEtag";
		goto out;
	}

	/* Tear down all HTTP state before proving a fresh client sees the write. */
	s3_client_release(&client);
	if (s3_client_init_from_env(&client)) {
		result.failure_phase = "restartInit";
		goto out;
	}
	if (s3_client_get(&client, manifest_key.buf, &response)) {
		result.failure_phase = "restartVisibility";
		goto out;
	}
	if (!record_step(&result, PROBE_RESTART_VISIBILITY, &response,
			 response.http_status == 200 &&
			 response_equals(&response, manifest_v2,
					 sizeof(manifest_v2) - 1) &&
			 !strbuf_cmp(&response.etag, &race_etag))) {
		result.failure_phase = "restartVisibility";
		goto out;
	}

	/* Each publisher gets independent HTTP state and the same starting ETag. */
	s3_client_release(&client);
	if (run_manifest_race(manifest_key.buf, race_etag.buf, publishers)) {
		result.failure_phase = "manifestRace";
		goto out;
	}
	result.race_winner_count =
		successful_status(publishers[0].status) +
		successful_status(publishers[1].status);
	race_ok = !publishers[0].request_error &&
		!publishers[1].request_error &&
		result.race_winner_count == 1 &&
		(conflict_status(publishers[0].status) ||
		 conflict_status(publishers[1].status));
	record_publisher(&result, PROBE_RACE_PUBLISHER_ONE, &publishers[0],
			 race_ok);
	record_publisher(&result, PROBE_RACE_PUBLISHER_TWO, &publishers[1],
			 race_ok);
	if (!race_ok) {
		result.failure_phase = "manifestRace";
		goto out;
	}
	if (successful_status(publishers[0].status)) {
		winning_manifest = race_manifest_one;
		winning_manifest_len = sizeof(race_manifest_one) - 1;
	} else if (successful_status(publishers[1].status)) {
		winning_manifest = race_manifest_two;
		winning_manifest_len = sizeof(race_manifest_two) - 1;
	}

	if (s3_client_init_from_env(&client)) {
		result.failure_phase = "raceFinalInit";
		goto out;
	}
	if (s3_client_get(&client, manifest_key.buf, &response)) {
		result.failure_phase = "raceFinalGet";
		goto out;
	}
	if (!record_step(&result, PROBE_RACE_FINAL_GET, &response,
			 response.http_status == 200 && winning_manifest &&
			 response_equals(&response, winning_manifest,
					 winning_manifest_len))) {
		result.failure_phase = "raceFinalGet";
		goto out;
	}

out:
	if (manifest_created &&
	    !cleanup_probe_object(&client, manifest_key.buf, &response, &result,
				  PROBE_CLEANUP_MANIFEST) &&
	    !result.failure_phase)
		result.failure_phase = "cleanupManifest";
	if (immutable_created &&
	    !cleanup_probe_object(&client, immutable_key.buf, &response, &result,
				  PROBE_CLEANUP_IMMUTABLE) &&
	    !result.failure_phase)
		result.failure_phase = "cleanupImmutable";

	result.ok = !result.failure_phase;
	for (i = 0; i < PROBE_STEP_NR; i++)
		if (!result.steps[i].attempted || !result.steps[i].passed)
			result.ok = 0;
	ret = result.ok ? 0 : 1;
	write_probe_json(&result);
	strbuf_release(&race_etag);
	strbuf_release(&manifest_etag);
	strbuf_release(&immutable_etag);
	strbuf_release(&expected_range);
	strbuf_release(&manifest_key);
	strbuf_release(&immutable_key);
	strbuf_release(&result.prefix);
	s3_response_release(&response);
	s3_client_release(&client);
	return ret;
}

static int validate_config(int print_storage_id)
{
	struct s3_client client = S3_CLIENT_INIT;
	struct strbuf storage_id = STRBUF_INIT;
	int ret = s3_client_init_from_env(&client);

	if (!ret && print_storage_id) {
		s3_client_storage_id(&client, &storage_id);
		printf("%s\n", storage_id.buf);
	}
	strbuf_release(&storage_id);
	s3_client_release(&client);
	return ret ? 1 : 0;
}

int cmd_main(int argc, const char **argv)
{
	trace2_cmd_name("cloud-bench");
	if (argc == 3 && !strcmp(argv[1], "--race-publisher"))
		return race_publisher_worker(argv[2]);
	if ((argc == 2 || argc == 3) &&
	    !strcmp(argv[1], "validate-config")) {
		if (argc == 3 && strcmp(argv[2], "--print-storage-id"))
			usage(cloud_bench_usage);
		return validate_config(argc == 3);
	}
	if (argc != 3 || strcmp(argv[1], "probe") || strcmp(argv[2], "--json"))
		usage(cloud_bench_usage);
	return run_probe();
}
