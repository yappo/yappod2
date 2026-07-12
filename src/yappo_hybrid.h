/* Rank-based hybrid fusion for lexical and vector candidate lists. */
#ifndef YAPPO_HYBRID_H
#define YAPPO_HYBRID_H

#include <stddef.h>

#include "yappo_index_v2.h"

#define YAP_HYBRID_RRF_K 60.0

typedef enum {
  YAP_HYBRID_OK = 0,
  YAP_HYBRID_INVALID_ARGUMENT = -1,
  YAP_HYBRID_BUFFER_TOO_SMALL = -2,
  YAP_HYBRID_ALLOCATION_FAILED = -3
} YAP_HYBRID_STATUS;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  double score;
} YAP_HYBRID_CANDIDATE;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  double lexical_score;
  double vector_score;
  double fused_score;
} YAP_HYBRID_HIT;

const char *YAP_Hybrid_status_string(YAP_HYBRID_STATUS status);

/* Input list order is rank order (rank 1 is index 0); IDs must be non-empty. */
int YAP_Hybrid_fuse_rrf(const YAP_HYBRID_CANDIDATE *lexical, size_t lexical_count,
                        const YAP_HYBRID_CANDIDATE *vector, size_t vector_count,
                        double lexical_weight, double vector_weight, size_t top_k,
                        YAP_HYBRID_HIT *hits, size_t hit_capacity, size_t *hit_count_out);

#endif
