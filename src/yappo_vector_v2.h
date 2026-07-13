#ifndef YAPPO_VECTOR_V2_H
#define YAPPO_VECTOR_V2_H

#include "yappo_embedding.h"
#include "yappo_vector.h"

typedef struct {
  void *map;
  size_t map_bytes;
  uint64_t generation;
  YAP_V2_VECTOR_METRIC metric;
  size_t dimensions;
  char model_id[YAP_V2_MAX_MODEL_ID_BYTES + 1U];
  YAP_VECTOR_ENTRY *entries;
  size_t entry_count;
} YAP_V2_VECTOR_SEGMENT;

void YAP_V2_vector_segment_init(YAP_V2_VECTOR_SEGMENT *segment);
void YAP_V2_vector_segment_close(YAP_V2_VECTOR_SEGMENT *segment);
int YAP_V2_vectors_write(const char *path, uint64_t generation, const YAP_V2_CONFIG *config,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         const YAP_EMBEDDING_RESULT *embeddings,
                         YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_vector_segment_open(const char *path, uint64_t expected_generation,
                               const YAP_V2_CONFIG *config, YAP_V2_VECTOR_SEGMENT *segment,
                               YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_vector_segment_search(const YAP_V2_VECTOR_SEGMENT *segment, const float *query,
                                 size_t query_dimensions, size_t top_k, YAP_VECTOR_HIT *hits,
                                 size_t hit_capacity, size_t *hit_count);

#endif
