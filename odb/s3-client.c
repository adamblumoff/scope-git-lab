#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "config.h"
#include "credential.h"
#include "environment.h"
#include "git-curl-compat.h"
#include "gettext.h"
#include "hash.h"
#include "odb/s3-client.h"
#include "strbuf.h"
#include "url.h"
#include "urlmatch.h"

#include <curl/urlapi.h>

#if LIBCURL_VERSION_NUM < 0x074b00
#error "USE_S3 requires libcurl 7.75.0 or later"
#endif

#define S3_METRICS_MAX_DATA_BYTES (15 * 1024 * 1024)
#define S3_MAX_WRITE_RESPONSE_BYTES (64 * 1024)

enum s3_request_method {
	S3_REQUEST_GET,
	S3_REQUEST_HEAD,
	S3_REQUEST_PUT,
	S3_REQUEST_DELETE,
};

static int build_object_url(struct s3_client *client, const char *key,
			    struct strbuf *result);

static const char *method_name(enum s3_request_method method)
{
	switch (method) {
	case S3_REQUEST_GET:
		return "GET";
	case S3_REQUEST_HEAD:
		return "HEAD";
	case S3_REQUEST_PUT:
		return "PUT";
	case S3_REQUEST_DELETE:
		return "DELETE";
	}
	BUG("unknown S3 request method");
}

static uint64_t range_length(const char *range)
{
	uintmax_t first, last;
	char *end;

	if (!range)
		return 0;
	errno = 0;
	first = strtoumax(range, &end, 10);
	if (errno || end == range || *end++ != '-')
		return 0;
	last = strtoumax(end, &end, 10);
	if (errno || *end || last < first)
		return 0;
	return last - first + 1;
}

static const char *metrics_run_id(void)
{
	const unsigned char *value =
		(const unsigned char *)getenv("GIT_CLOUD_ODB_METRICS_RUN");
	const unsigned char *p;

	if (!value || strlen((const char *)value) != 32)
		return NULL;
	for (p = value; *p; p++)
		if (!isxdigit(*p))
			return NULL;
	return (const char *)value;
}

void s3_metrics_append(const struct strbuf *line)
{
	const char *path = getenv("GIT_CLOUD_ODB_METRICS_PATH");
	struct strbuf truncated_path = STRBUF_INIT;
	struct stat st;
	int fd;
	int truncated = 0;

	if (!path || !*path)
		return;
	fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd < 0)
		return;
	if (!fstat(fd, &st) && st.st_size >= 0 &&
	    (uintmax_t)st.st_size <= S3_METRICS_MAX_DATA_BYTES &&
	    line->len <= S3_METRICS_MAX_DATA_BYTES - (size_t)st.st_size) {
		if (write_in_full(fd, line->buf, line->len) < 0 ||
		    fstat(fd, &st) || st.st_size < 0 ||
		    (uintmax_t)st.st_size > S3_METRICS_MAX_DATA_BYTES)
			truncated = 1;
	} else {
		truncated = 1;
	}
	close(fd);
	if (!truncated)
		return;

	strbuf_addf(&truncated_path, "%s.truncated", path);
	fd = open(truncated_path.buf, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0)
		close(fd);
	strbuf_release(&truncated_path);
}

static void append_request_metric(enum s3_request_method method,
				  const char *range,
				  const struct s3_response *response,
				  CURLcode curl_result)
{
	const char *run_id = metrics_run_id();
	struct strbuf line = STRBUF_INIT;
	uint64_t requested = range_length(range);

	if (!run_id)
		return;
	strbuf_addf(&line,
		    "{\"schema\":\"git-cloud-odb-call/v1\","
		    "\"pid\":%"PRIuMAX,
		    (uintmax_t)getpid());
	strbuf_addf(&line, ",\"runId\":\"%s\"", run_id);
	strbuf_addf(&line,
		    ",\"method\":\"%s\","
		    "\"status\":%ld,\"requests\":1,"
		    "\"transportFailures\":%d,\"transportError\":%d,"
		    "\"gets\":%d,"
		    "\"heads\":%d,\"puts\":%d,\"deletes\":%d,"
		    "\"rangeRequests\":%d,"
		    "\"rangeRequestedBytes\":%"PRIu64","
		    "\"uploadedBytes\":%"PRIu64","
		    "\"downloadedBytes\":%"PRIu64",\"conflicts\":%d}\n",
		    method_name(method), response->http_status,
		    curl_result != CURLE_OK, (int)curl_result,
		    method == S3_REQUEST_GET, method == S3_REQUEST_HEAD,
		    method == S3_REQUEST_PUT, method == S3_REQUEST_DELETE,
		    !!range, requested,
		    response->uploaded_bytes, response->downloaded_bytes,
		    response->http_status == 409 || response->http_status == 412);
	s3_metrics_append(&line);
	strbuf_release(&line);
}

