#ifndef YAPPO_ANN_CORPUS_V2_H
#define YAPPO_ANN_CORPUS_V2_H

#include "components/yappo_ann_v2.h"
#include "storage/yappo_manifest_v2.h"
#include "storage/yappo_snapshot_v2.h"

typedef struct {
  YAP_V2_ANN_INDEX index;
  YAP_V2_SEGMENT_DESCRIPTOR *segments;
  uint64_t *segment_fingerprints;
  size_t segment_count;
  size_t vector_count;
  uint64_t generation;
} YAP_V2_ANN_CORPUS;

typedef struct {
  size_t *base_to_current;
  unsigned char *current_is_delta;
  size_t base_segment_count;
  size_t current_segment_count;
  size_t delta_segment_count;
  size_t missing_base_segment_count;
} YAP_V2_ANN_QUERY_PLAN;

void YAP_V2_ann_corpus_init(YAP_V2_ANN_CORPUS *corpus);
void YAP_V2_ann_corpus_free(YAP_V2_ANN_CORPUS *corpus);
int YAP_V2_ann_corpus_build(const YAP_V2_MANIFEST *manifest,
                            const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                            const YAP_V2_ANN_SEGMENT *segments,
                            size_t segment_count, YAP_V2_ANN_CORPUS *corpus);
int YAP_V2_ann_corpus_search(const YAP_V2_ANN_CORPUS *corpus, const float *query,
                             size_t dimensions, size_t top_k, uint64_t *keys,
                             size_t key_capacity, size_t *key_count);
int YAP_V2_ann_corpus_save_cache(const char *index_dir,
                                 const YAP_V2_ANN_CORPUS *corpus);
int YAP_V2_ann_corpus_load_cache(const char *index_dir,
                                 const YAP_V2_CONFIG *config,
                                 const YAP_V2_MANIFEST *manifest,
                                 YAP_V2_ANN_CORPUS *corpus);
void YAP_V2_ann_query_plan_init(YAP_V2_ANN_QUERY_PLAN *plan);
void YAP_V2_ann_query_plan_free(YAP_V2_ANN_QUERY_PLAN *plan);
int YAP_V2_ann_query_plan_build(const YAP_V2_ANN_CORPUS *corpus,
                                const YAP_V2_MANIFEST *manifest,
                                YAP_V2_ANN_QUERY_PLAN *plan);

#endif
