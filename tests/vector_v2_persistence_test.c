#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_config_v2.h"
#include "yappo_vector_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static YAP_V2_BYTES_VIEW view(const char *text) {
  YAP_V2_BYTES_VIEW result = {(const unsigned char *)text, strlen(text)};
  return result;
}

static void vector_config(YAP_V2_CONFIG *config) {
  YAP_V2_config_init(config);
  strcpy(config->vector_model_id, "embed-v1");
  config->vector_dimensions = 3U;
  config->vector_metric = YAP_V2_VECTOR_COSINE;
}

static YAP_V2_PASSAGE_VIEW passage(const char *id, uint32_t ordinal) {
  YAP_V2_PASSAGE_VIEW result;
  memset(&result, 0, sizeof(result)); result.id = view(id); result.parent_document_id = view("doc");
  result.text = view("passage text"); result.ordinal = ordinal; result.end_char = 12U; return result;
}

static void test_roundtrip_and_exact_search(void **state) {
  char path[] = "/tmp/yappod-vectors-XXXXXX"; int fd; YAP_V2_CONFIG config;
  YAP_V2_PASSAGE_VIEW passages[] = {passage("p0", 0U), passage("p1", 1U), passage("p2", 2U)};
  float values[] = {1,0,0, 0,1,0, 0.9f,0.1f,0}; float query[] = {1,0,0};
  YAP_EMBEDDING_RESULT embeddings = {values, 3U, 3U}; YAP_V2_COMPONENT_DESCRIPTOR written, read;
  YAP_V2_VECTOR_SEGMENT segment; YAP_VECTOR_HIT hits[2]; size_t hit_count;
  (void)state; fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  vector_config(&config); assert_int_equal(YAP_V2_vectors_write(path, 9U, &config, passages, 3U, &embeddings, &written), YAP_V2_OK);
  assert_string_equal(written.name, "vectors.yap2"); assert_int_equal(written.record_count, 3U);
  YAP_V2_vector_segment_init(&segment);
  assert_int_equal(YAP_V2_vector_segment_open(path, 9U, &config, &segment, &read), YAP_V2_OK);
  assert_int_equal(segment.entry_count, 3U); assert_string_equal(segment.model_id, "embed-v1");
  assert_int_equal(YAP_V2_vector_segment_search(&segment, query, 3U, 2U, hits, 2U, &hit_count), YAP_VECTOR_OK);
  assert_int_equal(hit_count, 2U); assert_memory_equal(hits[0].id.data, "p0", 2U);
  assert_memory_equal(hits[1].id.data, "p2", 2U); assert_true(hits[0].score > hits[1].score);
  YAP_V2_vector_segment_close(&segment); assert_int_equal(unlink(path), 0);
}

static void test_rejects_config_conflict_and_corruption(void **state) {
  char path[] = "/tmp/yappod-vectors-XXXXXX"; int fd; FILE *file; YAP_V2_CONFIG config, wrong;
  YAP_V2_PASSAGE_VIEW passages[] = {passage("p0", 0U)}; float values[] = {1,2,3};
  YAP_EMBEDDING_RESULT embeddings = {values, 1U, 3U}; YAP_V2_COMPONENT_DESCRIPTOR component;
  YAP_V2_VECTOR_SEGMENT segment;
  (void)state; fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  vector_config(&config); assert_int_equal(YAP_V2_vectors_write(path, 3U, &config, passages, 1U, &embeddings, &component), YAP_V2_OK);
  wrong = config; wrong.vector_dimensions = 4U; YAP_V2_vector_segment_init(&segment);
  assert_int_equal(YAP_V2_vector_segment_open(path, 3U, &wrong, &segment, NULL), YAP_V2_CONFLICT);
  file = fopen(path, "r+b"); assert_non_null(file); assert_int_equal(fseek(file, -1L, SEEK_END), 0);
  assert_int_equal(fputc(0xff, file), 0xff); assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_vector_segment_open(path, 3U, &config, &segment, NULL), YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_vector_segment_close(&segment); assert_int_equal(unlink(path), 0);
}

int main(void) {
  const struct CMUnitTest tests[] = {cmocka_unit_test(test_roundtrip_and_exact_search),
                                     cmocka_unit_test(test_rejects_config_conflict_and_corruption)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
