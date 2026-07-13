#ifndef YAPPO_EMBEDDING_H
#define YAPPO_EMBEDDING_H

#include "yappo_index_v2.h"

typedef enum {
  YAP_EMBEDDING_OK = 0,
  YAP_EMBEDDING_INVALID_ARGUMENT = -1,
  YAP_EMBEDDING_IO_ERROR = -2,
  YAP_EMBEDDING_HTTP_ERROR = -3,
  YAP_EMBEDDING_INVALID_RESPONSE = -4,
  YAP_EMBEDDING_DIMENSION_MISMATCH = -5,
  YAP_EMBEDDING_NON_FINITE = -6,
  YAP_EMBEDDING_ALLOCATION_FAILED = -7,
  YAP_EMBEDDING_MISSING_ID = -8,
  YAP_EMBEDDING_DUPLICATE_ID = -9
} YAP_EMBEDDING_STATUS;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  YAP_V2_BYTES_VIEW text;
} YAP_EMBEDDING_INPUT;

typedef struct {
  float *values;
  size_t input_count;
  size_t dimensions;
} YAP_EMBEDDING_RESULT;

typedef int (*YAP_EMBEDDING_TRANSPORT)(void *context, const char *endpoint,
                                       const char *authorization, const char *request_body,
                                       size_t request_bytes, uint32_t timeout_ms,
                                       char **response_body, size_t *response_bytes,
                                       long *http_status);
typedef void (*YAP_EMBEDDING_SLEEP)(void *context, uint32_t milliseconds);

typedef struct {
  const char *endpoint;
  const char *api_key;
  const char *model_id;
  size_t dimensions;
  size_t batch_size;
  uint32_t timeout_ms;
  uint32_t max_retries;
  uint32_t retry_backoff_ms;
  YAP_EMBEDDING_TRANSPORT transport;
  void *transport_context;
  YAP_EMBEDDING_SLEEP sleep;
  void *sleep_context;
} YAP_EMBEDDING_HTTP_CONFIG;

const char *YAP_Embedding_status_string(YAP_EMBEDDING_STATUS status);
void YAP_Embedding_result_init(YAP_EMBEDDING_RESULT *result);
void YAP_Embedding_result_free(YAP_EMBEDDING_RESULT *result);
void YAP_Embedding_http_config_init(YAP_EMBEDDING_HTTP_CONFIG *config);
int YAP_Embedding_http(const YAP_EMBEDDING_HTTP_CONFIG *config,
                       const YAP_EMBEDDING_INPUT *inputs, size_t input_count,
                       YAP_EMBEDDING_RESULT *result);
int YAP_Embedding_precomputed_read(const char *path, const YAP_EMBEDDING_INPUT *inputs,
                                   size_t input_count, size_t dimensions,
                                   YAP_EMBEDDING_RESULT *result);

#endif
