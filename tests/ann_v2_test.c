#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
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

static uint32_t random_u32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13; value ^= value >> 17; value ^= value << 5;
  *state = value;
  return value;
}

static void test_ann_recall_at_10_against_exact_ground_truth(void **state) {
  enum { VECTOR_COUNT = 512, DIMENSIONS = 16, QUERY_COUNT = 40, TOP_K = 10 };
  YAP_V2_CONFIG cfg;
  YAP_V2_PASSAGE_VIEW *passages;
  YAP_EMBEDDING_RESULT embeddings;
  YAP_V2_VECTOR_SEGMENT vectors;
  YAP_V2_ANN_SEGMENT ann;
  YAP_V2_COMPONENT_DESCRIPTOR vector_component;
  YAP_VECTOR_HIT exact[TOP_K], approximate[TOP_K];
  float *values;
  char (*ids)[32];
  char vectors_path[] = "/tmp/yappod-ann-recall-vectors-XXXXXX";
  char ann_path[] = "/tmp/yappod-ann-recall-index-XXXXXX";
  uint32_t rng = 0x6d2b79f5U;
  size_t i, j, query, exact_count, approximate_count, matches = 0U;
  int fd;
  double recall;
  (void)state;

  passages = calloc(VECTOR_COUNT, sizeof(*passages));
  values = calloc((size_t)VECTOR_COUNT * DIMENSIONS, sizeof(*values));
  ids = calloc(VECTOR_COUNT, sizeof(*ids));
  assert_non_null(passages); assert_non_null(values); assert_non_null(ids);
  YAP_V2_config_init(&cfg);
  strcpy(cfg.vector_model_id, "ann-recall-v1");
  cfg.vector_dimensions = DIMENSIONS;
  cfg.vector_metric = YAP_V2_VECTOR_COSINE;
  for (i = 0U; i < VECTOR_COUNT; i++) {
    double squared_norm = 0.0;
    assert_true(snprintf(ids[i], sizeof(ids[i]), "passage-%04zu", i) > 0);
    passages[i].id = bytes(ids[i]); passages[i].parent_document_id = bytes("doc");
    passages[i].text = bytes("recall"); passages[i].ordinal = (uint32_t)i;
    passages[i].end_char = 6U;
    for (j = 0U; j < DIMENSIONS; j++) {
      float value = (float)((int32_t)(random_u32(&rng) & 0xffffU) - 32768) / 32768.0f;
      values[i * DIMENSIONS + j] = value;
      squared_norm += (double)value * (double)value;
    }
    assert_true(squared_norm > 0.0);
    for (j = 0U; j < DIMENSIONS; j++)
      values[i * DIMENSIONS + j] = (float)(values[i * DIMENSIONS + j] / sqrt(squared_norm));
  }
  fd = mkstemp(vectors_path); assert_true(fd >= 0); close(fd);
  fd = mkstemp(ann_path); assert_true(fd >= 0); close(fd); unlink(ann_path);
  embeddings.values = values; embeddings.input_count = VECTOR_COUNT;
  embeddings.dimensions = DIMENSIONS;
  assert_int_equal(YAP_V2_vectors_write(vectors_path, 1U, &cfg, passages, VECTOR_COUNT,
                                        &embeddings, &vector_component), YAP_V2_OK);
  YAP_V2_vector_segment_init(&vectors);
  assert_int_equal(YAP_V2_vector_segment_open(vectors_path, 1U, &cfg, &vectors, NULL),
                   YAP_V2_OK);
  assert_int_equal(YAP_V2_ann_build_save(ann_path, &vectors, 16U, 128U, 128U, NULL),
                   YAP_ANN_OK);
  YAP_V2_ann_segment_init(&ann);
  assert_int_equal(YAP_V2_ann_view(ann_path, &vectors, 128U, &ann, NULL), YAP_ANN_OK);
  for (query = 0U; query < QUERY_COUNT; query++) {
    const float *query_vector = &values[(query * 11U) * DIMENSIONS];
    assert_int_equal(YAP_V2_vector_segment_search(&vectors, query_vector, DIMENSIONS, TOP_K,
                                                  exact, TOP_K, &exact_count), YAP_VECTOR_OK);
    assert_int_equal(YAP_V2_ann_search(&ann, query_vector, DIMENSIONS, TOP_K, approximate,
                                       TOP_K, &approximate_count), YAP_VECTOR_OK);
    assert_int_equal(exact_count, TOP_K); assert_int_equal(approximate_count, TOP_K);
    for (i = 0U; i < approximate_count; i++)
      for (j = 0U; j < exact_count; j++)
        if (approximate[i].ordinal == exact[j].ordinal) { matches++; break; }
  }
  recall = (double)matches / (double)(QUERY_COUNT * TOP_K);
  print_message("ann_recall_at_10\t%.6f\n", recall);
  assert_true(recall >= 0.95);
  YAP_V2_ann_segment_close(&ann); YAP_V2_vector_segment_close(&vectors);
  assert_int_equal(unlink(ann_path), 0); assert_int_equal(unlink(vectors_path), 0);
  free(ids); free(values); free(passages);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_build_save_view_search_and_fallback),
    cmocka_unit_test(test_cross_segment_candidates),
    cmocka_unit_test(test_ann_recall_at_10_against_exact_ground_truth)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
