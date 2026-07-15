#ifndef YAPPO_SEGMENT_PLANNER_V2_H
#define YAPPO_SEGMENT_PLANNER_V2_H

#include "yappo_config_v2.h"

typedef struct {
  const YAP_V2_DOCUMENT_VIEW *document;
  const YAP_V2_PASSAGE_VIEW *passages;
  size_t passage_count;
  const float *vectors;
  YAP_V2_BYTES_VIEW tombstone;
} YAP_V2_SEGMENT_UNIT;

typedef struct {
  size_t first;
  size_t count;
  size_t payload_bytes[7];
} YAP_V2_SEGMENT_SLICE;

enum {
  YAP_V2_SEGMENT_COMPONENT_DOCUMENTS = 0,
  YAP_V2_SEGMENT_COMPONENT_TERMS = 1,
  YAP_V2_SEGMENT_COMPONENT_POSTINGS = 2,
  YAP_V2_SEGMENT_COMPONENT_POSITIONS = 3,
  YAP_V2_SEGMENT_COMPONENT_METADATA = 4,
  YAP_V2_SEGMENT_COMPONENT_VECTORS = 5,
  YAP_V2_SEGMENT_COMPONENT_TOMBSTONES = 6,
  YAP_V2_SEGMENT_COMPONENT_COUNT = 7
};

typedef struct {
  YAP_V2_SEGMENT_SLICE *slices;
  size_t count;
  void *prepared_units;
  size_t prepared_unit_count;
} YAP_V2_SEGMENT_PLAN;

typedef struct {
  const char *component;
  size_t required_bytes;
  size_t limit_bytes;
  YAP_V2_BYTES_VIEW document_id;
} YAP_V2_SEGMENT_CAPACITY_ERROR;

void YAP_V2_segment_plan_init(YAP_V2_SEGMENT_PLAN *plan);
void YAP_V2_segment_plan_free(YAP_V2_SEGMENT_PLAN *plan);
int YAP_V2_segment_plan_bisect(YAP_V2_SEGMENT_PLAN *plan, size_t slice_index);
int YAP_V2_segment_count_validate(size_t existing_count, size_t added_count);
size_t YAP_V2_segment_planner_payload_limit(void);
void YAP_V2_segment_planner_set_payload_limit_for_testing(size_t payload_limit);
int YAP_V2_segment_plan(const YAP_V2_CONFIG *config,
                        const YAP_V2_SEGMENT_UNIT *units, size_t unit_count,
                        size_t segment_id_bytes, size_t payload_limit,
                        YAP_V2_SEGMENT_PLAN *plan,
                        YAP_V2_SEGMENT_CAPACITY_ERROR *capacity_error);

int YAP_V2_segment_slice_write(const char *directory, const char *segment_id,
                               uint64_t generation, const YAP_V2_CONFIG *config,
                               const YAP_V2_SEGMENT_UNIT *units,
                               const YAP_V2_SEGMENT_PLAN *plan,
                               YAP_V2_SEGMENT_SLICE slice,
                               YAP_V2_SEGMENT_DESCRIPTOR *descriptor);

#endif
