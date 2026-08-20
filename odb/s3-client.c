#include "git-compat-util.h"
#include "config.h"
#include "git-curl-compat.h"
#include "gettext.h"
#include "hash.h"
#include "http.h"
#include "odb/s3-client.h"
#include "strbuf.h"

#include <curl/urlapi.h>

#if LIBCURL_VERSION_NUM < 0x074b00
#error "USE_S3 requires libcurl 7.75.0 or later"
#endif

enum s3_request_method {
	S3_REQUEST_GET,
	S3_REQUEST_HEAD,
	S3_REQUEST_PUT,
	S3_REQUEST_DELETE,
};

struct s3_upload {
	const unsigned char *data;
	size_t len;
	size_t pos;
};

static int valid_bucket_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!*p)
		return 0;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '-' && *p != '.')
			return 0;
	return 1;
}

static int valid_url_style(const char *style)
{
	return !strcmp(style, "path") || !strcmp(style, "path-style") ||
		!strcmp(style, "virtual") || !strcmp(style, "virtual-host") ||
		!strcmp(style, "virtual-hosted") ||
		!strcmp(style, "virtual-hosted-style");
}

static int virtual_url_style(const char *style)
{
	return starts_with(style, "virtual");
}

static const char *required_env(const char *name)
{
	const char *value = getenv(name);

	if (!value || !*value) {
		error(_("missing required environment variable '%s'"), name);
		return NULL;
	}
	return value;
}

int s3_client_init_from_env(struct s3_client *client)
{
	const char *endpoint = required_env("AWS_ENDPOINT_URL");
	const char *access_key = required_env("AWS_ACCESS_KEY_ID");
	const char *secret_key = required_env("AWS_SECRET_ACCESS_KEY");
	const char *bucket = required_env("AWS_S3_BUCKET_NAME");
	const char *region = required_env("AWS_DEFAULT_REGION");
	const char *url_style = required_env("AWS_S3_URL_STYLE");
	const char *unused;
	curl_version_info_data *curl_info;

	if (!endpoint || !access_key || !secret_key || !bucket || !region ||
	    !url_style)
		return -1;
	if (!valid_bucket_name(bucket))
		return error(_("invalid S3 bucket name"));
	if (!valid_url_style(url_style))
		return error(_("unsupported AWS_S3_URL_STYLE value"));
	if (!skip_iprefix(endpoint, "https://", &unused) &&
	    !(git_env_bool("GIT_TEST_S3_ALLOW_HTTP", 0) &&
	      skip_iprefix(endpoint, "http://", &unused)))
		return error(_("AWS_ENDPOINT_URL must use HTTPS"));

	*client = (struct s3_client)S3_CLIENT_INIT;
	client->endpoint = xstrdup(endpoint);
	client->access_key = xstrdup(access_key);
	client->secret_key = xstrdup(secret_key);
	client->bucket = xstrdup(bucket);
	client->region = xstrdup(region);
	client->url_style = xstrdup(url_style);
	client->sigv4 = xstrfmt("aws:amz:%s:s3", region);

	http_init(NULL, endpoint, 0);
	client->initialized = 1;
	curl_info = curl_version_info(CURLVERSION_NOW);
	if (!curl_info || curl_info->version_num < 0x074b00) {
		error(_("USE_S3 requires libcurl 7.75.0 or later at runtime"));
		s3_client_release(client);
		return -1;
	}
	return 0;
}

void s3_client_release(struct s3_client *client)
{
	if (client->initialized)
		http_cleanup();
	free(client->endpoint);
	free(client->access_key);
	if (client->secret_key) {
		memset(client->secret_key, 0, strlen(client->secret_key));
		free(client->secret_key);
	}
	free(client->bucket);
	free(client->region);
	free(client->url_style);
	free(client->sigv4);
	*client = (struct s3_client)S3_CLIENT_INIT;
}

void s3_response_reset(struct s3_response *response)
{
	response->http_status = 0;
	response->content_length = -1;
	response->uploaded_bytes = 0;
	response->downloaded_bytes = 0;
	strbuf_reset(&response->body);
	strbuf_reset(&response->etag);
	strbuf_reset(&response->content_range);
}

void s3_response_release(struct s3_response *response)
{
	strbuf_release(&response->body);
	strbuf_release(&response->etag);
	strbuf_release(&response->content_range);
	*response = (struct s3_response)S3_RESPONSE_INIT;
}

static void append_url_encoded(struct strbuf *buf, const char *value,
			       int preserve_slash)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *p = (const unsigned char *)value;

	for (; *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' ||
		    *p == '~' || (preserve_slash && *p == '/')) {
			strbuf_addch(buf, *p);
		} else {
			strbuf_addch(buf, '%');
			strbuf_addch(buf, hex[*p >> 4]);
			strbuf_addch(buf, hex[*p & 0xf]);
		}
	}
}

