#include "server/yappo_http_v2.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "config/yappo_config_v2.h"
#include "storage/yappo_manifest_v2.h"
#include "query/yappo_query_v2.h"
#include "query/yappo_retrieve_v2.h"
#include "query/yappo_snippet_v2.h"
#include "common/yappo_unicode.h"
#include "indexing/yappo_update_v2.h"

#define YAP_V2_CURSOR_MAX_OFFSET 10000U
#define YAP_V2_HTTP_SNIPPET_GRAPHEMES 180U
#define YAP_V2_ANN_MAX_DELTA_SEGMENTS 8U

typedef struct { const char *key; size_t key_len; yyjson_val *value; } JSON_PAIR;

static int compare_pairs(const void *left, const void *right) {
  const JSON_PAIR *a = left, *b = right; size_t common = a->key_len < b->key_len ? a->key_len : b->key_len;
  int compared = memcmp(a->key, b->key, common);
  if (compared != 0) return compared;
  return a->key_len < b->key_len ? -1 : a->key_len > b->key_len;
}

static yyjson_mut_val *canonical_json_copy(yyjson_mut_doc *doc, yyjson_val *value) {
  if (yyjson_is_obj(value)) {
    yyjson_mut_val *object = yyjson_mut_obj(doc); yyjson_obj_iter iterator;
    JSON_PAIR *pairs; yyjson_val *key; size_t count = yyjson_obj_size(value), i = 0U;
    if (object == NULL) return NULL;
    pairs = count == 0U ? NULL : malloc(sizeof(*pairs) * count);
    if (count != 0U && pairs == NULL) return NULL;
    iterator = yyjson_obj_iter_with(value);
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
      pairs[i].key = yyjson_get_str(key); pairs[i].key_len = yyjson_get_len(key);
      pairs[i++].value = yyjson_obj_iter_get_val(key);
    }
    if (count > 1U) qsort(pairs, count, sizeof(*pairs), compare_pairs);
    for (i = 0U; i < count; i++) {
      yyjson_mut_val *child = canonical_json_copy(doc, pairs[i].value);
      if (child == NULL || !yyjson_mut_obj_add_val(doc, object, pairs[i].key, child)) {
        free(pairs); return NULL;
      }
    }
    free(pairs); return object;
  }
  if (yyjson_is_arr(value)) {
    yyjson_mut_val *array = yyjson_mut_arr(doc); yyjson_arr_iter iterator; yyjson_val *item;
    if (array == NULL) {
      return NULL;
    }
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      yyjson_mut_val *child = canonical_json_copy(doc, item);
      if (child == NULL || !yyjson_mut_arr_append(array, child)) return NULL;
    }
    return array;
  }
  return yyjson_val_mut_copy(doc, value);
}

typedef struct {
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  YAP_V2_QUERY_SEGMENT *query;
  YAP_V2_QUERY_CORPUS_STATS corpus_stats;
  YAP_V2_LEXICAL_SEGMENT *lexical;
  YAP_V2_VECTOR_SEGMENT *vectors;
  YAP_V2_ANN_SEGMENT *ann;
  YAP_V2_ANN_CORPUS ann_corpus;
  YAP_V2_ANN_QUERY_PLAN ann_plan;
  pthread_mutex_t ann_stats_lock;
  YAP_V2_QUERY_STATS ann_stats;
  uint64_t ann_rebuilds;
  uint64_t ann_rebuild_failures;
  int ann_stats_initialized;
  YAP_V2_METADATA_INDEX *metadata;
  size_t count;
} HTTP_RUNTIME;

typedef struct {
  pthread_rwlock_t lock;
  pthread_mutex_t update_lock;
  pthread_mutex_t ann_maintenance_lock;
  char *index_dir;
  HTTP_RUNTIME *current;
} HTTP_RUNTIME_STATE;

