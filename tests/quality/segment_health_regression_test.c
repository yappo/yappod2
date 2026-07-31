#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "test_env.h"
#include "indexing/yappo_compact_v2.h"
#include "server/yappo_http_v2.h"
#include "server/yappo_observability_v2.h"
#include "v2_quality_fixture.h"

#define UPDATE_GENERATIONS 40U
#define MAX_MAINTAINED_SEGMENTS 3U

static void apply_update(ytest_env_t *env, size_t ordinal) {
  char request[512];
  char *response = NULL;
  size_t response_bytes = 0U;
  int http_status = 0;
  assert_true(snprintf(
    request, sizeof(request),
    "{\"operations\":[{\"operation\":\"upsert\","
    "\"id\":\"bounded-%zu\",\"body\":\"bounded generation %zu\","
    "\"metadata\":{\"topic\":\"bounded\"},"
    "\"vectors\":[[1,0,0]]}]}", ordinal, ordinal) > 0);
  assert_int_equal(YAP_V2_http_execute(
    env->tmp_root, YAP_V2_HTTP_INGEST,
    (const unsigned char *)request, strlen(request), &http_status,
    &response, &response_bytes), YAP_V2_OK);
  assert_int_equal(http_status, 200);
  assert_non_null(response);
  free(response);
}

static void maintain_index(ytest_env_t *env,
                           const YAP_V2_COMPACTION_POLICY *policy) {
  YAP_V2_COMPACTION_RESULT result;
  size_t small_segment_run = 0U;
  int compacted = 0;
  char error[256] = {0};
  YAP_V2_compaction_result_init(&result);
  assert_int_equal(YAP_V2_compact_if_needed(
    env->tmp_root, policy, &result, &compacted, &small_segment_run,
    error, sizeof(error)), YAP_V2_OK);
  YAP_V2_compaction_result_free(&result);
}

static void assert_searches_latest_generation(ytest_env_t *env) {
  static const char request[] =
    "{\"query\":\"bounded\",\"mode\":\"lexical\","
    "\"scope\":\"documents\",\"limit\":100}";
  char *response = NULL;
  size_t response_bytes = 0U;
  int http_status = 0;
  yyjson_doc *document;
  yyjson_val *results;
  assert_int_equal(YAP_V2_http_execute(
    env->tmp_root, YAP_V2_HTTP_SEARCH,
    (const unsigned char *)request, sizeof(request) - 1U, &http_status,
    &response, &response_bytes), YAP_V2_OK);
  assert_int_equal(http_status, 200);
  document = yyjson_read(response, response_bytes, 0U);
  assert_non_null(document);
  results = yyjson_obj_get(yyjson_doc_get_root(document), "results");
  assert_int_equal(yyjson_arr_size(results), UPDATE_GENERATIONS);
  yyjson_doc_free(document);
  free(response);
}

static void test_many_updates_keep_structural_work_bounded(void **state) {
  ytest_env_t env;
  YAP_V2_COMPACTION_POLICY policy;
  YAP_V2_OPERATIONAL_STATE operational;
  size_t i;
  char error[256] = {0};
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create(env.tmp_root), 0);
  YAP_V2_compaction_policy_init(&policy);
  for (i = 0U; i < UPDATE_GENERATIONS; i++) {
    apply_update(&env, i);
    maintain_index(&env, &policy);
    assert_int_equal(YAP_V2_operational_probe_index_with_policy(
      env.tmp_root, &policy, &operational, error, sizeof(error)),
      YAP_V2_OK);
    assert_true(operational.segment_count <= MAX_MAINTAINED_SEGMENTS);
    assert_true(operational.small_segment_run <
                policy.min_small_segments);
    assert_false(operational.auto_compaction_needed);
  }
  assert_int_equal(operational.document_records, 9U + UPDATE_GENERATIONS);
  assert_int_equal(operational.passage_records, 9U + UPDATE_GENERATIONS);
  assert_int_equal(operational.tombstone_records, 0U);
  assert_true(operational.component_file_bytes > 0U);
  assert_true(operational.smallest_segment_bytes > 0U);
  assert_true(operational.largest_segment_bytes >=
              operational.smallest_segment_bytes);
  assert_searches_latest_generation(&env);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_many_updates_keep_structural_work_bounded),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
