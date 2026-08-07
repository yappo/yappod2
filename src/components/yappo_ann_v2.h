#ifndef YAPPO_ANN_V2_H
#define YAPPO_ANN_V2_H

#include <usearch.h>

#include "components/yappo_vector_v2.h"

typedef enum {
  YAP_ANN_OK = 0,
  YAP_ANN_INVALID_ARGUMENT = -1,
  YAP_ANN_ALLOCATION_FAILED = -2,
  YAP_ANN_IO_ERROR = -3,
  YAP_ANN_BACKEND_ERROR = -4,
  YAP_ANN_CONFLICT = -5
} YAP_ANN_STATUS;

typedef struct {
  usearch_index_t index;
  const YAP_V2_VECTOR_SEGMENT *vectors;
} YAP_V2_ANN_SEGMENT;

typedef struct {
  usearch_index_t index;
  YAP_V2_VECTOR_METRIC metric;
  size_t dimensions;
  size_t entry_count;
} YAP_V2_ANN_INDEX;

typedef struct {
  size_t segment_ordinal;
  YAP_VECTOR_HIT hit;
} YAP_V2_ANN_HIT;

const char *YAP_V2_ann_status_string(YAP_ANN_STATUS status);
void YAP_V2_ann_segment_init(YAP_V2_ANN_SEGMENT *segment);
void YAP_V2_ann_segment_close(YAP_V2_ANN_SEGMENT *segment);
void YAP_V2_ann_index_init(YAP_V2_ANN_INDEX *index);
void YAP_V2_ann_index_close(YAP_V2_ANN_INDEX *index);
int YAP_V2_ann_index_create(YAP_V2_VECTOR_METRIC metric, size_t dimensions,
                            size_t capacity, size_t connectivity,
                            size_t expansion_add, size_t expansion_search,
                            YAP_V2_ANN_INDEX *index);
int YAP_V2_ann_index_add(YAP_V2_ANN_INDEX *index, uint64_t key, const float *vector);
int YAP_V2_ann_index_save(const YAP_V2_ANN_INDEX *index, const char *path);
int YAP_V2_ann_index_view(const char *path, YAP_V2_VECTOR_METRIC metric,
                          size_t dimensions, size_t expected_count,
                          size_t expansion_search, YAP_V2_ANN_INDEX *index);
int YAP_V2_ann_index_search(const YAP_V2_ANN_INDEX *index, const float *query,
                            size_t dimensions, size_t top_k, uint64_t *keys,
                            size_t key_capacity, size_t *key_count);
int YAP_V2_ann_build_save(const char *path, const YAP_V2_VECTOR_SEGMENT *vectors,
                          size_t connectivity, size_t expansion_add,
                          size_t expansion_search, YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_ann_view(const char *path, const YAP_V2_VECTOR_SEGMENT *vectors,
                    size_t expansion_search, YAP_V2_ANN_SEGMENT *segment,
                    YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_ann_search(const YAP_V2_ANN_SEGMENT *segment, const float *query,
                      size_t dimensions, size_t top_k, YAP_VECTOR_HIT *hits,
                      size_t hit_capacity, size_t *hit_count);
int YAP_V2_ann_search_segments(const YAP_V2_ANN_SEGMENT *segments, size_t segment_count,
                               const float *query, size_t dimensions, size_t top_k,
                               YAP_V2_ANN_HIT *hits, size_t hit_capacity,
                               size_t *hit_count);

#endif
