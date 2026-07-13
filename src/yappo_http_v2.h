#ifndef YAPPO_HTTP_V2_H
#define YAPPO_HTTP_V2_H

#include <stddef.h>

#define YAP_V2_HTTP_MAX_BODY_BYTES (1024U * 1024U)

typedef enum {
  YAP_V2_HTTP_SEARCH = 1,
  YAP_V2_HTTP_RETRIEVE = 2,
  YAP_V2_HTTP_INGEST = 3
} YAP_V2_HTTP_OPERATION;

/* Executes a request against one validated v2 snapshot. The returned UTF-8 JSON
 * buffer is owned by the caller and must be freed. */
int YAP_V2_http_execute(const char *index_dir, YAP_V2_HTTP_OPERATION operation,
                        const unsigned char *body, size_t body_bytes,
                        int *http_status, char **response, size_t *response_bytes);

#endif
