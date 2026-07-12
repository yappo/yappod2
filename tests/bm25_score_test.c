#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>

#include <cmocka.h>

#include "yappo_bm25.h"

static void test_bm25_matches_reference_formula(void **state) {
  double idf;
  double tf_component;
  double expected;

  (void)state;
  idf = log1p((100.0 - 10.0 + 0.5) / (10.0 + 0.5));
  tf_component = (3.0 * (YAP_BM25_DEFAULT_K1 + 1.0)) /
                 (3.0 + YAP_BM25_DEFAULT_K1);
  expected = idf * tf_component;
  assert_true(fabs(YAP_BM25_score(3U, 10U, 100U, 100U, 100.0, 1.0) - expected) < 1e-12);
}

static void test_bm25_saturates_tf_and_normalizes_length(void **state) {
  double short_document;
  double long_document;

  (void)state;
  short_document = YAP_BM25_score(2U, 10U, 100U, 50U, 100.0, 1.0);
  long_document = YAP_BM25_score(2U, 10U, 100U, 200U, 100.0, 1.0);
  assert_true(short_document > long_document);
  assert_true(YAP_BM25_score(100U, 10U, 100U, 100U, 100.0, 1.0) <
              2.2 * YAP_BM25_score(1U, 10U, 100U, 100U, 100.0, 1.0));
}

static void test_bm25_rejects_invalid_statistics_and_sanitizes_boost(void **state) {
  double normal;

  (void)state;
  assert_true(fabs(YAP_BM25_score(0U, 1U, 10U, 10U, 10.0, 1.0)) < 1e-12);
  assert_true(fabs(YAP_BM25_score(1U, 11U, 10U, 10U, 10.0, 1.0)) < 1e-12);
  normal = YAP_BM25_score(1U, 1U, 10U, 10U, 10.0, 1.0);
  assert_true(isfinite(normal));
  assert_true(fabs(YAP_BM25_score(1U, 1U, 10U, 10U, 10.0, NAN) - normal) < 1e-12);
  assert_true(fabs(YAP_BM25_score(1U, 1U, 10U, 10U, 0.0, 1.0) -
                   YAP_BM25_score(1U, 1U, 10U, 10U, 1.0, 1.0)) <
              1e-12);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_bm25_matches_reference_formula),
    cmocka_unit_test(test_bm25_saturates_tf_and_normalizes_length),
    cmocka_unit_test(test_bm25_rejects_invalid_statistics_and_sanitizes_boost),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
