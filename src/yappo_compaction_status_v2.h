#ifndef YAPPO_COMPACTION_STATUS_V2_H
#define YAPPO_COMPACTION_STATUS_V2_H

#include "yappo_types_v2.h"

typedef enum {
  YAP_V2_COMPACTION_IDLE = 0,
  YAP_V2_COMPACTION_RUNNING = 1,
  YAP_V2_COMPACTION_SUCCEEDED = 2,
  YAP_V2_COMPACTION_FAILED = 3,
  YAP_V2_COMPACTION_INTERRUPTED = 4,
  YAP_V2_COMPACTION_UNKNOWN = 5
} YAP_V2_COMPACTION_STATE;

const char *YAP_V2_compaction_state_name(YAP_V2_COMPACTION_STATE state);
int YAP_V2_compaction_status_write(const char *index_dir, YAP_V2_COMPACTION_STATE state,
                                   uint64_t generation);

#endif
