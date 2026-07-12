/* Exact vector search backend used as the reference implementation for v2. */
#include "yappo_vector.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *YAP_Vector_status_string(YAP_VECTOR_STATUS status) {
  switch (status) {
    case YAP_VECTOR_OK:
      return "ok";
    case YAP_VECTOR_INVALID_ARGUMENT:
      return "invalid argument";
    case YAP_VECTOR_DIMENSION_MISMATCH:
      return "dimension mismatch";
    case YAP_VECTOR_NON_FINITE:
      return "non-finite vector value";
    case YAP_VECTOR_BUFFER_TOO_SMALL:
      return "buffer too small";
    case YAP_VECTOR_ALLOCATION_FAILED:
      return "allocation failed";
    default:
      return "unknown status";
  }
}

static int validate_metric(YAP_V2_VECTOR_METRIC metric) {
  return metric >= YAP_V2_VECTOR_COSINE && metric <= YAP_V2_VECTOR_L2;
}

static int validate_values(const float *values, size_t dimensions) {
  size_t i;

  if (values == NULL || dimensions == 0U || dimensions > YAP_V2_MAX_VECTOR_DIMENSIONS) {
    return YAP_VECTOR_INVALID_ARGUMENT;
  }
  for (i = 0U; i < dimensions; i++) {
    if (!isfinite((double)values[i])) {
      return YAP_VECTOR_NON_FINITE;
    }
  }
  return YAP_VECTOR_OK;
}

int YAP_Vector_score(YAP_V2_VECTOR_METRIC metric, const float *query, const float *candidate,
                     size_t dimensions, double *score_out) {
  double dot = 0.0;
  double query_norm = 0.0;
  double candidate_norm = 0.0;
  size_t i;
  int status;

  if (score_out == NULL || !validate_metric(metric)) {
    return YAP_VECTOR_INVALID_ARGUMENT;
  }
  status = validate_values(query, dimensions);
  if (status != YAP_VECTOR_OK) return status;
  status = validate_values(candidate, dimensions);
  if (status != YAP_VECTOR_OK) return status;
  for (i = 0U; i < dimensions; i++) {
    double q = (double)query[i];
    double c = (double)candidate[i];
    dot += q * c;
    if (metric == YAP_V2_VECTOR_COSINE) {
      query_norm += q * q;
      candidate_norm += c * c;
    }
  }
  if (metric == YAP_V2_VECTOR_DOT) {
    *score_out = dot;
  } else if (metric == YAP_V2_VECTOR_L2) {
    double distance = 0.0;
    for (i = 0U; i < dimensions; i++) {
      double difference = (double)query[i] - (double)candidate[i];
      distance += difference * difference;
    }
    *score_out = -distance;
  } else if (query_norm == 0.0 || candidate_norm == 0.0) {
    *score_out = 0.0;
  } else {
    *score_out = dot / (sqrt(query_norm) * sqrt(candidate_norm));
  }
  return isfinite(*score_out) ? YAP_VECTOR_OK : YAP_VECTOR_NON_FINITE;
}

static int compare_hits(const void *left, const void *right) {
  const YAP_VECTOR_HIT *left_hit = (const YAP_VECTOR_HIT *)left;
  const YAP_VECTOR_HIT *right_hit = (const YAP_VECTOR_HIT *)right;

  if (left_hit->score > right_hit->score) return -1;
  if (left_hit->score < right_hit->score) return 1;
  if (left_hit->ordinal < right_hit->ordinal) return -1;
  if (left_hit->ordinal > right_hit->ordinal) return 1;
  return 0;
}

int YAP_Vector_search(const YAP_VECTOR_ENTRY *entries, size_t entry_count, const float *query,
                      size_t dimensions, YAP_V2_VECTOR_METRIC metric, size_t top_k,
                      YAP_VECTOR_HIT *hits, size_t hit_capacity, size_t *hit_count_out) {
  YAP_VECTOR_HIT *ranked;
  size_t i;
  size_t result_count;
  int status;

  if (hit_count_out == NULL || !validate_metric(metric) || entries == NULL || entry_count == 0U ||
      top_k == 0U || hits == NULL) {
    return YAP_VECTOR_INVALID_ARGUMENT;
  }
  if (hit_capacity < top_k) {
    return YAP_VECTOR_BUFFER_TOO_SMALL;
  }
  status = validate_values(query, dimensions);
  if (status != YAP_VECTOR_OK) {
    return status;
  }
  if (entry_count > SIZE_MAX / sizeof(*ranked)) {
    return YAP_VECTOR_ALLOCATION_FAILED;
  }
  ranked = (YAP_VECTOR_HIT *)malloc(sizeof(*ranked) * entry_count);
  if (ranked == NULL) {
    return YAP_VECTOR_ALLOCATION_FAILED;
  }
  for (i = 0U; i < entry_count; i++) {
    int status;
    double score;
    if (entries[i].dimensions != dimensions || entries[i].id.data == NULL || entries[i].id.len == 0U) {
      free(ranked);
      return entries[i].dimensions != dimensions ? YAP_VECTOR_DIMENSION_MISMATCH
                                                  : YAP_VECTOR_INVALID_ARGUMENT;
    }
    status = YAP_Vector_score(metric, query, entries[i].values, dimensions, &score);
    if (status != YAP_VECTOR_OK) {
      free(ranked);
      return status;
    }
    ranked[i].id = entries[i].id;
    ranked[i].ordinal = i;
    ranked[i].score = score;
  }
  qsort(ranked, entry_count, sizeof(*ranked), compare_hits);
  result_count = top_k < entry_count ? top_k : entry_count;
  memcpy(hits, ranked, sizeof(*hits) * result_count);
  *hit_count_out = result_count;
  free(ranked);
  return YAP_VECTOR_OK;
}
