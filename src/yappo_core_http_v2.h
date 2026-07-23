#ifndef YAPPO_CORE_HTTP_V2_H
#define YAPPO_CORE_HTTP_V2_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "yappo_runtime_policy_v2.h"

#define YAP_V2_CORE_HTTP_MAX_LINE_BYTES 8192U
#define YAP_V2_CORE_HTTP_MAX_HEADER_BYTES 65536U
#define YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES (16U * 1024U * 1024U)

typedef enum {
  YAP_V2_CORE_HTTP_OK = 0,
  YAP_V2_CORE_HTTP_EOF = 1,
  YAP_V2_CORE_HTTP_INVALID_ARGUMENT = -1,
  YAP_V2_CORE_HTTP_INVALID = -2,
  YAP_V2_CORE_HTTP_TOO_LARGE = -3,
  YAP_V2_CORE_HTTP_IO_ERROR = -4,
  YAP_V2_CORE_HTTP_NO_MEMORY = -5
} YAP_V2_CORE_HTTP_STATUS;

typedef struct {
  char method[16];
  char target[256];
  size_t content_length;
  int have_content_length;
  int json_content_type;
  char authorization[YAP_V2_AUTHORIZATION_MAX_BYTES + 1U];
  unsigned char *body;
  size_t body_bytes;
} YAP_V2_CORE_HTTP_REQUEST;

typedef struct {
  int status;
  unsigned char *body;
  size_t body_bytes;
} YAP_V2_CORE_HTTP_RESPONSE;

void YAP_V2_core_http_request_init(YAP_V2_CORE_HTTP_REQUEST *request);
void YAP_V2_core_http_request_free(YAP_V2_CORE_HTTP_REQUEST *request);
void YAP_V2_core_http_response_init(YAP_V2_CORE_HTTP_RESPONSE *response);
void YAP_V2_core_http_response_free(YAP_V2_CORE_HTTP_RESPONSE *response);

int YAP_V2_core_http_parse_head(const unsigned char *input, size_t input_bytes,
                                YAP_V2_CORE_HTTP_REQUEST *request);
int YAP_V2_core_http_read_request(FILE *stream, size_t max_body_bytes,
                                  YAP_V2_CORE_HTTP_REQUEST *request);
int YAP_V2_core_http_write_response(FILE *stream, int status, const char *content_type,
                                    const char *allow, int accept_query,
                                    const void *body, size_t body_bytes);
int YAP_V2_core_http_client_request(const char *host, int port, uint32_t timeout_ms,
                                    const char *method, const char *target,
                                    const char *authorization, const void *body,
                                    size_t body_bytes, YAP_V2_CORE_HTTP_RESPONSE *response);

#endif