struct s3_upload {
	const unsigned char *data;
	size_t len;
	size_t pos;
};

static int valid_bucket_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	size_t len = strlen(name);
	unsigned octets = 0;
	int ipv4 = 1;

	if (len < 3 || len > 63 ||
	    !isalnum(p[0]) || !isalnum(p[len - 1]) ||
	    starts_with(name, "xn--") ||
	    starts_with(name, "sthree-") ||
	    starts_with(name, "amzn-s3-demo-") ||
	    ends_with(name, "-s3alias") ||
	    ends_with(name, "--ol-s3") ||
	    ends_with(name, ".mrap") ||
	    ends_with(name, "--x-s3") ||
	    ends_with(name, "--table-s3"))
		return 0;
	for (; *p; p++) {
		if ((!islower(*p) && !isdigit(*p) && *p != '-' && *p != '.') ||
		    (*p == '.' && p[1] == '.'))
			return 0;
	}

	p = (const unsigned char *)name;
	while (*p && ipv4) {
		unsigned value = 0;
		unsigned digits = 0;

		while (isdigit(*p)) {
			if (value <= 255)
				value = value * 10 + (*p - '0');
			p++;
			digits++;
		}
		octets++;
		if (!digits || value > 255 ||
		    (*p && *p++ != '.') || octets > 4)
			ipv4 = 0;
	}
	if (ipv4 && octets == 4)
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

