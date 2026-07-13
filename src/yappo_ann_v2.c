#include "yappo_ann_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static usearch_metric_kind_t metric_kind(YAP_V2_VECTOR_METRIC metric) {
  if (metric == YAP_V2_VECTOR_COSINE) return usearch_metric_cos_k;
  if (metric == YAP_V2_VECTOR_DOT) return usearch_metric_ip_k;
  if (metric == YAP_V2_VECTOR_L2) return usearch_metric_l2sq_k;
  return usearch_metric_unknown_k;
}

static usearch_index_t create_index(const YAP_V2_VECTOR_SEGMENT *vectors, size_t connectivity,
                                    size_t expansion_add, size_t expansion_search,
                                    usearch_error_t *error) {
  usearch_init_options_t options;
  memset(&options, 0, sizeof(options));
  options.metric_kind = metric_kind(vectors->metric);
  options.quantization = usearch_scalar_f32_k;
  options.dimensions = vectors->dimensions;
  options.connectivity = connectivity;
  options.expansion_add = expansion_add;
  options.expansion_search = expansion_search;
  return usearch_init(&options, error);
}

static int descriptor(const char *path, size_t records, YAP_V2_COMPONENT_DESCRIPTOR *component) {
  uint64_t bytes;
  if (component == NULL) return YAP_ANN_OK;
  memset(component, 0, sizeof(*component));
  strcpy(component->name, "vectors.usearch");
  component->file_type = YAP_V2_FILE_ANN;
  component->record_count = records;
  if (YAP_V2_file_sha256(path, component->checksum, &bytes) != YAP_V2_OK)
    return YAP_ANN_IO_ERROR;
  component->file_bytes = bytes;
  return YAP_ANN_OK;
}

const char *YAP_V2_ann_status_string(YAP_ANN_STATUS status) {
  switch (status) {
    case YAP_ANN_OK: return "ok";
    case YAP_ANN_INVALID_ARGUMENT: return "invalid argument";
    case YAP_ANN_ALLOCATION_FAILED: return "allocation failed";
    case YAP_ANN_IO_ERROR: return "I/O error";
    case YAP_ANN_BACKEND_ERROR: return "ANN backend error";
    case YAP_ANN_CONFLICT: return "ANN/vector conflict";
    default: return "unknown status";
  }
}

void YAP_V2_ann_segment_init(YAP_V2_ANN_SEGMENT *segment) {
  if (segment != NULL) memset(segment, 0, sizeof(*segment));
}

void YAP_V2_ann_segment_close(YAP_V2_ANN_SEGMENT *segment) {
  usearch_error_t error = NULL;
  if (segment == NULL) return;
  if (segment->index != NULL) usearch_free(segment->index, &error);
  memset(segment, 0, sizeof(*segment));
}

int YAP_V2_ann_build_save(const char *path, const YAP_V2_VECTOR_SEGMENT *vectors,
                          size_t connectivity, size_t expansion_add,
                          size_t expansion_search, YAP_V2_COMPONENT_DESCRIPTOR *component) {
  usearch_error_t error = NULL;
  usearch_index_t index = NULL;
  char *temporary = NULL;
  size_t i, path_len;
  int status = YAP_ANN_BACKEND_ERROR;
  if (path == NULL || vectors == NULL || vectors->entries == NULL || vectors->entry_count == 0U ||
      metric_kind(vectors->metric) == usearch_metric_unknown_k || connectivity == 0U ||
      expansion_add == 0U || expansion_search == 0U)
    return YAP_ANN_INVALID_ARGUMENT;
  index = create_index(vectors, connectivity, expansion_add, expansion_search, &error);
  if (index == NULL || error != NULL) goto done;
  usearch_reserve(index, vectors->entry_count, &error);
  if (error != NULL) goto done;
  for (i = 0U; i < vectors->entry_count; i++) {
    usearch_add(index, (usearch_key_t)i, vectors->entries[i].values, usearch_scalar_f32_k, &error);
    if (error != NULL) goto done;
  }
  path_len = strlen(path);
  if (path_len > SIZE_MAX - 5U) { status = YAP_ANN_ALLOCATION_FAILED; goto done; }
  temporary = (char *)malloc(path_len + 5U);
  if (temporary == NULL) { status = YAP_ANN_ALLOCATION_FAILED; goto done; }
  (void)snprintf(temporary, path_len + 5U, "%s.tmp", path);
  unlink(temporary);
  usearch_save(index, temporary, &error);
  if (error != NULL) goto done;
  if (rename(temporary, path) != 0) { status = YAP_ANN_IO_ERROR; goto done; }
  status = descriptor(path, vectors->entry_count, component);
done:
  if (status != YAP_ANN_OK && temporary != NULL) unlink(temporary);
  free(temporary);
  if (index != NULL) { usearch_error_t ignored = NULL; usearch_free(index, &ignored); }
  return status;
}

