#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_vector.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view;
  view.data = (const unsigned char *)value;
  view.len = strlen(value);
  return view;
}

static void test_metric_scores(void **state) {
  const float query[] = {1.0f, 0.0f};
  const float candidate[] = {0.0f, 1.0f};
  double score;

  (void)state;
  assert_int_equal(YAP_Vector_score(YAP_V2_VECTOR_DOT, query, query, 2U, &score), YAP_VECTOR_OK);
  assert_float_equal(score, 1.0, 0.000001);
  assert_int_equal(YAP_Vector_score(YAP_V2_VECTOR_COSINE, query, candidate, 2U, &score),
                   YAP_VECTOR_OK);
  assert_float_equal(score, 0.0, 0.000001);
  assert_int_equal(YAP_Vector_score(YAP_V2_VECTOR_L2, query, candidate, 2U, &score), YAP_VECTOR_OK);
  assert_float_equal(score, -2.0, 0.000001);
  assert_int_equal(YAP_Vector_score(YAP_V2_VECTOR_DOT, query, candidate, 1U, &score),
                   YAP_VECTOR_OK);
  assert_float_equal(score, 0.0, 0.000001);
}

static void test_exact_search_and_ties(void **state) {
  const float query[] = {1.0f, 0.0f};
  const float first[] = {1.0f, 0.0f};
  const float second[] = {0.0f, 1.0f};
  const float third[] = {-1.0f, 0.0f};
  const YAP_VECTOR_ENTRY entries[] = {
    {bytes("first"), first, 2U},
    {bytes("second"), second, 2U},
    {bytes("third"), third, 2U},
  };
  YAP_VECTOR_HIT hits[2];
  size_t count = 0U;

  (void)state;
  assert_int_equal(YAP_Vector_search(entries, 3U, query, 2U, YAP_V2_VECTOR_COSINE, 2U, hits, 2U,
                                     &count),
                   YAP_VECTOR_OK);
  assert_int_equal(count, 2U);
  assert_memory_equal(hits[0].id.data, "first", 5U);
  assert_memory_equal(hits[1].id.data, "second", 6U);
  assert_int_equal(hits[0].ordinal, 0U);
  assert_int_not_equal(YAP_Vector_search(entries, 3U, query, 2U, YAP_V2_VECTOR_DOT, 2U, hits, 1U,
                                         &count),
                       YAP_VECTOR_OK);
  assert_int_equal(YAP_Vector_search(entries, 3U, query, 1U, YAP_V2_VECTOR_DOT, 1U, hits, 2U,
                                     &count),
                   YAP_VECTOR_DIMENSION_MISMATCH);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_metric_scores),
    cmocka_unit_test(test_exact_search_and_ties),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
