#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_index_v2.h"

static YAP_V2_DOCUMENT_VIEW sample_document(void) {
  static const unsigned char id[] = "doc-1";
  static const unsigned char url[] = "https://example.com/doc-1";
  static const unsigned char title[] = "A title";
  static const unsigned char body[] = "A body for retrieval";
  YAP_V2_DOCUMENT_VIEW document;

  memset(&document, 0, sizeof(document));
  document.id.data = id;
  document.id.len = sizeof(id) - 1U;
  document.url.data = url;
  document.url.len = sizeof(url) - 1U;
  document.title.data = title;
  document.title.len = sizeof(title) - 1U;
  document.body.data = body;
  document.body.len = sizeof(body) - 1U;
  document.updated_at_unix_ms = INT64_C(1700000000000);
  return document;
}

static YAP_V2_PASSAGE_VIEW sample_passage(void) {
  static const unsigned char id[] = "doc-1#0";
  static const unsigned char parent[] = "doc-1";
  static const unsigned char text[] = "A body for retrieval";
  YAP_V2_PASSAGE_VIEW passage;

  memset(&passage, 0, sizeof(passage));
  passage.id.data = id;
  passage.id.len = sizeof(id) - 1U;
  passage.parent_document_id.data = parent;
  passage.parent_document_id.len = sizeof(parent) - 1U;
  passage.text.data = text;
  passage.text.len = sizeof(text) - 1U;
  passage.ordinal = 0U;
  passage.start_char = 0U;
  passage.end_char = 20U;
  return passage;
}

static void test_segment_roundtrip_and_checksum(void **state) {
  ytest_env_t env;
  YAP_V2_DOCUMENT_VIEW document = sample_document();
  YAP_V2_PASSAGE_VIEW passage = sample_passage();
  YAP_V2_SEGMENT_DESCRIPTOR written;
  YAP_V2_SEGMENT_DESCRIPTOR read_descriptor;
  YAP_V2_SEGMENT segment;
  char path[PATH_MAX];
  char *file_data = NULL;
  size_t file_size = 0U;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "seg-000001.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "seg-000001", 7U, &document, 1U, &passage, 1U,
                                         &written),
                   YAP_V2_OK);
  assert_int_equal(written.document_count, 1U);
  assert_int_equal(written.passage_count, 1U);
  assert_true(written.file_bytes > YAP_V2_FILE_HEADER_BYTES);
  assert_true(written.checksum[0] != 0U || written.checksum[1] != 0U);

  YAP_V2_segment_init(&segment);
  assert_int_equal(YAP_V2_segment_read(path, 7U, &segment, &read_descriptor), YAP_V2_OK);
  assert_string_equal(segment.id, "seg-000001");
  assert_int_equal(segment.generation, 7U);
  assert_int_equal(segment.document_count, 1U);
  assert_int_equal(segment.passage_count, 1U);
  assert_memory_equal(segment.documents[0].id.data, "doc-1", 5U);
  assert_memory_equal(segment.documents[0].body.data, "A body for retrieval", 20U);
  assert_memory_equal(segment.passages[0].text.data, "A body for retrieval", 20U);
  assert_memory_equal(read_descriptor.checksum, written.checksum, sizeof(written.checksum));
  assert_int_equal(read_descriptor.file_bytes, written.file_bytes);
  YAP_V2_segment_free(&segment);

  YAP_V2_segment_init(&segment);
  assert_int_equal(YAP_V2_segment_read(path, 8U, &segment, NULL), YAP_V2_INVALID_FORMAT);
  YAP_V2_segment_free(&segment);

  assert_int_equal(ytest_read_file(path, &file_data, &file_size), 0);
  assert_true(file_size > YAP_V2_FILE_HEADER_BYTES);
  file_data[file_size - 1U] ^= 0x01;
  assert_int_equal(ytest_write_file(path, file_data, file_size), 0);
  free(file_data);
  file_data = NULL;

  YAP_V2_segment_init(&segment);
  assert_int_equal(YAP_V2_segment_read(path, 7U, &segment, NULL), YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_segment_free(&segment);
  ytest_env_destroy(&env);
}

static void test_segment_rejects_orphan_passage(void **state) {
  YAP_V2_DOCUMENT_VIEW document = sample_document();
  YAP_V2_PASSAGE_VIEW passage = sample_passage();
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;

  (void)state;
  passage.parent_document_id.data = (const unsigned char *)"missing";
  passage.parent_document_id.len = 7U;
  assert_int_equal(YAP_V2_segment_write("/tmp/yappod2-invalid-segment", "seg-invalid", 1U,
                                         &document, 1U, &passage, 1U, &descriptor),
                   YAP_V2_INVALID_FORMAT);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_segment_roundtrip_and_checksum),
    cmocka_unit_test(test_segment_rejects_orphan_passage),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