static int path_join(char *out, size_t capacity, const char *a, const char *b) {
  int written = snprintf(out, capacity, "%s/%s", a, b);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static uint64_t saturated_add_u64(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void runtime_record_ann_stats(HTTP_RUNTIME *runtime,
                                     const YAP_V2_QUERY_STATS *stats) {
  if (runtime == NULL || stats == NULL || !runtime->ann_stats_initialized) return;
  pthread_mutex_lock(&runtime->ann_stats_lock);
  runtime->ann_stats.base_search_calls = saturated_add_u64(
    runtime->ann_stats.base_search_calls, stats->base_search_calls);
  runtime->ann_stats.delta_search_calls = saturated_add_u64(
    runtime->ann_stats.delta_search_calls, stats->delta_search_calls);
  runtime->ann_stats.retry_search_calls = saturated_add_u64(
    runtime->ann_stats.retry_search_calls, stats->retry_search_calls);
  runtime->ann_stats.candidates_examined = saturated_add_u64(
    runtime->ann_stats.candidates_examined, stats->candidates_examined);
  runtime->ann_stats.candidates_rejected = saturated_add_u64(
    runtime->ann_stats.candidates_rejected, stats->candidates_rejected);
  pthread_mutex_unlock(&runtime->ann_stats_lock);
}

static const YAP_V2_COMPONENT_DESCRIPTOR *component(const YAP_V2_SEGMENT_DESCRIPTOR *segment,
                                                     uint32_t type) {
  size_t i;
  for (i = 0U; i < segment->component_count; i++)
    if (segment->components[i].file_type == type) return &segment->components[i];
  return NULL;
}

static void runtime_segment_close(YAP_V2_LEXICAL_SEGMENT *lexical,
                                  YAP_V2_VECTOR_SEGMENT *vectors,
                                  YAP_V2_ANN_SEGMENT *ann,
                                  YAP_V2_METADATA_INDEX *metadata) {
  YAP_V2_ann_segment_close(ann);
  YAP_V2_vector_segment_close(vectors);
  YAP_V2_lexical_segment_close(lexical);
  YAP_V2_metadata_index_free(metadata);
}

static int runtime_segment_open(
  const char *index_dir, const YAP_V2_CONFIG *config,
  const YAP_V2_SEGMENT_DESCRIPTOR *descriptor,
  YAP_V2_QUERY_SEGMENT *query, YAP_V2_LEXICAL_SEGMENT *lexical,
  YAP_V2_VECTOR_SEGMENT *vectors, YAP_V2_ANN_SEGMENT *ann,
  YAP_V2_METADATA_INDEX *metadata) {
  const YAP_V2_COMPONENT_DESCRIPTOR *ann_file;
  char segment_dir[4096], file_path[4096];
  int written;
  int status = YAP_V2_OK;
  memset(query, 0, sizeof(*query));
  YAP_V2_lexical_segment_init(lexical);
  YAP_V2_vector_segment_init(vectors);
  YAP_V2_ann_segment_init(ann);
  YAP_V2_metadata_index_init(metadata);
  written = snprintf(segment_dir, sizeof(segment_dir), "%s/segments/%s",
                     index_dir, descriptor->id);
  if (written < 0 || (size_t)written >= sizeof(segment_dir))
    return YAP_V2_INVALID_ARGUMENT;
  if (component(descriptor, YAP_V2_FILE_TERMS) != NULL) {
    status = YAP_V2_lexical_segment_open(segment_dir, 0U, lexical);
    if (status != YAP_V2_OK) return status;
    query->lexical = lexical;
  }
  if (component(descriptor, YAP_V2_FILE_VECTORS) != NULL) {
    if (path_join(file_path, sizeof(file_path), segment_dir, "vectors.yap2") != 0)
      return YAP_V2_INVALID_ARGUMENT;
    status = YAP_V2_vector_segment_open(file_path, 0U, config, vectors, NULL);
    if (status != YAP_V2_OK) return status;
    ann->vectors = vectors;
    ann_file = component(descriptor, YAP_V2_FILE_ANN);
    if (ann_file != NULL) {
      if (path_join(file_path, sizeof(file_path), segment_dir, ann_file->name) != 0)
        return YAP_V2_INVALID_ARGUMENT;
      status = YAP_V2_ann_view(file_path, vectors, 64U, ann, NULL);
      if (status != YAP_ANN_OK) return YAP_V2_CONFLICT;
    }
    query->vector = ann;
  }
  if (component(descriptor, YAP_V2_FILE_METADATA) != NULL) {
    if (path_join(file_path, sizeof(file_path), segment_dir, "metadata.yap2") != 0)
      return YAP_V2_INVALID_ARGUMENT;
    status = YAP_V2_metadata_read(file_path, 0U, config, metadata, NULL);
    if (status != YAP_V2_OK) return status;
    query->metadata = metadata;
  }
  return YAP_V2_OK;
}

static void runtime_close(HTTP_RUNTIME *runtime) {
  size_t i;
  if (runtime == NULL) return;
  if (runtime->ann != NULL && runtime->vectors != NULL &&
      runtime->lexical != NULL && runtime->metadata != NULL)
    for (i = 0U; i < runtime->count; i++)
      runtime_segment_close(&runtime->lexical[i], &runtime->vectors[i],
                            &runtime->ann[i], &runtime->metadata[i]);
  free(runtime->query); free(runtime->lexical); free(runtime->vectors);
  free(runtime->ann); free(runtime->metadata);
  YAP_V2_ann_query_plan_free(&runtime->ann_plan);
  YAP_V2_ann_corpus_free(&runtime->ann_corpus);
  if (runtime->snapshot != NULL) YAP_V2_snapshot_release(runtime->snapshot);
  YAP_V2_snapshot_manager_close(&runtime->manager);
  YAP_V2_manifest_free(&runtime->manifest);
  if (runtime->ann_stats_initialized) pthread_mutex_destroy(&runtime->ann_stats_lock);
  memset(runtime, 0, sizeof(*runtime));
}

static int runtime_open_once(HTTP_RUNTIME *runtime, const char *index_dir) {
  char config_path[4096], manifest_path[4096];
  char error[256]; size_t i; int status;
  memset(runtime, 0, sizeof(*runtime));
  YAP_V2_ann_corpus_init(&runtime->ann_corpus);
  YAP_V2_ann_query_plan_init(&runtime->ann_plan);
  if (pthread_mutex_init(&runtime->ann_stats_lock, NULL) != 0) return YAP_V2_IO_ERROR;
  runtime->ann_stats_initialized = 1;
  YAP_V2_manifest_init(&runtime->manifest); YAP_V2_snapshot_manager_init(&runtime->manager);
  if (path_join(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      path_join(manifest_path, sizeof(manifest_path), index_dir, "manifest.yap2") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_config_load(config_path, &runtime->config, error, sizeof(error));
  if (status != YAP_V2_OK) return status;
  status = YAP_V2_manifest_load_for_config(manifest_path, &runtime->config, &runtime->manifest);
  if (status != YAP_V2_OK) return status;
  status = YAP_V2_snapshot_manager_open(&runtime->manager, index_dir, manifest_path, &runtime->config);
  if (status != YAP_V2_OK) return status;
  runtime->snapshot = YAP_V2_snapshot_acquire(&runtime->manager);
  runtime->count = runtime->manifest.segment_count;
  if (runtime->snapshot == NULL || runtime->count == 0U) return YAP_V2_CONFLICT;
  runtime->query = calloc(runtime->count, sizeof(*runtime->query));
  runtime->lexical = calloc(runtime->count, sizeof(*runtime->lexical));
  runtime->vectors = calloc(runtime->count, sizeof(*runtime->vectors));
  runtime->ann = calloc(runtime->count, sizeof(*runtime->ann));
  runtime->metadata = calloc(runtime->count, sizeof(*runtime->metadata));
  if (runtime->query == NULL || runtime->lexical == NULL || runtime->vectors == NULL ||
      runtime->ann == NULL || runtime->metadata == NULL) return YAP_V2_ALLOCATION_FAILED;
  for (i = 0U; i < runtime->count; i++) {
    status = runtime_segment_open(
      index_dir, &runtime->config, &runtime->manifest.segments[i],
      &runtime->query[i], &runtime->lexical[i], &runtime->vectors[i],
      &runtime->ann[i], &runtime->metadata[i]);
    if (status != YAP_V2_OK) return status;
  }
  status = YAP_V2_query_corpus_stats_build(runtime->snapshot, runtime->query,
                                           runtime->count, &runtime->corpus_stats);
  if (status == YAP_V2_OK && runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED) {
    int cache_status = YAP_V2_ann_corpus_load_cache(index_dir, &runtime->config,
                                                    &runtime->manifest,
                                                    &runtime->ann_corpus);
    if (cache_status != YAP_V2_OK) {
      status = YAP_V2_ann_corpus_build(&runtime->manifest, runtime->snapshot,
                                       runtime->ann, runtime->count,
                                       &runtime->ann_corpus);
      if (status == YAP_V2_OK) {
        runtime->ann_rebuilds = saturated_add_u64(runtime->ann_rebuilds, 1U);
        if (runtime->ann_corpus.vector_count > 0U)
          (void)YAP_V2_ann_corpus_save_cache(index_dir, &runtime->ann_corpus);
      }
    }
  }
  if (status == YAP_V2_OK && runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED)
    status = YAP_V2_ann_query_plan_build(&runtime->ann_corpus,
                                         &runtime->manifest,
                                         &runtime->ann_plan);
  return status;
}

static int runtime_open(HTTP_RUNTIME *runtime, const char *index_dir) {
  size_t attempt;
  int status = YAP_V2_CONFLICT;
  for (attempt = 0U; attempt < 4U; attempt++) {
    status = runtime_open_once(runtime, index_dir);
    if (status != YAP_V2_OK) {
      runtime_close(runtime);
      return status;
    }
    if (YAP_V2_snapshot_generation(runtime->snapshot) == runtime->manifest.generation)
      return YAP_V2_OK;
    runtime_close(runtime);
  }
  return YAP_V2_CONFLICT;
}

static int runtime_reload_manifest(HTTP_RUNTIME *runtime,
                                   const char *index_dir) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_MANIFEST_SEGMENT_MAP previous_segments;
  YAP_V2_QUERY_SEGMENT *query = NULL;
  YAP_V2_LEXICAL_SEGMENT *lexical = NULL;
  YAP_V2_VECTOR_SEGMENT *vectors = NULL;
  YAP_V2_ANN_SEGMENT *ann = NULL;
  YAP_V2_METADATA_INDEX *metadata = NULL;
  YAP_V2_QUERY_CORPUS_STATS corpus_stats;
  YAP_V2_ANN_QUERY_PLAN ann_plan;
  YAP_V2_SEARCH_SNAPSHOT *snapshot = NULL;
  unsigned char *reused = NULL;
  unsigned char *opened = NULL;
  char manifest_path[4096];
  size_t i;
  int status, changed = 0;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_manifest_segment_map_init(&previous_segments);
  YAP_V2_ann_query_plan_init(&ann_plan);
  if (path_join(manifest_path, sizeof(manifest_path), index_dir, "manifest.yap2") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_manifest_load_for_config(manifest_path, &runtime->config, &manifest);
  if (status != YAP_V2_OK) goto done;
  if (manifest.generation < runtime->manifest.generation) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  if (manifest.generation == runtime->manifest.generation) {
    status = manifest.segment_count == runtime->manifest.segment_count ?
             YAP_V2_OK : YAP_V2_CONFLICT;
    for (i = 0U; status == YAP_V2_OK && i < manifest.segment_count; i++)
      if (!YAP_V2_segment_descriptor_equal(
            &manifest.segments[i], &runtime->manifest.segments[i]))
        status = YAP_V2_CONFLICT;
    goto done;
  }
  if (manifest.segment_count == 0U) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  status = YAP_V2_manifest_segment_map_build(&previous_segments,
                                             &runtime->manifest);
  if (status != YAP_V2_OK) goto done;
  query = calloc(manifest.segment_count, sizeof(*query));
  lexical = calloc(manifest.segment_count, sizeof(*lexical));
  vectors = calloc(manifest.segment_count, sizeof(*vectors));
  ann = calloc(manifest.segment_count, sizeof(*ann));
  metadata = calloc(manifest.segment_count, sizeof(*metadata));
  opened = calloc(manifest.segment_count, sizeof(*opened));
  reused = calloc(runtime->count, sizeof(*reused));
  if (query == NULL || lexical == NULL || vectors == NULL || ann == NULL ||
      metadata == NULL || opened == NULL ||
      (runtime->count > 0U && reused == NULL)) {
    status = YAP_V2_ALLOCATION_FAILED;
    goto done;
  }
  for (i = 0U; status == YAP_V2_OK && i < manifest.segment_count; i++) {
    size_t old_index = 0U;
    int found = YAP_V2_manifest_segment_map_find(
      &previous_segments, manifest.segments[i].id, &old_index);
    if (found == YAP_V2_OK &&
        YAP_V2_segment_descriptor_equal(
          &runtime->manifest.segments[old_index], &manifest.segments[i])) {
      query[i] = runtime->query[old_index];
      lexical[i] = runtime->lexical[old_index];
      vectors[i] = runtime->vectors[old_index];
      ann[i] = runtime->ann[old_index];
      metadata[i] = runtime->metadata[old_index];
      if (runtime->query[old_index].lexical != NULL)
        query[i].lexical = &lexical[i];
      if (runtime->query[old_index].vector != NULL) {
        ann[i].vectors = &vectors[i];
        query[i].vector = &ann[i];
      }
      if (runtime->query[old_index].metadata != NULL)
        query[i].metadata = &metadata[i];
      reused[old_index] = 1U;
    } else if (found == YAP_V2_OK || found == YAP_V2_NOT_FOUND) {
      opened[i] = 1U;
      status = runtime_segment_open(
        index_dir, &runtime->config, &manifest.segments[i], &query[i],
        &lexical[i], &vectors[i], &ann[i], &metadata[i]);
    } else {
      status = found;
    }
  }
  if (status == YAP_V2_OK) status = YAP_V2_snapshot_manager_reload(&runtime->manager, &changed);
  if (status == YAP_V2_OK) {
    snapshot = YAP_V2_snapshot_acquire(&runtime->manager);
    if (!YAP_V2_snapshot_matches_manifest(snapshot, &manifest))
      status = YAP_V2_CONFLICT;
  }
  if (status == YAP_V2_OK)
    status = YAP_V2_query_corpus_stats_build(snapshot, query,
                                             manifest.segment_count, &corpus_stats);
  if (status == YAP_V2_OK && runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED)
    status = YAP_V2_ann_query_plan_build(&runtime->ann_corpus, &manifest, &ann_plan);
  if (status == YAP_V2_OK) {
    for (i = 0U; i < runtime->count; i++)
      if (!reused[i])
        runtime_segment_close(&runtime->lexical[i], &runtime->vectors[i],
                              &runtime->ann[i], &runtime->metadata[i]);
    free(runtime->query); free(runtime->lexical); free(runtime->vectors);
    free(runtime->ann); free(runtime->metadata);
    YAP_V2_snapshot_release(runtime->snapshot);
    YAP_V2_manifest_free(&runtime->manifest);
    runtime->manifest = manifest; YAP_V2_manifest_init(&manifest);
    runtime->query = query; query = NULL;
    runtime->corpus_stats = corpus_stats;
    runtime->lexical = lexical; lexical = NULL;
    runtime->vectors = vectors; vectors = NULL;
    runtime->ann = ann; ann = NULL;
    runtime->metadata = metadata; metadata = NULL;
    runtime->snapshot = snapshot; snapshot = NULL;
    runtime->count = runtime->manifest.segment_count;
    YAP_V2_ann_query_plan_free(&runtime->ann_plan);
    runtime->ann_plan = ann_plan; YAP_V2_ann_query_plan_init(&ann_plan);
  }
done:
  if (status != YAP_V2_OK && opened != NULL)
    for (i = 0U; i < manifest.segment_count; i++)
      if (opened[i])
        runtime_segment_close(&lexical[i], &vectors[i], &ann[i],
                              &metadata[i]);
  free(query); free(lexical); free(vectors); free(ann); free(metadata);
  free(opened); free(reused);
  YAP_V2_snapshot_release(snapshot); YAP_V2_manifest_free(&manifest);
  YAP_V2_manifest_segment_map_free(&previous_segments);
  YAP_V2_ann_query_plan_free(&ann_plan);
  return status;
}

static int only_keys(yyjson_val *object, const char *const *allowed) {
  yyjson_obj_iter iterator; yyjson_val *key; size_t i;
  if (!yyjson_is_obj(object)) return 0;
  iterator = yyjson_obj_iter_with(object);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key); int found = 0;
    for (i = 0U; allowed[i] != NULL; i++) if (strcmp(name, allowed[i]) == 0) found = 1;
    if (!found) return 0;
  }
  return 1;
}

static int request_fingerprint(const YAP_V2_QUERY_REQUEST *request, yyjson_val *filter,
                               unsigned char output[32]) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); yyjson_mut_val *root, *vector;
  char *json; size_t json_bytes, i; int ok = 0;
  if (doc == NULL) return -1;
  root = yyjson_mut_obj(doc); vector = yyjson_mut_arr(doc); yyjson_mut_doc_set_root(doc, root);
  if (root == NULL || vector == NULL ||
      !yyjson_mut_obj_add_uint(doc, root, "mode", (uint64_t)request->mode) ||
      !yyjson_mut_obj_add_uint(doc, root, "scope", (uint64_t)request->scope) ||
      !yyjson_mut_obj_add_uint(doc, root, "operator", (uint64_t)request->query_operator) ||
      !yyjson_mut_obj_add_bool(doc, root, "phrase", request->phrase != 0) ||
      !yyjson_mut_obj_add_uint(doc, root, "limit", request->top_k) ||
      !yyjson_mut_obj_add_strncpy(doc, root, "query", request->query.data == NULL ? "" : (const char *)request->query.data,
                                  request->query.len)) goto done;
  for (i = 0U; i < request->query_dimensions; i++)
    if (!yyjson_mut_arr_add_real(doc, vector, request->query_vector[i])) goto done;
  if (!yyjson_mut_obj_add_val(doc, root, "vector", vector)) goto done;
  if (filter != NULL) {
    yyjson_mut_val *canonical = canonical_json_copy(doc, filter);
    if (canonical == NULL || !yyjson_mut_obj_add_val(doc, root, "filter", canonical)) goto done;
  } else if (!yyjson_mut_obj_add_null(doc, root, "filter")) goto done;
  json = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, NULL, &json_bytes, NULL);
  if (json == NULL) goto done;
  YAP_V2_sha256_bytes((const unsigned char *)json, json_bytes, output); free(json); ok = 1;
