#ifndef YAPPO_METADATA_V2_H
#define YAPPO_METADATA_V2_H

#include "yappo_index_v2.h"

typedef enum {
  YAP_V2_METADATA_NULL = 1,
  YAP_V2_METADATA_BOOL = 2,
  YAP_V2_METADATA_NUMBER = 3,
  YAP_V2_METADATA_STRING = 4
} YAP_V2_METADATA_TYPE;

typedef struct {
  uint32_t field_ordinal;
  uint64_t document_ordinal;
  YAP_V2_METADATA_TYPE type;
  YAP_V2_BYTES_VIEW value;
} YAP_V2_METADATA_ENTRY;

typedef struct {
  char fields[YAP_V2_MAX_FILTER_FIELDS][YAP_V2_MAX_FILTER_FIELD_BYTES + 1U];
  size_t field_count;
  uint64_t document_count;
  YAP_V2_METADATA_ENTRY *entries;
  size_t entry_count;
  unsigned char *storage;
  size_t storage_bytes;
} YAP_V2_METADATA_INDEX;

void YAP_V2_metadata_index_init(YAP_V2_METADATA_INDEX *index);
void YAP_V2_metadata_index_free(YAP_V2_METADATA_INDEX *index);
int YAP_V2_metadata_write(const char *path, uint64_t generation, const YAP_V2_CONFIG *config,
                          const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                          YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_metadata_write_preparsed(const char *path, uint64_t generation,
                                    const YAP_V2_CONFIG *config,
                                    const YAP_V2_DOCUMENT_VIEW *documents,
                                    const void *const *metadata_roots,
                                    size_t document_count,
                                    YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_metadata_read(const char *path, uint64_t expected_generation,
                         const YAP_V2_CONFIG *config, YAP_V2_METADATA_INDEX *index,
                         YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_metadata_field_ordinal(const YAP_V2_METADATA_INDEX *index, const char *field,
                                  uint32_t *ordinal);

#endif
