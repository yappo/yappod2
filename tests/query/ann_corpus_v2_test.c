#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "query/yappo_query_v2.h"
#include "storage/yappo_manifest_v2.h"
#include "test_env.h"
#include "test_fs.h"

enum { SEGMENT_COUNT = 1000, DIMENSIONS = 4, TOP_K = 10 };

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)};
  return view;
}

static void test_corpus_replaces_segment_fanout_and_preserves_results(void **state) {
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  YAP_V2_VECTOR_SEGMENT *vectors;
  YAP_V2_ANN_SEGMENT *ann;
  YAP_V2_QUERY_SEGMENT *query_segments;
  YAP_V2_ANN_CORPUS corpus;
  YAP_V2_ANN_CORPUS loaded;
  YAP_V2_ANN_QUERY_PLAN plan;
  YAP_V2_ANN_QUERY_PLAN loaded_plan;
  YAP_V2_QUERY_CORPUS_STATS corpus_stats;
  YAP_V2_QUERY_REQUEST request;
  YAP_V2_QUERY_HIT baseline[TOP_K], optimized[TOP_K], persisted[TOP_K];
  YAP_V2_QUERY_STATS baseline_stats, optimized_stats, persisted_stats;
  char ids[SEGMENT_COUNT][32], parents[SEGMENT_COUNT][32];
  float values[SEGMENT_COUNT][DIMENSIONS];
  float query_vector[DIMENSIONS] = {1.0f, 0.0f, 0.0f, 0.0f};
  char segments_dir[PATH_MAX], manifest_path[PATH_MAX], ann_cache_path[PATH_MAX];
  size_t s, i, baseline_count = 0U, optimized_count = 0U, persisted_count = 0U;
  size_t matches = 0U;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  YAP_V2_config_init(&config);
  config.vector_metric = YAP_V2_VECTOR_COSINE;
  config.vector_dimensions = DIMENSIONS;
  strcpy(config.vector_model_id, "ann-corpus-test");
  assert_int_equal(ytest_path_join(segments_dir, sizeof(segments_dir),
                                   env.tmp_root, "segments"), 0);
  assert_int_equal(ytest_mkdir_p(segments_dir, 0700), 0);
  YAP_V2_manifest_init(&manifest);
  manifest.generation = 1U;
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  vectors = calloc(SEGMENT_COUNT, sizeof(*vectors));
  ann = calloc(SEGMENT_COUNT, sizeof(*ann));
  query_segments = calloc(SEGMENT_COUNT, sizeof(*query_segments));
  assert_non_null(vectors); assert_non_null(ann); assert_non_null(query_segments);
  for (s = 0U; s < SEGMENT_COUNT; s++) {
    YAP_V2_DOCUMENT_VIEW document;
    YAP_V2_PASSAGE_VIEW passage;
    YAP_V2_COMPONENT_DESCRIPTOR vector_component;
    YAP_V2_SEGMENT_DESCRIPTOR descriptor;
    YAP_EMBEDDING_RESULT embedding;
    char segment_id[32], segment_dir[PATH_MAX], path[PATH_MAX];
    memset(&document, 0, sizeof(document));
    memset(&passage, 0, sizeof(passage));
    snprintf(segment_id, sizeof(segment_id), "seg-%04zu", s);
    snprintf(ids[s], sizeof(ids[s]), "passage-%04zu", s);
    snprintf(parents[s], sizeof(parents[s]), "document-%04zu", s);
    document.id = bytes(parents[s]); document.title = bytes(parents[s]);
    document.body = bytes(ids[s]); document.metadata_json = bytes("{}");
    passage.id = bytes(ids[s]); passage.parent_document_id = document.id;
    passage.text = document.body; passage.end_char = passage.text.len;
    values[s][0] = 1.0f - (float)s / 100.0f;
    values[s][1] = (float)s / 100.0f;
    values[s][2] = (float)(s % 3U) / 100.0f;
    values[s][3] = (float)(s % 5U) / 100.0f;
    assert_int_equal(ytest_path_join(segment_dir, sizeof(segment_dir),
                                     segments_dir, segment_id), 0);
    assert_int_equal(ytest_mkdir_p(segment_dir, 0700), 0);
    assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir,
                                     "documents.yap2"), 0);
    assert_int_equal(YAP_V2_segment_write(path, segment_id, 1U, &document, 1U,
                                          &passage, 1U, &descriptor), YAP_V2_OK);
    embedding.values = values[s]; embedding.input_count = 1U;
    embedding.dimensions = DIMENSIONS;
    assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir,
                                     "vectors.yap2"), 0);
    assert_int_equal(YAP_V2_vectors_write(path, 1U, &config, &passage, 1U,
                                          &embedding, &vector_component), YAP_V2_OK);
    assert_int_equal(YAP_V2_segment_descriptor_add_component(&descriptor,
                                                              &vector_component), YAP_V2_OK);
    assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &descriptor), YAP_V2_OK);
  }
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root,
                                   "manifest.yap2"), 0);
  assert_int_equal(YAP_V2_manifest_save_atomic(manifest_path, &manifest), YAP_V2_OK);
  YAP_V2_snapshot_manager_init(&manager);
  assert_int_equal(YAP_V2_snapshot_manager_open(&manager, env.tmp_root, manifest_path,
                                                &config), YAP_V2_OK);
  snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(snapshot);
  for (s = 0U; s < SEGMENT_COUNT; s++) {
    char segment_dir[PATH_MAX], path[PATH_MAX];
    assert_int_equal(ytest_path_join(segment_dir, sizeof(segment_dir), segments_dir,
                                     manifest.segments[s].id), 0);
    assert_int_equal(ytest_path_join(path, sizeof(path), segment_dir,
                                     "vectors.yap2"), 0);
    YAP_V2_vector_segment_init(&vectors[s]);
    assert_int_equal(YAP_V2_vector_segment_open(path, 1U, &config, &vectors[s], NULL),
                     YAP_V2_OK);
    YAP_V2_ann_segment_init(&ann[s]); ann[s].vectors = &vectors[s];
    memset(&query_segments[s], 0, sizeof(query_segments[s]));
    query_segments[s].vector = &ann[s];
  }
  YAP_V2_ann_corpus_init(&corpus); YAP_V2_ann_query_plan_init(&plan);
  YAP_V2_ann_corpus_init(&loaded); YAP_V2_ann_query_plan_init(&loaded_plan);
  assert_int_equal(YAP_V2_ann_corpus_build(&manifest, snapshot, ann, SEGMENT_COUNT,
                                           &corpus), YAP_V2_OK);
  assert_int_equal(corpus.vector_count, SEGMENT_COUNT);
  assert_int_equal(YAP_V2_ann_query_plan_build(&corpus, &manifest, &plan), YAP_V2_OK);
  assert_int_equal(plan.delta_segment_count, 0U);
  assert_int_equal(YAP_V2_query_corpus_stats_build(snapshot, query_segments,
                                                   SEGMENT_COUNT, &corpus_stats), YAP_V2_OK);
  YAP_V2_query_request_init(&request);
  request.mode = YAP_V2_SEARCH_VECTOR; request.scope = YAP_V2_SEARCH_PASSAGES;
  request.query_vector = query_vector; request.query_dimensions = DIMENSIONS;
  request.top_k = TOP_K; request.candidate_k = TOP_K;
  assert_int_equal(YAP_V2_query_execute_with_ann(
    snapshot, query_segments, SEGMENT_COUNT, &corpus_stats, NULL, NULL, &request,
    baseline, TOP_K, &baseline_count, &baseline_stats), YAP_V2_OK);
  assert_int_equal(YAP_V2_query_execute_with_ann(
    snapshot, query_segments, SEGMENT_COUNT, &corpus_stats, &corpus, &plan, &request,
    optimized, TOP_K, &optimized_count, &optimized_stats), YAP_V2_OK);
  assert_int_equal(baseline_count, TOP_K); assert_int_equal(optimized_count, TOP_K);
  assert_int_equal(baseline_stats.delta_search_calls, SEGMENT_COUNT);
  assert_int_equal(baseline_stats.base_search_calls, 0U);
  assert_int_equal(optimized_stats.base_search_calls, 1U);
  assert_int_equal(optimized_stats.delta_search_calls, 0U);
  for (i = 0U; i < optimized_count; i++) {
    size_t j;
    for (j = 0U; j < baseline_count; j++)
      if (optimized[i].id.len == baseline[j].id.len &&
          memcmp(optimized[i].id.data, baseline[j].id.data, optimized[i].id.len) == 0) {
        matches++;
        break;
      }
  }
  assert_true(matches >= 9U);
  {
    usearch_index_t published_index = corpus.index.index;
    assert_int_equal(YAP_V2_ann_corpus_build(&manifest, snapshot, ann,
                                             SEGMENT_COUNT - 1U, &corpus),
                     YAP_V2_INVALID_ARGUMENT);
    assert_ptr_equal(corpus.index.index, published_index);
    assert_int_equal(corpus.vector_count, SEGMENT_COUNT);
  }
  assert_int_equal(YAP_V2_ann_corpus_save_cache(env.tmp_root, &corpus), YAP_V2_OK);
  assert_int_equal(YAP_V2_ann_corpus_load_cache(env.tmp_root, &config, &manifest,
                                               &loaded), YAP_V2_OK);
  assert_int_equal(loaded.generation, corpus.generation);
  assert_int_equal(loaded.vector_count, corpus.vector_count);
  assert_int_equal(YAP_V2_ann_query_plan_build(&loaded, &manifest, &loaded_plan),
                   YAP_V2_OK);
  assert_int_equal(YAP_V2_query_execute_with_ann(
    snapshot, query_segments, SEGMENT_COUNT, &corpus_stats, &loaded, &loaded_plan,
    &request, persisted, TOP_K, &persisted_count, &persisted_stats), YAP_V2_OK);
  assert_int_equal(persisted_count, optimized_count);
  assert_int_equal(persisted_stats.base_search_calls, 1U);
  assert_int_equal(persisted_stats.delta_search_calls, 0U);
  for (i = 0U; i < persisted_count; i++)
    assert_memory_equal(persisted[i].id.data, optimized[i].id.data,
                        optimized[i].id.len);
  YAP_V2_ann_query_plan_free(&loaded_plan);
  YAP_V2_ann_corpus_free(&loaded);
  assert_int_equal(ytest_path_join(ann_cache_path, sizeof(ann_cache_path),
                                   env.tmp_root, "ann-base.usearch"), 0);
  {
    FILE *cache_file = fopen(ann_cache_path, "r+b");
    int first;
    assert_non_null(cache_file);
    first = fgetc(cache_file);
    assert_true(first != EOF);
    assert_int_equal(fseek(cache_file, 0L, SEEK_SET), 0);
    assert_int_equal(fputc(first ^ 0xff, cache_file), first ^ 0xff);
    assert_int_equal(fclose(cache_file), 0);
  }
  assert_int_equal(YAP_V2_ann_corpus_load_cache(env.tmp_root, &config, &manifest,
                                               &loaded), YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_ann_query_plan_free(&plan); YAP_V2_ann_corpus_free(&corpus);
  for (s = 0U; s < SEGMENT_COUNT; s++) {
    YAP_V2_ann_segment_close(&ann[s]);
    YAP_V2_vector_segment_close(&vectors[s]);
  }
  free(query_segments); free(ann); free(vectors);
  YAP_V2_snapshot_release(snapshot); YAP_V2_snapshot_manager_close(&manager);
  YAP_V2_manifest_free(&manifest); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_corpus_replaces_segment_fanout_and_preserves_results)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
