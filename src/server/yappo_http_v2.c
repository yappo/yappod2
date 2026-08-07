#include "server/yappo_http_v2.h"

#include <math.h>
#include <stddef.h>
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
  pthread_mutex_t references_lock;
  size_t references;
  YAP_V2_SNAPSHOT_MANAGER manager;
} HTTP_MANAGER_RESOURCE;

typedef struct {
  pthread_mutex_t references_lock;
  size_t references;
  YAP_V2_LEXICAL_SEGMENT lexical;
  YAP_V2_VECTOR_SEGMENT vectors;
  YAP_V2_ANN_SEGMENT ann;
  YAP_V2_METADATA_INDEX metadata;
  int has_lexical;
  int has_vector;
  int has_metadata;
} HTTP_SEGMENT_RESOURCE;

typedef struct {
  pthread_mutex_t references_lock;
  size_t references;
  YAP_V2_ANN_CORPUS corpus;
} HTTP_ANN_RESOURCE;

typedef struct {
  pthread_mutex_t references_lock;
  size_t references;
  int references_initialized;
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  HTTP_MANAGER_RESOURCE *manager_resource;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  YAP_V2_QUERY_SEGMENT *query;
  HTTP_SEGMENT_RESOURCE **segments;
  YAP_V2_QUERY_CORPUS_STATS corpus_stats;
  HTTP_ANN_RESOURCE *ann_resource;
  YAP_V2_ANN_QUERY_PLAN ann_plan;
  pthread_mutex_t ann_stats_lock;
  YAP_V2_QUERY_STATS ann_stats;
  uint64_t ann_rebuilds;
  uint64_t ann_rebuild_failures;
  int ann_stats_initialized;
  size_t count;
} HTTP_RUNTIME;

