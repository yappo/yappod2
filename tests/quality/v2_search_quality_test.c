#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "search_quality_metrics.h"
#include "test_env.h"
#include "v2_quality_fixture.h"
#include "yappo_http_v2.h"

static const char *const document_ids[9] = {
  "doc-red-0", "doc-red-1", "doc-red-2", "doc-green-0", "doc-green-1",
  "doc-green-2", "doc-blue-0", "doc-blue-1", "doc-blue-2"
};
static const char *const topics[3] = {"red", "green", "blue"};

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

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_v2_mode_quality_and_hybrid_guard)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
