#ifndef YAPPOD_V2_QUALITY_FIXTURE_H
#define YAPPOD_V2_QUALITY_FIXTURE_H

#include <stddef.h>

int YAP_Test_v2_quality_index_create(const char *index_dir);
int YAP_Test_v2_quality_index_create_segments(const char *index_dir,
                                              size_t segment_count);

#endif
