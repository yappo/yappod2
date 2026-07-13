#ifndef YAPPO_LEXICAL_SEARCH_V2_H
#define YAPPO_LEXICAL_SEARCH_V2_H

#include "yappo_lexical_v2.h"

typedef enum { YAP_V2_QUERY_OR = 1, YAP_V2_QUERY_AND = 2 } YAP_V2_QUERY_OPERATOR;

typedef struct {
  uint32_t object_type;
  YAP_V2_QUERY_OPERATOR query_operator;
  int phrase;
  double field_boost[3];
  size_t top_k;
  int (*accept)(void *context, uint32_t object_type, uint64_t object_ordinal);
  void *accept_context;
} YAP_V2_LEXICAL_SEARCH_OPTIONS;

typedef struct {
  uint32_t object_type;
  uint64_t object_ordinal;
  double score;
  uint32_t matched_terms;
} YAP_V2_LEXICAL_HIT;

void YAP_V2_lexical_search_options_init(YAP_V2_LEXICAL_SEARCH_OPTIONS *options);
int YAP_V2_lexical_search(const YAP_V2_LEXICAL_SEGMENT *segment, YAP_V2_BYTES_VIEW query,
                          const YAP_V2_LEXICAL_SEARCH_OPTIONS *options, YAP_V2_LEXICAL_HIT *hits,
                          size_t hit_capacity, size_t *hit_count);

#endif
