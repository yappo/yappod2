#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
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

static void test_writer_is_deterministic_and_describes_components(void **state) {
  ytest_env_t env;
  YAP_V2_DOCUMENT_VIEW documents[2];
  YAP_V2_PASSAGE_VIEW passage = {0};
  YAP_V2_COMPONENT_DESCRIPTOR first[3];
  YAP_V2_COMPONENT_DESCRIPTOR second[3];
  char first_dir[PATH_MAX], second_dir[PATH_MAX], first_path[PATH_MAX], second_path[PATH_MAX];
  char *first_data = NULL, *second_data = NULL;
  size_t first_size = 0U, second_size = 0U;
  size_t i;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(first_dir, sizeof(first_dir), env.tmp_root, "first"), 0);
  assert_int_equal(ytest_path_join(second_dir, sizeof(second_dir), env.tmp_root, "second"), 0);
  assert_int_equal(ytest_mkdir_p(first_dir, 0700), 0);
  assert_int_equal(ytest_mkdir_p(second_dir, 0700), 0);
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-1");
  documents[0].title = bytes("Search Search");
  documents[0].body = bytes("Modern retrieval engine");
  documents[0].updated_at_unix_ms = 1;
  documents[1].id = bytes("doc-2");
  documents[1].title = bytes("SEARCH");
  documents[1].body = bytes("vector retrieval");
  documents[1].updated_at_unix_ms = 2;
  passage.id = bytes("doc-1#0");
  passage.parent_document_id = bytes("doc-1");
  passage.text = bytes("Search retrieval");
  passage.end_char = 16U;

  assert_int_equal(YAP_V2_lexical_write(first_dir, 9U, documents, 2U, &passage, 1U, first),
                   YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(second_dir, 9U, documents, 2U, &passage, 1U, second),
                   YAP_V2_OK);
  assert_int_equal(first[0].file_type, YAP_V2_FILE_TERMS);
  assert_int_equal(first[1].file_type, YAP_V2_FILE_POSTINGS);
  assert_int_equal(first[2].file_type, YAP_V2_FILE_POSITIONS);
  assert_int_equal(first[0].record_count, 5U);
  assert_int_equal(first[1].record_count, 9U);
  assert_int_equal(first[2].record_count, 10U);
  for (i = 0U; i < 3U; i++) {
    assert_int_equal(first[i].file_bytes, second[i].file_bytes);
    assert_memory_equal(first[i].checksum, second[i].checksum, sizeof(first[i].checksum));
    assert_int_equal(ytest_path_join(first_path, sizeof(first_path), first_dir, first[i].name), 0);
    assert_int_equal(ytest_path_join(second_path, sizeof(second_path), second_dir, second[i].name),
                     0);
    assert_int_equal(ytest_read_file(first_path, &first_data, &first_size), 0);
    assert_int_equal(ytest_read_file(second_path, &second_data, &second_size), 0);
    assert_int_equal(first_size, second_size);
    assert_memory_equal(first_data, second_data, first_size);
    free(first_data);
    free(second_data);
    first_data = second_data = NULL;
  }
  ytest_env_destroy(&env);
}

static void test_writer_rejects_invalid_utf8(void **state) {
  static const unsigned char invalid[] = {0xffU};
  YAP_V2_DOCUMENT_VIEW document = {0};
  YAP_V2_COMPONENT_DESCRIPTOR components[3];

  (void)state;
  document.id = bytes("doc-invalid");
  document.body.data = invalid;
  document.body.len = sizeof(invalid);
  document.updated_at_unix_ms = 1;
  assert_int_equal(YAP_V2_lexical_write("/tmp", 1U, &document, 1U, NULL, 0U, components),
                   YAP_V2_INVALID_FORMAT);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_writer_is_deterministic_and_describes_components),
    cmocka_unit_test(test_writer_rejects_invalid_utf8),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
