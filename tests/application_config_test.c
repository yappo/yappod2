#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "yappo_application_config.h"
#include "yappo_config_v2.h"

static char *write_config(const char *source) {
  char path[] = "/tmp/yappod-application-config-XXXXXX";
  int fd = mkstemp(path);
  FILE *file;
  char *result;
  assert_true(fd >= 0);
  file = fdopen(fd, "w"); assert_non_null(file);
  assert_true(fputs(source, file) >= 0); assert_int_equal(fclose(file), 0);
  result = strdup(path); assert_non_null(result);
  return result;
}

static const char valid[] =
  "schema_version=1\nformat_version=2\n"
  "[index]\ndirectory='./data/../index'\n"
  "[tokenizer]\nid='unicode_nfkc_casefold_v2'\n"
  "[chunking]\nmax_chars=1200\noverlap_chars=200\n"
  "[vector]\nenabled=false\n"
  "[metadata]\nfilterable_fields=['language','source']\n"
  "[daemon]\nrun_directory='./run'\ncore_host='127.0.0.1'\ncore_port=18401\n"
  "front_host='127.0.0.1'\nfront_port=18400\nmax_inflight=8\n"
  "max_inflight_bytes=8192\nrequest_timeout_ms=2500\n"
  "[web]\nhost='127.0.0.1'\n"
  "[llm]\nauthorization_token_env='LLM_API_KEY'\n";

static void test_loads_shared_config_and_resolves_paths(void **state) {
  char *path = write_config(valid);
  char error[256] = {0};
  YAP_APPLICATION_CONFIG config;
  (void)state;
  assert_int_equal(YAP_application_config_load(path, &config, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(config.index_directory[0], '/');
  assert_non_null(strstr(config.index_directory, "/index"));
  assert_null(strstr(config.index_directory, "/../"));
  assert_non_null(strstr(config.run_directory, "/run"));
  assert_int_equal(config.runtime_policy.max_inflight, 8U);
  assert_int_equal(config.runtime_policy.request_timeout_ms, 2500U);
  assert_int_equal(unlink(path), 0); free(path);
}

static void test_rejects_missing_required_table_and_unknown_key(void **state) {
  char *missing = write_config("schema_version=1\nformat_version=2\n[index]\ndirectory='./x'\n");
  char *unknown = write_config(
    "schema_version=1\nformat_version=2\n[index]\ndirectory='./x'\nsurprise=true\n"
    "[tokenizer]\n[chunking]\n[vector]\nenabled=false\n[daemon]\n"
    "run_directory='./run'\ncore_host='127.0.0.1'\ncore_port=1\n"
    "front_host='127.0.0.1'\nfront_port=2\n");
  YAP_APPLICATION_CONFIG config; char error[256] = {0};
  (void)state;
  assert_int_equal(YAP_application_config_load(missing, &config, error, sizeof(error)),
                   YAP_V2_INVALID_FORMAT);
  assert_int_equal(YAP_application_config_load(unknown, &config, error, sizeof(error)),
                   YAP_V2_INVALID_FORMAT);
  assert_non_null(strstr(error, "unknown"));
  unlink(missing); unlink(unknown); free(missing); free(unknown);
}

static void test_daemon_fields_do_not_change_index_fingerprint(void **state) {
  char *first = write_config(valid);
  char changed[4096];
  char *second;
  YAP_APPLICATION_CONFIG a, b;
  unsigned char left[32], right[32];
  (void)state;
  assert_true(snprintf(changed, sizeof(changed), "%s", valid) > 0);
  {
    char *port = strstr(changed, "core_port=18401");
    assert_non_null(port); memcpy(port, "core_port=18402", strlen("core_port=18402"));
  }
  second = write_config(changed);
  assert_int_equal(YAP_application_config_load(first, &a, NULL, 0U), YAP_V2_OK);
  assert_int_equal(YAP_application_config_load(second, &b, NULL, 0U), YAP_V2_OK);
  assert_int_equal(YAP_V2_config_fingerprint(&a.index_config, left), YAP_V2_OK);
  assert_int_equal(YAP_V2_config_fingerprint(&b.index_config, right), YAP_V2_OK);
  assert_memory_equal(left, right, sizeof(left));
  unlink(first); unlink(second); free(first); free(second);
}

static void test_rejects_invalid_types_and_ranges(void **state) {
  char source[4096];
  char *invalid;
  YAP_APPLICATION_CONFIG config;
  (void)state;
  assert_true(snprintf(source, sizeof(source), "%s", valid) > 0);
  {
    char *port = strstr(source, "core_port=18401");
    assert_non_null(port); memcpy(port, "core_port=70000", strlen("core_port=70000"));
  }
  invalid = write_config(source);
  assert_int_equal(YAP_application_config_load(invalid, &config, NULL, 0U), YAP_V2_OUT_OF_RANGE);
  unlink(invalid); free(invalid);
  invalid = write_config(
    "schema_version='one'\nformat_version=2\nindex.directory='./x'\n"
    "[tokenizer]\n[chunking]\n[vector]\nenabled=false\n[daemon]\n"
    "run_directory='./run'\ncore_host='127.0.0.1'\ncore_port=1\n"
    "front_host='127.0.0.1'\nfront_port=2\n");
  assert_int_equal(YAP_application_config_load(invalid, &config, NULL, 0U),
                   YAP_V2_INVALID_FORMAT);
  unlink(invalid); free(invalid);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_loads_shared_config_and_resolves_paths),
    cmocka_unit_test(test_rejects_missing_required_table_and_unknown_key),
    cmocka_unit_test(test_daemon_fields_do_not_change_index_fingerprint),
    cmocka_unit_test(test_rejects_invalid_types_and_ranges),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
