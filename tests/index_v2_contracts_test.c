#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_index_v2.h"

static YAP_V2_DOCUMENT_VIEW sample_document(void) {
  static const unsigned char id[] = "doc-1";
  static const unsigned char url[] = "https://example.com/doc-1";
  static const unsigned char title[] = "A title";
  static const unsigned char body[] = "A body";
  static const unsigned char metadata[] = "{\"tenant\":\"default\"}";
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
  document.metadata_json.data = metadata;
  document.metadata_json.len = sizeof(metadata) - 1U;
  document.updated_at_unix_ms = 1700000000000LL;
  return document;
}

static void test_document_and_passage_contract(void **state) {
  static const unsigned char passage_id[] = "doc-1#0";
  static const unsigned char parent_id[] = "doc-1";
  static const unsigned char text[] = "A passage";
  YAP_V2_DOCUMENT_VIEW document = sample_document();
  YAP_V2_PASSAGE_VIEW passage;
  (void)state;

  memset(&passage, 0, sizeof(passage));
  passage.id.data = passage_id;
  passage.id.len = sizeof(passage_id) - 1U;
  passage.parent_document_id.data = parent_id;
  passage.parent_document_id.len = sizeof(parent_id) - 1U;
  passage.text.data = text;
  passage.text.len = sizeof(text) - 1U;
  passage.end_char = 9U;

  assert_int_equal(YAP_V2_document_validate(&document), YAP_V2_OK);
  assert_int_equal(YAP_V2_passage_validate(&passage), YAP_V2_OK);
  passage.end_char = 8U;
  passage.start_char = 9U;
  assert_int_equal(YAP_V2_passage_validate(&passage), YAP_V2_OUT_OF_RANGE);
}

static void test_document_rejects_nul_and_negative_time(void **state) {
  static const unsigned char id[] = {'d', 'o', 'c', '\0', 'x'};
  YAP_V2_DOCUMENT_VIEW document;
  (void)state;

  memset(&document, 0, sizeof(document));
  document.id.data = id;
  document.id.len = sizeof(id);
  assert_int_equal(YAP_V2_document_validate(&document), YAP_V2_INVALID_FORMAT);
  document = sample_document();
  document.updated_at_unix_ms = -1;
  assert_int_equal(YAP_V2_document_validate(&document), YAP_V2_OUT_OF_RANGE);
}

static void test_document_accepts_long_url_with_separate_limit(void **state) {
  unsigned char url[YAP_V2_MAX_URL_BYTES + 1U];
  YAP_V2_DOCUMENT_VIEW document = sample_document();
  (void)state;

  memset(url, 'u', sizeof(url));
  document.url.data = url;
  document.url.len = 316U;
  assert_int_equal(YAP_V2_document_validate(&document), YAP_V2_OK);
  document.url.len = sizeof(url);
  assert_int_equal(YAP_V2_document_validate(&document), YAP_V2_OUT_OF_RANGE);
}

static void test_config_vector_contract(void **state) {
  YAP_V2_CONFIG config;
  (void)state;

  memset(&config, 0, sizeof(config));
  config.format_version = YAP_V2_FORMAT_VERSION;
  strcpy(config.tokenizer_id, "unicode_bigram_v2");
  config.chunk_max_chars = 1200U;
  config.chunk_overlap_chars = 200U;
  assert_int_equal(YAP_V2_config_validate(&config), YAP_V2_OK);

  config.vector_metric = YAP_V2_VECTOR_COSINE;
  strcpy(config.vector_model_id, "japanese-embedding-v1");
  config.vector_dimensions = 768U;
  assert_int_equal(YAP_V2_config_validate(&config), YAP_V2_OK);

  config.vector_dimensions = 0U;
  assert_int_equal(YAP_V2_config_validate(&config), YAP_V2_OUT_OF_RANGE);
}

static void test_config_rejects_invalid_chunking(void **state) {
  YAP_V2_CONFIG config;
  (void)state;

  memset(&config, 0, sizeof(config));
  config.format_version = YAP_V2_FORMAT_VERSION;
  strcpy(config.tokenizer_id, "legacy_bigram_ascii");
  config.chunk_max_chars = 100U;
  config.chunk_overlap_chars = 100U;
  assert_int_equal(YAP_V2_config_validate(&config), YAP_V2_OUT_OF_RANGE);
}