static int s3_http_option(const char *var, const char *value,
			  const struct config_context *ctx UNUSED, void *data)
{
	struct s3_client *client = data;

	if (!strcmp("http.version", var))
		return git_config_string(&client->http_version, var, value);
	if (!strcmp("http.sslverify", var)) {
		client->ssl_verify = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.sslcainfo", var))
		return git_config_pathname(&client->ssl_ca_info, var, value);
	if (!strcmp("http.sslcapath", var))
		return git_config_pathname(&client->ssl_ca_path, var, value);
	if (!strcmp("http.sslbackend", var))
		return git_config_string(&client->ssl_backend, var, value);
	if (!strcmp("http.schannelcheckrevoke", var)) {
		client->schannel_check_revoke = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.schannelusesslcainfo", var)) {
		client->schannel_use_ssl_ca_info = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.sslcert", var))
		return git_config_pathname(&client->ssl_cert, var, value);
	if (!strcmp("http.sslcerttype", var))
		return git_config_string(&client->ssl_cert_type, var, value);
	if (!strcmp("http.sslkey", var))
		return git_config_pathname(&client->ssl_key, var, value);
	if (!strcmp("http.sslkeytype", var))
		return git_config_string(&client->ssl_key_type, var, value);
	if (!strcmp("http.sslcipherlist", var))
		return git_config_string(&client->ssl_cipher_list, var, value);
	if (!strcmp("http.sslversion", var))
		return git_config_string(&client->ssl_version, var, value);
	if (!strcmp("http.sslcertpasswordprotected", var)) {
		client->ssl_cert_password_protected = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.pinnedpubkey", var))
		return git_config_pathname(&client->ssl_pinned_key, var, value);
	if (!strcmp("http.proxy", var))
		return git_config_string(&client->proxy, var, value);
	if (!strcmp("http.proxyauthmethod", var))
		return git_config_string(&client->proxy_auth_method, var, value);
	if (!strcmp("http.proxysslcainfo", var))
		return git_config_string(&client->proxy_ssl_ca_info, var, value);
	if (!strcmp("http.proxysslcert", var))
		return git_config_string(&client->proxy_ssl_cert, var, value);
	if (!strcmp("http.proxysslkey", var))
		return git_config_string(&client->proxy_ssl_key, var, value);
	if (!strcmp("http.proxysslcertpasswordprotected", var)) {
		client->proxy_ssl_cert_password_protected =
			git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.proxysslverify", var)) {
		client->proxy_ssl_verify = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp("http.curloptresolve", var)) {
		if (!value)
			return config_error_nonbool(var);
		if (!*value) {
			curl_slist_free_all(client->host_resolutions);
			client->host_resolutions = NULL;
		} else {
			struct curl_slist *entry =
				curl_slist_append(client->host_resolutions, value);

			if (!entry)
				return error(_("unable to allocate HTTP host resolution"));
			client->host_resolutions = entry;
		}
		return 0;
	}
	return 0;
}

static void override_http_option(char **option, const char *value)
{
	if (!value)
		return;
	free(*option);
	*option = xstrdup(value);
}

static int load_http_options(struct s3_client *client, const char *endpoint)
{
	struct urlmatch_config config = URLMATCH_CONFIG_INIT;
	char *normalized_url;

	config.section = "http";
	config.collect_fn = s3_http_option;
	config.cb = client;
	normalized_url = url_normalize(endpoint, &config.url);
	if (!normalized_url) {
		urlmatch_config_release(&config);
		return error(_("unable to normalize AWS_ENDPOINT_URL"));
	}
	repo_config(the_repository, urlmatch_config_entry, &config);
	free(normalized_url);
	urlmatch_config_release(&config);

	if (getenv("GIT_SSL_NO_VERIFY"))
		client->ssl_verify = 0;
	override_http_option(&client->ssl_ca_info, getenv("GIT_SSL_CAINFO"));
	override_http_option(&client->ssl_ca_path, getenv("GIT_SSL_CAPATH"));
	override_http_option(&client->ssl_cert, getenv("GIT_SSL_CERT"));
	override_http_option(&client->ssl_cert_type, getenv("GIT_SSL_CERT_TYPE"));
	override_http_option(&client->ssl_key, getenv("GIT_SSL_KEY"));
	override_http_option(&client->ssl_key_type, getenv("GIT_SSL_KEY_TYPE"));
	override_http_option(&client->ssl_cipher_list,
			     getenv("GIT_SSL_CIPHER_LIST"));
	override_http_option(&client->ssl_version, getenv("GIT_SSL_VERSION"));
	override_http_option(&client->proxy_auth_method,
			     getenv("GIT_HTTP_PROXY_AUTHMETHOD"));
	override_http_option(&client->proxy_ssl_ca_info,
			     getenv("GIT_PROXY_SSL_CAINFO"));
	override_http_option(&client->proxy_ssl_cert,
			     getenv("GIT_PROXY_SSL_CERT"));
	override_http_option(&client->proxy_ssl_key,
			     getenv("GIT_PROXY_SSL_KEY"));
	if (getenv("GIT_SSL_CERT_PASSWORD_PROTECTED"))
		client->ssl_cert_password_protected = 1;
	if (getenv("GIT_PROXY_SSL_CERT_PASSWORD_PROTECTED"))
		client->proxy_ssl_cert_password_protected = 1;
	if (!client->proxy) {
		override_http_option(&client->proxy, getenv("HTTPS_PROXY"));
		override_http_option(&client->proxy, getenv("https_proxy"));
		if (!client->proxy) {
			override_http_option(&client->proxy, getenv("ALL_PROXY"));
			override_http_option(&client->proxy, getenv("all_proxy"));
		}
	}
	override_http_option(&client->no_proxy, getenv("NO_PROXY"));
	override_http_option(&client->no_proxy, getenv("no_proxy"));
	return 0;
}

static int load_proxy_credentials(struct s3_client *client)
{
	struct credential credential = CREDENTIAL_INIT;
	struct strbuf url = STRBUF_INIT;
	struct strbuf auth = STRBUF_INIT;
	const char *proxy_url;
	int ret = 0;

	if (!client->proxy || !*client->proxy)
		return 0;
	if (strstr(client->proxy, "://")) {
		proxy_url = client->proxy;
	} else {
		strbuf_addf(&url, "http://%s", client->proxy);
		proxy_url = url.buf;
	}
	credential_from_url(&credential, proxy_url);
	if (credential.username && !credential.password && !credential.credential)
		credential_fill(the_repository, &credential, 1);
	client->proxy_username = xstrdup_or_null(credential.username);
	client->proxy_password = xstrdup_or_null(credential.password);
	if (credential.authtype && credential.credential) {
		strbuf_addf(&auth, "Proxy-Authorization: %s %s",
			    credential.authtype, credential.credential);
		client->proxy_auth_header = strbuf_detach(&auth, NULL);
	}
	if (credential.username && !credential.password && !credential.credential) {
		error(_("proxy credentials do not contain a password"));
		ret = -1;
	}
	credential_clear(&credential);
	strbuf_release(&auth);
	strbuf_release(&url);
	return ret;
}

static long proxy_auth_method(const char *method)
{
	static const struct {
		const char *name;
		long value;
	} methods[] = {
		{ "basic", CURLAUTH_BASIC },
		{ "digest", CURLAUTH_DIGEST },
		{ "negotiate", CURLAUTH_GSSNEGOTIATE },
		{ "ntlm", CURLAUTH_NTLM },
		{ "anyauth", CURLAUTH_ANY },
	};
	size_t i;

	if (!method || !*method)
		return CURLAUTH_ANY;
	for (i = 0; i < ARRAY_SIZE(methods); i++)
		if (!strcmp(method, methods[i].name))
			return methods[i].value;
	warning(_("unsupported proxy authentication method %s: using anyauth"),
		method);
	return CURLAUTH_ANY;
}

static long ssl_version(const char *version)
{
	static const struct {
		const char *name;
		long value;
	} versions[] = {
		{ "sslv2", CURL_SSLVERSION_SSLv2 },
		{ "sslv3", CURL_SSLVERSION_SSLv3 },
		{ "tlsv1", CURL_SSLVERSION_TLSv1 },
		{ "tlsv1.0", CURL_SSLVERSION_TLSv1_0 },
		{ "tlsv1.1", CURL_SSLVERSION_TLSv1_1 },
		{ "tlsv1.2", CURL_SSLVERSION_TLSv1_2 },
		{ "tlsv1.3", CURL_SSLVERSION_TLSv1_3 },
	};
	size_t i;

	if (!version || !*version)
		return CURL_SSLVERSION_DEFAULT;
	for (i = 0; i < ARRAY_SIZE(versions); i++)
		if (!strcmp(version, versions[i].name))
			return versions[i].value;
	warning(_("unsupported ssl version %s: using default"), version);
	return CURL_SSLVERSION_DEFAULT;
}

static long http_version(const char *version)
{
	if (!version || !*version)
		return CURL_HTTP_VERSION_NONE;
	if (!strcmp(version, "HTTP/1.1"))
		return CURL_HTTP_VERSION_1_1;
	if (!strcmp(version, "HTTP/2"))
		return CURL_HTTP_VERSION_2;
	warning(_("unknown value given to http.version: '%s'"), version);
	return CURL_HTTP_VERSION_NONE;
}

static int load_client_key_password(struct s3_client *client)
{
	struct credential credential = CREDENTIAL_INIT;

	if (!client->ssl_cert || !client->ssl_cert_password_protected)
		return 0;
	credential.protocol = xstrdup("cert");
	credential.host = xstrdup("");
	credential.username = xstrdup("");
	credential.path = xstrdup(client->ssl_cert);
	credential_fill(the_repository, &credential, 0);
	client->ssl_key_password = xstrdup_or_null(credential.password);
	credential_clear(&credential);
	if (!client->ssl_key_password)
		return error(_("credential helper did not provide the client key password"));
	return 0;
}

static int load_proxy_key_password(struct s3_client *client)
{
	struct credential credential = CREDENTIAL_INIT;

	if (!client->proxy_ssl_cert ||
	    !client->proxy_ssl_cert_password_protected)
		return 0;
	credential.protocol = xstrdup("cert");
	credential.host = xstrdup("");
	credential.username = xstrdup("");
	credential.path = xstrdup(client->proxy_ssl_cert);
	credential_fill(the_repository, &credential, 0);
	client->proxy_ssl_key_password = xstrdup_or_null(credential.password);
	credential_clear(&credential);
	if (!client->proxy_ssl_key_password)
		return error(_("credential helper did not provide the proxy client key password"));
	return 0;
}

static int select_ssl_backend(const char *backend)
{
	CURLsslset result;
	curl_version_info_data *info;
	const char *runtime;
	static char *selected_backend;
	static const char *openssl_names[] = {
		"OpenSSL", "AmiSSL", "AWS-LC", "BoringSSL", "LibreSSL",
		"quictls",
	};
	size_t i;

	if (!backend || !*backend)
		return 0;
	if (selected_backend) {
		if (!strcasecmp(selected_backend, backend))
			return 0;
		return error(_("could not change SSL backend from '%s' to '%s'"),
			     selected_backend, backend);
	}
	result = curl_global_sslset(CURLSSLBACKEND_NONE, backend, NULL);
	switch (result) {
	case CURLSSLSET_OK:
		selected_backend = xstrdup(backend);
		return 0;
	case CURLSSLSET_UNKNOWN_BACKEND:
		return error(_("unsupported SSL backend '%s'"), backend);
	case CURLSSLSET_NO_BACKENDS:
		return error(_("cURL was built without SSL backends"));
	case CURLSSLSET_TOO_LATE:
		info = curl_version_info(CURLVERSION_NOW);
		runtime = info ? info->ssl_version : NULL;
		if (!runtime)
			break;
		if (!strcasecmp(backend, "openssl")) {
			for (i = 0; i < ARRAY_SIZE(openssl_names); i++)
				if (!strncasecmp(runtime, openssl_names[i],
						 strlen(openssl_names[i]))) {
					selected_backend = xstrdup(backend);
					return 0;
				}
		} else if (!strncasecmp(runtime, backend, strlen(backend))) {
			selected_backend = xstrdup(backend);
			return 0;
		}
		break;
	}
	return error(_("could not select SSL backend '%s'"), backend);
}

int s3_client_init_from_env(struct s3_client *client)
{
	const char *endpoint = required_env("AWS_ENDPOINT_URL");
	const char *access_key = required_env("AWS_ACCESS_KEY_ID");
	const char *secret_key = required_env("AWS_SECRET_ACCESS_KEY");
	const char *session_token = getenv("AWS_SESSION_TOKEN");
	const char *bucket = required_env("AWS_S3_BUCKET_NAME");
	const char *region = required_env("AWS_DEFAULT_REGION");
	const char *url_style = required_env("AWS_S3_URL_STYLE");
	const char *unused;
	struct strbuf config_url = STRBUF_INIT;
	curl_version_info_data *curl_info;

	if (!endpoint || !access_key || !secret_key || !bucket || !region ||
	    !url_style)
		return -1;
	if (!valid_bucket_name(bucket))
		return error(_("invalid S3 bucket name"));
	if (!valid_url_style(url_style))
		return error(_("unsupported AWS_S3_URL_STYLE value"));
	if (virtual_url_style(url_style) && strchr(bucket, '.'))
		return error(_("virtual-hosted HTTPS does not support dotted S3 "
			       "bucket names; use path style"));
	if (!skip_iprefix(endpoint, "https://", &unused) &&
	    !(git_env_bool("GIT_TEST_S3_ALLOW_HTTP", 0) &&
	      skip_iprefix(endpoint, "http://", &unused)))
		return error(_("AWS_ENDPOINT_URL must use HTTPS"));

	*client = (struct s3_client)S3_CLIENT_INIT;
	client->ssl_verify = 1;
	client->proxy_ssl_verify = 1;
	client->schannel_check_revoke = 1;
	client->endpoint = xstrdup(endpoint);
	client->access_key = xstrdup(access_key);
	client->secret_key = xstrdup(secret_key);
	client->session_token = session_token && *session_token ?
		xstrdup(session_token) : NULL;
	client->bucket = xstrdup(bucket);
	client->region = xstrdup(region);
	client->url_style = xstrdup(url_style);
	client->sigv4 = xstrfmt("aws:amz:%s:s3", region);
	if (build_object_url(client, "git-http-options", &config_url) ||
	    load_http_options(client, config_url.buf) ||
	    select_ssl_backend(client->ssl_backend) ||
	    load_proxy_credentials(client) ||
	    load_client_key_password(client) ||
	    load_proxy_key_password(client)) {
		strbuf_release(&config_url);
		s3_client_release(client);
		return -1;
	}
	strbuf_release(&config_url);

	/*
	 * Keep S3 on its own easy handle. Git's HTTP transport has process-global
	 * slots whose handles may later be reused for an unrelated remote; putting
	 * SigV4 credentials there would both corrupt that lifecycle and risk
	 * carrying credentials into the next request.
	 *
	 * Do not call curl_global_cleanup() from this client. Other Git HTTP users
	 * may coexist in the process, and libcurl releases global state at process
	 * exit. Repeated global initialization is supported and keeps their cleanup
	 * from invalidating this client's easy handle.
	 */
	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
		error(_("unable to initialize libcurl for S3"));
		s3_client_release(client);
		return -1;
	}
	client->curl = curl_easy_init();
	if (!client->curl) {
		error(_("unable to allocate S3 curl handle"));
		s3_client_release(client);
		return -1;
	}
	client->initialized = 1;
	curl_info = curl_version_info(CURLVERSION_NOW);
	if (!curl_info || curl_info->version_num < 0x074b00) {
		error(_("USE_S3 requires libcurl 7.75.0 or later at runtime"));
		s3_client_release(client);
		return -1;
	}
	return 0;
}

static void free_sensitive(char **value)
{
	if (!*value)
		return;
	memset(*value, 0, strlen(*value));
	free(*value);
	*value = NULL;
}

void s3_client_release(struct s3_client *client)
{
	if (client->curl)
		curl_easy_cleanup(client->curl);
	free(client->endpoint);
	free(client->http_version);
	free(client->access_key);
	free_sensitive(&client->secret_key);
	free_sensitive(&client->session_token);
	free(client->bucket);
	free(client->region);
	free(client->url_style);
	free(client->sigv4);
	free_sensitive(&client->proxy);
	free(client->no_proxy);
	free(client->proxy_auth_method);
	free(client->proxy_username);
	free_sensitive(&client->proxy_password);
	free_sensitive(&client->proxy_auth_header);
	free(client->proxy_ssl_ca_info);
	free(client->proxy_ssl_cert);
	free(client->proxy_ssl_key);
	free_sensitive(&client->proxy_ssl_key_password);
	free(client->ssl_ca_info);
	free(client->ssl_ca_path);
	free(client->ssl_backend);
	free(client->ssl_cert);
	free(client->ssl_cert_type);
	free(client->ssl_key);
	free(client->ssl_key_type);
	free_sensitive(&client->ssl_key_password);
	free(client->ssl_cipher_list);
	free(client->ssl_pinned_key);
	free(client->ssl_version);
	curl_slist_free_all(client->host_resolutions);
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

struct s3_download {
	struct strbuf *body;
	size_t maximum;
	int exceeded;
};

static size_t response_body(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct s3_download *download = data;
	size_t bytes;

	if (size && nmemb > SIZE_MAX / size)
		return 0;
	bytes = size * nmemb;
	if (bytes > download->maximum - download->body->len) {
		download->exceeded = 1;
		return 0;
	}
	strbuf_add(download->body, ptr, bytes);
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

void s3_client_storage_id(const struct s3_client *client, struct strbuf *out)
{
	struct strbuf identity = STRBUF_INIT;
	char hex[GIT_SHA256_HEXSZ + 1];

	strbuf_addstr(&identity, client->endpoint);
	strbuf_addch(&identity, '\0');
	strbuf_addstr(&identity, client->bucket);
	sha256_hex(identity.buf, identity.len, hex);
	strbuf_addstr(out, hex);
	strbuf_release(&identity);
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
		      const char *range, size_t maximum_response_size,
		      struct s3_response *response)
{
	struct curl_slist *headers = NULL;
	struct curl_slist *proxy_headers = NULL;
	struct s3_upload upload = { .data = data, .len = size };
	struct s3_download download = {
		.body = &response->body,
		.maximum = maximum_response_size,
	};
	struct strbuf url = STRBUF_INIT;
	struct strbuf header = STRBUF_INIT;
	curl_off_t transferred;
	char payload_hash[GIT_SHA256_HEXSZ + 1];
	CURLcode curl_result;
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
	if (client->session_token) {
		if (!valid_condition(client->session_token)) {
			error(_("invalid AWS_SESSION_TOKEN value"));
			goto out;
		}
		strbuf_reset(&header);
		strbuf_addf(&header, "x-amz-security-token: %s",
			    client->session_token);
		if (append_header(&headers, header.buf))
			goto out;
	}

	s3_response_reset(response);
	curl_easy_reset(client->curl);
	curl_easy_setopt(client->curl, CURLOPT_URL, url.buf);
	curl_easy_setopt(client->curl, CURLOPT_HTTP_VERSION,
			 http_version(client->http_version));
	curl_easy_setopt(client->curl, CURLOPT_SSL_VERIFYPEER,
			 client->ssl_verify ? 1L : 0L);
	curl_easy_setopt(client->curl, CURLOPT_SSL_VERIFYHOST,
			 client->ssl_verify ? 2L : 0L);
	if (client->proxy)
		curl_easy_setopt(client->curl, CURLOPT_PROXY, client->proxy);
	if (client->no_proxy)
		curl_easy_setopt(client->curl, CURLOPT_NOPROXY, client->no_proxy);
	curl_easy_setopt(client->curl, CURLOPT_PROXYAUTH,
			 proxy_auth_method(client->proxy_auth_method));
	if (client->proxy_username)
		curl_easy_setopt(client->curl, CURLOPT_PROXYUSERNAME,
				 client->proxy_username);
	if (client->proxy_password)
		curl_easy_setopt(client->curl, CURLOPT_PROXYPASSWORD,
				 client->proxy_password);
	if (client->proxy_auth_header) {
		if (append_header(&proxy_headers, client->proxy_auth_header))
			goto out;
		curl_easy_setopt(client->curl, CURLOPT_PROXYHEADER, proxy_headers);
	}
	if (client->proxy_ssl_ca_info &&
	    !(client->ssl_backend && !strcmp(client->ssl_backend, "schannel") &&
	      !client->schannel_use_ssl_ca_info))
		curl_easy_setopt(client->curl, CURLOPT_PROXY_CAINFO,
				 client->proxy_ssl_ca_info);
	curl_easy_setopt(client->curl, CURLOPT_PROXY_SSL_VERIFYPEER,
			 client->proxy_ssl_verify ? 1L : 0L);
	curl_easy_setopt(client->curl, CURLOPT_PROXY_SSL_VERIFYHOST,
			 client->proxy_ssl_verify ? 2L : 0L);
	if (client->proxy_ssl_cert)
		curl_easy_setopt(client->curl, CURLOPT_PROXY_SSLCERT,
				 client->proxy_ssl_cert);
	if (client->proxy_ssl_key)
		curl_easy_setopt(client->curl, CURLOPT_PROXY_SSLKEY,
				 client->proxy_ssl_key);
	if (client->proxy_ssl_key_password)
		curl_easy_setopt(client->curl, CURLOPT_PROXY_KEYPASSWD,
				 client->proxy_ssl_key_password);
	if (client->ssl_ca_info &&
	    !(client->ssl_backend && !strcmp(client->ssl_backend, "schannel") &&
	      !client->schannel_use_ssl_ca_info))
		curl_easy_setopt(client->curl, CURLOPT_CAINFO, client->ssl_ca_info);
	if (client->ssl_ca_path)
		curl_easy_setopt(client->curl, CURLOPT_CAPATH, client->ssl_ca_path);
	if (client->ssl_cert)
		curl_easy_setopt(client->curl, CURLOPT_SSLCERT, client->ssl_cert);
	if (client->ssl_cert_type)
		curl_easy_setopt(client->curl, CURLOPT_SSLCERTTYPE,
				 client->ssl_cert_type);
	if (client->ssl_key)
		curl_easy_setopt(client->curl, CURLOPT_SSLKEY, client->ssl_key);
	if (client->ssl_key_type)
		curl_easy_setopt(client->curl, CURLOPT_SSLKEYTYPE,
				 client->ssl_key_type);
	if (client->ssl_key_password)
		curl_easy_setopt(client->curl, CURLOPT_KEYPASSWD,
				 client->ssl_key_password);
	if (client->ssl_cipher_list && *client->ssl_cipher_list)
		curl_easy_setopt(client->curl, CURLOPT_SSL_CIPHER_LIST,
				 client->ssl_cipher_list);
	if (client->ssl_pinned_key)
		curl_easy_setopt(client->curl, CURLOPT_PINNEDPUBLICKEY,
				 client->ssl_pinned_key);
	curl_easy_setopt(client->curl, CURLOPT_SSLVERSION,
			 ssl_version(client->ssl_version));
	if (client->host_resolutions)
		curl_easy_setopt(client->curl, CURLOPT_RESOLVE,
				 client->host_resolutions);
#ifdef CURLSSLOPT_NO_REVOKE
	if (client->ssl_backend && !strcmp(client->ssl_backend, "schannel") &&
	    !client->schannel_check_revoke)
		curl_easy_setopt(client->curl, CURLOPT_SSL_OPTIONS,
				 (long)CURLSSLOPT_NO_REVOKE);
#endif
	curl_easy_setopt(client->curl, CURLOPT_HTTPAUTH, CURLAUTH_NONE);
	curl_easy_setopt(client->curl, CURLOPT_AWS_SIGV4, client->sigv4);
	curl_easy_setopt(client->curl, CURLOPT_USERNAME, client->access_key);
	curl_easy_setopt(client->curl, CURLOPT_PASSWORD, client->secret_key);
	curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(client->curl, CURLOPT_CUSTOMREQUEST,
			 method == S3_REQUEST_DELETE ? "DELETE" : NULL);
	curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(client->curl, CURLOPT_FAILONERROR, 0L);
	curl_easy_setopt(client->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(client->curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION, response_header);
	curl_easy_setopt(client->curl, CURLOPT_HEADERDATA, response);
	curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, response_body);
	curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &download);
	curl_easy_setopt(client->curl, CURLOPT_NOBODY,
			 method == S3_REQUEST_HEAD ? 1L : 0L);
	curl_easy_setopt(client->curl, CURLOPT_UPLOAD,
			 method == S3_REQUEST_PUT ? 1L : 0L);
	curl_easy_setopt(client->curl, CURLOPT_RANGE, range);
	curl_easy_setopt(client->curl, CURLOPT_READFUNCTION,
			 method == S3_REQUEST_PUT ? upload_read : NULL);
	curl_easy_setopt(client->curl, CURLOPT_READDATA,
			 method == S3_REQUEST_PUT ? &upload : NULL);
	curl_easy_setopt(client->curl, CURLOPT_SEEKFUNCTION,
			 method == S3_REQUEST_PUT ? upload_seek : NULL);
	curl_easy_setopt(client->curl, CURLOPT_SEEKDATA,
			 method == S3_REQUEST_PUT ? &upload : NULL);
	curl_easy_setopt(client->curl, CURLOPT_INFILESIZE_LARGE,
			 method == S3_REQUEST_PUT ?
			 (curl_off_t)size : (curl_off_t)-1);
	if (method == S3_REQUEST_GET)
		curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);

	curl_result = curl_easy_perform(client->curl);
	curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
			  &response->http_status);
	if (curl_easy_getinfo(client->curl, CURLINFO_SIZE_UPLOAD_T,
			      &transferred) == CURLE_OK && transferred > 0)
		response->uploaded_bytes = transferred;
	if (curl_easy_getinfo(client->curl, CURLINFO_SIZE_DOWNLOAD_T,
			      &transferred) == CURLE_OK && transferred > 0)
		response->downloaded_bytes = transferred;
	client->metrics.requests++;
	client->metrics.uploaded_bytes += response->uploaded_bytes;
	client->metrics.downloaded_bytes += response->downloaded_bytes;
	if (curl_result != CURLE_OK)
		client->metrics.transport_failures++;
	if (response->http_status == 409 || response->http_status == 412)
		client->metrics.conflicts++;
	switch (method) {
	case S3_REQUEST_GET:
		client->metrics.gets++;
		if (range)
			client->metrics.range_requests++;
		break;
	case S3_REQUEST_HEAD:
		client->metrics.heads++;
		break;
	case S3_REQUEST_PUT:
		client->metrics.puts++;
		break;
	case S3_REQUEST_DELETE:
		client->metrics.deletes++;
		break;
	}
	append_request_metric(method, range, response, curl_result);
	if (curl_result != CURLE_OK) {
		if (download.exceeded)
			error(_("S3 response exceeds the configured size limit"));
		else
			error(_("S3 request failed: %s"),
			      curl_easy_strerror(curl_result));
		goto out;
	}
	ret = 0;
out:
	curl_slist_free_all(proxy_headers);
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
			  if_match, if_none_match, NULL,
			  S3_MAX_WRITE_RESPONSE_BYTES, response);
}

