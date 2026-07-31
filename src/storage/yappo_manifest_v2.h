#ifndef YAPPO_MANIFEST_V2_H
#define YAPPO_MANIFEST_V2_H

#include "storage/yappo_storage_v2.h"

typedef struct {
  const YAP_V2_MANIFEST *manifest;
  size_t *slots;
  size_t capacity;
} YAP_V2_MANIFEST_SEGMENT_MAP;

int YAP_V2_segment_descriptor_equal(
  const YAP_V2_SEGMENT_DESCRIPTOR *left,
  const YAP_V2_SEGMENT_DESCRIPTOR *right);
void YAP_V2_manifest_segment_map_init(YAP_V2_MANIFEST_SEGMENT_MAP *map);
void YAP_V2_manifest_segment_map_free(YAP_V2_MANIFEST_SEGMENT_MAP *map);
int YAP_V2_manifest_segment_map_build(YAP_V2_MANIFEST_SEGMENT_MAP *map,
                                      const YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_segment_map_find(const YAP_V2_MANIFEST_SEGMENT_MAP *map,
                                     const char *segment_id,
                                     size_t *segment_index);

int YAP_V2_manifest_load(const char *path, YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_load_for_config(const char *path, const YAP_V2_CONFIG *config,
                                    YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_save_atomic(const char *path, const YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_publish_next(const char *path, YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_publish_if_generation(const char *path, uint64_t expected_generation,
                                          YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_verify_components(const char *index_dir, const YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_verify_segment_components(
  const char *index_dir, uint64_t manifest_generation,
  const YAP_V2_SEGMENT_DESCRIPTOR *segment);

#endif