done:
  yyjson_mut_doc_free(doc); return ok ? 0 : -1;
}

static void hex_encode(const unsigned char digest[32], char output[65]) {
  static const char digits[] = "0123456789abcdef"; size_t i;
  for (i = 0U; i < 32U; i++) { output[i * 2U] = digits[digest[i] >> 4]; output[i * 2U + 1U] = digits[digest[i] & 15U]; }
  output[64] = '\0';
}

static int cursor_digest(uint64_t generation, const unsigned char query_digest[32], size_t offset,
                         char output[65]) {
  unsigned char material[48], digest[32]; size_t i;
  for (i = 0U; i < 8U; i++) material[i] = (unsigned char)(generation >> ((7U - i) * 8U));
  memcpy(material + 8U, query_digest, 32U);
  for (i = 0U; i < 8U; i++) material[40U + i] = (unsigned char)((uint64_t)offset >> ((7U - i) * 8U));
  YAP_V2_sha256_bytes(material, sizeof(material), digest); hex_encode(digest, output); return 0;
}

static int cursor_encode(uint64_t generation, const unsigned char query_digest[32], size_t offset,
                         char *output, size_t capacity) {
  char digest[65]; int written; cursor_digest(generation, query_digest, offset, digest);
  written = snprintf(output, capacity, "v1.%llu.%zu.%s", (unsigned long long)generation, offset, digest);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int cursor_decode(const char *cursor, uint64_t expected_generation,
                         const unsigned char query_digest[32], size_t *offset) {
  unsigned long long generation, parsed_offset; char digest[65], expected[65], trailing; int matched;
  if (cursor == NULL || offset == NULL) return -1;
  matched = sscanf(cursor, "v1.%llu.%llu.%64[0-9a-f]%c", &generation, &parsed_offset, digest, &trailing);
  if (matched != 3 || strlen(digest) != 64U || generation != expected_generation ||
      parsed_offset > YAP_V2_CURSOR_MAX_OFFSET || parsed_offset > SIZE_MAX) return -1;
  cursor_digest(expected_generation, query_digest, (size_t)parsed_offset, expected);
  if (memcmp(digest, expected, 64U) != 0) return -1;
  *offset = (size_t)parsed_offset; return 0;
}

static int parse_request(yyjson_val *root, const HTTP_RUNTIME *runtime,
                         YAP_V2_HTTP_OPERATION operation, YAP_V2_QUERY_REQUEST *request,
                         float **vector_out, YAP_V2_RETRIEVE_OPTIONS *retrieve) {
  static const char *const search_keys[] = {"query","vector","mode","scope","filter","operator","phrase","limit","cursor",NULL};
  static const char *const retrieve_keys[] = {"query","vector","mode","filter","operator","phrase","limit","max_passages_per_document","max_context_bytes",NULL};
  yyjson_val *query, *vector, *mode, *scope, *filter, *op, *phrase, *limit, *value;
  float *values = NULL; size_t i;
  if (!only_keys(root, operation == YAP_V2_HTTP_SEARCH ? search_keys : retrieve_keys)) return -1;
  YAP_V2_query_request_init(request); YAP_V2_retrieve_options_init(retrieve);
  query = yyjson_obj_get(root, "query"); vector = yyjson_obj_get(root, "vector");
  mode = yyjson_obj_get(root, "mode"); scope = yyjson_obj_get(root, "scope");
  filter = yyjson_obj_get(root, "filter"); op = yyjson_obj_get(root, "operator");
  phrase = yyjson_obj_get(root, "phrase"); limit = yyjson_obj_get(root, "limit");
  if (mode != NULL && !yyjson_is_str(mode)) return -1;
  if (mode == NULL || strcmp(yyjson_get_str(mode), "hybrid") == 0) request->mode = YAP_V2_SEARCH_HYBRID;
  else if (strcmp(yyjson_get_str(mode), "lexical") == 0) request->mode = YAP_V2_SEARCH_LEXICAL;
  else if (strcmp(yyjson_get_str(mode), "vector") == 0) request->mode = YAP_V2_SEARCH_VECTOR;
  else return -1;
  if (operation == YAP_V2_HTTP_RETRIEVE) request->scope = YAP_V2_SEARCH_PASSAGES;
  else if (scope == NULL || (yyjson_is_str(scope) && strcmp(yyjson_get_str(scope), "documents") == 0))
    request->scope = YAP_V2_SEARCH_DOCUMENTS;
  else if (yyjson_is_str(scope) && strcmp(yyjson_get_str(scope), "passages") == 0)
    request->scope = YAP_V2_SEARCH_PASSAGES;
  else return -1;
  if (query != NULL) {
    if (!yyjson_is_str(query) || yyjson_get_len(query) == 0U) return -1;
    request->query.data = (const unsigned char *)yyjson_get_str(query); request->query.len = yyjson_get_len(query);
  }
  if (vector != NULL) {
    if (!yyjson_is_arr(vector) || yyjson_arr_size(vector) != runtime->config.vector_dimensions ||
        runtime->config.vector_dimensions == 0U) return -1;
    values = malloc(sizeof(*values) * runtime->config.vector_dimensions);
    if (values == NULL) return -2;
    for (i = 0U; i < runtime->config.vector_dimensions; i++) {
      value = yyjson_arr_get(vector, i);
      if (!yyjson_is_num(value) || !isfinite(yyjson_get_num(value))) { free(values); return -1; }
      values[i] = (float)yyjson_get_num(value);
      if (!isfinite(values[i])) { free(values); return -1; }
    }
    request->query_vector = values; request->query_dimensions = runtime->config.vector_dimensions;
  }
  if ((request->mode != YAP_V2_SEARCH_VECTOR && request->query.len == 0U) ||
      (request->mode != YAP_V2_SEARCH_LEXICAL && request->query_vector == NULL)) { free(values); return -1; }
  if (filter != NULL) {
    char *json = yyjson_val_write(filter, YYJSON_WRITE_NOFLAG, NULL);
    if (json == NULL) { free(values); return -2; }
    /* The document owns input only, so retain this copy until execution. */
    request->filter_json.data = (const unsigned char *)json; request->filter_json.len = strlen(json);
  }
  if (op != NULL) {
    if (!yyjson_is_str(op)) goto invalid;
    if (strcmp(yyjson_get_str(op), "and") == 0) request->query_operator = YAP_V2_QUERY_AND;
    else if (strcmp(yyjson_get_str(op), "or") != 0) goto invalid;
  }
  if (phrase != NULL) {
    if (!yyjson_is_bool(phrase)) goto invalid;
    request->phrase = yyjson_get_bool(phrase);
  }
  if (limit != NULL) {
    if (!yyjson_is_uint(limit) || yyjson_get_uint(limit) == 0U || yyjson_get_uint(limit) > 100U) goto invalid;
    request->top_k = (size_t)yyjson_get_uint(limit);
  }
  request->candidate_k = request->top_k < 100U ? 100U : request->top_k;
  retrieve->max_passages = request->top_k;
  value = yyjson_obj_get(root, "max_passages_per_document");
  if (value != NULL) {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0U || yyjson_get_uint(value) > request->top_k) goto invalid;
    retrieve->max_passages_per_document = (size_t)yyjson_get_uint(value);
  }
  value = yyjson_obj_get(root, "max_context_bytes");
  if (value != NULL) {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0U || yyjson_get_uint(value) > YAP_V2_HTTP_MAX_BODY_BYTES) goto invalid;
    retrieve->max_context_bytes = (size_t)yyjson_get_uint(value);
  }
  *vector_out = values; return 0;