int YAP_V2_ann_view(const char *path, const YAP_V2_VECTOR_SEGMENT *vectors,
                    size_t expansion_search, YAP_V2_ANN_SEGMENT *segment,
                    YAP_V2_COMPONENT_DESCRIPTOR *component) {
  usearch_error_t error = NULL;
  usearch_index_t index;
  usearch_init_options_t metadata;
  size_t size, dimensions;
  if (path == NULL || vectors == NULL || vectors->entries == NULL || segment == NULL ||
      expansion_search == 0U || metric_kind(vectors->metric) == usearch_metric_unknown_k)
    return YAP_ANN_INVALID_ARGUMENT;
  memset(&metadata, 0, sizeof(metadata));
  usearch_metadata(path, &metadata, &error);
  if (error != NULL) return YAP_ANN_IO_ERROR;
  if (metadata.metric_kind != metric_kind(vectors->metric) ||
      metadata.quantization != usearch_scalar_f32_k ||
      metadata.dimensions != vectors->dimensions)
    return YAP_ANN_CONFLICT;
  index = create_index(vectors, 0U, 0U, expansion_search, &error);
  if (index == NULL || error != NULL) return YAP_ANN_BACKEND_ERROR;
  usearch_view(index, path, &error);
  if (error != NULL) { usearch_free(index, &error); return YAP_ANN_BACKEND_ERROR; }
  dimensions = usearch_dimensions(index, &error);
  size = usearch_size(index, &error);
  if (error != NULL) { usearch_free(index, &error); return YAP_ANN_BACKEND_ERROR; }
  if (dimensions != vectors->dimensions || size != vectors->entry_count) {
    usearch_free(index, &error);
    return YAP_ANN_CONFLICT;
  }
  usearch_change_expansion_search(index, expansion_search, &error);
  if (error != NULL) { usearch_free(index, &error); return YAP_ANN_BACKEND_ERROR; }
  YAP_V2_ann_segment_close(segment);
  segment->index = index;
  segment->vectors = vectors;
  return descriptor(path, vectors->entry_count, component);
}

static int compare_vector_hits(const void *left, const void *right) {
  const YAP_VECTOR_HIT *a = (const YAP_VECTOR_HIT *)left;
  const YAP_VECTOR_HIT *b = (const YAP_VECTOR_HIT *)right;
  if (a->score > b->score) return -1;
  if (a->score < b->score) return 1;
  return a->ordinal < b->ordinal ? -1 : a->ordinal > b->ordinal;
}

