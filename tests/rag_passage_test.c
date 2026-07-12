#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_index_v2.h"
#include "yappo_rag.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view;
  view.data = (const unsigned char *)value;
  view.len = strlen(value);
  return view;
}

static void test_rag_lookup_and_ordinal_order(void **state) {
  ytest_env_t env;
  YAP_V2_DOCUMENT_VIEW documents[2];
  YAP_V2_PASSAGE_VIEW passages[3];
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_SEGMENT segment;
  const YAP_V2_PASSAGE_VIEW *found = NULL;
  const YAP_V2_PASSAGE_VIEW *selected[2];
  size_t count = 0U;
  char path[PATH_MAX];

  (void)state;
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-a");
  documents[0].body = bytes("body a");
  documents[1].id = bytes("doc-b");
  documents[1].body = bytes("body b");
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes("doc-a#2");
  passages[0].parent_document_id = bytes("doc-a");
  passages[0].text = bytes("third");
  passages[0].ordinal = 2U;
  passages[0].start_char = 6U;
  passages[0].end_char = 11U;
  passages[1].id = bytes("doc-a#0");
  passages[1].parent_document_id = bytes("doc-a");
  passages[1].text = bytes("first");
  passages[1].ordinal = 0U;
  passages[1].end_char = 5U;
  passages[2].id = bytes("doc-b#0");
  passages[2].parent_document_id = bytes("doc-b");
  passages[2].text = bytes("other");
  passages[2].ordinal = 0U;
  passages[2].end_char = 5U;

  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "rag-segment.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "rag-segment", 3U, documents, 2U, passages, 3U,
                                         &descriptor),
                   YAP_V2_OK);
  YAP_V2_segment_init(&segment);
  assert_int_equal(YAP_V2_segment_read(path, 3U, &segment, NULL), YAP_V2_OK);

  assert_int_equal(YAP_RAG_find_passage(&segment, bytes("doc-a#0"), &found), YAP_RAG_OK);
  assert_non_null(found);
  assert_int_equal(found->ordinal, 0U);
  assert_int_equal(YAP_RAG_find_passage(&segment, bytes("missing"), &found), YAP_RAG_NOT_FOUND);
  assert_int_equal(YAP_RAG_list_passages(&segment, bytes("doc-a"), NULL, 0U, &count),
                   YAP_RAG_BUFFER_TOO_SMALL);
  assert_int_equal(count, 2U);
  assert_int_equal(YAP_RAG_list_passages(&segment, bytes("doc-a"), selected, 2U, &count),
                   YAP_RAG_OK);
  assert_int_equal(count, 2U);
  assert_int_equal(selected[0]->ordinal, 0U);
  assert_int_equal(selected[1]->ordinal, 2U);
  assert_int_equal(YAP_RAG_list_passages(&segment, bytes("missing"), selected, 2U, &count),
                   YAP_RAG_OK);
  assert_int_equal(count, 0U);

  YAP_V2_segment_free(&segment);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_rag_lookup_and_ordinal_order),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