invalid:
  free((void *)request->filter_json.data); free(values); request->filter_json.data = NULL; return -1;
}

static yyjson_mut_val *view_string(yyjson_mut_doc *doc, YAP_V2_BYTES_VIEW value) {
  if (value.len == 0U) return yyjson_mut_strn(doc, "", 0U);
  return yyjson_mut_strncpy(doc, (const char *)value.data, value.len);
}

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len &&
         (left.len == 0U || (left.data != NULL && right.data != NULL &&
                            memcmp(left.data, right.data, left.len) == 0));
}

static int search_result_views(const HTTP_RUNTIME *runtime,
                               const YAP_V2_QUERY_REQUEST *request,
                               const YAP_V2_QUERY_HIT *hit,
                               const YAP_V2_DOCUMENT_VIEW **document,
                               YAP_V2_BYTES_VIEW *snippet) {
  const YAP_V2_SEGMENT *segment;
  YAP_V2_BYTES_VIEW source;
  YAP_V2_DOCUMENT_HIT document_hit;
  const YAP_V2_BYTES_VIEW *terms = NULL;
  size_t term_count = 0U;
  if (hit->segment_ordinal >= YAP_V2_snapshot_segment_count(runtime->snapshot))
    return YAP_V2_CONFLICT;
  segment = YAP_V2_snapshot_segment_documents(runtime->snapshot, hit->segment_ordinal);
  if (segment == NULL) return YAP_V2_CONFLICT;
  if (request->scope == YAP_V2_SEARCH_DOCUMENTS) {
    if (hit->object_ordinal >= segment->document_count) return YAP_V2_CONFLICT;
    *document = &segment->documents[hit->object_ordinal];
    if (!bytes_equal((*document)->id, hit->id) ||
        !bytes_equal((*document)->id, hit->parent_document_id)) return YAP_V2_CONFLICT;
    source = (*document)->body;
  } else {
    const YAP_V2_PASSAGE_VIEW *passage;
    if (hit->object_ordinal >= segment->passage_count) return YAP_V2_CONFLICT;
    passage = &segment->passages[hit->object_ordinal];
    if (!bytes_equal(passage->id, hit->id) ||
        !bytes_equal(passage->parent_document_id, hit->parent_document_id) ||
        YAP_V2_snapshot_lookup_document(runtime->snapshot, hit->parent_document_id,
                                        &document_hit) != YAP_V2_OK ||
        document_hit.document == NULL) return YAP_V2_CONFLICT;
    *document = document_hit.document;
    source = passage->text;
  }
  if (request->query.len > 0U) {
    terms = &request->query;
    term_count = 1U;
  }
  return YAP_V2_snippet_window(source, terms, term_count,
                               YAP_V2_HTTP_SNIPPET_GRAPHEMES, snippet);
}

