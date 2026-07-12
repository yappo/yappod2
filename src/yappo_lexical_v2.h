#ifndef YAPPO_LEXICAL_V2_H
#define YAPPO_LEXICAL_V2_H

#include "yappo_index_v2.h"

#include <stddef.h>
#include <stdint.h>

#define YAP_V2_LEXICAL_PAYLOAD_VERSION UINT32_C(1)
#define YAP_V2_POSTINGS_BLOCK_SIZE 128U

typedef enum { YAP_V2_LEXICAL_DOCUMENT = 1, YAP_V2_LEXICAL_PASSAGE = 2 } YAP_V2_LEXICAL_OBJECT_TYPE;

typedef enum {
  YAP_V2_FIELD_TITLE = 1,
  YAP_V2_FIELD_BODY = 2,
  YAP_V2_FIELD_PASSAGE = 3
} YAP_V2_LEXICAL_FIELD;

int YAP_V2_lexical_write(const char *segment_dir, uint64_t generation,
                         const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         YAP_V2_COMPONENT_DESCRIPTOR components[3]);

#endif
