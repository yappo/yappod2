#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "query/yappo_lexical_search_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view;
  view.data = (const unsigned char *)value;
  view.len = strlen(value);
  return view;
}

static void build_small(const char *directory) {
  YAP_V2_DOCUMENT_VIEW documents[3];
  YAP_V2_COMPONENT_DESCRIPTOR components[3];
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-0");
  documents[0].title = bytes("quick brown fox");
  documents[0].body = bytes("search engine");
  documents[1].id = bytes("doc-1");
  documents[1].title = bytes("quick fox brown");
  documents[1].body = bytes("search");
  documents[2].id = bytes("doc-2");
  documents[2].body = bytes("brown archive");
  assert_int_equal(YAP_V2_lexical_write(directory, 20U, documents, 3U, NULL, 0U, components),
                   YAP_V2_OK);
}

static void test_bm25f_boolean_and_phrase(void **state) {
  ytest_env_t env;
  YAP_V2_LEXICAL_SEGMENT segment;
  YAP_V2_LEXICAL_SEARCH_OPTIONS options;
  YAP_V2_LEXICAL_HIT hits[10];
  size_t count;
  char directory[PATH_MAX];

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "segment"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  build_small(directory);
  YAP_V2_lexical_segment_init(&segment);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 20U, &segment), YAP_V2_OK);
  assert_int_equal(segment.field_token_count[0], 6U);
  assert_int_equal(segment.field_token_count[1], 5U);
  YAP_V2_lexical_search_options_init(&options);
  options.object_type = YAP_V2_LEXICAL_DOCUMENT;
  options.top_k = 10U;
  assert_int_equal(
    YAP_V2_lexical_search(&segment, bytes("quick archive"), &options, hits, 10U, &count),
    YAP_V2_OK);
  assert_int_equal(count, 3U);
  options.query_operator = YAP_V2_QUERY_AND;
  assert_int_equal(
    YAP_V2_lexical_search(&segment, bytes("quick brown"), &options, hits, 10U, &count), YAP_V2_OK);
  assert_int_equal(count, 2U);
  assert_int_equal(hits[0].matched_terms, 2U);
  options.phrase = 1;
  assert_int_equal(
    YAP_V2_lexical_search(&segment, bytes("quick brown"), &options, hits, 10U, &count), YAP_V2_OK);
  assert_int_equal(count, 1U);
  assert_int_equal(hits[0].object_ordinal, 0U);
  YAP_V2_lexical_segment_close(&segment);
  ytest_env_destroy(&env);
}

static void test_block_max_wand_keeps_rare_top_hit(void **state) {
  ytest_env_t env;
  YAP_V2_DOCUMENT_VIEW *documents;
  YAP_V2_COMPONENT_DESCRIPTOR components[3];
  YAP_V2_LEXICAL_SEGMENT segment;
  YAP_V2_LEXICAL_SEARCH_OPTIONS options;
  YAP_V2_LEXICAL_HIT hit;
  size_t count;
  size_t i;
  char directory[PATH_MAX];

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "segment"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  documents = (YAP_V2_DOCUMENT_VIEW *)calloc(260U, sizeof(*documents));
  assert_non_null(documents);
  for (i = 0U; i < 260U; i++) {
    documents[i].id = bytes("doc");
    documents[i].body = bytes("common");
  }
  documents[259].title = bytes("rare rare rare");
  assert_int_equal(YAP_V2_lexical_write(directory, 21U, documents, 260U, NULL, 0U, components),
                   YAP_V2_OK);
  free(documents);
  YAP_V2_lexical_segment_init(&segment);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 21U, &segment), YAP_V2_OK);
  YAP_V2_lexical_search_options_init(&options);
  options.object_type = YAP_V2_LEXICAL_DOCUMENT;
  options.top_k = 1U;
  assert_int_equal(
    YAP_V2_lexical_search(&segment, bytes("common rare"), &options, &hit, 1U, &count), YAP_V2_OK);
  assert_int_equal(count, 1U);
  assert_int_equal(hit.object_ordinal, 259U);
  YAP_V2_lexical_segment_close(&segment);
  ytest_env_destroy(&env);
}

