/* Rank-based hybrid fusion for lexical and vector candidate lists. */
#include "yappo_hybrid.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  YAP_HYBRID_HIT hit;
  size_t ordinal;
  int lexical_present;
  int vector_present;
} YAP_HYBRID_MERGED;

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len && left.len > 0U && left.data != NULL && right.data != NULL &&
         memcmp(left.data, right.data, left.len) == 0;
}

const char *YAP_Hybrid_status_string(YAP_HYBRID_STATUS status) {
  switch (status) {
    case YAP_HYBRID_OK:
      return "ok";
    case YAP_HYBRID_INVALID_ARGUMENT:
      return "invalid argument";
    case YAP_HYBRID_BUFFER_TOO_SMALL:
      return "buffer too small";
    case YAP_HYBRID_ALLOCATION_FAILED:
      return "allocation failed";
    default:
      return "unknown status";
  }
}

static int validate_candidates(const YAP_HYBRID_CANDIDATE *candidates, size_t count) {
  size_t i;

  if (count > 0U && candidates == NULL) return YAP_HYBRID_INVALID_ARGUMENT;
  for (i = 0U; i < count; i++) {
    if (candidates[i].id.data == NULL || candidates[i].id.len == 0U ||
        !isfinite(candidates[i].score)) {
      return YAP_HYBRID_INVALID_ARGUMENT;
    }
  }
  return YAP_HYBRID_OK;
}

static int compare_merged(const void *left, const void *right) {
  const YAP_HYBRID_MERGED *left_hit = (const YAP_HYBRID_MERGED *)left;
  const YAP_HYBRID_MERGED *right_hit = (const YAP_HYBRID_MERGED *)right;

  if (left_hit->hit.fused_score > right_hit->hit.fused_score) return -1;
  if (left_hit->hit.fused_score < right_hit->hit.fused_score) return 1;
  if (left_hit->ordinal < right_hit->ordinal) return -1;
  if (left_hit->ordinal > right_hit->ordinal) return 1;
  return 0;
}

static void add_candidate(YAP_HYBRID_MERGED *merged, size_t *merged_count,
                          const YAP_HYBRID_CANDIDATE *candidate, double weight, size_t rank,
                          int lexical) {
  size_t i;
  double contribution;

  contribution = weight / (YAP_HYBRID_RRF_K + (double)rank);
  for (i = 0U; i < *merged_count; i++) {
    if (bytes_equal(merged[i].hit.id, candidate->id)) {
      if (lexical) {
        if (merged[i].lexical_present) return;
        merged[i].hit.lexical_score = candidate->score;
        merged[i].lexical_present = 1;
      } else {
        if (merged[i].vector_present) return;
        merged[i].hit.vector_score = candidate->score;
        merged[i].vector_present = 1;
      }
      merged[i].hit.fused_score += contribution;
      return;
    }
  }
  merged[*merged_count].hit.id = candidate->id;
  merged[*merged_count].hit.lexical_score = lexical ? candidate->score : 0.0;
  merged[*merged_count].hit.vector_score = lexical ? 0.0 : candidate->score;
  merged[*merged_count].hit.fused_score = contribution;
  merged[*merged_count].ordinal = *merged_count;
  merged[*merged_count].lexical_present = lexical;
  merged[*merged_count].vector_present = !lexical;
  (*merged_count)++;
}

int YAP_Hybrid_fuse_rrf(const YAP_HYBRID_CANDIDATE *lexical, size_t lexical_count,
                        const YAP_HYBRID_CANDIDATE *vector, size_t vector_count,
                        double lexical_weight, double vector_weight, size_t top_k,
                        YAP_HYBRID_HIT *hits, size_t hit_capacity, size_t *hit_count_out) {
  YAP_HYBRID_MERGED *merged;
  size_t merged_count = 0U;
  size_t i;
  size_t result_count;
  int status;

  if (hit_count_out == NULL || top_k == 0U || hits == NULL || hit_capacity < top_k ||
      lexical_count > SIZE_MAX - vector_count ||
      (!isfinite(lexical_weight) || !isfinite(vector_weight)) || lexical_weight < 0.0 ||
      vector_weight < 0.0 || (lexical_weight == 0.0 && vector_weight == 0.0)) {
    return hit_capacity < top_k && top_k > 0U ? YAP_HYBRID_BUFFER_TOO_SMALL
                                              : YAP_HYBRID_INVALID_ARGUMENT;
  }
  status = validate_candidates(lexical, lexical_count);
  if (status != YAP_HYBRID_OK) return status;
  status = validate_candidates(vector, vector_count);
  if (status != YAP_HYBRID_OK) return status;
  if (lexical_count + vector_count == 0U) {
    *hit_count_out = 0U;
    return YAP_HYBRID_OK;
  }
  if (lexical_count + vector_count > SIZE_MAX / sizeof(*merged)) {
    return YAP_HYBRID_ALLOCATION_FAILED;
  }
  merged = (YAP_HYBRID_MERGED *)calloc(lexical_count + vector_count, sizeof(*merged));
  if (merged == NULL) return YAP_HYBRID_ALLOCATION_FAILED;
  for (i = 0U; i < lexical_count; i++) {
    add_candidate(merged, &merged_count, &lexical[i], lexical_weight, i + 1U, 1);
  }
  for (i = 0U; i < vector_count; i++) {
    add_candidate(merged, &merged_count, &vector[i], vector_weight, i + 1U, 0);
  }
  qsort(merged, merged_count, sizeof(*merged), compare_merged);
  result_count = top_k < merged_count ? top_k : merged_count;
  for (i = 0U; i < result_count; i++) {
    hits[i] = merged[i].hit;
  }
  *hit_count_out = result_count;
  free(merged);
  return YAP_HYBRID_OK;
}