static int reject_endpoint_part(CURLU *url, CURLUPart part,
				CURLUcode missing_part,
				const char *description)
{
	char *value = NULL;
	CURLUcode result = curl_url_get(url, part, &value, 0);

	if (result == CURLUE_OK && value && *value) {
		curl_free(value);
		return error(_("AWS_ENDPOINT_URL must not contain a %s"),
			     description);
	}
	curl_free(value);
	if (result != CURLUE_OK && result != missing_part)
		return error(_("unable to inspect AWS_ENDPOINT_URL %s"),
			     description);
	return 0;
}

static int build_object_url(struct s3_client *client, const char *key,
			    struct strbuf *result)
{
	CURLU *url = curl_url();
	CURLUcode url_result;
	struct strbuf path = STRBUF_INIT;
	struct strbuf host = STRBUF_INIT;
	char *old_path = NULL;
	char *old_host = NULL;
	char *serialized = NULL;
	int ret = -1;

	if (!url)
		return error(_("unable to allocate S3 URL"));
	url_result = curl_url_set(url, CURLUPART_URL, client->endpoint, 0);
	if (url_result != CURLUE_OK) {
		error(_("invalid AWS_ENDPOINT_URL"));
		goto out;
	}
	if (reject_endpoint_part(url, CURLUPART_QUERY, CURLUE_NO_QUERY,
				 "query") ||
	    reject_endpoint_part(url, CURLUPART_FRAGMENT, CURLUE_NO_FRAGMENT,
				 "fragment"))
		goto out;

	url_result = curl_url_get(url, CURLUPART_PATH, &old_path, 0);
	if (url_result != CURLUE_OK) {
		error(_("unable to read AWS_ENDPOINT_URL path"));
		goto out;
	}
	if (old_path && strcmp(old_path, "/")) {
		strbuf_addstr(&path, old_path);
		while (path.len && path.buf[path.len - 1] == '/')
			strbuf_setlen(&path, path.len - 1);
	}
	strbuf_addch(&path, '/');

	if (virtual_url_style(client->url_style)) {
		url_result = curl_url_get(url, CURLUPART_HOST, &old_host, 0);
		if (url_result != CURLUE_OK || !old_host || !*old_host) {
			error(_("AWS_ENDPOINT_URL has no host"));
			goto out;
		}
		strbuf_addf(&host, "%s.%s", client->bucket, old_host);
		url_result = curl_url_set(url, CURLUPART_HOST, host.buf, 0);
		if (url_result != CURLUE_OK) {
			error(_("unable to construct virtual-hosted S3 URL"));
			goto out;
		}
	} else {
		append_url_encoded(&path, client->bucket, 0);
		strbuf_addch(&path, '/');
	}
	append_url_encoded(&path, key, 1);
	if (curl_url_set(url, CURLUPART_PATH, path.buf, 0) != CURLUE_OK ||
	    curl_url_get(url, CURLUPART_URL, &serialized,
			 CURLU_NO_DEFAULT_PORT) != CURLUE_OK) {
		error(_("unable to construct S3 object URL"));
		goto out;
	}
	strbuf_reset(result);
	strbuf_addstr(result, serialized);
	ret = 0;
out:
	curl_free(serialized);
	curl_free(old_host);
	curl_free(old_path);
	curl_url_cleanup(url);
	strbuf_release(&host);
	strbuf_release(&path);
	return ret;
}

static size_t upload_read(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct s3_upload *upload = data;
	size_t capacity;
	size_t remaining = upload->len - upload->pos;

	if (size && nmemb > SIZE_MAX / size)
		return CURL_READFUNC_ABORT;
	capacity = size * nmemb;
	if (capacity > remaining)
		capacity = remaining;
	memcpy(ptr, upload->data + upload->pos, capacity);
	upload->pos += capacity;
	return capacity;
}

static int upload_seek(void *data, curl_off_t offset, int origin)
{
	struct s3_upload *upload = data;

	if (origin != SEEK_SET || offset < 0 || (uintmax_t)offset > upload->len)
		return CURL_SEEKFUNC_CANTSEEK;
	upload->pos = offset;
	return CURL_SEEKFUNC_OK;
}

static void set_header_value(struct strbuf *out, const char *value,
			     size_t value_len)
{
	strbuf_reset(out);
	strbuf_add(out, value, value_len);
	strbuf_trim(out);
}

