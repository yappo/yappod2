#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_manifest_v2.h"
#include "yappo_query_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)}; return view;
}

static void add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                          const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  assert_int_equal(YAP_V2_segment_descriptor_add_component(segment, component), YAP_V2_OK);
}

static void test_hybrid_filter_and_scope_aggregation(void **state) {
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_DOCUMENT_VIEW documents[2];
  YAP_V2_PASSAGE_VIEW passages[2];
  YAP_V2_COMPONENT_DESCRIPTOR lexical_components[3], vectors_component, metadata_component;
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_MANIFEST manifest;
  YAP_EMBEDDING_RESULT embeddings;
  float values[4] = {1.0f, 0.0f, 0.0f, 1.0f}, query_vector[2] = {1.0f, 0.0f};
  char segments_dir[PATH_MAX], segment_dir[PATH_MAX], path[PATH_MAX], manifest_path[PATH_MAX];
  YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  YAP_V2_LEXICAL_SEGMENT lexical;
  YAP_V2_VECTOR_SEGMENT vectors;
  YAP_V2_ANN_SEGMENT ann;
  YAP_V2_METADATA_INDEX metadata;
  YAP_V2_QUERY_SEGMENT runtime;
  YAP_V2_QUERY_REQUEST request;
  YAP_V2_QUERY_HIT hits[2];
  size_t hit_count;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  YAP_V2_config_init(&config); config.vector_metric = YAP_V2_VECTOR_COSINE;
  config.vector_dimensions = 2U; strcpy(config.vector_model_id, "embed-v1");
  strcpy(config.filterable_fields[0], "category"); config.filterable_field_count = 1U;
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-fruit"); documents[0].url = bytes("https://e.test/fruit");
  documents[0].title = bytes("apple guide"); documents[0].body = bytes("fresh apple");
  documents[0].metadata_json = bytes("{\"category\":\"fruit\"}");
  documents[1].id = bytes("doc-tech"); documents[1].url = bytes("https://e.test/tech");
  documents[1].title = bytes("apple computer"); documents[1].body = bytes("technology");
  documents[1].metadata_json = bytes("{\"category\":\"tech\"}");
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes("passage-fruit"); passages[0].parent_document_id = documents[0].id;
  passages[0].text = bytes("fresh apple"); passages[0].end_char = 11U;
  passages[1].id = bytes("passage-tech"); passages[1].parent_document_id = documents[1].id;
  passages[1].text = bytes("apple computer"); passages[1].end_char = 14U;
  assert_int_equal(ytest_path_join(segments_dir, sizeof(segments_dir), env.tmp_root, "segments"), 0);
  assert_int_equal(ytest_path_join(segment_dir, sizeof(segment_dir), segments_dir, "seg-1"), 0);
  assert_int_equal(ytest_mkdir_p(segment_dir, 0700), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir, "documents.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "seg-1", 1U, documents, 2U, passages, 2U,
                                        &descriptor), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(segment_dir, 1U, documents, 2U, passages, 2U,
                                        lexical_components), YAP_V2_OK);
  add_component(&descriptor, &lexical_components[0]); add_component(&descriptor, &lexical_components[1]);
  add_component(&descriptor, &lexical_components[2]);
  embeddings.values = values; embeddings.input_count = 2U; embeddings.dimensions = 2U;
  assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir, "vectors.yap2"), 0);
  assert_int_equal(YAP_V2_vectors_write(path, 1U, &config, passages, 2U, &embeddings,
                                        &vectors_component), YAP_V2_OK);
  add_component(&descriptor, &vectors_component);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir, "metadata.yap2"), 0);
  assert_int_equal(YAP_V2_metadata_write(path, 1U, &config, documents, 2U,
                                         &metadata_component), YAP_V2_OK);
  add_component(&descriptor, &metadata_component);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &descriptor), YAP_V2_OK);
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root,
                                   "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_save_atomic(manifest_path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
  YAP_V2_snapshot_manager_init(&manager);
  assert_int_equal(YAP_V2_snapshot_manager_open(&manager, env.tmp_root, manifest_path, &config),
                   YAP_V2_OK);
  snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(snapshot);
  YAP_V2_lexical_segment_init(&lexical);
  assert_int_equal(YAP_V2_lexical_segment_open(segment_dir, 1U, &lexical), YAP_V2_OK);
  YAP_V2_vector_segment_init(&vectors);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir, "vectors.yap2"), 0);
  assert_int_equal(YAP_V2_vector_segment_open(path, 1U, &config, &vectors, NULL), YAP_V2_OK);
  YAP_V2_ann_segment_init(&ann); ann.vectors = &vectors;
  YAP_V2_metadata_index_init(&metadata);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir, "metadata.yap2"), 0);
  assert_int_equal(YAP_V2_metadata_read(path, 1U, &config, &metadata, NULL), YAP_V2_OK);
  runtime.lexical = &lexical; runtime.vector = &ann; runtime.metadata = &metadata;
  YAP_V2_query_request_init(&request); request.query = bytes("apple");
  request.query_vector = query_vector; request.query_dimensions = 2U;
  request.filter_json = bytes("{\"eq\":{\"field\":\"category\",\"value\":\"fruit\"}}");
  request.top_k = 1U; request.candidate_k = 1U;
  assert_int_equal(YAP_V2_query_execute(snapshot, &runtime, 1U, &request, hits, 2U, &hit_count),
                   YAP_V2_OK);
  assert_int_equal(hit_count, 1U); assert_memory_equal(hits[0].id.data, "doc-fruit", 9U);
  assert_true(hits[0].lexical_score > 0.0); assert_true(hits[0].vector_score > 0.0);
  request.mode = YAP_V2_SEARCH_LEXICAL;
  assert_int_equal(YAP_V2_query_execute(snapshot, &runtime, 1U, &request, hits, 2U, &hit_count),
                   YAP_V2_OK);
  assert_int_equal(hit_count, 1U); assert_true(hits[0].lexical_score > 0.0);
  assert_float_equal(hits[0].vector_score, 0.0, 0.0);
  request.mode = YAP_V2_SEARCH_VECTOR;
  assert_int_equal(YAP_V2_query_execute(snapshot, &runtime, 1U, &request, hits, 2U, &hit_count),
                   YAP_V2_OK);
  assert_int_equal(hit_count, 1U); assert_true(hits[0].vector_score > 0.0);
  assert_float_equal(hits[0].lexical_score, 0.0, 0.0);
  request.mode = YAP_V2_SEARCH_HYBRID;
  request.scope = YAP_V2_SEARCH_PASSAGES;
  assert_int_equal(YAP_V2_query_execute(snapshot, &runtime, 1U, &request, hits, 2U, &hit_count),
                   YAP_V2_OK);
  assert_int_equal(hit_count, 1U); assert_memory_equal(hits[0].id.data, "passage-fruit", 13U);
  assert_memory_equal(hits[0].parent_document_id.data, "doc-fruit", 9U);
  YAP_V2_metadata_index_free(&metadata); YAP_V2_vector_segment_close(&vectors);
  YAP_V2_lexical_segment_close(&lexical); YAP_V2_snapshot_release(snapshot);
  YAP_V2_snapshot_manager_close(&manager); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {cmocka_unit_test(test_hybrid_filter_and_scope_aggregation)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