static void test_prepared_query_reuses_normalized_unique_terms(void **state) {
  ytest_env_t env;
  YAP_V2_LEXICAL_SEGMENT segment;
  YAP_V2_LEXICAL_QUERY_PLAN plan;
  YAP_V2_LEXICAL_CORPUS_STATS stats;
  const YAP_V2_LEXICAL_SEGMENT *segments[1];
  YAP_V2_LEXICAL_SEARCH_OPTIONS options;
  YAP_V2_LEXICAL_HIT prepared[10], direct[10];
  size_t prepared_count, direct_count, i;
  char directory[PATH_MAX];

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "segment"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  build_small(directory);
  YAP_V2_lexical_segment_init(&segment);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 20U, &segment), YAP_V2_OK);
  YAP_V2_lexical_query_plan_init(&plan);
  assert_int_equal(YAP_V2_lexical_query_plan_prepare(bytes("QUICK quick brown"), &plan),
                   YAP_V2_OK);
  assert_int_equal(plan.tokens.token_count, 3U);
  assert_int_equal(plan.term_count, 2U);
  assert_int_equal(plan.token_terms[0], plan.token_terms[1]);
  segments[0] = &segment;
  assert_int_equal(YAP_V2_lexical_query_plan_bind(&plan, segments, 1U), YAP_V2_OK);
  assert_int_equal(plan.type_frequency[0][0], 2U);
  assert_int_equal(plan.type_frequency[0][1], 3U);
  memset(&stats, 0, sizeof(stats));
  stats.document_count = segment.document_count;
  stats.passage_count = segment.passage_count;
  memcpy(stats.field_token_count, segment.field_token_count, sizeof(stats.field_token_count));
  YAP_V2_lexical_search_options_init(&options);
  options.object_type = YAP_V2_LEXICAL_DOCUMENT;
  options.top_k = 10U;
  assert_int_equal(YAP_V2_lexical_search_prepared(&plan, 0U, &stats, &options,
                                                  prepared, 10U, &prepared_count), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_search(&segment, bytes("QUICK quick brown"), &options,
                                         direct, 10U, &direct_count), YAP_V2_OK);
  assert_int_equal(prepared_count, direct_count);
  for (i = 0U; i < direct_count; i++) {
    assert_int_equal(prepared[i].object_ordinal, direct[i].object_ordinal);
    assert_true(fabs(prepared[i].score - direct[i].score) < 1e-12);
  }
  YAP_V2_lexical_query_plan_free(&plan);
  YAP_V2_lexical_segment_close(&segment);
  ytest_env_destroy(&env);
}

static double score_for_ordinal(const YAP_V2_LEXICAL_HIT *hits, size_t count,
                                uint64_t ordinal) {
  size_t i;
  for (i = 0U; i < count; i++)
    if (hits[i].object_ordinal == ordinal)
      return hits[i].score;
  return -1.0;
}