static int parse_content_length(const char *value, size_t value_len,
				int64_t *result)
{
	struct strbuf buf = STRBUF_INIT;
	uintmax_t parsed;
	char *end;
	int ret = -1;

	strbuf_add(&buf, value, value_len);
	strbuf_trim(&buf);
	errno = 0;
	parsed = strtoumax(buf.buf, &end, 10);
	if (!errno && end != buf.buf && !*end && parsed <= INT64_MAX) {
		*result = parsed;
		ret = 0;
	}
	strbuf_release(&buf);
	return ret;
}

static size_t response_header(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct s3_response *response = data;
	const char *value;
	size_t bytes;
	size_t value_len;

	if (size && nmemb > SIZE_MAX / size)
		return 0;
	bytes = size * nmemb;
	if (bytes >= 5 && !strncasecmp(ptr, "HTTP/", 5)) {
		response->content_length = -1;
		strbuf_reset(&response->etag);
		strbuf_reset(&response->content_range);
		return bytes;
	}
	if (skip_iprefix_mem(ptr, bytes, "etag:", &value, &value_len))
		set_header_value(&response->etag, value, value_len);
	else if (skip_iprefix_mem(ptr, bytes, "content-range:", &value,
				 &value_len))
		set_header_value(&response->content_range, value, value_len);
	else if (skip_iprefix_mem(ptr, bytes, "content-length:", &value,
				 &value_len))
		parse_content_length(value, value_len, &response->content_length);
	return bytes;
}

static size_t response_body(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct strbuf *body = data;
	size_t bytes;

	if (size && nmemb > SIZE_MAX / size)
		return 0;
	bytes = size * nmemb;
	strbuf_add(body, ptr, bytes);
	return bytes;
}

static void sha256_hex(const void *data, size_t size,
		       char hex[GIT_SHA256_HEXSZ + 1])
{
	static const char hex_digits[] = "0123456789abcdef";
	git_SHA256_CTX context;
	unsigned char hash[GIT_SHA256_RAWSZ];
	size_t i;

	git_SHA256_Init(&context);
	git_SHA256_Update(&context, data, size);
	git_SHA256_Final(hash, &context);
	for (i = 0; i < ARRAY_SIZE(hash); i++) {
		hex[2 * i] = hex_digits[hash[i] >> 4];
		hex[2 * i + 1] = hex_digits[hash[i] & 0xf];
	}
	hex[GIT_SHA256_HEXSZ] = '\0';
}

static int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *appended = curl_slist_append(*headers, header);

	if (!appended)
		return error(_("unable to allocate S3 request header"));
	*headers = appended;
	return 0;
}

static int valid_condition(const char *value)
{
	return !value || (!strchr(value, '\r') && !strchr(value, '\n'));
}

