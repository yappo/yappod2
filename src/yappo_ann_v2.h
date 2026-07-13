#ifndef YAPPO_ANN_V2_H
#define YAPPO_ANN_V2_H

#include <usearch.h>

#include "yappo_vector_v2.h"

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
  size_t segment_ordinal;
  YAP_VECTOR_HIT hit;
} YAP_V2_ANN_HIT;

const char *YAP_V2_ann_status_string(YAP_ANN_STATUS status);
void YAP_V2_ann_segment_init(YAP_V2_ANN_SEGMENT *segment);
void YAP_V2_ann_segment_close(YAP_V2_ANN_SEGMENT *segment);
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
