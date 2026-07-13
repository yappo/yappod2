#ifndef YAPPO_FILTER_V2_H
#define YAPPO_FILTER_V2_H

#include "yappo_metadata_v2.h"

typedef struct {
  void *document;
  const YAP_V2_METADATA_INDEX *metadata;
} YAP_V2_FILTER;

void YAP_V2_filter_init(YAP_V2_FILTER *filter);
void YAP_V2_filter_free(YAP_V2_FILTER *filter);
int YAP_V2_filter_compile(YAP_V2_BYTES_VIEW json, const YAP_V2_METADATA_INDEX *metadata,
                          YAP_V2_FILTER *filter);
int YAP_V2_filter_matches(const YAP_V2_FILTER *filter, uint64_t document_ordinal, int *matches);
int YAP_V2_filter_accept(void *context, uint32_t object_type, uint64_t object_ordinal);

#endif
