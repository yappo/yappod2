#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_config_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_observability_v2.h"

static void write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb"); assert_non_null(file);
  assert_true(fputs(text, file) >= 0); assert_int_equal(fclose(file), 0);
}

static void make_empty_index(ytest_env_t *env) {
  char path[PATH_MAX]; YAP_V2_CONFIG config; YAP_V2_MANIFEST manifest;
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "config.toml"), 0);
  write_text(path,
    "format_version=2\n[tokenizer]\nid=\"unicode_nfkc_cf_v1\"\n"
    "[chunking]\nmax_chars=100\noverlap_chars=0\n"
    "[vector]\nenabled=true\nmodel_id=\"embed-v1\"\ndimensions=2\nmetric=\"cosine\"\n"
    "[metadata]\nfilterable_fields=[]\n");
  assert_int_equal(YAP_V2_config_load(path, &config, NULL, 0U), YAP_V2_OK);
  YAP_V2_manifest_init(&manifest); manifest.generation = 7U;
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
}

static void test_probe_json_and_compaction_status(void **state) {
  ytest_env_t env; YAP_V2_OPERATIONAL_STATE operational; char *json = NULL; size_t json_bytes = 0U;
  char path[PATH_MAX], error[256] = {0}; (void)state;
  assert_int_equal(ytest_env_init(&env), 0); make_empty_index(&env);
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, error, sizeof(error)), YAP_V2_OK);
  assert_true(operational.ready); assert_int_equal(operational.generation, 7U);
  assert_true(operational.embedding_configured); assert_string_equal(operational.embedding_model_id, "embed-v1");
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_IDLE);
  assert_int_equal(YAP_V2_compaction_status_write(env.tmp_root, YAP_V2_COMPACTION_RUNNING, 7U), YAP_V2_OK);
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_RUNNING);
  assert_int_equal(YAP_V2_compaction_status_write(env.tmp_root, YAP_V2_COMPACTION_SUCCEEDED, 8U), YAP_V2_OK);
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_SUCCEEDED);
  assert_int_equal(operational.compaction_generation, 8U);
  assert_int_equal(YAP_V2_operational_state_json(&operational, "test-service", &json, &json_bytes), YAP_V2_OK);
  assert_non_null(strstr(json, "\"generation\":7")); assert_non_null(strstr(json, "\"precomputed_ready\""));
  assert_non_null(strstr(json, "\"succeeded\"")); assert_int_equal(strlen(json), json_bytes); free(json);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "compaction.state"), 0);
  write_text(path, "invalid\n");
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_UNKNOWN);
  ytest_env_destroy(&env);
}

typedef struct { YAP_V2_METRICS *metrics; size_t count; } record_context;
static void *record_requests(void *argument) {
  record_context *context = argument; size_t i;
  for (i = 0U; i < context->count; i++)
    YAP_V2_metrics_record(context->metrics, YAP_V2_OBSERVE_SEARCH, 200, 6000U);
  return NULL;
}

static void test_metrics_are_thread_safe_and_bounded(void **state) {
  YAP_V2_METRICS metrics; YAP_V2_OPERATIONAL_STATE operational; pthread_t threads[4];
  record_context context; char *output = NULL; size_t output_bytes = 0U, i; (void)state;
  assert_int_equal(YAP_V2_metrics_init(&metrics), YAP_V2_OK);
  context.metrics = &metrics; context.count = 250U;
  for (i = 0U; i < 4U; i++) assert_int_equal(pthread_create(&threads[i], NULL, record_requests, &context), 0);
  for (i = 0U; i < 4U; i++) assert_int_equal(pthread_join(threads[i], NULL), 0);
  YAP_V2_metrics_record(&metrics, YAP_V2_OBSERVE_SEARCH, 503, 2000000U);
  YAP_V2_metrics_record(&metrics, YAP_V2_OBSERVE_INGEST, 400, 100U);
  YAP_V2_operational_state_init(&operational); operational.ready = 1; operational.generation = 9U;
  operational.embedding_configured = 1; operational.compaction_state = YAP_V2_COMPACTION_RUNNING;
  assert_int_equal(YAP_V2_metrics_render(&metrics, &operational, 2U, 100U, 4U, 4096U,
                                         &output, &output_bytes), YAP_V2_OK);
  assert_non_null(strstr(output, "yappod_v2_requests_total{operation=\"search\",status_class=\"2xx\"} 1000"));
  assert_non_null(strstr(output, "yappod_v2_requests_total{operation=\"search\",status_class=\"5xx\"} 1"));
  assert_non_null(strstr(output, "yappod_v2_requests_total{operation=\"ingest\",status_class=\"4xx\"} 1"));
  assert_non_null(strstr(output, "operation=\"search\",le=\"0.010\"} 1000"));
  assert_non_null(strstr(output, "operation=\"search\",le=\"+Inf\"} 1001"));
  assert_non_null(strstr(output, "yappod_v2_manifest_generation 9"));
  assert_non_null(strstr(output, "yappod_v2_compaction_state{state=\"running\"} 1"));
  assert_int_equal(strlen(output), output_bytes); free(output); YAP_V2_metrics_close(&metrics);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_probe_json_and_compaction_status),
    cmocka_unit_test(test_metrics_are_thread_safe_and_bounded)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
