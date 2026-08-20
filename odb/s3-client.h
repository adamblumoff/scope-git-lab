#ifndef ODB_S3_CLIENT_H
#define ODB_S3_CLIENT_H

#include "strbuf.h"

struct s3_client {
	char *endpoint;
	char *access_key;
	char *secret_key;
	char *bucket;
	char *region;
	char *url_style;
	char *sigv4;
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

int s3_client_put(struct s3_client *client, const char *key,
		  const void *data, size_t size,
		  const char *if_match, int if_none_match,
		  struct s3_response *response);
int s3_client_head(struct s3_client *client, const char *key,
		   struct s3_response *response);
int s3_client_get(struct s3_client *client, const char *key,
		  struct s3_response *response);
int s3_client_get_range(struct s3_client *client, const char *key,
			uint64_t offset, uint64_t length,
			struct s3_response *response);
int s3_client_delete(struct s3_client *client, const char *key,
		     struct s3_response *response);

#endif /* ODB_S3_CLIENT_H */