static int s3_request(struct s3_client *client, const char *key,
		      enum s3_request_method method,
		      const void *data, size_t size,
		      const char *if_match, int if_none_match,
		      const char *range, struct s3_response *response)
{
	struct active_request_slot *slot;
	struct slot_results results = { 0 };
	struct curl_slist *headers = NULL;
	struct s3_upload upload = { .data = data, .len = size };
	struct strbuf url = STRBUF_INIT;
	struct strbuf header = STRBUF_INIT;
	curl_off_t transferred;
	char payload_hash[GIT_SHA256_HEXSZ + 1];
	int http_result;
	int ret = -1;

	if (!client->initialized)
		BUG("using an uninitialized S3 client");
	if (!key || !*key)
		return error(_("S3 object key must not be empty"));
	if (if_match && if_none_match)
		return error(_("S3 request has incompatible conditions"));
	if (!valid_condition(if_match))
		return error(_("invalid S3 If-Match value"));
	if (build_object_url(client, key, &url))
		goto out;

	if (method == S3_REQUEST_PUT) {
		sha256_hex(data, size, payload_hash);
		strbuf_addf(&header, "x-amz-content-sha256: %s", payload_hash);
		if (append_header(&headers, header.buf) ||
		    append_header(&headers,
				  "Content-Type: application/octet-stream"))
			goto out;
		if (if_match) {
			strbuf_reset(&header);
			strbuf_addf(&header, "If-Match: %s", if_match);
			if (append_header(&headers, header.buf))
				goto out;
		} else if (if_none_match &&
			   append_header(&headers, "If-None-Match: *")) {
			goto out;
		}
	}

	s3_response_reset(response);
	slot = get_active_slot();
	curl_easy_setopt(slot->curl, CURLOPT_URL, url.buf);
	curl_easy_setopt(slot->curl, CURLOPT_AWS_SIGV4, client->sigv4);
	curl_easy_setopt(slot->curl, CURLOPT_USERNAME, client->access_key);
	curl_easy_setopt(slot->curl, CURLOPT_PASSWORD, client->secret_key);
	curl_easy_setopt(slot->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(slot->curl, CURLOPT_CUSTOMREQUEST,
			 method == S3_REQUEST_DELETE ? "DELETE" : NULL);
	curl_easy_setopt(slot->curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(slot->curl, CURLOPT_FAILONERROR, 0L);
	curl_easy_setopt(slot->curl, CURLOPT_HEADERFUNCTION, response_header);
	curl_easy_setopt(slot->curl, CURLOPT_HEADERDATA, response);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEFUNCTION, response_body);
	curl_easy_setopt(slot->curl, CURLOPT_WRITEDATA, &response->body);
	curl_easy_setopt(slot->curl, CURLOPT_NOBODY,
			 method == S3_REQUEST_HEAD ? 1L : 0L);
	curl_easy_setopt(slot->curl, CURLOPT_UPLOAD,
			 method == S3_REQUEST_PUT ? 1L : 0L);
	curl_easy_setopt(slot->curl, CURLOPT_RANGE, range);
	curl_easy_setopt(slot->curl, CURLOPT_READFUNCTION,
			 method == S3_REQUEST_PUT ? upload_read : NULL);
	curl_easy_setopt(slot->curl, CURLOPT_READDATA,
			 method == S3_REQUEST_PUT ? &upload : NULL);
	curl_easy_setopt(slot->curl, CURLOPT_SEEKFUNCTION,
			 method == S3_REQUEST_PUT ? upload_seek : NULL);
	curl_easy_setopt(slot->curl, CURLOPT_SEEKDATA,
			 method == S3_REQUEST_PUT ? &upload : NULL);
	curl_easy_setopt(slot->curl, CURLOPT_INFILESIZE_LARGE,
			 method == S3_REQUEST_PUT ?
			 (curl_off_t)size : (curl_off_t)-1);
	if (method == S3_REQUEST_GET)
		curl_easy_setopt(slot->curl, CURLOPT_HTTPGET, 1L);

	http_result = run_one_slot(slot, &results);
	response->http_status = results.http_code;
	if (curl_easy_getinfo(slot->curl, CURLINFO_SIZE_UPLOAD_T,
			      &transferred) == CURLE_OK && transferred > 0)
		response->uploaded_bytes = transferred;
	if (curl_easy_getinfo(slot->curl, CURLINFO_SIZE_DOWNLOAD_T,
			      &transferred) == CURLE_OK && transferred > 0)
		response->downloaded_bytes = transferred;
	/*
	 * run_one_slot() normalizes every HTTP status >= 300 to
	 * CURLE_HTTP_RETURNED_ERROR, even with FAILONERROR disabled.  Preserve
	 * those as completed HTTP exchanges so callers can inspect 403, 409,
	 * and 412 without exposing the response body or request metadata.
	 */
	if (http_result == HTTP_START_FAILED) {
		error(_("unable to start S3 request"));
		goto out;
	}
	if (results.curl_result != CURLE_OK &&
	    !(results.curl_result == CURLE_HTTP_RETURNED_ERROR &&
	      response->http_status >= 300)) {
		error(_("S3 request failed: %s"),
		      curl_easy_strerror(results.curl_result));
		goto out;
	}
	ret = 0;
out:
	curl_slist_free_all(headers);
	strbuf_release(&header);
	strbuf_release(&url);
	return ret;
}

int s3_client_put(struct s3_client *client, const char *key,
		  const void *data, size_t size,
		  const char *if_match, int if_none_match,
		  struct s3_response *response)
{
	if (size > maximum_signed_value_of_type(curl_off_t))
		return error(_("S3 upload is too large"));
	return s3_request(client, key, S3_REQUEST_PUT, data, size,
			  if_match, if_none_match, NULL, response);
}

int s3_client_head(struct s3_client *client, const char *key,
		   struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_HEAD, NULL, 0,
			  NULL, 0, NULL, response);
}

int s3_client_get(struct s3_client *client, const char *key,
		  struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_GET, NULL, 0,
			  NULL, 0, NULL, response);
}

int s3_client_get_range(struct s3_client *client, const char *key,
			uint64_t offset, uint64_t length,
			struct s3_response *response)
{
	struct strbuf range = STRBUF_INIT;
	uint64_t last;
	int ret;

	if (!length || offset > UINT64_MAX - (length - 1))
		return error(_("invalid S3 byte range"));
	last = offset + length - 1;
	if (last > maximum_signed_value_of_type(curl_off_t))
		return error(_("S3 byte range is too large"));
	strbuf_addf(&range, "%"PRIu64"-%"PRIu64, offset, last);
	ret = s3_request(client, key, S3_REQUEST_GET, NULL, 0,
			 NULL, 0, range.buf, response);
	strbuf_release(&range);
	return ret;
}

int s3_client_delete(struct s3_client *client, const char *key,
		     struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_DELETE, NULL, 0,
			  NULL, 0, NULL, response);
}
