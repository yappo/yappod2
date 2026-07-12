/* Exact vector search backend used as the reference implementation for v2. */
#ifndef YAPPO_VECTOR_H
#define YAPPO_VECTOR_H

#include <stddef.h>

#include "yappo_index_v2.h"

typedef enum {
  YAP_VECTOR_OK = 0,
  YAP_VECTOR_INVALID_ARGUMENT = -1,
  YAP_VECTOR_DIMENSION_MISMATCH = -2,
  YAP_VECTOR_NON_FINITE = -3,
  YAP_VECTOR_BUFFER_TOO_SMALL = -4,
  YAP_VECTOR_ALLOCATION_FAILED = -5
} YAP_VECTOR_STATUS;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  const float *values;
  size_t dimensions;
} YAP_VECTOR_ENTRY;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  size_t ordinal;
  double score;
} YAP_VECTOR_HIT;

const char *YAP_Vector_status_string(YAP_VECTOR_STATUS status);
int YAP_Vector_score(YAP_V2_VECTOR_METRIC metric, const float *query, const float *candidate,
                     size_t dimensions, double *score_out);
int YAP_Vector_search(const YAP_VECTOR_ENTRY *entries, size_t entry_count, const float *query,
                      size_t dimensions, YAP_V2_VECTOR_METRIC metric, size_t top_k,
                      YAP_VECTOR_HIT *hits, size_t hit_capacity, size_t *hit_count_out);

#endif
