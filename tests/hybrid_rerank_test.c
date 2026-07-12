#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_hybrid.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view;
  view.data = (const unsigned char *)value;
  view.len = strlen(value);
  return view;
}

static void test_rrf_merges_and_ranks(void **state) {
  const YAP_HYBRID_CANDIDATE lexical[] = {
    {bytes("lexical-only"), 8.0},
    {bytes("shared"), 7.0},
  };
  const YAP_HYBRID_CANDIDATE vector[] = {
    {bytes("vector-only"), 0.95},
    {bytes("shared"), 0.90},
  };
  YAP_HYBRID_HIT hits[3];
  size_t count = 0U;

  (void)state;
  assert_int_equal(YAP_Hybrid_fuse_rrf(lexical, 2U, vector, 2U, 1.0, 1.0, 3U, hits, 3U,
                                       &count),
                   YAP_HYBRID_OK);
  assert_int_equal(count, 3U);
  assert_memory_equal(hits[0].id.data, "shared", 6U);
  assert_true(hits[0].lexical_score > 0.0);
  assert_true(hits[0].vector_score > 0.0);
  assert_memory_equal(hits[1].id.data, "lexical-only", 12U);
  assert_memory_equal(hits[2].id.data, "vector-only", 11U);
}

static void test_rrf_validation(void **state) {
  const YAP_HYBRID_CANDIDATE candidate = {bytes("one"), 1.0};
  YAP_HYBRID_HIT hit;
  size_t count = 0U;

  (void)state;
  assert_int_equal(YAP_Hybrid_fuse_rrf(&candidate, 1U, NULL, 0U, 1.0, 0.0, 1U, &hit, 1U,
                                       &count),
                   YAP_HYBRID_OK);
  assert_int_equal(YAP_Hybrid_fuse_rrf(&candidate, 1U, NULL, 0U, 1.0, 0.0, 2U, &hit, 1U,
                                       &count),
                   YAP_HYBRID_BUFFER_TOO_SMALL);
  assert_int_equal(YAP_Hybrid_fuse_rrf(&candidate, 1U, NULL, 0U, 0.0, 0.0, 1U, &hit, 1U,
                                       &count),
                   YAP_HYBRID_INVALID_ARGUMENT);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_rrf_merges_and_ranks),
    cmocka_unit_test(test_rrf_validation),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
