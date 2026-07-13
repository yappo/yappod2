#include "yappo_runtime_policy_v2.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#define YAP_V2_DEFAULT_MAX_INFLIGHT 4U
#define YAP_V2_DEFAULT_MAX_INFLIGHT_BYTES (4U * 1024U * 1024U)
#define YAP_V2_DEFAULT_TIMEOUT_MS 5000U
#define YAP_V2_MAX_TIMEOUT_MS 60000U

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

static int parse_size_env(const char *name, size_t default_value, size_t maximum,
                          size_t *output, char *error, size_t error_size) {
  const char *value = getenv(name); char *end = NULL; unsigned long long parsed;
  if (value == NULL) { *output = default_value; return YAP_V2_OK; }
  errno = 0; parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0U || parsed > maximum ||
      parsed > SIZE_MAX) {
    set_error(error, error_size, "runtime limit environment variable is invalid");
    return YAP_V2_INVALID_FORMAT;
  }
  *output = (size_t)parsed; return YAP_V2_OK;
}

void YAP_V2_runtime_policy_init(YAP_V2_RUNTIME_POLICY *policy) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy)); policy->max_inflight = YAP_V2_DEFAULT_MAX_INFLIGHT;
  policy->max_inflight_bytes = YAP_V2_DEFAULT_MAX_INFLIGHT_BYTES;
  policy->request_timeout_ms = YAP_V2_DEFAULT_TIMEOUT_MS;
}

