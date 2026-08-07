#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "config/yappo_application_config.h"
#include "config/yappo_config_v2.h"

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
  "format_version=2\n"
  "[index]\ndirectory='./data/../index'\n"
  "[tokenizer]\nid='unicode_nfkc_casefold_v2'\n"
  "[chunking]\nmax_chars=1200\noverlap_chars=200\n"
  "[vector]\nenabled=false\n"
  "[metadata]\nfilterable_fields=['language','source']\n"
  "[daemon]\nrun_directory='./run'\ncore_host='127.0.0.1'\ncore_port=18401\n"
  "front_host='127.0.0.1'\nfront_port=18400\nmax_inflight=8\n"
  "front_io_threads=4\ncore_io_threads=5\ncore_search_threads=6\n"
  "core_writer_queue_capacity=7\n"
  "max_inflight_bytes=8192\nrequest_timeout_ms=2500\n"
  "ingest_max_body_bytes=33554432\ningest_timeout_ms=120000\n"
  "auto_compact_enabled=false\nauto_compact_check_interval_ms=5000\n"
  "auto_compact_small_segment_bytes=1048576\n"
  "auto_compact_min_small_segments=3\n"
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
  assert_int_equal(config.front_io_threads, 4U);
  assert_int_equal(config.core_io_threads, 5U);
  assert_int_equal(config.core_search_threads, 6U);
  assert_int_equal(config.core_writer_queue_capacity, 7U);
  assert_int_equal(config.runtime_policy.max_inflight, 8U);
  assert_int_equal(config.runtime_policy.request_timeout_ms, 2500U);
  assert_int_equal(config.runtime_policy.ingest_max_body_bytes, 33554432U);
  assert_int_equal(config.runtime_policy.ingest_timeout_ms, 120000U);
  assert_false(config.compaction_policy.enabled);
  assert_int_equal(config.compaction_policy.check_interval_ms, 5000U);
  assert_int_equal(config.compaction_policy.small_segment_bytes, 1048576U);
  assert_int_equal(config.compaction_policy.min_small_segments, 3U);
  assert_int_equal(unlink(path), 0); free(path);
}

static void test_rejects_missing_required_table_and_unknown_key(void **state) {
  char *missing = write_config("format_version=2\n[index]\ndirectory='./x'\n");
  char *unknown = write_config(
    "format_version=2\n[index]\ndirectory='./x'\nsurprise=true\n"
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

static void test_rejects_invalid_ranges_and_removed_schema_version(void **state) {
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
    "schema_version=1\nformat_version=2\nindex.directory='./x'\n"
    "[tokenizer]\n[chunking]\n[vector]\nenabled=false\n[daemon]\n"
    "run_directory='./run'\ncore_host='127.0.0.1'\ncore_port=1\n"
    "front_host='127.0.0.1'\nfront_port=2\n");
  assert_int_equal(YAP_application_config_load(invalid, &config, NULL, 0U),
                   YAP_V2_INVALID_FORMAT);
  unlink(invalid); free(invalid);
}

static void test_execution_threads_defaults_and_ranges(void **state) {
  char source[4096];
  char *path;
  YAP_APPLICATION_CONFIG config;
  (void)state;
  YAP_application_config_init(&config);
  assert_int_equal(config.front_io_threads, YAP_APPLICATION_DEFAULT_IO_THREADS);
  assert_int_equal(config.core_io_threads, YAP_APPLICATION_DEFAULT_IO_THREADS);
  assert_int_equal(config.core_search_threads, YAP_APPLICATION_DEFAULT_SEARCH_THREADS);
  assert_int_equal(config.core_writer_queue_capacity, 1U);

  assert_true(snprintf(source, sizeof(source), "%s", valid) > 0);
  {
    char *value = strstr(source, "core_search_threads=6");
    assert_non_null(value);
    value[strlen("core_search_threads=")] = '0';
  }
  path = write_config(source);
  assert_int_equal(YAP_application_config_load(path, &config, NULL, 0U), YAP_V2_OUT_OF_RANGE);
  unlink(path); free(path);

  assert_true(snprintf(source, sizeof(source), "%s", valid) > 0);
  {
    char *value = strstr(source, "core_search_threads=6");
    const char *replacement = "core_search_threads=1025";
    size_t old_bytes = strlen("core_search_threads=6");
    size_t new_bytes = strlen(replacement);
    assert_non_null(value);
    memmove(value + new_bytes, value + old_bytes, strlen(value + old_bytes) + 1U);
    memcpy(value, replacement, new_bytes);
  }
  path = write_config(source);
  assert_int_equal(YAP_application_config_load(path, &config, NULL, 0U), YAP_V2_OUT_OF_RANGE);
  unlink(path); free(path);

  path = write_config(
    "format_version=2\n[index]\ndirectory='./x'\n[tokenizer]\n[chunking]\n"
    "[vector]\nenabled=false\n[daemon]\nrun_directory='./run'\n"
    "core_host='127.0.0.1'\ncore_port=1\nfront_host='127.0.0.1'\nfront_port=2\n"
    "worker_threads=16\n");
  assert_int_equal(YAP_application_config_load(path, &config, NULL, 0U),
                   YAP_V2_INVALID_FORMAT);
  unlink(path); free(path);
}

static void test_automatic_compaction_defaults_and_ranges(void **state) {
  char source[4096];
  char *path;
  YAP_APPLICATION_CONFIG config;
  (void)state;
  YAP_application_config_init(&config);
  assert_true(config.compaction_policy.enabled);
  assert_int_equal(config.compaction_policy.check_interval_ms, 30000U);
  assert_int_equal(config.compaction_policy.small_segment_bytes, 67108864U);
  assert_int_equal(config.compaction_policy.min_small_segments, 4U);

  assert_true(snprintf(source, sizeof(source), "%s", valid) > 0);
  {
    char *value = strstr(source, "auto_compact_min_small_segments=3");
    assert_non_null(value);
    value[strlen("auto_compact_min_small_segments=")] = '1';
  }
  path = write_config(source);
  assert_int_equal(YAP_application_config_load(path, &config, NULL, 0U),
                   YAP_V2_OUT_OF_RANGE);
  unlink(path); free(path);

  assert_true(snprintf(source, sizeof(source), "%s", valid) > 0);
  {
    char *value = strstr(source, "auto_compact_enabled=false");
    assert_non_null(value);
    memcpy(value, "auto_compact_enabled=3    ",
           strlen("auto_compact_enabled=false"));
  }
  path = write_config(source);
  assert_int_equal(YAP_application_config_load(path, &config, NULL, 0U),
                   YAP_V2_INVALID_FORMAT);
  unlink(path); free(path);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_loads_shared_config_and_resolves_paths),
    cmocka_unit_test(test_rejects_missing_required_table_and_unknown_key),
    cmocka_unit_test(test_daemon_fields_do_not_change_index_fingerprint),
    cmocka_unit_test(test_rejects_invalid_ranges_and_removed_schema_version),
    cmocka_unit_test(test_execution_threads_defaults_and_ranges),
    cmocka_unit_test(test_automatic_compaction_defaults_and_ranges),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
