#ifndef YAPPO_RUNTIME_POLICY_V2_H
#define YAPPO_RUNTIME_POLICY_V2_H

#include "common/yappo_types_v2.h"

#include <pthread.h>

#define YAP_V2_WRITE_TOKEN_MAX_BYTES 255U
#define YAP_V2_AUTHORIZATION_MAX_BYTES (7U + YAP_V2_WRITE_TOKEN_MAX_BYTES)
#define YAP_V2_DEFAULT_WORKER_THREADS 16U
#define YAP_V2_MAX_WORKER_THREADS 1024U
#define YAP_V2_DEFAULT_INGEST_MAX_BODY_BYTES (64U * 1024U * 1024U)
#define YAP_V2_MAX_INGEST_BODY_BYTES (256U * 1024U * 1024U)
#define YAP_V2_DEFAULT_INGEST_TIMEOUT_MS 60000U
#define YAP_V2_MAX_INGEST_TIMEOUT_MS 600000U

typedef struct {
  size_t worker_threads;
  size_t max_inflight;
  size_t max_inflight_bytes;
  uint32_t request_timeout_ms;
  size_t ingest_max_body_bytes;
  uint32_t ingest_timeout_ms;
  char write_token[YAP_V2_WRITE_TOKEN_MAX_BYTES + 1U];
  size_t write_token_bytes;
} YAP_V2_RUNTIME_POLICY;

typedef struct {
  pthread_mutex_t lock;
  size_t inflight;
  size_t inflight_bytes;
  size_t max_inflight;
  size_t max_inflight_bytes;
  int initialized;
} YAP_V2_RUNTIME_LIMITER;

void YAP_V2_runtime_policy_init(YAP_V2_RUNTIME_POLICY *policy);
int YAP_V2_runtime_policy_load_config(YAP_V2_RUNTIME_POLICY *policy, const char *config_path,
                                      char *error, size_t error_size);
int YAP_V2_runtime_limiter_init(YAP_V2_RUNTIME_LIMITER *limiter,
                                const YAP_V2_RUNTIME_POLICY *policy);
void YAP_V2_runtime_limiter_close(YAP_V2_RUNTIME_LIMITER *limiter);
int YAP_V2_runtime_limiter_acquire(YAP_V2_RUNTIME_LIMITER *limiter, size_t request_bytes);
void YAP_V2_runtime_limiter_release(YAP_V2_RUNTIME_LIMITER *limiter, size_t request_bytes);
int YAP_V2_runtime_limiter_snapshot(YAP_V2_RUNTIME_LIMITER *limiter, size_t *inflight,
                                    size_t *inflight_bytes, size_t *max_inflight,
                                    size_t *max_inflight_bytes);
int YAP_V2_authorize_write(const YAP_V2_RUNTIME_POLICY *policy, const char *authorization);
int YAP_V2_socket_set_deadline(int fd, uint32_t timeout_ms);

#endif
