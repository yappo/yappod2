#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "search_quality_metrics.h"
#include "test_env.h"
#include "test_fs.h"
#include "v2_quality_fixture.h"
#include "indexing/yappo_compact_v2.h"
#include "server/yappo_http_v2.h"

static const char *const document_ids[9] = {
  "doc-red-0", "doc-red-1", "doc-red-2", "doc-green-0", "doc-green-1",
  "doc-green-2", "doc-blue-0", "doc-blue-1", "doc-blue-2"
};
static const char *const topics[3] = {"red", "green", "blue"};

typedef struct {
  char ids[9][32];
  double lexical_scores[9];
  size_t count;
} CAPTURED_RESULTS;

static void capture_results(const char *index_dir, const char *mode, size_t topic,
                            CAPTURED_RESULTS *captured) {
  static const char *const vectors[3] = {"[1,0,0]", "[0,1,0]", "[0,0,1]"};
  char request[256], *response = NULL;
  size_t response_bytes = 0U, i;
  int http_status = 0;
  yyjson_doc *document;
  yyjson_val *results;
  memset(captured, 0, sizeof(*captured));
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"%s\",\"vector\":%s,\"mode\":\"%s\","
    "\"scope\":\"documents\",\"limit\":9}", topics[topic], vectors[topic], mode) > 0);
  assert_int_equal(YAP_V2_http_execute(index_dir, YAP_V2_HTTP_SEARCH,
    (const unsigned char *)request, strlen(request), &http_status, &response, &response_bytes), 0);
  assert_int_equal(http_status, 200);
  document = yyjson_read(response, response_bytes, 0U); assert_non_null(document);
  results = yyjson_obj_get(yyjson_doc_get_root(document), "results");
  assert_true(yyjson_is_arr(results)); captured->count = yyjson_arr_size(results);
  assert_true(captured->count <= 9U);
  for (i = 0U; i < captured->count; i++) {
    yyjson_val *item = yyjson_arr_get(results, i);
    yyjson_val *id = yyjson_obj_get(item, "id");
    yyjson_val *score = yyjson_obj_get(item, "lexical_score");
    assert_true(yyjson_is_str(id)); assert_true(yyjson_is_num(score));
    assert_true(strlen(yyjson_get_str(id)) < sizeof(captured->ids[i]));
    strcpy(captured->ids[i], yyjson_get_str(id));
    captured->lexical_scores[i] = yyjson_get_real(score);
  }
  yyjson_doc_free(document); free(response);
}

static void assert_same_lexical_results(const CAPTURED_RESULTS *expected,
                                        const CAPTURED_RESULTS *actual) {
  size_t i;
  assert_int_equal(actual->count, expected->count);
  for (i = 0U; i < expected->count; i++) {
    assert_string_equal(actual->ids[i], expected->ids[i]);
    assert_true(fabs(actual->lexical_scores[i] - expected->lexical_scores[i]) < 1e-12);
  }
}

static double captured_score(const CAPTURED_RESULTS *captured, const char *document_id) {
  size_t i;
  for (i = 0U; i < captured->count; i++)
    if (strcmp(captured->ids[i], document_id) == 0)
      return captured->lexical_scores[i];
  fail_msg("missing captured document: %s", document_id);
  return 0.0;
}

static double evaluate(const char *index_dir, const char *mode, size_t topic) {
  static const char *const vectors[3] = {"[1,0,0]", "[0,1,0]", "[0,0,1]"};
  YAP_QUALITY_JUDGMENT judgments[9];
  YAP_QUALITY_HIT hits[9];
  YAP_QUALITY_QUERY_METRICS metrics;
  char request[256], *response = NULL;
  size_t response_bytes = 0U, i, hit_count;
  int http_status = 0;
  yyjson_doc *document;
  yyjson_val *results;

  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"%s\",\"vector\":%s,\"mode\":\"%s\","
    "\"scope\":\"documents\",\"limit\":9}", topics[topic], vectors[topic], mode) > 0);
  assert_int_equal(YAP_V2_http_execute(index_dir, YAP_V2_HTTP_SEARCH,
    (const unsigned char *)request, strlen(request), &http_status, &response, &response_bytes), 0);
  assert_int_equal(http_status, 200);
  document = yyjson_read(response, response_bytes, 0U);
  assert_non_null(document);
  results = yyjson_obj_get(yyjson_doc_get_root(document), "results");
  assert_true(yyjson_is_arr(results));
  hit_count = yyjson_arr_size(results);
  assert_true(hit_count <= 9U);
  for (i = 0U; i < 9U; i++) {
    judgments[i].document_id = document_ids[i];
    judgments[i].relevance = i / 3U == topic ? 3 : 0;
  }
  for (i = 0U; i < hit_count; i++) {
    yyjson_val *item = yyjson_arr_get(results, i);
    yyjson_val *id = yyjson_obj_get(item, "id");
    assert_true(yyjson_is_str(id));
    hits[i].document_id = yyjson_get_str(id);
  }
  assert_int_equal(YAP_Quality_metrics_calculate(judgments, 9U, hits, hit_count, 10U,
                                                  &metrics), 0);
  assert_true(metrics.recall_at_k >= 0.999999);
  yyjson_doc_free(document);
  free(response);
  return metrics.ndcg_at_k;
}