int YAP_V2_ann_search(const YAP_V2_ANN_SEGMENT *segment, const float *query,
                      size_t dimensions, size_t top_k, YAP_VECTOR_HIT *hits,
                      size_t hit_capacity, size_t *hit_count) {
  usearch_key_t *keys;
  usearch_distance_t *distances;
  usearch_error_t error = NULL;
  size_t found, i;
  double validation_score;
  int validation_status;
  if (segment == NULL || segment->vectors == NULL)
    return YAP_VECTOR_INVALID_ARGUMENT;
  if (segment->index == NULL)
    return YAP_V2_vector_segment_search(segment->vectors, query, dimensions, top_k, hits,
                                        hit_capacity, hit_count);
  if (query == NULL || hits == NULL || hit_count == NULL || top_k == 0U || hit_capacity < top_k)
    return hit_capacity < top_k ? YAP_VECTOR_BUFFER_TOO_SMALL : YAP_VECTOR_INVALID_ARGUMENT;
  if (dimensions != segment->vectors->dimensions) return YAP_VECTOR_DIMENSION_MISMATCH;
  validation_status = YAP_Vector_score(segment->vectors->metric, query,
                                       segment->vectors->entries[0].values,
                                       dimensions, &validation_score);
  if (validation_status != YAP_VECTOR_OK) return validation_status;
  keys = (usearch_key_t *)malloc(sizeof(*keys) * top_k);
  distances = (usearch_distance_t *)malloc(sizeof(*distances) * top_k);
  if (keys == NULL || distances == NULL) { free(keys); free(distances); return YAP_VECTOR_ALLOCATION_FAILED; }
  found = usearch_search(segment->index, query, usearch_scalar_f32_k, top_k, keys, distances, &error);
  free(distances);
  if (error != NULL) { free(keys); return YAP_VECTOR_INVALID_ARGUMENT; }
  for (i = 0U; i < found; i++) {
    size_t ordinal = (size_t)keys[i];
    int score_status;
    if (keys[i] > SIZE_MAX || ordinal >= segment->vectors->entry_count) { free(keys); return YAP_VECTOR_INVALID_ARGUMENT; }
    hits[i].id = segment->vectors->entries[ordinal].id;
    hits[i].ordinal = ordinal;
    score_status = YAP_Vector_score(segment->vectors->metric, query,
                                    segment->vectors->entries[ordinal].values,
                                    dimensions, &hits[i].score);
    if (score_status != YAP_VECTOR_OK) { free(keys); return score_status; }
  }
  free(keys);
  qsort(hits, found, sizeof(*hits), compare_vector_hits);
  *hit_count = found;
  return YAP_VECTOR_OK;
}

static int compare_ann_hits(const void *left, const void *right) {
  const YAP_V2_ANN_HIT *a = (const YAP_V2_ANN_HIT *)left;
  const YAP_V2_ANN_HIT *b = (const YAP_V2_ANN_HIT *)right;
  if (a->hit.score > b->hit.score) return -1;
  if (a->hit.score < b->hit.score) return 1;
  if (a->segment_ordinal != b->segment_ordinal)
    return a->segment_ordinal < b->segment_ordinal ? -1 : 1;
  return a->hit.ordinal < b->hit.ordinal ? -1 : a->hit.ordinal > b->hit.ordinal;
}

int YAP_V2_ann_search_segments(const YAP_V2_ANN_SEGMENT *segments, size_t segment_count,
                               const float *query, size_t dimensions, size_t top_k,
                               YAP_V2_ANN_HIT *hits, size_t hit_capacity,
                               size_t *hit_count) {
  YAP_V2_ANN_HIT *candidates;
  YAP_VECTOR_HIT *local;
  size_t candidate_count = 0U, i, j, count, output_count;
  int status;
  if (segments == NULL || segment_count == 0U || hits == NULL || hit_count == NULL ||
      top_k == 0U || hit_capacity < top_k || segment_count > SIZE_MAX / top_k)
    return hit_capacity < top_k ? YAP_VECTOR_BUFFER_TOO_SMALL : YAP_VECTOR_INVALID_ARGUMENT;
  candidates = (YAP_V2_ANN_HIT *)malloc(sizeof(*candidates) * segment_count * top_k);
  local = (YAP_VECTOR_HIT *)malloc(sizeof(*local) * top_k);
  if (candidates == NULL || local == NULL) { free(candidates); free(local); return YAP_VECTOR_ALLOCATION_FAILED; }
  for (i = 0U; i < segment_count; i++) {
    status = YAP_V2_ann_search(&segments[i], query, dimensions, top_k, local, top_k, &count);
    if (status != YAP_VECTOR_OK) { free(candidates); free(local); return status; }
    for (j = 0U; j < count; j++) {
      candidates[candidate_count].segment_ordinal = i;
      candidates[candidate_count++].hit = local[j];
    }
  }
  qsort(candidates, candidate_count, sizeof(*candidates), compare_ann_hits);
  output_count = candidate_count < top_k ? candidate_count : top_k;
  memcpy(hits, candidates, sizeof(*hits) * output_count);
  *hit_count = output_count;
  free(candidates);
  free(local);
  return YAP_VECTOR_OK;
}