int s3_client_head(struct s3_client *client, const char *key,
		   struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_HEAD, NULL, 0,
			  NULL, 0, NULL, SIZE_MAX, response);
}

int s3_client_get(struct s3_client *client, const char *key,
		  struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_GET, NULL, 0,
			  NULL, 0, NULL, SIZE_MAX, response);
}

int s3_client_get_limited(struct s3_client *client, const char *key,
			  size_t maximum_size,
			  struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_GET, NULL, 0,
			  NULL, 0, NULL, maximum_size, response);
}

int s3_client_get_range(struct s3_client *client, const char *key,
			uint64_t offset, uint64_t length,
			struct s3_response *response)
{
	struct strbuf range = STRBUF_INIT;
	uint64_t last;
	int ret;

	if (!length || length > SIZE_MAX ||
	    offset > UINT64_MAX - (length - 1))
		return error(_("invalid S3 byte range"));
	last = offset + length - 1;
	if (last > maximum_signed_value_of_type(curl_off_t))
		return error(_("S3 byte range is too large"));
	strbuf_addf(&range, "%"PRIu64"-%"PRIu64, offset, last);
	ret = s3_request(client, key, S3_REQUEST_GET, NULL, 0,
			 NULL, 0, range.buf, (size_t)length, response);
	if (!ret)
		client->metrics.range_requested_bytes += length;
	strbuf_release(&range);
	return ret;
}

int s3_client_read_range(struct s3_client *client, const char *key,
			 uint64_t object_size, uint64_t offset,
			 size_t length, void *buffer)
{
	struct s3_response response = S3_RESPONSE_INIT;
	struct strbuf expected = STRBUF_INIT;
	int ret = -1;

	if (!length || offset > object_size || length > object_size - offset)
		return error(_("S3 read is outside the object bounds"));
	if (s3_client_get_range(client, key, offset, length, &response))
		goto out;
	strbuf_addf(&expected, "bytes %"PRIu64"-%"PRIu64"/%"PRIu64,
		    offset, offset + length - 1, object_size);
	if (response.http_status != 206 || response.body.len != length ||
	    strbuf_cmp(&response.content_range, &expected)) {
		error(_("S3 returned an invalid byte range"));
		goto out;
	}
	memcpy(buffer, response.body.buf, length);
	ret = 0;
out:
	strbuf_release(&expected);
	s3_response_release(&response);
	return ret;
}

int s3_client_delete(struct s3_client *client, const char *key,
		     struct s3_response *response)
{
	return s3_request(client, key, S3_REQUEST_DELETE, NULL, 0,
			  NULL, 0, NULL, S3_MAX_WRITE_RESPONSE_BYTES, response);
}
