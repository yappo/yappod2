#ifndef YAPPO_COMPACTION_POLICY_V2_H
#define YAPPO_COMPACTION_POLICY_V2_H

#include "common/yappo_types_v2.h"

#include <stddef.h>
#include <stdint.h>

#define YAP_V2_DEFAULT_AUTO_COMPACT_CHECK_INTERVAL_MS 30000U
#define YAP_V2_MIN_AUTO_COMPACT_CHECK_INTERVAL_MS 1000U
#define YAP_V2_MAX_AUTO_COMPACT_CHECK_INTERVAL_MS 3600000U
#define YAP_V2_DEFAULT_AUTO_COMPACT_SMALL_SEGMENT_BYTES \
  (64U * 1024U * 1024U)
#define YAP_V2_MAX_AUTO_COMPACT_SMALL_SEGMENT_BYTES \
  (256U * 1024U * 1024U)
#define YAP_V2_DEFAULT_AUTO_COMPACT_MIN_SMALL_SEGMENTS 4U
#define YAP_V2_MIN_AUTO_COMPACT_SMALL_SEGMENTS 2U
#define YAP_V2_MAX_AUTO_COMPACT_SMALL_SEGMENTS 8U

typedef struct {
  int enabled;
  uint32_t check_interval_ms;
  size_t small_segment_bytes;
  size_t min_small_segments;
} YAP_V2_COMPACTION_POLICY;

void YAP_V2_compaction_policy_init(YAP_V2_COMPACTION_POLICY *policy);
int YAP_V2_compaction_policy_validate(
  const YAP_V2_COMPACTION_POLICY *policy);

#endif
