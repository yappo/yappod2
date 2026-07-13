#ifndef YAPPO_SNAPSHOT_V2_H
#define YAPPO_SNAPSHOT_V2_H

#include "yappo_config_v2.h"

typedef struct YAP_V2_SEARCH_SNAPSHOT YAP_V2_SEARCH_SNAPSHOT;

typedef struct {
  void *state;
} YAP_V2_SNAPSHOT_MANAGER;

typedef struct {
  size_t segment_ordinal;
  size_t document_ordinal;
  const YAP_V2_DOCUMENT_VIEW *document;
} YAP_V2_DOCUMENT_HIT;

void YAP_V2_snapshot_manager_init(YAP_V2_SNAPSHOT_MANAGER *manager);
void YAP_V2_snapshot_manager_close(YAP_V2_SNAPSHOT_MANAGER *manager);
int YAP_V2_snapshot_manager_open(YAP_V2_SNAPSHOT_MANAGER *manager, const char *index_dir,
                                 const char *manifest_path, const YAP_V2_CONFIG *config);
int YAP_V2_snapshot_manager_reload(YAP_V2_SNAPSHOT_MANAGER *manager, int *changed);
YAP_V2_SEARCH_SNAPSHOT *YAP_V2_snapshot_acquire(YAP_V2_SNAPSHOT_MANAGER *manager);
void YAP_V2_snapshot_release(YAP_V2_SEARCH_SNAPSHOT *snapshot);
uint64_t YAP_V2_snapshot_generation(const YAP_V2_SEARCH_SNAPSHOT *snapshot);
size_t YAP_V2_snapshot_segment_count(const YAP_V2_SEARCH_SNAPSHOT *snapshot);
const YAP_V2_SEGMENT *YAP_V2_snapshot_segment_documents(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                                        size_t segment_ordinal);
int YAP_V2_snapshot_document_visible(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                     size_t segment_ordinal, YAP_V2_BYTES_VIEW document_id);
int YAP_V2_snapshot_lookup_document(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                    YAP_V2_BYTES_VIEW document_id, YAP_V2_DOCUMENT_HIT *hit);

#endif
