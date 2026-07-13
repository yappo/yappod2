#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "yappo_ann_v2.h"
#include "yappo_config_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)};
  return view;
}

static void config(YAP_V2_CONFIG *value) {
  YAP_V2_config_init(value);
  strcpy(value->vector_model_id, "embed-v1");
  value->vector_dimensions = 3U;
  value->vector_metric = YAP_V2_VECTOR_COSINE;
}

static void create_vectors(char *path, YAP_V2_VECTOR_SEGMENT *segment,
                           const char *first_id, const char *second_id,
                           const float values[6]) {
  YAP_V2_CONFIG cfg;
  YAP_V2_PASSAGE_VIEW passages[2];
  YAP_EMBEDDING_RESULT embeddings;
  YAP_V2_COMPONENT_DESCRIPTOR component;
  int fd = mkstemp(path);
  assert_true(fd >= 0);
  close(fd);
  config(&cfg);
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes(first_id);
  passages[1].id = bytes(second_id);
  passages[0].parent_document_id = bytes("doc");
  passages[1].parent_document_id = bytes("doc");
  passages[0].text = bytes("first"); passages[0].end_char = 5U;
  passages[1].text = bytes("second"); passages[1].ordinal = 1U; passages[1].end_char = 6U;
  memset(&embeddings, 0, sizeof(embeddings));
  embeddings.values = (float *)values;
  embeddings.input_count = 2U;
  embeddings.dimensions = 3U;
  assert_int_equal(YAP_V2_vectors_write(path, 1U, &cfg, passages, 2U, &embeddings, &component),
                   YAP_V2_OK);
  YAP_V2_vector_segment_init(segment);
  assert_int_equal(YAP_V2_vector_segment_open(path, 1U, &cfg, segment, NULL), YAP_V2_OK);
}

static void test_build_save_view_search_and_fallback(void **state) {
  const float values[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  const float query[3] = {1.0f, 0.0f, 0.0f};
  char vectors_path[] = "/tmp/yappod-ann-vectors-XXXXXX";
  char ann_path[] = "/tmp/yappod-ann-index-XXXXXX";
  YAP_V2_VECTOR_SEGMENT vectors;
  YAP_V2_ANN_SEGMENT ann, fallback;
  YAP_VECTOR_HIT hits[2];
  YAP_V2_COMPONENT_DESCRIPTOR component;
  size_t count;
  int fd;
  (void)state;
  create_vectors(vectors_path, &vectors, "p1", "p2", values);
  fd = mkstemp(ann_path); assert_true(fd >= 0); close(fd); unlink(ann_path);
  assert_int_equal(YAP_V2_ann_build_save(ann_path, &vectors, 8U, 32U, 24U, &component),
                   YAP_ANN_OK);
  assert_string_equal(component.name, "vectors.usearch");
  assert_int_equal(component.file_type, YAP_V2_FILE_ANN);
  assert_int_equal(component.record_count, 2U);
  YAP_V2_ann_segment_init(&ann);
  assert_int_equal(YAP_V2_ann_view(ann_path, &vectors, 24U, &ann, NULL), YAP_ANN_OK);
  assert_int_equal(YAP_V2_ann_search(&ann, query, 3U, 2U, hits, 2U, &count), YAP_VECTOR_OK);
  assert_int_equal(count, 2U);
  assert_memory_equal(hits[0].id.data, "p1", 2U);
  assert_true(hits[0].score > hits[1].score);
  YAP_V2_ann_segment_init(&fallback); fallback.vectors = &vectors;
  assert_int_equal(YAP_V2_ann_search(&fallback, query, 3U, 1U, hits, 1U, &count),
                   YAP_VECTOR_OK);
  assert_memory_equal(hits[0].id.data, "p1", 2U);
  YAP_V2_ann_segment_close(&ann);
  YAP_V2_vector_segment_close(&vectors);
  assert_int_equal(unlink(ann_path), 0);
  assert_int_equal(unlink(vectors_path), 0);
}

static void test_cross_segment_candidates(void **state) {
  const float first_values[6] = {0.8f, 0.2f, 0.0f, 0.0f, 1.0f, 0.0f};
  const float second_values[6] = {1.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f};
  const float query[3] = {1.0f, 0.0f, 0.0f};
  char path1[] = "/tmp/yappod-ann-seg1-XXXXXX";
  char path2[] = "/tmp/yappod-ann-seg2-XXXXXX";
  YAP_V2_VECTOR_SEGMENT vectors[2];
  YAP_V2_ANN_SEGMENT segments[2];
  YAP_V2_ANN_HIT hits[3];
  size_t count;
  (void)state;
  create_vectors(path1, &vectors[0], "a", "b", first_values);
  create_vectors(path2, &vectors[1], "c", "d", second_values);
  YAP_V2_ann_segment_init(&segments[0]); segments[0].vectors = &vectors[0];
  YAP_V2_ann_segment_init(&segments[1]); segments[1].vectors = &vectors[1];
  assert_int_equal(YAP_V2_ann_search_segments(segments, 2U, query, 3U, 3U, hits, 3U, &count),
                   YAP_VECTOR_OK);
  assert_int_equal(count, 3U);
  assert_int_equal(hits[0].segment_ordinal, 1U);
  assert_memory_equal(hits[0].hit.id.data, "c", 1U);
  assert_true(hits[0].hit.score >= hits[1].hit.score);
  YAP_V2_vector_segment_close(&vectors[0]); YAP_V2_vector_segment_close(&vectors[1]);
  assert_int_equal(unlink(path1), 0); assert_int_equal(unlink(path2), 0);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_build_save_view_search_and_fallback),
    cmocka_unit_test(test_cross_segment_candidates)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