static int make_response(const HTTP_RUNTIME *runtime, YAP_V2_HTTP_OPERATION operation,
                         const YAP_V2_QUERY_HIT *hits, size_t hit_count,
                         const YAP_V2_QUERY_REQUEST *request,
                         const YAP_V2_RETRIEVE_OPTIONS *options, int has_more,
                         size_t next_offset, const unsigned char query_digest[32], char **response,
                         size_t *response_bytes) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); yyjson_mut_val *root, *array; size_t i;
  unsigned char *context = NULL; YAP_V2_CITATION *citations = NULL;
  size_t context_bytes = 0U, citation_count = 0U; int status = YAP_V2_OK;
  if (doc == NULL) return YAP_V2_ALLOCATION_FAILED;
  root = yyjson_mut_obj(doc); array = yyjson_mut_arr(doc); yyjson_mut_doc_set_root(doc, root);
  if (root == NULL || array == NULL || !yyjson_mut_obj_add_uint(doc, root, "api_version", 2U) ||
      !yyjson_mut_obj_add_uint(doc, root, "generation", YAP_V2_snapshot_generation(runtime->snapshot))) goto memory;
  if (operation == YAP_V2_HTTP_SEARCH) {
    char cursor[160];
    if (!yyjson_mut_obj_add_uint(doc, root, "total", hit_count) ||
        !yyjson_mut_obj_add_val(doc, root, "results", array)) goto memory;
    if (has_more) {
      if (cursor_encode(YAP_V2_snapshot_generation(runtime->snapshot), query_digest, next_offset,
                        cursor, sizeof(cursor)) != 0 ||
          !yyjson_mut_obj_add_strcpy(doc, root, "next_cursor", cursor)) goto memory;
    } else if (!yyjson_mut_obj_add_null(doc, root, "next_cursor")) goto memory;
    for (i = 0U; i < hit_count; i++) {
      const YAP_V2_DOCUMENT_VIEW *document;
      YAP_V2_BYTES_VIEW snippet;
      yyjson_mut_val *item = yyjson_mut_obj(doc);
      status = search_result_views(runtime, request, &hits[i], &document, &snippet);
      if (status != YAP_V2_OK) goto done;
      if (item == NULL || !yyjson_mut_obj_add_val(doc, item, "id", view_string(doc, hits[i].id)) ||
          !yyjson_mut_obj_add_val(doc, item, "document_id", view_string(doc, hits[i].parent_document_id)) ||
          !yyjson_mut_obj_add_val(doc, item, "title", view_string(doc, document->title)) ||
          !yyjson_mut_obj_add_val(doc, item, "url", view_string(doc, document->url)) ||
          !yyjson_mut_obj_add_val(doc, item, "snippet", view_string(doc, snippet)) ||
          !yyjson_mut_obj_add_real(doc, item, "lexical_score", hits[i].lexical_score) ||
          !yyjson_mut_obj_add_real(doc, item, "vector_score", hits[i].vector_score) ||
          !yyjson_mut_obj_add_real(doc, item, "fused_score", hits[i].fused_score) ||
          !yyjson_mut_arr_append(array, item)) goto memory;
    }
  } else {
    context = malloc(options->max_context_bytes); citations = calloc(options->max_passages, sizeof(*citations));
    if (context == NULL || citations == NULL) goto memory;
    status = YAP_V2_retrieve_context(runtime->snapshot, hits, hit_count, options, context,
                                     options->max_context_bytes, &context_bytes, citations,
                                     options->max_passages, &citation_count);
    if (status != YAP_V2_OK) goto done;
    if (!yyjson_mut_obj_add_val(doc, root, "context", yyjson_mut_strncpy(doc, (const char *)context, context_bytes)) ||
        !yyjson_mut_obj_add_val(doc, root, "citations", array)) goto memory;
    for (i = 0U; i < citation_count; i++) {
      const YAP_V2_CITATION *c = &citations[i]; yyjson_mut_val *item = yyjson_mut_obj(doc);
      if (item == NULL || !yyjson_mut_obj_add_val(doc, item, "passage_id", view_string(doc, c->passage_id)) ||
          !yyjson_mut_obj_add_val(doc, item, "document_id", view_string(doc, c->document_id)) ||
          !yyjson_mut_obj_add_val(doc, item, "url", view_string(doc, c->url)) ||
          !yyjson_mut_obj_add_val(doc, item, "title", view_string(doc, c->title)) ||
          !yyjson_mut_obj_add_val(doc, item, "text", view_string(doc, c->text)) ||
          !yyjson_mut_obj_add_uint(doc, item, "start_char", c->start_char) ||
          !yyjson_mut_obj_add_uint(doc, item, "end_char", c->end_char) ||
          !yyjson_mut_obj_add_uint(doc, item, "context_start", c->context_start) ||
          !yyjson_mut_obj_add_uint(doc, item, "context_end", c->context_end) ||
          !yyjson_mut_obj_add_real(doc, item, "lexical_score", c->lexical_score) ||
          !yyjson_mut_obj_add_real(doc, item, "vector_score", c->vector_score) ||
          !yyjson_mut_obj_add_real(doc, item, "fused_score", c->fused_score) ||
          !yyjson_mut_arr_append(array, item)) goto memory;
    }
  }
  *response = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, NULL, response_bytes, NULL);
  if (*response == NULL) goto memory;
  goto done;