typedef struct {
  pthread_mutex_t lock;
  pthread_mutex_t update_lock;
  pthread_mutex_t ann_maintenance_lock;
  char *index_dir;
  HTTP_RUNTIME *current;
  uint64_t ingest_microbatches;
  uint64_t ingest_requests;
  uint64_t ingest_operations;
  uint64_t ingest_published_generations;
  uint64_t ingest_generations_saved;
  uint64_t ingest_max_batch_requests;
  uint64_t ingest_max_batch_operations;
  uint64_t update_wal_recoveries;
  uint64_t maintenance_foreground_deferrals;
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

static void manager_resource_retain(HTTP_MANAGER_RESOURCE *resource) {
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  resource->references++;
  pthread_mutex_unlock(&resource->references_lock);
}

static void manager_resource_release(HTTP_MANAGER_RESOURCE *resource) {
  int destroy = 0;
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  if (resource->references > 0U) {
    resource->references--;
    destroy = resource->references == 0U;
  }
  pthread_mutex_unlock(&resource->references_lock);
  if (destroy) {
    YAP_V2_snapshot_manager_close(&resource->manager);
    pthread_mutex_destroy(&resource->references_lock);
    free(resource);
  }
}

static int manager_resource_open(const char *index_dir,
                                 const char *manifest_path,
                                 const YAP_V2_CONFIG *config,
                                 HTTP_MANAGER_RESOURCE **output) {
  HTTP_MANAGER_RESOURCE *resource;
  int status;
  resource = calloc(1U, sizeof(*resource));
  if (resource == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_mutex_init(&resource->references_lock, NULL) != 0) {
    free(resource);
    return YAP_V2_IO_ERROR;
  }
  resource->references = 1U;
  YAP_V2_snapshot_manager_init(&resource->manager);
  status = YAP_V2_snapshot_manager_open(&resource->manager, index_dir,
                                        manifest_path, config);
  if (status != YAP_V2_OK) {
    pthread_mutex_destroy(&resource->references_lock);
    free(resource);
    return status;
  }
  *output = resource;
  return YAP_V2_OK;
}

static void ann_resource_retain(HTTP_ANN_RESOURCE *resource) {
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  resource->references++;
  pthread_mutex_unlock(&resource->references_lock);
}

static void ann_resource_release(HTTP_ANN_RESOURCE *resource) {
  int destroy = 0;
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  if (resource->references > 0U) {
    resource->references--;
    destroy = resource->references == 0U;
  }
  pthread_mutex_unlock(&resource->references_lock);
  if (destroy) {
    YAP_V2_ann_corpus_free(&resource->corpus);
    pthread_mutex_destroy(&resource->references_lock);
    free(resource);
  }
}

static int ann_resource_create(HTTP_ANN_RESOURCE **output) {
  HTTP_ANN_RESOURCE *resource = calloc(1U, sizeof(*resource));
  if (resource == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_mutex_init(&resource->references_lock, NULL) != 0) {
    free(resource);
    return YAP_V2_IO_ERROR;
  }
  resource->references = 1U;
  YAP_V2_ann_corpus_init(&resource->corpus);
  *output = resource;
  return YAP_V2_OK;
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

static void segment_resource_retain(HTTP_SEGMENT_RESOURCE *resource) {
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  resource->references++;
  pthread_mutex_unlock(&resource->references_lock);
}

static void segment_resource_release(HTTP_SEGMENT_RESOURCE *resource) {
  int destroy = 0;
  if (resource == NULL) return;
  pthread_mutex_lock(&resource->references_lock);
  if (resource->references > 0U) {
    resource->references--;
    destroy = resource->references == 0U;
  }
  pthread_mutex_unlock(&resource->references_lock);
  if (destroy) {
    runtime_segment_close(&resource->lexical, &resource->vectors,
                          &resource->ann, &resource->metadata);
    pthread_mutex_destroy(&resource->references_lock);
    free(resource);
  }
}

static void segment_resource_bind(HTTP_SEGMENT_RESOURCE *resource,
                                  YAP_V2_QUERY_SEGMENT *query) {
  memset(query, 0, sizeof(*query));
  if (resource->has_lexical) query->lexical = &resource->lexical;
  if (resource->has_vector) query->vector = &resource->ann;
  if (resource->has_metadata) query->metadata = &resource->metadata;
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

static int segment_resource_open(
    const char *index_dir, const YAP_V2_CONFIG *config,
    const YAP_V2_SEGMENT_DESCRIPTOR *descriptor,
    HTTP_SEGMENT_RESOURCE **output) {
  HTTP_SEGMENT_RESOURCE *resource;
  YAP_V2_QUERY_SEGMENT query;
  int status;
  resource = calloc(1U, sizeof(*resource));
  if (resource == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_mutex_init(&resource->references_lock, NULL) != 0) {
    free(resource);
    return YAP_V2_IO_ERROR;
  }
  resource->references = 1U;
  status = runtime_segment_open(
    index_dir, config, descriptor, &query, &resource->lexical,
    &resource->vectors, &resource->ann, &resource->metadata);
  if (status != YAP_V2_OK) {
    runtime_segment_close(&resource->lexical, &resource->vectors,
                          &resource->ann, &resource->metadata);
    pthread_mutex_destroy(&resource->references_lock);
    free(resource);
    return status;
  }
  resource->has_lexical = query.lexical != NULL;
  resource->has_vector = query.vector != NULL;
  resource->has_metadata = query.metadata != NULL;
  *output = resource;
  return YAP_V2_OK;
}

static int runtime_ann_views(const HTTP_RUNTIME *runtime,
                             YAP_V2_ANN_SEGMENT **output) {
  YAP_V2_ANN_SEGMENT *views;
  size_t i;
  if (runtime == NULL || output == NULL || runtime->count == 0U)
    return YAP_V2_INVALID_ARGUMENT;
  *output = NULL;
  views = calloc(runtime->count, sizeof(*views));
  if (views == NULL) return YAP_V2_ALLOCATION_FAILED;
  for (i = 0U; i < runtime->count; i++) {
    if (runtime->segments[i] == NULL) {
      free(views);
      return YAP_V2_CONFLICT;
    }
    views[i] = runtime->segments[i]->ann;
    views[i].vectors = runtime->segments[i]->has_vector ?
                       &runtime->segments[i]->vectors : NULL;
  }
  *output = views;
  return YAP_V2_OK;
}

static int runtime_build_ann_corpus(const HTTP_RUNTIME *runtime,
                                    YAP_V2_ANN_CORPUS *corpus) {
  YAP_V2_ANN_SEGMENT *views = NULL;
  int status;
  status = runtime_ann_views(runtime, &views);
  if (status != YAP_V2_OK) return status;
  status = YAP_V2_ann_corpus_build(&runtime->manifest, runtime->snapshot,
                                   views, runtime->count, corpus);
  free(views);
  return status;
}

static void runtime_close(HTTP_RUNTIME *runtime) {
  size_t i;
  if (runtime == NULL) return;
  if (runtime->segments != NULL)
    for (i = 0U; i < runtime->count; i++)
      segment_resource_release(runtime->segments[i]);
  free(runtime->segments);
  free(runtime->query);
  YAP_V2_ann_query_plan_free(&runtime->ann_plan);
  ann_resource_release(runtime->ann_resource);
  if (runtime->snapshot != NULL) YAP_V2_snapshot_release(runtime->snapshot);
  manager_resource_release(runtime->manager_resource);
  YAP_V2_manifest_free(&runtime->manifest);
  if (runtime->ann_stats_initialized) pthread_mutex_destroy(&runtime->ann_stats_lock);
  if (runtime->references_initialized)
    pthread_mutex_destroy(&runtime->references_lock);
  memset(runtime, 0, sizeof(*runtime));
}

static int runtime_enable_references(HTTP_RUNTIME *runtime) {
  if (runtime == NULL || runtime->references_initialized)
    return YAP_V2_INVALID_ARGUMENT;
  if (pthread_mutex_init(&runtime->references_lock, NULL) != 0)
    return YAP_V2_IO_ERROR;
  runtime->references = 1U;
  runtime->references_initialized = 1;
  return YAP_V2_OK;
}

static void runtime_retain(HTTP_RUNTIME *runtime) {
  if (runtime == NULL || !runtime->references_initialized) return;
  pthread_mutex_lock(&runtime->references_lock);
  runtime->references++;
  pthread_mutex_unlock(&runtime->references_lock);
}

static void runtime_release(HTTP_RUNTIME *runtime) {
  int destroy = 0;
  if (runtime == NULL || !runtime->references_initialized) return;
  pthread_mutex_lock(&runtime->references_lock);
  if (runtime->references > 0U) {
    runtime->references--;
    destroy = runtime->references == 0U;
  }
  pthread_mutex_unlock(&runtime->references_lock);
  if (destroy) {
    runtime_close(runtime);
    free(runtime);
  }
}

static int runtime_open_once(HTTP_RUNTIME *runtime, const char *index_dir) {
  char config_path[4096], manifest_path[4096];
  char error[256]; size_t i; int status;
  memset(runtime, 0, sizeof(*runtime));
  YAP_V2_ann_query_plan_init(&runtime->ann_plan);
  if (pthread_mutex_init(&runtime->ann_stats_lock, NULL) != 0) return YAP_V2_IO_ERROR;
  runtime->ann_stats_initialized = 1;
  YAP_V2_manifest_init(&runtime->manifest);
  if (path_join(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      path_join(manifest_path, sizeof(manifest_path), index_dir, "manifest.yap2") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_config_load(config_path, &runtime->config, error, sizeof(error));
  if (status != YAP_V2_OK) return status;
  status = YAP_V2_manifest_load_for_config(manifest_path, &runtime->config, &runtime->manifest);
  if (status != YAP_V2_OK) return status;
  status = manager_resource_open(index_dir, manifest_path, &runtime->config,
                                 &runtime->manager_resource);
  if (status != YAP_V2_OK) return status;
  runtime->snapshot = YAP_V2_snapshot_acquire(&runtime->manager_resource->manager);
  runtime->count = runtime->manifest.segment_count;
  if (runtime->snapshot == NULL || runtime->count == 0U ||
      !YAP_V2_snapshot_matches_manifest(runtime->snapshot, &runtime->manifest))
    return YAP_V2_CONFLICT;
  runtime->query = calloc(runtime->count, sizeof(*runtime->query));
  runtime->segments = calloc(runtime->count, sizeof(*runtime->segments));
  if (runtime->query == NULL || runtime->segments == NULL)
    return YAP_V2_ALLOCATION_FAILED;
  for (i = 0U; i < runtime->count; i++) {
    status = segment_resource_open(index_dir, &runtime->config,
                                   &runtime->manifest.segments[i],
                                   &runtime->segments[i]);
    if (status != YAP_V2_OK) return status;
    segment_resource_bind(runtime->segments[i], &runtime->query[i]);
  }
  status = YAP_V2_query_corpus_stats_build(runtime->snapshot, runtime->query,
                                           runtime->count, &runtime->corpus_stats);
  if (status == YAP_V2_OK) status = ann_resource_create(&runtime->ann_resource);
  if (status == YAP_V2_OK && runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED) {
    int cache_status = YAP_V2_ann_corpus_load_cache(index_dir, &runtime->config,
                                                    &runtime->manifest,
                                                    &runtime->ann_resource->corpus);
    if (cache_status != YAP_V2_OK) {
      status = runtime_build_ann_corpus(runtime, &runtime->ann_resource->corpus);
      if (status == YAP_V2_OK) {
        runtime->ann_rebuilds = saturated_add_u64(runtime->ann_rebuilds, 1U);
        if (runtime->ann_resource->corpus.vector_count > 0U)
          (void)YAP_V2_ann_corpus_save_cache(
            index_dir, &runtime->ann_resource->corpus);
      }
    }
  }
  if (status == YAP_V2_OK && runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED)
    status = YAP_V2_ann_query_plan_build(&runtime->ann_resource->corpus,
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

static int runtime_allocate_open(const char *index_dir, HTTP_RUNTIME **output) {
  HTTP_RUNTIME *runtime;
  int status;
  if (index_dir == NULL || output == NULL) return YAP_V2_INVALID_ARGUMENT;
  *output = NULL;
  runtime = calloc(1U, sizeof(*runtime));
  if (runtime == NULL) return YAP_V2_ALLOCATION_FAILED;
  status = runtime_open(runtime, index_dir);
  if (status == YAP_V2_OK) status = runtime_enable_references(runtime);
  if (status != YAP_V2_OK) {
    runtime_close(runtime);
    free(runtime);
    return status;
  }
  *output = runtime;
  return YAP_V2_OK;
}

static int runtime_allocate_candidate(
    HTTP_RUNTIME *previous, const char *index_dir,
    HTTP_ANN_RESOURCE *replacement_ann, HTTP_RUNTIME **output) {
  HTTP_RUNTIME *runtime = NULL;
  YAP_V2_MANIFEST_SEGMENT_MAP previous_segments;
  char manifest_path[4096];
  size_t i;
  int manager_changed = 0;
  int status = YAP_V2_OK;
  if (previous == NULL || index_dir == NULL || output == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  *output = NULL;
  YAP_V2_manifest_segment_map_init(&previous_segments);
  runtime = calloc(1U, sizeof(*runtime));
  if (runtime == NULL) return YAP_V2_ALLOCATION_FAILED;
  runtime->config = previous->config;
  YAP_V2_manifest_init(&runtime->manifest);
  YAP_V2_ann_query_plan_init(&runtime->ann_plan);
  if (pthread_mutex_init(&runtime->ann_stats_lock, NULL) != 0) {
    free(runtime);
    return YAP_V2_IO_ERROR;
  }
  runtime->ann_stats_initialized = 1;
  if (path_join(manifest_path, sizeof(manifest_path), index_dir,
                "manifest.yap2") != 0) {
    status = YAP_V2_INVALID_ARGUMENT;
    goto done;
  }
  status = YAP_V2_manifest_load_for_config(manifest_path, &runtime->config,
                                           &runtime->manifest);
  if (status != YAP_V2_OK) goto done;
  runtime->manager_resource = previous->manager_resource;
  manager_resource_retain(runtime->manager_resource);
  status = YAP_V2_snapshot_manager_reload(
    &runtime->manager_resource->manager, &manager_changed);
  if (status != YAP_V2_OK) goto done;
  runtime->snapshot = YAP_V2_snapshot_acquire(&runtime->manager_resource->manager);
  runtime->count = runtime->manifest.segment_count;
  if (runtime->snapshot == NULL || runtime->count == 0U ||
      !YAP_V2_snapshot_matches_manifest(runtime->snapshot, &runtime->manifest)) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  runtime->query = calloc(runtime->count, sizeof(*runtime->query));
  runtime->segments = calloc(runtime->count, sizeof(*runtime->segments));
  if (runtime->query == NULL || runtime->segments == NULL) {
    status = YAP_V2_ALLOCATION_FAILED;
    goto done;
  }
  status = YAP_V2_manifest_segment_map_build(&previous_segments,
                                              &previous->manifest);
  if (status != YAP_V2_OK) goto done;
  for (i = 0U; i < runtime->count; i++) {
    size_t previous_index = 0U;
    const YAP_V2_SEGMENT_DESCRIPTOR *descriptor = &runtime->manifest.segments[i];
    if (YAP_V2_manifest_segment_map_find(&previous_segments, descriptor->id,
                                         &previous_index) == YAP_V2_OK &&
        YAP_V2_segment_descriptor_equal(
          descriptor, &previous->manifest.segments[previous_index])) {
      runtime->segments[i] = previous->segments[previous_index];
      segment_resource_retain(runtime->segments[i]);
    } else {
      status = segment_resource_open(index_dir, &runtime->config, descriptor,
                                     &runtime->segments[i]);
      if (status != YAP_V2_OK) goto done;
    }
    segment_resource_bind(runtime->segments[i], &runtime->query[i]);
  }
  status = YAP_V2_query_corpus_stats_build(runtime->snapshot, runtime->query,
                                           runtime->count,
                                           &runtime->corpus_stats);
  if (status != YAP_V2_OK) goto done;
  runtime->ann_resource = replacement_ann != NULL ? replacement_ann :
                          previous->ann_resource;
  ann_resource_retain(runtime->ann_resource);
  if (runtime->ann_resource == NULL) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  if (runtime->config.vector_metric != YAP_V2_VECTOR_DISABLED) {
    status = YAP_V2_ann_query_plan_build(&runtime->ann_resource->corpus,
                                         &runtime->manifest,
                                         &runtime->ann_plan);
    if (status != YAP_V2_OK) goto done;
  }
  status = runtime_enable_references(runtime);
done:
  YAP_V2_manifest_segment_map_free(&previous_segments);
  if (status != YAP_V2_OK) {
    runtime_close(runtime);
    free(runtime);
    return status;
  }
  *output = runtime;
  return YAP_V2_OK;
}

static HTTP_RUNTIME *runtime_state_acquire(HTTP_RUNTIME_STATE *state) {
  HTTP_RUNTIME *current;
  pthread_mutex_lock(&state->lock);
  current = state->current;
  runtime_retain(current);
  pthread_mutex_unlock(&state->lock);
  return current;
}

static void runtime_copy_observability(HTTP_RUNTIME *candidate,
                                       HTTP_RUNTIME *previous) {
  if (candidate == NULL || previous == NULL) return;
  pthread_mutex_lock(&previous->ann_stats_lock);
  candidate->ann_stats = previous->ann_stats;
  candidate->ann_rebuilds = previous->ann_rebuilds;
  candidate->ann_rebuild_failures = previous->ann_rebuild_failures;
  pthread_mutex_unlock(&previous->ann_stats_lock);
}

static int runtime_state_publish_replacement(HTTP_RUNTIME_STATE *state,
                                             HTTP_RUNTIME *expected,
                                             HTTP_RUNTIME **candidate) {
  HTTP_RUNTIME *published_previous = NULL;
  int status = YAP_V2_CONFLICT;
  if (state == NULL || expected == NULL || candidate == NULL || *candidate == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  pthread_mutex_lock(&state->lock);
  if (state->current == expected) {
    published_previous = state->current;
    state->current = *candidate;
    *candidate = NULL;
    status = YAP_V2_OK;
  }
  pthread_mutex_unlock(&state->lock);
  runtime_release(published_previous);
  return status;
}

static int runtime_manifest_relation(const HTTP_RUNTIME *current,
                                     const char *index_dir, int *changed) {
  YAP_V2_MANIFEST manifest;
  char manifest_path[4096];
  size_t i;
  int status;
  if (current == NULL || index_dir == NULL || changed == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  *changed = 0;
  YAP_V2_manifest_init(&manifest);
  if (path_join(manifest_path, sizeof(manifest_path), index_dir,
                "manifest.yap2") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_manifest_load_for_config(manifest_path, &current->config,
                                           &manifest);
  if (status != YAP_V2_OK) goto done;
  if (manifest.generation < current->manifest.generation) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  if (manifest.generation > current->manifest.generation) {
    *changed = 1;
    goto done;
  }
  if (manifest.segment_count != current->manifest.segment_count) {
    status = YAP_V2_CONFLICT;
    goto done;
  }
  for (i = 0U; i < manifest.segment_count; i++) {
    if (!YAP_V2_segment_descriptor_equal(&manifest.segments[i],
                                         &current->manifest.segments[i])) {
      status = YAP_V2_CONFLICT;
      break;
    }
  }
done:
  YAP_V2_manifest_free(&manifest);
  return status;
}

static int runtime_state_reload(HTTP_RUNTIME_STATE *state) {
  HTTP_RUNTIME *previous, *candidate = NULL;
  int changed = 0;
  int status;
  previous = runtime_state_acquire(state);
  if (previous == NULL) return YAP_V2_CONFLICT;
  status = runtime_manifest_relation(previous, state->index_dir, &changed);
  if (status != YAP_V2_OK || !changed) {
    runtime_release(previous);
    return status;
  }
  status = runtime_allocate_candidate(previous, state->index_dir, NULL,
                                      &candidate);
  if (status != YAP_V2_OK) {
    runtime_release(previous);
    return status;
  }
  if (candidate->manifest.generation <= previous->manifest.generation) {
    runtime_release(candidate);
    runtime_release(previous);
    return YAP_V2_CONFLICT;
  }
  runtime_copy_observability(candidate, previous);
  status = runtime_state_publish_replacement(state, previous, &candidate);
  runtime_release(candidate);
  runtime_release(previous);
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
    runtime->config.vector_metric == YAP_V2_VECTOR_DISABLED ? NULL :
      &runtime->ann_resource->corpus,
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

typedef struct {
  YAP_V2_INGEST_OPERATION *operations;
  size_t operation_count;
  size_t upserts;
  size_t deletes;
  int parse_status;
  char error[256];
} HTTP_PARSED_INGEST;

static int update_status_is_client_error(int status) {
  return status == YAP_V2_INVALID_ARGUMENT ||
         status == YAP_V2_INVALID_FORMAT || status == YAP_V2_OUT_OF_RANGE ||
         status == YAP_V2_DUPLICATE ||
         status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED;
}

static void update_error_response(int status, const char *message,
                                  YAP_V2_HTTP_INGEST_ITEM *item) {
  const char *resolved = message != NULL && message[0] != '\0' ? message :
                         YAP_V2_status_string(status);
  if (update_status_is_client_error(status)) {
    item->http_status = 400;
    item->response = error_json("invalid_batch", resolved,
                                &item->response_bytes);
  } else if (status == YAP_V2_CONFLICT) {
    item->http_status = 409;
    item->response = error_json("generation_conflict", resolved,
                                &item->response_bytes);
  } else {
    item->http_status = 503;
    item->response = error_json("update_unavailable", resolved,
                                &item->response_bytes);
  }
  item->result = item->response == NULL ? -1 : 0;
}

static size_t update_id_hash(const char *value) {
  size_t hash = (size_t)1469598103934665603ULL;
  const unsigned char *cursor = (const unsigned char *)value;
  while (*cursor != '\0') {
    hash ^= *cursor++;
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static int update_group_contains_ids(const char *const *slots, size_t capacity,
                                     const HTTP_PARSED_INGEST *parsed) {
  size_t i;
  for (i = 0U; i < parsed->operation_count; i++) {
    const char *id = parsed->operations[i].id;
    size_t slot = update_id_hash(id) & (capacity - 1U);
    while (slots[slot] != NULL) {
      if (strcmp(slots[slot], id) == 0) return 1;
      slot = (slot + 1U) & (capacity - 1U);
    }
  }
  return 0;
}

static void update_group_add_ids(const char **slots, size_t capacity,
                                 const HTTP_PARSED_INGEST *parsed) {
  size_t i;
  for (i = 0U; i < parsed->operation_count; i++) {
    const char *id = parsed->operations[i].id;
    size_t slot = update_id_hash(id) & (capacity - 1U);
    while (slots[slot] != NULL) slot = (slot + 1U) & (capacity - 1U);
    slots[slot] = id;
  }
}

static int apply_ingest_group(
    const char *index_dir, HTTP_PARSED_INGEST *parsed,
    YAP_V2_HTTP_INGEST_ITEM *items, const size_t *indices,
    size_t index_count, uint64_t *published_generations,
    uint64_t *published_requests) {
  YAP_V2_INGEST_OPERATION *combined;
  YAP_V2_UPDATE_RESULT update;
  char error[256] = {0};
  size_t operation_count = 0U, offset = 0U, i;
  int status;
  for (i = 0U; i < index_count; i++)
    operation_count += parsed[indices[i]].operation_count;
  combined = calloc(operation_count, sizeof(*combined));
  if (combined == NULL) {
    for (i = 0U; i < index_count; i++)
      update_error_response(YAP_V2_ALLOCATION_FAILED, NULL,
                            &items[indices[i]]);
    return YAP_V2_ALLOCATION_FAILED;
  }
  for (i = 0U; i < index_count; i++) {
    HTTP_PARSED_INGEST *batch = &parsed[indices[i]];
    memcpy(combined + offset, batch->operations,
           batch->operation_count * sizeof(*combined));
    offset += batch->operation_count;
  }
  YAP_V2_update_result_init(&update);
  status = YAP_V2_update_apply(index_dir, combined, operation_count, &update,
                               error, sizeof(error));
  free(combined);
  if (status != YAP_V2_OK && index_count > 1U &&
      update_status_is_client_error(status)) {
    YAP_V2_update_result_free(&update);
    for (i = 0U; i < index_count; i++)
      (void)apply_ingest_group(index_dir, parsed, items, &indices[i], 1U,
                               published_generations, published_requests);
    return YAP_V2_OK;
  }
  for (i = 0U; i < index_count; i++) {
    size_t index = indices[i];
    if (status == YAP_V2_OK) {
      YAP_V2_UPDATE_RESULT individual = update;
      individual.accepted = parsed[index].operation_count;
      individual.upserts = parsed[index].upserts;
      individual.deletes = parsed[index].deletes;
      items[index].http_status = 200;
      items[index].response = update_json(&individual,
                                           &items[index].response_bytes);
      items[index].result = items[index].response == NULL ? -1 : 0;
    } else {
      update_error_response(status, error, &items[index]);
    }
  }
  if (status == YAP_V2_OK) {
    *published_generations = saturated_add_u64(*published_generations, 1U);
    *published_requests = saturated_add_u64(
      *published_requests, (uint64_t)index_count);
  }
  YAP_V2_update_result_free(&update);
  return status;
}

int YAP_V2_http_runtime_execute_ingest_batch(
    YAP_V2_HTTP_RUNTIME *runtime, YAP_V2_HTTP_INGEST_ITEM *items,
    size_t item_count) {
  enum { ID_SLOT_CAPACITY = 32768 };
  HTTP_RUNTIME_STATE *state;
  HTTP_PARSED_INGEST *parsed;
  const char **id_slots;
  size_t *group_indices;
  size_t group_count = 0U, group_operations = 0U;
  size_t parsed_operations = 0U, i, j;
  uint64_t published_generations = 0U, published_requests = 0U;
  if (runtime == NULL || runtime->state == NULL || items == NULL ||
      item_count == 0U)
    return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  parsed = calloc(item_count, sizeof(*parsed));
  group_indices = calloc(item_count, sizeof(*group_indices));
  id_slots = calloc(ID_SLOT_CAPACITY, sizeof(*id_slots));
  if (parsed == NULL || group_indices == NULL || id_slots == NULL) {
    free(parsed); free(group_indices); free(id_slots);
    return YAP_V2_ALLOCATION_FAILED;
  }
  pthread_mutex_lock(&state->update_lock);
  for (i = 0U; i < item_count; i++) {
    memset(&items[i].http_status, 0,
           sizeof(items[i]) - offsetof(YAP_V2_HTTP_INGEST_ITEM, http_status));
    if (items[i].body == NULL || items[i].body_bytes == 0U ||
        items[i].body_bytes > YAP_V2_HTTP_MAX_INGEST_BODY_BYTES) {
      parsed[i].parse_status = YAP_V2_INVALID_ARGUMENT;
      (void)snprintf(parsed[i].error, sizeof(parsed[i].error),
                     "request body is invalid");
      continue;
    }
    parsed[i].parse_status = YAP_V2_update_parse_json_batch(
      items[i].body, items[i].body_bytes, &parsed[i].operations,
      &parsed[i].operation_count, parsed[i].error,
      sizeof(parsed[i].error));
    if (parsed[i].parse_status == YAP_V2_OK)
      for (j = 0U; j < parsed[i].operation_count; j++) {
        if (parsed[i].operations[j].kind == YAP_V2_INGEST_DELETE)
          parsed[i].deletes++;
        else
          parsed[i].upserts++;
      }
    if (parsed[i].parse_status == YAP_V2_OK)
      parsed_operations += parsed[i].operation_count;
  }
  for (i = 0U; i < item_count; i++) {
    int split = 0;
    if (parsed[i].parse_status != YAP_V2_OK) {
      if (group_count != 0U) {
        (void)apply_ingest_group(
          state->index_dir, parsed, items, group_indices, group_count,
          &published_generations, &published_requests);
        group_count = 0U; group_operations = 0U;
        memset(id_slots, 0, ID_SLOT_CAPACITY * sizeof(*id_slots));
      }
      update_error_response(parsed[i].parse_status, parsed[i].error, &items[i]);
      continue;
    }
    split = group_count != 0U &&
            (parsed[i].operation_count >
               YAP_V2_UPDATE_MAX_OPERATIONS - group_operations ||
             update_group_contains_ids(id_slots, ID_SLOT_CAPACITY,
                                       &parsed[i]));
    if (split) {
      (void)apply_ingest_group(
        state->index_dir, parsed, items, group_indices, group_count,
        &published_generations, &published_requests);
      group_count = 0U; group_operations = 0U;
      memset(id_slots, 0, ID_SLOT_CAPACITY * sizeof(*id_slots));
    }
    group_indices[group_count++] = i;
    group_operations += parsed[i].operation_count;
    update_group_add_ids(id_slots, ID_SLOT_CAPACITY, &parsed[i]);
  }
  if (group_count != 0U)
    (void)apply_ingest_group(
      state->index_dir, parsed, items, group_indices, group_count,
      &published_generations, &published_requests);
  if (published_generations != 0U && runtime_state_reload(state) != YAP_V2_OK) {
    for (i = 0U; i < item_count; i++) {
      if (items[i].http_status != 200) continue;
      free(items[i].response);
      items[i].response = error_json(
        "reload_failed",
        "index was updated but the new snapshot could not be loaded",
        &items[i].response_bytes);
      items[i].http_status = 503;
      items[i].result = items[i].response == NULL ? -1 : 0;
    }
  }
  pthread_mutex_lock(&state->lock);
  state->ingest_microbatches = saturated_add_u64(
    state->ingest_microbatches, 1U);
  state->ingest_requests = saturated_add_u64(
    state->ingest_requests, (uint64_t)item_count);
  state->ingest_operations = saturated_add_u64(
    state->ingest_operations, (uint64_t)parsed_operations);
  state->ingest_published_generations = saturated_add_u64(
    state->ingest_published_generations, published_generations);
  state->ingest_generations_saved = saturated_add_u64(
    state->ingest_generations_saved,
    published_requests > published_generations ?
      published_requests - published_generations : 0U);
  if ((uint64_t)item_count > state->ingest_max_batch_requests)
    state->ingest_max_batch_requests = (uint64_t)item_count;
  if ((uint64_t)parsed_operations > state->ingest_max_batch_operations)
    state->ingest_max_batch_operations = (uint64_t)parsed_operations;
  pthread_mutex_unlock(&state->lock);
  pthread_mutex_unlock(&state->update_lock);
  for (i = 0U; i < item_count; i++)
    YAP_V2_update_operations_free(parsed[i].operations,
                                  parsed[i].operation_count);
  free(parsed); free(group_indices); free(id_slots);
  return YAP_V2_OK;
}

void YAP_V2_http_runtime_init(YAP_V2_HTTP_RUNTIME *runtime) {
  if (runtime != NULL) runtime->state = NULL;
}

int YAP_V2_http_runtime_open(YAP_V2_HTTP_RUNTIME *runtime, const char *index_dir) {
  HTTP_RUNTIME_STATE *state;
  char recovery_error[256] = {0};
  int had_wal;
  int status;
  if (runtime == NULL || runtime->state != NULL || index_dir == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  had_wal = YAP_V2_update_wal_exists(index_dir);
  status = YAP_V2_update_recover(index_dir, recovery_error,
                                 sizeof(recovery_error));
  if (status != YAP_V2_OK) return status;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return YAP_V2_ALLOCATION_FAILED;
  state->update_wal_recoveries = had_wal ? 1U : 0U;
  if (pthread_mutex_init(&state->lock, NULL) != 0) { free(state); return YAP_V2_IO_ERROR; }
  if (pthread_mutex_init(&state->update_lock, NULL) != 0) {
    pthread_mutex_destroy(&state->lock); free(state); return YAP_V2_IO_ERROR;
  }
  if (pthread_mutex_init(&state->ann_maintenance_lock, NULL) != 0) {
    pthread_mutex_destroy(&state->update_lock); pthread_mutex_destroy(&state->lock);
    free(state); return YAP_V2_IO_ERROR;
  }
  state->index_dir = strdup(index_dir);
  if (state->index_dir == NULL) {
    free(state->index_dir);
    pthread_mutex_destroy(&state->ann_maintenance_lock);
    pthread_mutex_destroy(&state->update_lock); pthread_mutex_destroy(&state->lock);
    free(state); return YAP_V2_ALLOCATION_FAILED;
  }
  status = runtime_allocate_open(state->index_dir, &state->current);
  if (status != YAP_V2_OK) {
    runtime_release(state->current); free(state->index_dir);
    pthread_mutex_destroy(&state->ann_maintenance_lock);
    pthread_mutex_destroy(&state->update_lock); pthread_mutex_destroy(&state->lock);
    free(state); return status;
  }
  runtime->state = state;
  return YAP_V2_OK;
}

void YAP_V2_http_runtime_close(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  if (runtime == NULL || runtime->state == NULL) return;
  state = runtime->state;
  pthread_mutex_lock(&state->lock);
  {
    HTTP_RUNTIME *current = state->current;
    state->current = NULL;
    pthread_mutex_unlock(&state->lock);
    runtime_release(current);
  }
  pthread_mutex_destroy(&state->ann_maintenance_lock);
  pthread_mutex_destroy(&state->update_lock); pthread_mutex_destroy(&state->lock);
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
      if (runtime_state_reload(state) != YAP_V2_OK) {
        free(*response); *response = error_json("reload_failed",
          "index was updated but the new snapshot could not be loaded", response_bytes);
        *http_status = 503; result = *response == NULL ? -1 : 0;
      }
    }
    pthread_mutex_unlock(&state->update_lock);
    return result;
  }
  {
    HTTP_RUNTIME *current = runtime_state_acquire(state);
    if (current == NULL) return -1;
    result = http_execute_loaded(current, state->index_dir, operation, body,
                               body_bytes, http_status, response, response_bytes);
    runtime_release(current);
  }
  return result;
}

int YAP_V2_http_runtime_state(YAP_V2_HTTP_RUNTIME *runtime,
                              YAP_V2_OPERATIONAL_STATE *operational) {
  HTTP_RUNTIME_STATE *state;
  HTTP_RUNTIME *current;
  if (runtime == NULL || runtime->state == NULL || operational == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  current = runtime_state_acquire(state);
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
    operational->ann_base_generation = current->ann_resource->corpus.generation;
    operational->ann_base_vectors = current->ann_resource->corpus.vector_count;
    operational->ann_delta_segments = current->ann_plan.delta_segment_count;
    operational->ann_missing_base_segments = current->ann_plan.missing_base_segment_count;
    pthread_mutex_lock(&current->ann_stats_lock);
    operational->ann_base_search_calls = current->ann_stats.base_search_calls;
    operational->ann_delta_search_calls = current->ann_stats.delta_search_calls;
    operational->ann_retry_search_calls = current->ann_stats.retry_search_calls;
    operational->ann_candidates_examined = current->ann_stats.candidates_examined;
    operational->ann_candidates_rejected = current->ann_stats.candidates_rejected;
    operational->ann_rebuilds = current->ann_rebuilds;
    operational->ann_rebuild_failures = current->ann_rebuild_failures;
    pthread_mutex_unlock(&current->ann_stats_lock);
  }
  pthread_mutex_lock(&state->lock);
  operational->ingest_microbatches = state->ingest_microbatches;
  operational->ingest_requests = state->ingest_requests;
  operational->ingest_operations = state->ingest_operations;
  operational->ingest_published_generations =
    state->ingest_published_generations;
  operational->ingest_generations_saved = state->ingest_generations_saved;
  operational->ingest_max_batch_requests = state->ingest_max_batch_requests;
  operational->ingest_max_batch_operations = state->ingest_max_batch_operations;
  operational->update_wal_recoveries = state->update_wal_recoveries;
  operational->maintenance_foreground_deferrals =
    state->maintenance_foreground_deferrals;
  pthread_mutex_unlock(&state->lock);
  {
    int available = current != NULL;
    runtime_release(current);
    return available ? YAP_V2_OK : YAP_V2_CONFLICT;
  }
}

void YAP_V2_http_runtime_record_maintenance_deferral(
    YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  if (runtime == NULL || runtime->state == NULL) return;
  state = runtime->state;
  pthread_mutex_lock(&state->lock);
  state->maintenance_foreground_deferrals = saturated_add_u64(
    state->maintenance_foreground_deferrals, 1U);
  pthread_mutex_unlock(&state->lock);
}

int YAP_V2_http_runtime_reload(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  int status;
  if (runtime == NULL || runtime->state == NULL) return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  pthread_mutex_lock(&state->update_lock);
  status = runtime_state_reload(state);
  pthread_mutex_unlock(&state->update_lock);
  return status;
}

int YAP_V2_http_runtime_maintain_ann(YAP_V2_HTTP_RUNTIME *runtime) {
  HTTP_RUNTIME_STATE *state;
  HTTP_RUNTIME *base = NULL, *replacement = NULL;
  HTTP_ANN_RESOURCE *replacement_ann = NULL;
  int status = YAP_V2_OK, needed = 0;
  if (runtime == NULL || runtime->state == NULL) return YAP_V2_INVALID_ARGUMENT;
  state = runtime->state;
  pthread_mutex_lock(&state->ann_maintenance_lock);
  pthread_mutex_lock(&state->update_lock);
  base = runtime_state_acquire(state);
  if (base != NULL && base->config.vector_metric != YAP_V2_VECTOR_DISABLED &&
      (base->ann_plan.delta_segment_count > YAP_V2_ANN_MAX_DELTA_SEGMENTS ||
       base->ann_plan.missing_base_segment_count > 0U)) {
    needed = 1;
    status = ann_resource_create(&replacement_ann);
    if (status == YAP_V2_OK)
      status = runtime_build_ann_corpus(base, &replacement_ann->corpus);
  }
  if (!needed) {
    runtime_release(base);
    pthread_mutex_unlock(&state->update_lock);
    pthread_mutex_unlock(&state->ann_maintenance_lock);
    return YAP_V2_OK;
  }
  if (status != YAP_V2_OK) {
    pthread_mutex_lock(&base->ann_stats_lock);
    base->ann_rebuild_failures = saturated_add_u64(
      base->ann_rebuild_failures, 1U);
    pthread_mutex_unlock(&base->ann_stats_lock);
    goto done;
  }
  if (replacement_ann->corpus.vector_count > 0U)
    (void)YAP_V2_ann_corpus_save_cache(state->index_dir,
                                       &replacement_ann->corpus);
  status = runtime_allocate_candidate(base, state->index_dir, replacement_ann,
                                      &replacement);
  if (status == YAP_V2_OK &&
      replacement->manifest.generation != base->manifest.generation)
    status = YAP_V2_CONFLICT;
  if (status == YAP_V2_OK) {
    runtime_copy_observability(replacement, base);
    pthread_mutex_lock(&replacement->ann_stats_lock);
    replacement->ann_rebuilds = saturated_add_u64(
      replacement->ann_rebuilds, 1U);
    pthread_mutex_unlock(&replacement->ann_stats_lock);
    status = runtime_state_publish_replacement(state, base, &replacement);
  }
  if (status != YAP_V2_OK) {
    pthread_mutex_lock(&base->ann_stats_lock);
    base->ann_rebuild_failures = saturated_add_u64(
      base->ann_rebuild_failures, 1U);
    pthread_mutex_unlock(&base->ann_stats_lock);
  }
done:
  runtime_release(replacement);
  ann_resource_release(replacement_ann);
  runtime_release(base);
  pthread_mutex_unlock(&state->update_lock);
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