static void test_config_rejects_unterminated_identifier(void **state) {
  YAP_V2_CONFIG config;
  (void)state;

  memset(&config, 'x', sizeof(config));
  config.format_version = YAP_V2_FORMAT_VERSION;
  config.chunk_max_chars = 100U;
  config.chunk_overlap_chars = 10U;
  assert_int_equal(YAP_V2_config_validate(&config), YAP_V2_INVALID_FORMAT);
}

static void test_manifest_owns_segments_and_rejects_duplicates(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_COMPONENT_DESCRIPTOR component;
  size_t i;
  (void)state;

  YAP_V2_manifest_init(&manifest);
  memset(&segment, 0, sizeof(segment));
  strcpy(segment.id, "seg-000001");
  segment.document_count = 10U;
  segment.passage_count = 20U;
  memset(&component, 0, sizeof(component));
  strcpy(component.name, "documents.yap2");
  component.file_type = YAP_V2_FILE_DOCUMENTS;
  component.file_bytes = YAP_V2_FILE_HEADER_BYTES;
  assert_int_equal(YAP_V2_segment_descriptor_add_component(&segment, &component), YAP_V2_OK);
  for (i = 0U; i < sizeof(manifest.config_fingerprint); i++) {
    manifest.config_fingerprint[i] = (unsigned char)(i + 1U);
  }
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_DUPLICATE);
  assert_int_equal(YAP_V2_manifest_validate(&manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
  assert_null(manifest.segments);
  assert_int_equal(manifest.segment_count, 0U);
}

static void test_manifest_rejects_path_traversal_id(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  (void)state;

  YAP_V2_manifest_init(&manifest);
  memset(&segment, 0, sizeof(segment));
  strcpy(segment.id, "../segment");
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_INVALID_FORMAT);
  YAP_V2_manifest_free(&manifest);
}

static void test_file_header_is_fixed_little_endian(void **state) {
  unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  YAP_V2_FILE_HEADER input;
  YAP_V2_FILE_HEADER output;
  (void)state;

  memset(&input, 0, sizeof(input));
  input.format_version = YAP_V2_FORMAT_VERSION;
  input.header_bytes = YAP_V2_FILE_HEADER_BYTES;
  input.file_type = YAP_V2_FILE_VECTORS;
  input.generation = UINT64_C(0x0102030405060708);
  input.payload_bytes = UINT64_C(0x1112131415161718);
  input.payload_crc32c = UINT32_C(0xaabbccdd);
  assert_int_equal(YAP_V2_file_header_encode(&input, encoded), YAP_V2_OK);
  assert_int_equal(encoded[0], 'Y');
  assert_int_equal(encoded[4], 2);
  assert_int_equal(encoded[12], 8);
  assert_int_equal(encoded[19], 1);
  assert_int_equal(YAP_V2_file_header_decode(encoded, &output), YAP_V2_OK);
  assert_int_equal(output.file_type, input.file_type);
  assert_int_equal(output.generation, input.generation);
  assert_int_equal(output.payload_bytes, input.payload_bytes);
  assert_int_equal(output.payload_crc32c, input.payload_crc32c);
  encoded[0] = 'X';
  assert_int_equal(YAP_V2_file_header_decode(encoded, &output), YAP_V2_INVALID_FORMAT);
  input.generation = 0U;
  assert_int_equal(YAP_V2_file_header_encode(&input, encoded), YAP_V2_INVALID_FORMAT);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_document_and_passage_contract),
    cmocka_unit_test(test_document_rejects_nul_and_negative_time),
    cmocka_unit_test(test_document_accepts_long_url_with_separate_limit),
    cmocka_unit_test(test_config_vector_contract),
    cmocka_unit_test(test_config_rejects_invalid_chunking),
    cmocka_unit_test(test_config_rejects_unterminated_identifier),
    cmocka_unit_test(test_manifest_owns_segments_and_rejects_duplicates),
    cmocka_unit_test(test_manifest_rejects_path_traversal_id),
    cmocka_unit_test(test_file_header_is_fixed_little_endian),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
