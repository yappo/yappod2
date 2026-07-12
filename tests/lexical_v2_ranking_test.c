#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_lexical_search_v2.h"

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

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_bm25f_boolean_and_phrase),
    cmocka_unit_test(test_block_max_wand_keeps_rare_top_hit),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