static void test_global_bm25_is_independent_of_segment_split(void **state) {
  ytest_env_t env;
  YAP_V2_DOCUMENT_VIEW documents[4];
  YAP_V2_COMPONENT_DESCRIPTOR components[3];
  YAP_V2_LEXICAL_SEGMENT split[2], merged;
  const YAP_V2_LEXICAL_SEGMENT *segments[2];
  YAP_V2_LEXICAL_QUERY_PLAN plan;
  YAP_V2_LEXICAL_CORPUS_STATS stats;
  YAP_V2_LEXICAL_SEARCH_OPTIONS options;
  YAP_V2_LEXICAL_HIT first[4], second[4], all[4];
  size_t first_count, second_count, all_count, field;
  char first_dir[PATH_MAX], second_dir[PATH_MAX], merged_dir[PATH_MAX];

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(first_dir, sizeof(first_dir), env.tmp_root, "first"), 0);
  assert_int_equal(ytest_path_join(second_dir, sizeof(second_dir), env.tmp_root, "second"), 0);
  assert_int_equal(ytest_path_join(merged_dir, sizeof(merged_dir), env.tmp_root, "merged"), 0);
  assert_int_equal(ytest_mkdir_p(first_dir, 0700), 0);
  assert_int_equal(ytest_mkdir_p(second_dir, 0700), 0);
  assert_int_equal(ytest_mkdir_p(merged_dir, 0700), 0);
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-0"); documents[0].title = bytes("rare common");
  documents[0].body = bytes("short");
  documents[1].id = bytes("doc-1"); documents[1].title = bytes("common");
  documents[1].body = bytes("plain plain");
  documents[2].id = bytes("doc-2"); documents[2].body = bytes("common common");
  documents[3].id = bytes("doc-3"); documents[3].body = bytes("common");
  assert_int_equal(YAP_V2_lexical_write(first_dir, 30U, documents, 1U, NULL, 0U,
                                        components), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(second_dir, 30U, documents + 1U, 3U, NULL, 0U,
                                        components), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(merged_dir, 30U, documents, 4U, NULL, 0U,
                                        components), YAP_V2_OK);
  YAP_V2_lexical_segment_init(&split[0]); YAP_V2_lexical_segment_init(&split[1]);
  YAP_V2_lexical_segment_init(&merged);
  assert_int_equal(YAP_V2_lexical_segment_open(first_dir, 30U, &split[0]), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_segment_open(second_dir, 30U, &split[1]), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_segment_open(merged_dir, 30U, &merged), YAP_V2_OK);
  segments[0] = &split[0]; segments[1] = &split[1];
  YAP_V2_lexical_query_plan_init(&plan);
  assert_int_equal(YAP_V2_lexical_query_plan_prepare(bytes("rare common"), &plan), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_query_plan_bind(&plan, segments, 2U), YAP_V2_OK);
  assert_int_equal(plan.type_frequency[0][0], 1U);
  assert_int_equal(plan.type_frequency[0][1], 4U);
  memset(&stats, 0, sizeof(stats));
  for (field = 0U; field < 3U; field++)
    stats.field_token_count[field] = split[0].field_token_count[field] +
                                     split[1].field_token_count[field];
  stats.document_count = split[0].document_count + split[1].document_count;
  stats.passage_count = split[0].passage_count + split[1].passage_count;
  YAP_V2_lexical_search_options_init(&options);
  options.object_type = YAP_V2_LEXICAL_DOCUMENT; options.top_k = 4U;
  assert_int_equal(YAP_V2_lexical_search_prepared(&plan, 0U, &stats, &options,
                                                  first, 4U, &first_count), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_search_prepared(&plan, 1U, &stats, &options,
                                                  second, 4U, &second_count), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_search(&merged, bytes("rare common"), &options,
                                         all, 4U, &all_count), YAP_V2_OK);
  assert_true(fabs(score_for_ordinal(first, first_count, 0U) -
                   score_for_ordinal(all, all_count, 0U)) < 1e-12);
  assert_true(fabs(score_for_ordinal(second, second_count, 0U) -
                   score_for_ordinal(all, all_count, 1U)) < 1e-12);
  YAP_V2_lexical_query_plan_free(&plan);
  YAP_V2_lexical_segment_close(&split[0]); YAP_V2_lexical_segment_close(&split[1]);
  YAP_V2_lexical_segment_close(&merged); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_bm25f_boolean_and_phrase),
    cmocka_unit_test(test_block_max_wand_keeps_rare_top_hit),
    cmocka_unit_test(test_prepared_query_reuses_normalized_unique_terms),
    cmocka_unit_test(test_global_bm25_is_independent_of_segment_split),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
