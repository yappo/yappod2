#ifndef YAPPO_MANIFEST_V2_H
#define YAPPO_MANIFEST_V2_H

#include "yappo_index_v2.h"

int YAP_V2_manifest_load(const char *path, YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_load_for_config(const char *path, const YAP_V2_CONFIG *config,
                                    YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_save_atomic(const char *path, const YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_publish_next(const char *path, YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_verify_components(const char *index_dir, const YAP_V2_MANIFEST *manifest);

#endif
