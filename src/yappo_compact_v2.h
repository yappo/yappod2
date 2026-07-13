#ifndef YAPPO_COMPACT_V2_H
#define YAPPO_COMPACT_V2_H

#include "yappo_index_v2.h"

typedef struct {
  uint64_t generation;
  size_t documents;
  size_t passages;
  size_t removed_segments;
  char segment_id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
} YAP_V2_COMPACTION_RESULT;

int YAP_V2_compact_gc(const char *index_dir, const YAP_V2_MANIFEST *manifest,
                      size_t *removed_segments);
int YAP_V2_compact(const char *index_dir, YAP_V2_COMPACTION_RESULT *result,
                   char *error, size_t error_size);
int YAP_V2_compact_main(int argc, char **argv);

#endif
