#ifndef YAPPO_QUERY_V2_H
#define YAPPO_QUERY_V2_H

#include "yappo_ann_v2.h"
#include "yappo_filter_v2.h"
#include "yappo_hybrid.h"
#include "yappo_lexical_search_v2.h"
#include "yappo_snapshot_v2.h"

typedef enum {
  YAP_V2_SEARCH_LEXICAL = 1,
  YAP_V2_SEARCH_VECTOR = 2,
  YAP_V2_SEARCH_HYBRID = 3
} YAP_V2_SEARCH_MODE;

typedef enum {
  YAP_V2_SEARCH_DOCUMENTS = 1,
  YAP_V2_SEARCH_PASSAGES = 2
} YAP_V2_SEARCH_SCOPE;

typedef struct {
  const YAP_V2_LEXICAL_SEGMENT *lexical;
  const YAP_V2_ANN_SEGMENT *vector;
  const YAP_V2_METADATA_INDEX *metadata;
} YAP_V2_QUERY_SEGMENT;

typedef struct {
  YAP_V2_SEARCH_MODE mode;
  YAP_V2_SEARCH_SCOPE scope;
  YAP_V2_BYTES_VIEW query;
  const float *query_vector;
  size_t query_dimensions;
  YAP_V2_BYTES_VIEW filter_json;
  YAP_V2_QUERY_OPERATOR query_operator;
  int phrase;
  size_t top_k;
  size_t candidate_k;
  double lexical_weight;
  double vector_weight;
} YAP_V2_QUERY_REQUEST;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  YAP_V2_BYTES_VIEW parent_document_id;
  size_t segment_ordinal;
  size_t object_ordinal;
  double lexical_score;
  double vector_score;
  double fused_score;
} YAP_V2_QUERY_HIT;

void YAP_V2_query_request_init(YAP_V2_QUERY_REQUEST *request);
int YAP_V2_query_execute(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                         const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                         const YAP_V2_QUERY_REQUEST *request, YAP_V2_QUERY_HIT *hits,
                         size_t hit_capacity, size_t *hit_count);

#endif
