#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_lexical_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view;
  view.data = (const unsigned char *)value;
  view.len = strlen(value);
  return view;
}

static void write_fixture(const char *directory) {
  YAP_V2_DOCUMENT_VIEW documents[2];
  YAP_V2_PASSAGE_VIEW passage;
  YAP_V2_COMPONENT_DESCRIPTOR components[3];
  memset(documents, 0, sizeof(documents));
  memset(&passage, 0, sizeof(passage));
  documents[0].id = bytes("doc-1");
  documents[0].title = bytes("Search Search");
  documents[0].body = bytes("modern retrieval searching");
  documents[0].updated_at_unix_ms = 1;
  documents[1].id = bytes("doc-2");
  documents[1].title = bytes("search");
  documents[1].body = bytes("vector retrieval");
  documents[1].updated_at_unix_ms = 2;
  passage.id = bytes("doc-1#0");
  passage.parent_document_id = bytes("doc-1");
  passage.text = bytes("search retrieval");
  passage.end_char = 16U;
  assert_int_equal(YAP_V2_lexical_write(directory, 11U, documents, 2U, &passage, 1U, components),
                   YAP_V2_OK);
}

static void test_reader_lookup_and_iterators(void **state) {
  ytest_env_t env;
  YAP_V2_LEXICAL_SEGMENT segment;
  const YAP_V2_TERM_ENTRY *term;
  YAP_V2_POSTING_ITERATOR postings;
  YAP_V2_POSITION_ITERATOR positions;
  YAP_V2_POSTING posting;
  YAP_V2_POSITION position;
  YAP_V2_POSTINGS_BLOCK block;
  char directory[PATH_MAX];
  size_t posting_count = 0U, position_count = 0U;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "segment"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  write_fixture(directory);
  YAP_V2_lexical_segment_init(&segment);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 11U, &segment), YAP_V2_OK);
  assert_int_equal(segment.generation, 11U);
  assert_int_equal(segment.document_count, 2U);
  assert_int_equal(segment.passage_count, 1U);
  term = YAP_V2_lexical_term_find(&segment, bytes("search"));
  assert_non_null(term);
  assert_int_equal(term->document_frequency, 3U);
  assert_non_null(YAP_V2_lexical_term_find(&segment, bytes("searching")));
  assert_null(YAP_V2_lexical_term_find(&segment, bytes("missing")));
  assert_int_equal(YAP_V2_posting_iterator_init(&segment, term, &postings), YAP_V2_OK);
  assert_int_equal(YAP_V2_posting_iterator_block(&postings, 0U, &block), YAP_V2_OK);
  assert_int_equal(block.first_posting, 0U);
  assert_int_equal(block.posting_count, 3U);
  while (YAP_V2_posting_iterator_next(&postings, &posting) == YAP_V2_OK) {
    if (posting_count == 0U) {
      assert_int_equal(posting.object_type, YAP_V2_LEXICAL_DOCUMENT);
      assert_int_equal(posting.object_ordinal, 0U);
      assert_int_equal(posting.term_frequency[0], 2U);
      assert_int_equal(posting.field_length[0], 2U);
    }
    posting_count++;
  }
  assert_int_equal(posting_count, 3U);
  assert_int_equal(YAP_V2_position_iterator_init(&segment, term, &positions), YAP_V2_OK);
  while (YAP_V2_position_iterator_next(&positions, &position) == YAP_V2_OK) {
    assert_true(position.field >= YAP_V2_FIELD_TITLE && position.field <= YAP_V2_FIELD_PASSAGE);
    position_count++;
  }
  assert_int_equal(position_count, 4U);
  YAP_V2_lexical_segment_close(&segment);
  ytest_env_destroy(&env);
}

static void test_reader_rejects_generation_and_corruption(void **state) {
  ytest_env_t env;
  YAP_V2_LEXICAL_SEGMENT segment;
  char directory[PATH_MAX], path[PATH_MAX];
  FILE *file;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "segment"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  write_fixture(directory);
  YAP_V2_lexical_segment_init(&segment);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 12U, &segment), YAP_V2_INVALID_FORMAT);
  assert_int_equal(ytest_path_join(path, sizeof(path), directory, "positions.yap2"), 0);
  file = fopen(path, "r+b");
  assert_non_null(file);
  assert_int_equal(fseek(file, -1L, SEEK_END), 0);
  assert_int_equal(fputc(0xff, file), 0xff);
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_lexical_segment_open(directory, 11U, &segment), YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_lexical_segment_close(&segment);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_reader_lookup_and_iterators),
    cmocka_unit_test(test_reader_rejects_generation_and_corruption),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
