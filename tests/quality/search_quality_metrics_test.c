#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "search_quality_metrics.h"

static void test_metrics_use_graded_relevance(void **state) {
  static const YAP_QUALITY_JUDGMENT judgments[] = {
    {"doc-best", 3},
    {"doc-good", 2},
    {"doc-irrelevant", 0},
  };
  static const YAP_QUALITY_HIT hits[] = {
    {"doc-good"},
    {"doc-best"},
    {"doc-irrelevant"},
  };
  YAP_QUALITY_QUERY_METRICS metrics;
  (void)state;

  assert_int_equal(YAP_Quality_metrics_calculate(
                     judgments, sizeof(judgments) / sizeof(judgments[0]), hits,
                     sizeof(hits) / sizeof(hits[0]), 10U, &metrics),
                   0);
  assert_true(metrics.ndcg_at_k > 0.83 && metrics.ndcg_at_k < 0.84);
  assert_float_equal(metrics.mrr_at_k, 1.0, 0.000001);
  assert_float_equal(metrics.recall_at_k, 1.0, 0.000001);
}

static void test_metrics_ignore_duplicate_hits(void **state) {
  static const YAP_QUALITY_JUDGMENT judgments[] = {
    {"doc-a", 1},
    {"doc-b", 1},
  };
  static const YAP_QUALITY_HIT hits[] = {
    {"doc-a"},
    {"doc-a"},
  };
  YAP_QUALITY_QUERY_METRICS metrics;
  (void)state;

  assert_int_equal(YAP_Quality_metrics_calculate(
                     judgments, sizeof(judgments) / sizeof(judgments[0]), hits,
                     sizeof(hits) / sizeof(hits[0]), 10U, &metrics),
                   0);
  assert_float_equal(metrics.mrr_at_k, 1.0, 0.000001);
  assert_float_equal(metrics.recall_at_k, 0.5, 0.000001);
}

static void test_percentile_uses_nearest_rank(void **state) {
  static const double values[] = {1.0, 5.0, 2.0, 4.0, 3.0};
  (void)state;

  assert_float_equal(
    YAP_Quality_percentile_nearest_rank(values, sizeof(values) / sizeof(values[0]), 0.50), 3.0,
    0.000001);
  assert_float_equal(
    YAP_Quality_percentile_nearest_rank(values, sizeof(values) / sizeof(values[0]), 0.95), 5.0,
    0.000001);
}

static void test_metrics_reject_missing_relevant_judgment(void **state) {
  static const YAP_QUALITY_JUDGMENT judgments[] = {{"doc-a", 0}};
  YAP_QUALITY_QUERY_METRICS metrics;
  (void)state;

  assert_int_equal(YAP_Quality_metrics_calculate(judgments, 1U, NULL, 0U, 10U, &metrics), -1);
}

static void test_metrics_skip_null_hit_ids(void **state) {
  static const YAP_QUALITY_JUDGMENT judgments[] = {{"doc-a", 1}};
  static const YAP_QUALITY_HIT hits[] = {{NULL}, {"doc-a"}};
  YAP_QUALITY_QUERY_METRICS metrics;
  (void)state;

  assert_int_equal(YAP_Quality_metrics_calculate(judgments, 1U, hits, 2U, 10U, &metrics), 0);
  assert_float_equal(metrics.mrr_at_k, 0.5, 0.000001);
  assert_float_equal(metrics.recall_at_k, 1.0, 0.000001);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_metrics_use_graded_relevance),
    cmocka_unit_test(test_metrics_ignore_duplicate_hits),
    cmocka_unit_test(test_percentile_uses_nearest_rank),
    cmocka_unit_test(test_metrics_reject_missing_relevant_judgment),
    cmocka_unit_test(test_metrics_skip_null_hit_ids),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
