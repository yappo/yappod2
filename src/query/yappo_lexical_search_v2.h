#ifndef YAPPO_LEXICAL_SEARCH_V2_H
#define YAPPO_LEXICAL_SEARCH_V2_H

#include "common/yappo_unicode.h"
#include "components/yappo_lexical_v2.h"

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

typedef struct {
  YAP_V2_TOKEN_SEQUENCE tokens;
  YAP_V2_BYTES_VIEW *terms;
  size_t *token_terms;
  size_t term_count;
  const YAP_V2_LEXICAL_SEGMENT **segments;
  const YAP_V2_TERM_ENTRY **segment_terms;
  uint64_t *type_frequency[2];
  size_t segment_count;
} YAP_V2_LEXICAL_QUERY_PLAN;

typedef struct {
  uint64_t document_count;
  uint64_t passage_count;
  uint64_t field_token_count[3];
} YAP_V2_LEXICAL_CORPUS_STATS;

void YAP_V2_lexical_search_options_init(YAP_V2_LEXICAL_SEARCH_OPTIONS *options);
void YAP_V2_lexical_query_plan_init(YAP_V2_LEXICAL_QUERY_PLAN *plan);
void YAP_V2_lexical_query_plan_free(YAP_V2_LEXICAL_QUERY_PLAN *plan);
int YAP_V2_lexical_query_plan_prepare(YAP_V2_BYTES_VIEW query,
                                      YAP_V2_LEXICAL_QUERY_PLAN *plan);
int YAP_V2_lexical_query_plan_bind(YAP_V2_LEXICAL_QUERY_PLAN *plan,
                                   const YAP_V2_LEXICAL_SEGMENT *const *segments,
                                   size_t segment_count);
int YAP_V2_lexical_search_prepared(const YAP_V2_LEXICAL_QUERY_PLAN *plan,
                                   size_t segment_index,
                                   const YAP_V2_LEXICAL_CORPUS_STATS *stats,
                                   const YAP_V2_LEXICAL_SEARCH_OPTIONS *options,
                                   YAP_V2_LEXICAL_HIT *hits, size_t hit_capacity,
                                   size_t *hit_count);
int YAP_V2_lexical_search(const YAP_V2_LEXICAL_SEGMENT *segment, YAP_V2_BYTES_VIEW query,
                          const YAP_V2_LEXICAL_SEARCH_OPTIONS *options, YAP_V2_LEXICAL_HIT *hits,
                          size_t hit_capacity, size_t *hit_count);

#endif