static void test_v2_mode_quality_and_hybrid_guard(void **state) {
  ytest_env_t env;
  double totals[3] = {0.0, 0.0, 0.0};
  size_t mode, query;
  static const char *const modes[3] = {"lexical", "vector", "hybrid"};
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create(env.tmp_root), 0);
  for (mode = 0U; mode < 3U; mode++)
    for (query = 0U; query < 3U; query++) totals[mode] += evaluate(env.tmp_root, modes[mode], query);
  for (mode = 0U; mode < 3U; mode++) totals[mode] /= 3.0;
  print_message("v2_lexical_ndcg_at_10\t%.6f\n", totals[0]);
  print_message("v2_vector_ndcg_at_10\t%.6f\n", totals[1]);
  print_message("v2_hybrid_ndcg_at_10\t%.6f\n", totals[2]);
  assert_true(totals[0] >= 0.99);
  assert_true(totals[1] >= 0.99);
  assert_true(totals[2] + 0.01 >= (totals[0] > totals[1] ? totals[0] : totals[1]));
  ytest_env_destroy(&env);
}

static void test_segment_split_and_compaction_preserve_lexical_scores(void **state) {
  ytest_env_t env;
  CAPTURED_RESULTS one[3], two[3], four[3], compacted[3];
  YAP_V2_COMPACTION_RESULT result;
  char index_one[PATH_MAX], index_two[PATH_MAX], index_four[PATH_MAX];
  char error[256] = {0};
  size_t topic;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(index_one, sizeof(index_one), env.tmp_root, "one"), 0);
  assert_int_equal(ytest_path_join(index_two, sizeof(index_two), env.tmp_root, "two"), 0);
  assert_int_equal(ytest_path_join(index_four, sizeof(index_four), env.tmp_root, "four"), 0);
  assert_int_equal(ytest_mkdir_p(index_one, 0700), 0);
  assert_int_equal(ytest_mkdir_p(index_two, 0700), 0);
  assert_int_equal(ytest_mkdir_p(index_four, 0700), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create_segments(index_one, 1U), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create_segments(index_two, 2U), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create_segments(index_four, 4U), 0);
  for (topic = 0U; topic < 3U; topic++) {
    capture_results(index_one, "lexical", topic, &one[topic]);
    capture_results(index_two, "lexical", topic, &two[topic]);
    capture_results(index_four, "lexical", topic, &four[topic]);
    assert_same_lexical_results(&one[topic], &two[topic]);
    assert_same_lexical_results(&one[topic], &four[topic]);
    assert_true(evaluate(index_four, "hybrid", topic) >= 0.99);
  }
  YAP_V2_compaction_result_init(&result);
  assert_int_equal(YAP_V2_compact(index_four, &result, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(result.removed_segments, 4U);
  for (topic = 0U; topic < 3U; topic++) {
    capture_results(index_four, "lexical", topic, &compacted[topic]);
    assert_same_lexical_results(&four[topic], &compacted[topic]);
  }
  YAP_V2_compaction_result_free(&result); ytest_env_destroy(&env);
}

static void test_stale_versions_bias_physical_bm25_until_compaction(void **state) {
  static const char update[] =
    "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-red-0\","
    "\"url\":\"https://quality.test/red/0\",\"title\":\"red reference 0\","
    "\"body\":\"red evidence corpus item 0\",\"metadata\":{\"topic\":\"red\"},"
    "\"vectors\":[[1,0,0]]}]}";
  ytest_env_t env;
  CAPTURED_RESULTS original, stale, compacted;
  YAP_V2_COMPACTION_RESULT result;
  char *response = NULL, error[256] = {0};
  size_t response_bytes = 0U;
  int http_status = 0;
  double original_score, stale_score;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create(env.tmp_root), 0);
  capture_results(env.tmp_root, "lexical", 0U, &original);
  assert_int_equal(YAP_V2_http_execute(env.tmp_root, YAP_V2_HTTP_INGEST,
    (const unsigned char *)update, strlen(update), &http_status, &response, &response_bytes), 0);
  assert_int_equal(http_status, 200); free(response);
  capture_results(env.tmp_root, "lexical", 0U, &stale);
  original_score = captured_score(&original, "doc-red-0");
  stale_score = captured_score(&stale, "doc-red-0");
  print_message("stale_version_lexical_score_delta\t%.12f\n", stale_score - original_score);
  assert_true(fabs(stale_score - original_score) > 1e-9);
  YAP_V2_compaction_result_init(&result);
  assert_int_equal(YAP_V2_compact(env.tmp_root, &result, error, sizeof(error)), YAP_V2_OK);
  capture_results(env.tmp_root, "lexical", 0U, &compacted);
  assert_same_lexical_results(&original, &compacted);
  YAP_V2_compaction_result_free(&result); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_v2_mode_quality_and_hybrid_guard),
    cmocka_unit_test(test_segment_split_and_compaction_preserve_lexical_scores),
    cmocka_unit_test(test_stale_versions_bias_physical_bm25_until_compaction)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