memory:
  status = YAP_V2_ALLOCATION_FAILED;
done:
  free(context); free(citations); yyjson_mut_doc_free(doc); return status;
}

static char *error_json(const char *code, const char *message, size_t *bytes) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); yyjson_mut_val *root, *error; char *json;
  if (doc == NULL) return NULL;
  root = yyjson_mut_obj(doc); error = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
  if (root == NULL || error == NULL || !yyjson_mut_obj_add_str(doc, error, "code", code) ||
      !yyjson_mut_obj_add_str(doc, error, "message", message) ||
      !yyjson_mut_obj_add_val(doc, root, "error", error)) { yyjson_mut_doc_free(doc); return NULL; }
  json = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, NULL, bytes, NULL);
  yyjson_mut_doc_free(doc); return json;
}

static char *update_json(const YAP_V2_UPDATE_RESULT *result, size_t *bytes) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); yyjson_mut_val *root, *segment_ids; char *json;
  size_t i;
  if (doc == NULL) return NULL;
  root = yyjson_mut_obj(doc); segment_ids = yyjson_mut_arr(doc); yyjson_mut_doc_set_root(doc, root);
  if (root == NULL || !yyjson_mut_obj_add_uint(doc, root, "generation", result->generation) ||
      !yyjson_mut_obj_add_uint(doc, root, "accepted", result->accepted) ||
      !yyjson_mut_obj_add_uint(doc, root, "upserts", result->upserts) ||
      !yyjson_mut_obj_add_uint(doc, root, "deletes", result->deletes) || segment_ids == NULL) {
    yyjson_mut_doc_free(doc); return NULL;
  }
  for (i = 0U; i < result->segment_ids.count; i++)
    if (!yyjson_mut_arr_add_str(doc, segment_ids, result->segment_ids.items[i])) {
      yyjson_mut_doc_free(doc); return NULL;
    }
  if (!yyjson_mut_obj_add_val(doc, root, "segment_ids", segment_ids)) {
    yyjson_mut_doc_free(doc); return NULL;
  }
  json = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, NULL, bytes, NULL);
  yyjson_mut_doc_free(doc); return json;
}

