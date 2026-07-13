#ifndef YAPPO_RETRIEVE_V2_H
#define YAPPO_RETRIEVE_V2_H

#include "yappo_query_v2.h"

typedef struct {
  size_t max_passages;
  size_t max_passages_per_document;
  size_t max_context_bytes;
} YAP_V2_RETRIEVE_OPTIONS;

typedef struct {
  YAP_V2_BYTES_VIEW passage_id;
  YAP_V2_BYTES_VIEW document_id;
  YAP_V2_BYTES_VIEW url;
  YAP_V2_BYTES_VIEW title;
  YAP_V2_BYTES_VIEW text;
  uint32_t start_char;
  uint32_t end_char;
  double lexical_score;
  double vector_score;
  double fused_score;
  size_t context_start;
  size_t context_end;
} YAP_V2_CITATION;

void YAP_V2_retrieve_options_init(YAP_V2_RETRIEVE_OPTIONS *options);
int YAP_V2_retrieve_context(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                            const YAP_V2_QUERY_HIT *ranked_passages, size_t ranked_count,
                            const YAP_V2_RETRIEVE_OPTIONS *options,
                            unsigned char *context, size_t context_capacity,
                            size_t *context_bytes, YAP_V2_CITATION *citations,
                            size_t citation_capacity, size_t *citation_count);

#endif
