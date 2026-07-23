#ifndef YAPPO_RUNTIME_POLICY_V2_H
#define YAPPO_RUNTIME_POLICY_V2_H

#include "yappo_index_v2.h"

#include <pthread.h>

#define YAP_V2_WRITE_TOKEN_MAX_BYTES 255U
#define YAP_V2_AUTHORIZATION_MAX_BYTES (7U + YAP_V2_WRITE_TOKEN_MAX_BYTES)

typedef struct {
  size_t max_inflight;
  size_t max_inflight_bytes;
  uint32_t request_timeout_ms;
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