static int prepare_json(const YAP_V2_CONFIG *config, yyjson_val *root,
                        char **response, size_t *response_bytes) {
  static const char *const keys[] = {"id", "body", NULL};
  yyjson_val *id, *body;
  YAP_V2_CHUNK_SEQUENCE chunks;
  yyjson_mut_doc *document = NULL;
  yyjson_mut_val *output, *passages;
  char *rendered = NULL;
  size_t i;
  int status;
  memset(&chunks, 0, sizeof(chunks));
  if (!only_keys(root, keys) || yyjson_obj_size(root) != 2U) return YAP_V2_INVALID_FORMAT;
  id = yyjson_obj_get(root, "id"); body = yyjson_obj_get(root, "body");
  if (!yyjson_is_str(id) || yyjson_get_len(id) == 0U ||
      yyjson_get_len(id) > YAP_V2_MAX_IDENTIFIER_BYTES || !yyjson_is_str(body) ||
      yyjson_get_len(body) == 0U || yyjson_get_len(body) > YAP_V2_HTTP_MAX_BODY_BYTES)
    return YAP_V2_INVALID_FORMAT;
  status = YAP_V2_unicode_chunk(yyjson_get_str(id), yyjson_get_str(body), yyjson_get_len(body),
                                config->chunk_max_chars, config->chunk_overlap_chars, &chunks);
  if (status != YAP_V2_OK) return status;
  document = yyjson_mut_doc_new(NULL);
  output = document == NULL ? NULL : yyjson_mut_obj(document);
  passages = document == NULL ? NULL : yyjson_mut_arr(document);
  if (document == NULL || output == NULL || passages == NULL) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  yyjson_mut_doc_set_root(document, output);
  if (!yyjson_mut_obj_add_str(document, output, "model_id", config->vector_model_id) ||
      !yyjson_mut_obj_add_uint(document, output, "dimensions", config->vector_dimensions) ||
      !yyjson_mut_obj_add_val(document, output, "passages", passages)) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  for (i = 0U; i < chunks.chunk_count; i++) {
    yyjson_mut_val *item = yyjson_mut_obj(document);
    yyjson_mut_val *text = yyjson_mut_strncpy(document, chunks.chunks[i].text,
                                              chunks.chunks[i].text_bytes);
    if (item == NULL || text == NULL ||
        !yyjson_mut_obj_add_str(document, item, "id", chunks.chunks[i].id) ||
        !yyjson_mut_obj_add_uint(document, item, "ordinal", chunks.chunks[i].ordinal) ||
        !yyjson_mut_obj_add_uint(document, item, "start_char", chunks.chunks[i].start_char) ||
        !yyjson_mut_obj_add_uint(document, item, "end_char", chunks.chunks[i].end_char) ||
        !yyjson_mut_obj_add_val(document, item, "text", text) ||
        !yyjson_mut_arr_append(passages, item)) {
      status = YAP_V2_ALLOCATION_FAILED; goto done;
    }
  }
  rendered = yyjson_mut_write_opts(document, YYJSON_WRITE_NOFLAG, NULL, response_bytes, NULL);
  if (rendered == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  *response = rendered; rendered = NULL; status = YAP_V2_OK;
done:
  free(rendered);
  if (document != NULL) yyjson_mut_doc_free(document);
  YAP_V2_chunk_sequence_free(&chunks);
  return status;
}

static int http_execute_loaded(HTTP_RUNTIME *runtime, const char *index_dir,
                               YAP_V2_HTTP_OPERATION operation,
                               const unsigned char *body, size_t body_bytes,
                               int *http_status, char **response,
                               size_t *response_bytes) {
  yyjson_doc *document = NULL; yyjson_val *root;
  YAP_V2_QUERY_REQUEST request; YAP_V2_RETRIEVE_OPTIONS retrieve;
  YAP_V2_QUERY_STATS query_stats;
  YAP_V2_QUERY_HIT *hits = NULL; float *vector = NULL; size_t hit_count = 0U, offset = 0U;
  size_t page_limit, execution_limit, page_count, body_limit;
  unsigned char query_digest[32]; int status, parsed;
  if (http_status == NULL || response == NULL || response_bytes == NULL) return -1;
  memset(&request, 0, sizeof(request));
  memset(&query_stats, 0, sizeof(query_stats));
  *http_status = 500; *response = NULL; *response_bytes = 0U;
  body_limit = operation == YAP_V2_HTTP_INGEST ?
               YAP_V2_HTTP_MAX_INGEST_BODY_BYTES : YAP_V2_HTTP_MAX_BODY_BYTES;
  if (index_dir == NULL || body == NULL || body_bytes == 0U || body_bytes > body_limit ||
      (operation != YAP_V2_HTTP_SEARCH && operation != YAP_V2_HTTP_RETRIEVE &&
       operation != YAP_V2_HTTP_INGEST && operation != YAP_V2_HTTP_PREPARE)) return -1;
  if (operation == YAP_V2_HTTP_INGEST) {
    YAP_V2_UPDATE_RESULT update; char update_error[256] = {0};
    YAP_V2_update_result_init(&update);
    status = YAP_V2_update_json_batch(index_dir, body, body_bytes, &update,
                                      update_error, sizeof(update_error));
    if (update_error[0] == '\0')
      (void)snprintf(update_error, sizeof(update_error), "%s", YAP_V2_status_string(status));
    if (status == YAP_V2_OK) {
      *http_status = 200; *response = update_json(&update, response_bytes);
    } else if (status == YAP_V2_INVALID_ARGUMENT || status == YAP_V2_INVALID_FORMAT ||
               status == YAP_V2_OUT_OF_RANGE || status == YAP_V2_DUPLICATE ||
               status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED) {
      *http_status = 400; *response = error_json("invalid_batch", update_error, response_bytes);
    } else if (status == YAP_V2_CONFLICT) {
      *http_status = 409; *response = error_json("generation_conflict", update_error, response_bytes);
    } else {
      *http_status = 503; *response = error_json("update_unavailable", update_error, response_bytes);
    }
    YAP_V2_update_result_free(&update);
    return *response == NULL ? -1 : 0;
  }
  if (runtime == NULL) return -1;
  document = yyjson_read((const char *)body, body_bytes, YYJSON_READ_NOFLAG);
  root = document == NULL ? NULL : yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) goto bad_request;
  if (operation == YAP_V2_HTTP_PREPARE) {
    status = prepare_json(&runtime->config, root, response, response_bytes);
    if (status == YAP_V2_INVALID_ARGUMENT || status == YAP_V2_INVALID_FORMAT ||
        status == YAP_V2_OUT_OF_RANGE) goto bad_request;
    if (status != YAP_V2_OK) goto unavailable;
    *http_status = 200; goto done;
  }
  parsed = parse_request(root, runtime, operation, &request, &vector, &retrieve);
  if (parsed != 0) {
    if (parsed == -2) goto unavailable;
    goto bad_request;
  }
  if (request_fingerprint(&request, yyjson_obj_get(root, "filter"), query_digest) != 0) goto unavailable;
  if (operation == YAP_V2_HTTP_SEARCH && yyjson_obj_get(root, "cursor") != NULL) {
    yyjson_val *cursor = yyjson_obj_get(root, "cursor");
    if (!yyjson_is_str(cursor) || cursor_decode(yyjson_get_str(cursor),
        YAP_V2_snapshot_generation(runtime->snapshot), query_digest, &offset) != 0) goto bad_request;
  }
  page_limit = request.top_k;
  if (operation == YAP_V2_HTTP_SEARCH) {
    if (offset > SIZE_MAX - page_limit - 1U || offset + page_limit + 1U > YAP_V2_CURSOR_MAX_OFFSET + 101U)
      goto bad_request;
    execution_limit = offset + page_limit + 1U;
  } else execution_limit = page_limit;
  hits = calloc(execution_limit, sizeof(*hits));
  if (hits == NULL) goto unavailable;
  request.top_k = execution_limit; request.candidate_k = execution_limit < 100U ? 100U : execution_limit;
  status = YAP_V2_query_execute_with_ann(
    runtime->snapshot, runtime->query, runtime->count, &runtime->corpus_stats,
    runtime->config.vector_metric == YAP_V2_VECTOR_DISABLED ? NULL : &runtime->ann_corpus,
    runtime->config.vector_metric == YAP_V2_VECTOR_DISABLED ? NULL : &runtime->ann_plan,
    &request, hits, execution_limit, &hit_count, &query_stats);
  runtime_record_ann_stats(runtime, &query_stats);
  if (status == YAP_V2_INVALID_ARGUMENT || status == YAP_V2_INVALID_FORMAT) goto bad_request;
  if (status != YAP_V2_OK) goto unavailable;
  if (offset > hit_count) goto bad_request;
  page_count = hit_count - offset < page_limit ? hit_count - offset : page_limit;
  status = make_response(runtime, operation, hits + offset, page_count, &request, &retrieve,
                         operation == YAP_V2_HTTP_SEARCH && hit_count > offset + page_count,
                         offset + page_count, query_digest, response, response_bytes);
  if (status != YAP_V2_OK) goto unavailable;
  *http_status = 200; goto done;
bad_request:
  *http_status = 400; *response = error_json("invalid_request", "request does not match the v2 schema", response_bytes); goto done;
unavailable:
  *http_status = 503; *response = error_json("search_unavailable", "validated search snapshot is unavailable", response_bytes);
done:
  free((void *)request.filter_json.data); free(vector); free(hits); if (document != NULL) yyjson_doc_free(document);
  return *response == NULL ? -1 : 0;
}

void YAP_V2_http_runtime_init(YAP_V2_HTTP_RUNTIME *runtime) {
  if (runtime != NULL) runtime->state = NULL;
}

int YAP_V2_http_runtime_open(YAP_V2_HTTP_RUNTIME *runtime, const char *index_dir) {
  HTTP_RUNTIME_STATE *state;
  int status;
  if (runtime == NULL || runtime->state != NULL || index_dir == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_rwlock_init(&state->lock, NULL) != 0) { free(state); return YAP_V2_IO_ERROR; }
  if (pthread_mutex_init(&state->update_lock, NULL) != 0) {
    pthread_rwlock_destroy(&state->lock); free(state); return YAP_V2_IO_ERROR;
  }
  if (pthread_mutex_init(&state->ann_maintenance_lock, NULL) != 0) {
    pthread_mutex_destroy(&state->update_lock); pthread_rwlock_destroy(&state->lock);
    free(state); return YAP_V2_IO_ERROR;
  }
  state->index_dir = strdup(index_dir);
  state->current = calloc(1U, sizeof(*state->current));
  if (state->index_dir == NULL || state->current == NULL) {
    free(state->index_dir); free(state->current);
    pthread_mutex_destroy(&state->ann_maintenance_lock);
    pthread_mutex_destroy(&state->update_lock); pthread_rwlock_destroy(&state->lock);
    free(state); return YAP_V2_ALLOCATION_FAILED;
  }
  status = runtime_open(state->current, state->index_dir);
  if (status != YAP_V2_OK) {
    runtime_close(state->current); free(state->current); free(state->index_dir);
    pthread_mutex_destroy(&state->ann_maintenance_lock);
    pthread_mutex_destroy(&state->update_lock); pthread_rwlock_destroy(&state->lock);
    free(state); return status;
  }
  runtime->state = state;
  return YAP_V2_OK;
}

void YAP_V2_http_runtime_close(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  if (runtime == NULL || runtime->state == NULL) return;
  state = runtime->state;
  pthread_rwlock_wrlock(&state->lock);
  runtime_close(state->current); free(state->current); state->current = NULL;
  pthread_rwlock_unlock(&state->lock);
  pthread_mutex_destroy(&state->ann_maintenance_lock);
  pthread_mutex_destroy(&state->update_lock); pthread_rwlock_destroy(&state->lock);
  free(state->index_dir); free(state); runtime->state = NULL;
}

