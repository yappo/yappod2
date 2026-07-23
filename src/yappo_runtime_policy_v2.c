#include "yappo_runtime_policy_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <toml.h>

#define YAP_V2_DEFAULT_MAX_INFLIGHT 4U
#define YAP_V2_DEFAULT_MAX_INFLIGHT_BYTES (4U * 1024U * 1024U)
#define YAP_V2_DEFAULT_TIMEOUT_MS 5000U
#define YAP_V2_MAX_TIMEOUT_MS 60000U

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

void YAP_V2_runtime_policy_init(YAP_V2_RUNTIME_POLICY *policy) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy)); policy->max_inflight = YAP_V2_DEFAULT_MAX_INFLIGHT;
  policy->max_inflight_bytes = YAP_V2_DEFAULT_MAX_INFLIGHT_BYTES;
  policy->request_timeout_ms = YAP_V2_DEFAULT_TIMEOUT_MS;
}

static int read_size(toml_table_t *table, const char *key, size_t maximum, size_t *output,
                     char *error, size_t error_size) {
  toml_datum_t value = toml_int_in(table, key);
  if (!value.ok) {
    if (toml_key_exists(table, key)) {
      set_error(error, error_size, "daemon runtime limit must be an integer");
      return YAP_V2_INVALID_FORMAT;
    }
    return YAP_V2_OK;
  }
  if (value.u.i <= 0 || (uint64_t)value.u.i > maximum || (uint64_t)value.u.i > SIZE_MAX) {
    set_error(error, error_size, "daemon runtime limit is out of range");
    return YAP_V2_INVALID_FORMAT;
  }
  *output = (size_t)value.u.i;
  return YAP_V2_OK;
}

int YAP_V2_runtime_policy_load_config(YAP_V2_RUNTIME_POLICY *policy, const char *config_path,
                                      char *error, size_t error_size) {
  FILE *file;
  toml_table_t *root = NULL, *daemon;
  toml_datum_t token;
  char parse_error[256] = {0};
  size_t timeout;
  int status;
  if (policy == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_runtime_policy_init(policy);
  if (config_path == NULL) return YAP_V2_OK;
  file = fopen(config_path, "r");
  if (file == NULL) {
    set_error(error, error_size, "cannot open daemon config");
    return YAP_V2_IO_ERROR;
  }
  root = toml_parse_file(file, parse_error, (int)sizeof(parse_error));
  (void)fclose(file);
  if (root == NULL) {
    set_error(error, error_size, parse_error[0] != '\0' ? parse_error : "invalid daemon config");
    return YAP_V2_INVALID_FORMAT;
  }
  daemon = toml_table_in(root, "daemon");
  if (daemon == NULL) { toml_free(root); return YAP_V2_OK; }
  status = read_size(daemon, "max_inflight", 1024U, &policy->max_inflight,
                     error, error_size);
  if (status == YAP_V2_OK)
    status = read_size(daemon, "max_inflight_bytes", 1024U * 1024U * 1024U,
                       &policy->max_inflight_bytes, error, error_size);
  timeout = policy->request_timeout_ms;
  if (status == YAP_V2_OK)
    status = read_size(daemon, "request_timeout_ms", YAP_V2_MAX_TIMEOUT_MS, &timeout,
                       error, error_size);
  if (status != YAP_V2_OK) { toml_free(root); return status; }
  policy->request_timeout_ms = (uint32_t)timeout;
  token = toml_string_in(daemon, "write_token");
  if (!token.ok && toml_key_exists(daemon, "write_token")) {
    set_error(error, error_size, "daemon.write_token must be a string");
    toml_free(root);
    return YAP_V2_INVALID_FORMAT;
  }
  if (token.ok) {
    size_t i, length = strlen(token.u.s);
    if (length < 16U || length > YAP_V2_WRITE_TOKEN_MAX_BYTES) {
      set_error(error, error_size, "daemon.write_token must contain 16 to 255 bytes");
      free(token.u.s); toml_free(root);
      return YAP_V2_INVALID_FORMAT;
    }
    for (i = 0U; i < length; i++)
      if ((unsigned char)token.u.s[i] <= 0x20U || (unsigned char)token.u.s[i] == 0x7fU) {
        set_error(error, error_size, "daemon.write_token contains whitespace or control bytes");
        free(token.u.s); toml_free(root);
        return YAP_V2_INVALID_FORMAT;
      }
    memcpy(policy->write_token, token.u.s, length + 1U); policy->write_token_bytes = length;
    free(token.u.s);
  }
  toml_free(root);
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

int YAP_V2_runtime_limiter_snapshot(YAP_V2_RUNTIME_LIMITER *limiter, size_t *inflight,
                                    size_t *inflight_bytes, size_t *max_inflight,
                                    size_t *max_inflight_bytes) {
  if (limiter == NULL || !limiter->initialized || inflight == NULL || inflight_bytes == NULL ||
      max_inflight == NULL || max_inflight_bytes == NULL) return YAP_V2_INVALID_ARGUMENT;
  pthread_mutex_lock(&limiter->lock);
  *inflight = limiter->inflight; *inflight_bytes = limiter->inflight_bytes;
  *max_inflight = limiter->max_inflight; *max_inflight_bytes = limiter->max_inflight_bytes;
  pthread_mutex_unlock(&limiter->lock); return YAP_V2_OK;
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