int YAP_V2_runtime_policy_load_env(YAP_V2_RUNTIME_POLICY *policy, char *error, size_t error_size) {
  const char *token; size_t timeout; int status;
  if (policy == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_runtime_policy_init(policy);
  status = parse_size_env("YAPPOD_V2_MAX_INFLIGHT", policy->max_inflight, 1024U,
                          &policy->max_inflight, error, error_size);
  if (status == YAP_V2_OK)
    status = parse_size_env("YAPPOD_V2_MAX_INFLIGHT_BYTES", policy->max_inflight_bytes,
                            1024U * 1024U * 1024U, &policy->max_inflight_bytes, error, error_size);
  timeout = policy->request_timeout_ms;
  if (status == YAP_V2_OK)
    status = parse_size_env("YAPPOD_V2_REQUEST_TIMEOUT_MS", timeout, YAP_V2_MAX_TIMEOUT_MS,
                            &timeout, error, error_size);
  if (status != YAP_V2_OK) return status;
  policy->request_timeout_ms = (uint32_t)timeout;
  token = getenv("YAPPOD_V2_WRITE_TOKEN");
  if (token != NULL) {
    size_t i, length = strlen(token);
    if (length < 16U || length > YAP_V2_WRITE_TOKEN_MAX_BYTES) {
      set_error(error, error_size, "YAPPOD_V2_WRITE_TOKEN must contain 16 to 255 bytes");
      return YAP_V2_INVALID_FORMAT;
    }
    for (i = 0U; i < length; i++)
      if ((unsigned char)token[i] <= 0x20U || (unsigned char)token[i] == 0x7fU) {
        set_error(error, error_size, "YAPPOD_V2_WRITE_TOKEN contains whitespace or control bytes");
        return YAP_V2_INVALID_FORMAT;
      }
    memcpy(policy->write_token, token, length + 1U); policy->write_token_bytes = length;
  }
  return YAP_V2_OK;
}

int YAP_V2_runtime_limiter_init(YAP_V2_RUNTIME_LIMITER *limiter,
                                const YAP_V2_RUNTIME_POLICY *policy) {
  if (limiter == NULL || policy == NULL || policy->max_inflight == 0U ||
      policy->max_inflight_bytes == 0U || limiter->initialized) return YAP_V2_INVALID_ARGUMENT;
  memset(limiter, 0, sizeof(*limiter));
  if (pthread_mutex_init(&limiter->lock, NULL) != 0) return YAP_V2_IO_ERROR;
  limiter->max_inflight = policy->max_inflight;
  limiter->max_inflight_bytes = policy->max_inflight_bytes; limiter->initialized = 1;
  return YAP_V2_OK;
}

void YAP_V2_runtime_limiter_close(YAP_V2_RUNTIME_LIMITER *limiter) {
  if (limiter == NULL || !limiter->initialized) return;
  (void)pthread_mutex_destroy(&limiter->lock); memset(limiter, 0, sizeof(*limiter));
}

int YAP_V2_runtime_limiter_acquire(YAP_V2_RUNTIME_LIMITER *limiter, size_t request_bytes) {
  int status = YAP_V2_OK;
  if (limiter == NULL || !limiter->initialized || request_bytes == 0U)
    return YAP_V2_INVALID_ARGUMENT;
  pthread_mutex_lock(&limiter->lock);
  if (limiter->inflight >= limiter->max_inflight ||
      request_bytes > limiter->max_inflight_bytes - limiter->inflight_bytes)
    status = YAP_V2_OUT_OF_RANGE;
  else { limiter->inflight++; limiter->inflight_bytes += request_bytes; }
  pthread_mutex_unlock(&limiter->lock); return status;
}

void YAP_V2_runtime_limiter_release(YAP_V2_RUNTIME_LIMITER *limiter, size_t request_bytes) {
  if (limiter == NULL || !limiter->initialized) return;
  pthread_mutex_lock(&limiter->lock);
  if (limiter->inflight > 0U && request_bytes <= limiter->inflight_bytes) {
    limiter->inflight--; limiter->inflight_bytes -= request_bytes;
  }
  pthread_mutex_unlock(&limiter->lock);
}

static int constant_time_equal(const char *left, size_t left_bytes,
                               const char *right, size_t right_bytes) {
  size_t i, maximum = left_bytes > right_bytes ? left_bytes : right_bytes;
  unsigned int difference = (unsigned int)(left_bytes ^ right_bytes);
  for (i = 0U; i < maximum; i++) {
    unsigned char a = i < left_bytes ? (unsigned char)left[i] : 0U;
    unsigned char b = i < right_bytes ? (unsigned char)right[i] : 0U;
    difference |= (unsigned int)(a ^ b);
  }
  return difference == 0U;
}

int YAP_V2_authorize_write(const YAP_V2_RUNTIME_POLICY *policy, const char *authorization) {
  const char *provided; size_t provided_bytes;
  if (policy == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (policy->write_token_bytes == 0U) return YAP_V2_OK;
  if (authorization == NULL || strncmp(authorization, "Bearer ", 7U) != 0)
    return YAP_V2_CONFLICT;
  provided = authorization + 7U; provided_bytes = strlen(provided);
  return constant_time_equal(provided, provided_bytes, policy->write_token,
                             policy->write_token_bytes) ? YAP_V2_OK : YAP_V2_CONFLICT;
}

int YAP_V2_ingest_envelope_wrap(const YAP_V2_RUNTIME_POLICY *policy,
                                const unsigned char *json, size_t json_bytes,
                                unsigned char **payload, size_t *payload_bytes) {
  unsigned char *result; size_t total;
  if (policy == NULL || json == NULL || json_bytes == 0U || payload == NULL || payload_bytes == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (policy->write_token_bytes == 0U) { *payload = NULL; *payload_bytes = json_bytes; return YAP_V2_OK; }
  if (json_bytes > SIZE_MAX - policy->write_token_bytes - 6U) return YAP_V2_OUT_OF_RANGE;
  total = json_bytes + policy->write_token_bytes + 6U; result = malloc(total);
  if (result == NULL) return YAP_V2_ALLOCATION_FAILED;
  memcpy(result, "YTK1", 4U); result[4] = (unsigned char)(policy->write_token_bytes >> 8);
  result[5] = (unsigned char)policy->write_token_bytes;
  memcpy(result + 6U, policy->write_token, policy->write_token_bytes);
  memcpy(result + 6U + policy->write_token_bytes, json, json_bytes);
  *payload = result; *payload_bytes = total; return YAP_V2_OK;
}

int YAP_V2_ingest_envelope_unwrap(const YAP_V2_RUNTIME_POLICY *policy,
                                  const unsigned char *payload, size_t payload_bytes,
                                  const unsigned char **json, size_t *json_bytes) {
  size_t token_bytes;
  if (policy == NULL || payload == NULL || payload_bytes == 0U || json == NULL || json_bytes == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (policy->write_token_bytes == 0U) { *json = payload; *json_bytes = payload_bytes; return YAP_V2_OK; }
  if (payload_bytes < 7U || memcmp(payload, "YTK1", 4U) != 0) return YAP_V2_CONFLICT;
  token_bytes = ((size_t)payload[4] << 8) | payload[5];
  if (token_bytes == 0U || token_bytes > payload_bytes - 6U ||
      !constant_time_equal((const char *)payload + 6U, token_bytes, policy->write_token,
                           policy->write_token_bytes) || payload_bytes == token_bytes + 6U)
    return YAP_V2_CONFLICT;
  *json = payload + 6U + token_bytes; *json_bytes = payload_bytes - 6U - token_bytes;
  return YAP_V2_OK;
}

int YAP_V2_socket_set_deadline(int fd, uint32_t timeout_ms) {
  struct timeval timeout;
  if (fd < 0 || timeout_ms == 0U || timeout_ms > YAP_V2_MAX_TIMEOUT_MS)
    return YAP_V2_INVALID_ARGUMENT;
  timeout.tv_sec = (time_t)(timeout_ms / 1000U);
  timeout.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 ?
         YAP_V2_OK : YAP_V2_IO_ERROR;
}