int YAP_V2_http_runtime_execute(YAP_V2_HTTP_RUNTIME *runtime,
                                YAP_V2_HTTP_OPERATION operation,
                                const unsigned char *body, size_t body_bytes,
                                int *http_status, char **response,
                                size_t *response_bytes) {
  HTTP_RUNTIME_STATE *state;
  int result;
  if (runtime == NULL || runtime->state == NULL) return -1;
  state = runtime->state;
  if (operation == YAP_V2_HTTP_INGEST) {
    pthread_mutex_lock(&state->update_lock);
    result = http_execute_loaded(NULL, state->index_dir, operation, body, body_bytes,
                                 http_status, response, response_bytes);
    if (result == 0 && *http_status == 200) {
      pthread_rwlock_wrlock(&state->lock);
      if (runtime_reload_manifest(state->current, state->index_dir) != YAP_V2_OK) {
        free(*response); *response = error_json("reload_failed",
          "index was updated but the new snapshot could not be loaded", response_bytes);
        *http_status = 503; result = *response == NULL ? -1 : 0;
      }
      pthread_rwlock_unlock(&state->lock);
    }
    pthread_mutex_unlock(&state->update_lock);
    return result;
  }
  pthread_rwlock_rdlock(&state->lock);
  result = http_execute_loaded(state->current, state->index_dir, operation, body,
                               body_bytes, http_status, response, response_bytes);
  pthread_rwlock_unlock(&state->lock);
  return result;
}

int YAP_V2_http_runtime_state(YAP_V2_HTTP_RUNTIME *runtime,
                              YAP_V2_OPERATIONAL_STATE *operational) {
  HTTP_RUNTIME_STATE *state;
  HTTP_RUNTIME *current;
  if (runtime == NULL || runtime->state == NULL || operational == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  pthread_rwlock_rdlock(&state->lock); current = state->current;
  memset(operational, 0, sizeof(*operational));
  operational->ready = current != NULL;
  if (current != NULL) {
    operational->generation = current->manifest.generation;
    operational->segment_count = current->manifest.segment_count;
    operational->embedding_configured =
      current->config.vector_metric != YAP_V2_VECTOR_DISABLED;
    operational->embedding_dimensions = current->config.vector_dimensions;
    memcpy(operational->embedding_model_id, current->config.vector_model_id,
           strlen(current->config.vector_model_id) + 1U);
    operational->ann_base_generation = current->ann_corpus.generation;
    operational->ann_base_vectors = current->ann_corpus.vector_count;
    operational->ann_delta_segments = current->ann_plan.delta_segment_count;
    operational->ann_missing_base_segments = current->ann_plan.missing_base_segment_count;
    pthread_mutex_lock(&current->ann_stats_lock);
    operational->ann_base_search_calls = current->ann_stats.base_search_calls;
    operational->ann_delta_search_calls = current->ann_stats.delta_search_calls;
    operational->ann_retry_search_calls = current->ann_stats.retry_search_calls;
    operational->ann_candidates_examined = current->ann_stats.candidates_examined;
    operational->ann_candidates_rejected = current->ann_stats.candidates_rejected;
    pthread_mutex_unlock(&current->ann_stats_lock);
    operational->ann_rebuilds = current->ann_rebuilds;
    operational->ann_rebuild_failures = current->ann_rebuild_failures;
  }
  pthread_rwlock_unlock(&state->lock);
  return current == NULL ? YAP_V2_CONFLICT : YAP_V2_OK;
}

int YAP_V2_http_runtime_reload(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  int status;
  if (runtime == NULL || runtime->state == NULL) return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  pthread_mutex_lock(&state->update_lock);
  pthread_rwlock_wrlock(&state->lock);
  status = runtime_reload_manifest(state->current, state->index_dir);
  pthread_rwlock_unlock(&state->lock);
  pthread_mutex_unlock(&state->update_lock);
  return status;
}

int YAP_V2_http_runtime_maintain_ann(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  YAP_V2_ANN_CORPUS candidate;
  YAP_V2_ANN_QUERY_PLAN plan;
  uint64_t generation = 0U;
  int status = YAP_V2_OK, needed = 0;
  if (runtime == NULL || runtime->state == NULL) return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  pthread_mutex_lock(&state->ann_maintenance_lock);
  YAP_V2_ann_corpus_init(&candidate);
  YAP_V2_ann_query_plan_init(&plan);
  pthread_rwlock_rdlock(&state->lock);
  if (state->current->config.vector_metric != YAP_V2_VECTOR_DISABLED &&
      (state->current->ann_plan.delta_segment_count > YAP_V2_ANN_MAX_DELTA_SEGMENTS ||
       state->current->ann_plan.missing_base_segment_count > 0U)) {
    needed = 1;
    generation = state->current->manifest.generation;
    status = YAP_V2_ann_corpus_build(&state->current->manifest,
                                     state->current->snapshot,
                                     state->current->ann,
                                     state->current->count, &candidate);
  }
  pthread_rwlock_unlock(&state->lock);
  if (!needed) {
    pthread_mutex_unlock(&state->ann_maintenance_lock);
    return YAP_V2_OK;
  }
  if (status != YAP_V2_OK) {
    pthread_rwlock_wrlock(&state->lock);
    state->current->ann_rebuild_failures = saturated_add_u64(
      state->current->ann_rebuild_failures, 1U);
    pthread_rwlock_unlock(&state->lock);
    YAP_V2_ann_corpus_free(&candidate);
    pthread_mutex_unlock(&state->ann_maintenance_lock);
    return status;
  }
  if (candidate.vector_count > 0U)
    (void)YAP_V2_ann_corpus_save_cache(state->index_dir, &candidate);
  pthread_rwlock_wrlock(&state->lock);
  if (state->current->manifest.generation != generation) {
    status = YAP_V2_CONFLICT;
    state->current->ann_rebuild_failures = saturated_add_u64(
      state->current->ann_rebuild_failures, 1U);
  } else {
    status = YAP_V2_ann_query_plan_build(&candidate,
                                         &state->current->manifest, &plan);
    if (status == YAP_V2_OK) {
      YAP_V2_ann_corpus_free(&state->current->ann_corpus);
      YAP_V2_ann_query_plan_free(&state->current->ann_plan);
      state->current->ann_corpus = candidate;
      state->current->ann_plan = plan;
      YAP_V2_ann_corpus_init(&candidate);
      YAP_V2_ann_query_plan_init(&plan);
      state->current->ann_rebuilds = saturated_add_u64(
        state->current->ann_rebuilds, 1U);
    } else {
      state->current->ann_rebuild_failures = saturated_add_u64(
        state->current->ann_rebuild_failures, 1U);
    }
  }
  pthread_rwlock_unlock(&state->lock);
  YAP_V2_ann_query_plan_free(&plan);
  YAP_V2_ann_corpus_free(&candidate);
  pthread_mutex_unlock(&state->ann_maintenance_lock);
  return status;
}

int YAP_V2_http_execute(const char *index_dir, YAP_V2_HTTP_OPERATION operation,
                        const unsigned char *body, size_t body_bytes, int *http_status,
                        char **response, size_t *response_bytes) {
  HTTP_RUNTIME runtime;
  int status, result;
  if (operation == YAP_V2_HTTP_INGEST)
    return http_execute_loaded(NULL, index_dir, operation, body, body_bytes,
                               http_status, response, response_bytes);
  memset(&runtime, 0, sizeof(runtime));
  status = runtime_open(&runtime, index_dir);
  if (status != YAP_V2_OK) return -1;
  result = http_execute_loaded(&runtime, index_dir, operation, body, body_bytes,
                               http_status, response, response_bytes);
  runtime_close(&runtime);
  return result;
}
