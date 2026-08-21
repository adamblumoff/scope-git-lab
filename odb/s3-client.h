#ifndef ODB_S3_CLIENT_H
#define ODB_S3_CLIENT_H

#include "strbuf.h"

#include <curl/curl.h>

struct s3_metrics {
	uint64_t requests;
	uint64_t gets;
	uint64_t heads;
	uint64_t puts;
	uint64_t deletes;
	uint64_t range_requests;
	uint64_t range_requested_bytes;
	uint64_t uploaded_bytes;
	uint64_t downloaded_bytes;
	uint64_t conflicts;
};

struct s3_client {
	char *endpoint;
	char *http_version;
	char *access_key;
	char *secret_key;
	char *session_token;
	char *bucket;
	char *region;
	char *url_style;
	char *sigv4;
	char *proxy;
	char *no_proxy;
	char *proxy_auth_method;
	char *proxy_username;
	char *proxy_password;
	char *proxy_auth_header;
	char *proxy_ssl_ca_info;
	char *proxy_ssl_cert;
	char *proxy_ssl_key;
	char *proxy_ssl_key_password;
	char *ssl_ca_info;
	char *ssl_ca_path;
	char *ssl_backend;
	char *ssl_cert;
	char *ssl_cert_type;
	char *ssl_key;
	char *ssl_key_type;
	char *ssl_key_password;
	char *ssl_cipher_list;
	char *ssl_pinned_key;
	char *ssl_version;
	struct curl_slist *host_resolutions;
	CURL *curl;
	struct s3_metrics metrics;
	unsigned int proxy_ssl_cert_password_protected:1;
	unsigned int proxy_ssl_verify:1;
	unsigned int schannel_check_revoke:1;
	unsigned int schannel_use_ssl_ca_info:1;
	unsigned int ssl_cert_password_protected:1;
	unsigned int ssl_verify:1;
	unsigned int initialized:1;
};

#define S3_CLIENT_INIT { 0 }

struct s3_response {
	long http_status;
	int64_t content_length;
	uint64_t uploaded_bytes;
	uint64_t downloaded_bytes;
	struct strbuf body;
	struct strbuf etag;
	struct strbuf content_range;
};

#define S3_RESPONSE_INIT { \
	.content_length = -1, \
	.body = STRBUF_INIT, \
	.etag = STRBUF_INIT, \
	.content_range = STRBUF_INIT, \
}

int s3_client_init_from_env(struct s3_client *client);
void s3_client_release(struct s3_client *client);

void s3_response_reset(struct s3_response *response);
void s3_response_release(struct s3_response *response);
void s3_metrics_append(const struct strbuf *line);

int s3_client_put(struct s3_client *client, const char *key,
		  const void *data, size_t size,
		  const char *if_match, int if_none_match,
		  struct s3_response *response);
int s3_client_head(struct s3_client *client, const char *key,
		   struct s3_response *response);
int s3_client_get(struct s3_client *client, const char *key,
		  struct s3_response *response);
int s3_client_get_limited(struct s3_client *client, const char *key,
			  size_t maximum_size,
			  struct s3_response *response);
int s3_client_get_range(struct s3_client *client, const char *key,
			uint64_t offset, uint64_t length,
			struct s3_response *response);
int s3_client_read_range(struct s3_client *client, const char *key,
			 uint64_t object_size, uint64_t offset,
			 size_t length, void *buffer);
int s3_client_delete(struct s3_client *client, const char *key,
		     struct s3_response *response);

#endif /* ODB_S3_CLIENT_H */
