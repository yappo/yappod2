#ifndef YAPPO_TYPES_V2_H
#define YAPPO_TYPES_V2_H

#include <stddef.h>
#include <stdint.h>

#define YAP_V2_FORMAT_VERSION UINT16_C(2)
#define YAP_V2_MAX_IDENTIFIER_BYTES 255U
#define YAP_V2_MAX_URL_BYTES (8U * 1024U)
#define YAP_V2_MAX_METADATA_BYTES (1024U * 1024U)
#define YAP_V2_MAX_CHUNK_CHARS (1024U * 1024U)

typedef enum {
  YAP_V2_OK = 0,
  YAP_V2_INVALID_ARGUMENT = -1,
  YAP_V2_INVALID_FORMAT = -2,
  YAP_V2_OUT_OF_RANGE = -3,
  YAP_V2_DUPLICATE = -4,
  YAP_V2_ALLOCATION_FAILED = -5,
  YAP_V2_IO_ERROR = -6,
  YAP_V2_CHECKSUM_MISMATCH = -7,
  YAP_V2_CONFLICT = -8,
  YAP_V2_NOT_FOUND = -9,
  YAP_V2_SEGMENT_CAPACITY_EXCEEDED = -10
} YAP_V2_STATUS;

/* A byte view never owns data. All text values must be UTF-8 without NUL bytes. */
typedef struct {
  const unsigned char *data;
  size_t len;
} YAP_V2_BYTES_VIEW;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  YAP_V2_BYTES_VIEW url;
  YAP_V2_BYTES_VIEW title;
  YAP_V2_BYTES_VIEW body;
  YAP_V2_BYTES_VIEW metadata_json;
  int64_t updated_at_unix_ms;
} YAP_V2_DOCUMENT_VIEW;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  YAP_V2_BYTES_VIEW parent_document_id;
  YAP_V2_BYTES_VIEW text;
  uint32_t ordinal;
  uint32_t start_char;
  uint32_t end_char;
} YAP_V2_PASSAGE_VIEW;

const char *YAP_V2_status_string(YAP_V2_STATUS status);
int YAP_V2_document_validate(const YAP_V2_DOCUMENT_VIEW *document);
int YAP_V2_passage_validate(const YAP_V2_PASSAGE_VIEW *passage);

#endif
