#include "config/yappo_compaction_policy_v2.h"

#include <string.h>

void YAP_V2_compaction_policy_init(YAP_V2_COMPACTION_POLICY *policy) {
  if (policy == NULL) return;
  memset(policy, 0, sizeof(*policy));
  policy->enabled = 1;
  policy->check_interval_ms =
    YAP_V2_DEFAULT_AUTO_COMPACT_CHECK_INTERVAL_MS;
  policy->small_segment_bytes =
    YAP_V2_DEFAULT_AUTO_COMPACT_SMALL_SEGMENT_BYTES;
  policy->min_small_segments =
    YAP_V2_DEFAULT_AUTO_COMPACT_MIN_SMALL_SEGMENTS;
}

int YAP_V2_compaction_policy_validate(
    const YAP_V2_COMPACTION_POLICY *policy) {
  if (policy == NULL) return YAP_V2_INVALID_ARGUMENT;
  if ((policy->enabled != 0 && policy->enabled != 1) ||
      policy->check_interval_ms <
        YAP_V2_MIN_AUTO_COMPACT_CHECK_INTERVAL_MS ||
      policy->check_interval_ms >
        YAP_V2_MAX_AUTO_COMPACT_CHECK_INTERVAL_MS ||
      policy->small_segment_bytes == 0U ||
      policy->small_segment_bytes >
        YAP_V2_MAX_AUTO_COMPACT_SMALL_SEGMENT_BYTES ||
      policy->min_small_segments <
        YAP_V2_MIN_AUTO_COMPACT_SMALL_SEGMENTS ||
      policy->min_small_segments >
        YAP_V2_MAX_AUTO_COMPACT_SMALL_SEGMENTS)
    return YAP_V2_OUT_OF_RANGE;
  return YAP_V2_OK;
}
