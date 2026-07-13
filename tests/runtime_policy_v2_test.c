#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cmocka.h>

#include "yappo_runtime_policy_v2.h"

static void clear_environment(void) {
  (void)unsetenv("YAPPOD_V2_MAX_INFLIGHT");
  (void)unsetenv("YAPPOD_V2_MAX_INFLIGHT_BYTES");
  (void)unsetenv("YAPPOD_V2_REQUEST_TIMEOUT_MS");
  (void)unsetenv("YAPPOD_V2_WRITE_TOKEN");
}

static void test_policy_defaults_and_strict_environment(void **state) {
  YAP_V2_RUNTIME_POLICY policy; char error[256] = {0};
  (void)state; clear_environment();
  assert_int_equal(YAP_V2_runtime_policy_load_env(&policy, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(policy.max_inflight, 4U); assert_int_equal(policy.max_inflight_bytes, 4U * 1024U * 1024U);
  assert_int_equal(policy.request_timeout_ms, 5000U); assert_int_equal(policy.write_token_bytes, 0U);
  assert_int_equal(setenv("YAPPOD_V2_MAX_INFLIGHT", "0", 1), 0);
  assert_int_equal(YAP_V2_runtime_policy_load_env(&policy, error, sizeof(error)), YAP_V2_INVALID_FORMAT);
  clear_environment(); assert_int_equal(setenv("YAPPOD_V2_WRITE_TOKEN", "short", 1), 0);
  assert_int_equal(YAP_V2_runtime_policy_load_env(&policy, error, sizeof(error)), YAP_V2_INVALID_FORMAT);
  clear_environment();
}

static void test_limiter_fails_closed_on_count_and_bytes(void **state) {
  YAP_V2_RUNTIME_POLICY policy; YAP_V2_RUNTIME_LIMITER limiter = {0};
  (void)state; YAP_V2_runtime_policy_init(&policy); policy.max_inflight = 2U; policy.max_inflight_bytes = 10U;
  assert_int_equal(YAP_V2_runtime_limiter_init(&limiter, &policy), YAP_V2_OK);
  assert_int_equal(YAP_V2_runtime_limiter_acquire(&limiter, 6U), YAP_V2_OK);
  assert_int_equal(YAP_V2_runtime_limiter_acquire(&limiter, 5U), YAP_V2_OUT_OF_RANGE);
  assert_int_equal(YAP_V2_runtime_limiter_acquire(&limiter, 4U), YAP_V2_OK);
  assert_int_equal(YAP_V2_runtime_limiter_acquire(&limiter, 1U), YAP_V2_OUT_OF_RANGE);
  YAP_V2_runtime_limiter_release(&limiter, 6U);
  assert_int_equal(YAP_V2_runtime_limiter_acquire(&limiter, 1U), YAP_V2_OK);
  YAP_V2_runtime_limiter_release(&limiter, 4U); YAP_V2_runtime_limiter_release(&limiter, 1U);
  YAP_V2_runtime_limiter_close(&limiter);
}

static void test_write_token_and_core_envelope(void **state) {
  static const unsigned char json[] = "{\"operations\":[]}";
  YAP_V2_RUNTIME_POLICY policy; unsigned char *payload = NULL; size_t payload_bytes = 0U;
  const unsigned char *decoded = NULL; size_t decoded_bytes = 0U; char authorization[300];
  (void)state; clear_environment();
  assert_int_equal(setenv("YAPPOD_V2_WRITE_TOKEN", "0123456789abcdef-secure", 1), 0);
  assert_int_equal(YAP_V2_runtime_policy_load_env(&policy, NULL, 0U), YAP_V2_OK);
  assert_int_equal(YAP_V2_authorize_write(&policy, NULL), YAP_V2_CONFLICT);
  assert_int_equal(YAP_V2_authorize_write(&policy, "Bearer wrong"), YAP_V2_CONFLICT);
  assert_true(snprintf(authorization, sizeof(authorization), "Bearer %s", policy.write_token) > 0);
  assert_int_equal(YAP_V2_authorize_write(&policy, authorization), YAP_V2_OK);
  assert_int_equal(YAP_V2_ingest_envelope_wrap(&policy, json, sizeof(json) - 1U,
                                               &payload, &payload_bytes), YAP_V2_OK);
  assert_non_null(payload); assert_true(payload_bytes > sizeof(json));
  assert_int_equal(YAP_V2_ingest_envelope_unwrap(&policy, payload, payload_bytes,
                                                 &decoded, &decoded_bytes), YAP_V2_OK);
  assert_int_equal(decoded_bytes, sizeof(json) - 1U); assert_memory_equal(decoded, json, decoded_bytes);
  payload[6] ^= 1U;
  assert_int_equal(YAP_V2_ingest_envelope_unwrap(&policy, payload, payload_bytes,
                                                 &decoded, &decoded_bytes), YAP_V2_CONFLICT);
  free(payload); clear_environment();
}

static void test_socket_deadline_is_applied(void **state) {
  int sockets[2]; struct timeval receive_timeout; socklen_t size = sizeof(receive_timeout);
  (void)state; assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  assert_int_equal(YAP_V2_socket_set_deadline(sockets[0], 1250U), YAP_V2_OK);
  assert_int_equal(getsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, &size), 0);
  assert_true(receive_timeout.tv_sec >= 1); close(sockets[0]); close(sockets[1]);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_policy_defaults_and_strict_environment),
    cmocka_unit_test(test_limiter_fails_closed_on_count_and_bytes),
    cmocka_unit_test(test_write_token_and_core_envelope),
    cmocka_unit_test(test_socket_deadline_is_applied)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
