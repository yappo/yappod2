#ifndef YAPPO_INDEX_V2_H
#define YAPPO_INDEX_V2_H

#include <stddef.h>
#include <stdint.h>

#define YAP_V2_FORMAT_VERSION UINT16_C(2)
#define YAP_V2_FILE_HEADER_BYTES 32U
#define YAP_V2_MAGIC_0 'Y'
#define YAP_V2_MAGIC_1 'A'
#define YAP_V2_MAGIC_2 'P'
#define YAP_V2_MAGIC_3 '2'

#define YAP_V2_MAX_IDENTIFIER_BYTES 255U
#define YAP_V2_MAX_MODEL_ID_BYTES 255U
#define YAP_V2_MAX_METADATA_BYTES (1024U * 1024U)
#define YAP_V2_MAX_SEGMENTS 100000U
#define YAP_V2_MAX_VECTOR_DIMENSIONS 65536U
#define YAP_V2_MAX_CHUNK_CHARS (1024U * 1024U)
#define YAP_V2_MAX_SEGMENT_DOCUMENTS 1000000U
#define YAP_V2_MAX_SEGMENT_PASSAGES 4000000U
#define YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES (256U * 1024U * 1024U)
#define YAP_V2_MAX_COMPONENTS 8U
#define YAP_V2_MAX_COMPONENT_NAME_BYTES 63U

typedef enum {
  YAP_V2_OK = 0,
  YAP_V2_INVALID_ARGUMENT = -1,
  YAP_V2_INVALID_FORMAT = -2,
  YAP_V2_OUT_OF_RANGE = -3,
  YAP_V2_DUPLICATE = -4,
  YAP_V2_ALLOCATION_FAILED = -5,
  YAP_V2_IO_ERROR = -6,
  YAP_V2_CHECKSUM_MISMATCH = -7,
  YAP_V2_CONFLICT = -8
} YAP_V2_STATUS;

typedef enum {
  YAP_V2_FILE_TERMS = 1,
  YAP_V2_FILE_POSTINGS = 2,
  YAP_V2_FILE_POSITIONS = 3,
  YAP_V2_FILE_DOCUMENTS = 4,
  YAP_V2_FILE_METADATA = 5,
  YAP_V2_FILE_VECTORS = 6,
  YAP_V2_FILE_TOMBSTONES = 7
} YAP_V2_FILE_TYPE;

typedef enum {
  YAP_V2_VECTOR_DISABLED = 0,
  YAP_V2_VECTOR_COSINE = 1,
  YAP_V2_VECTOR_DOT = 2,
  YAP_V2_VECTOR_L2 = 3
} YAP_V2_VECTOR_METRIC;

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

typedef struct {
  uint32_t format_version;
  char tokenizer_id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  uint32_t chunk_max_chars;
  uint32_t chunk_overlap_chars;
  char vector_model_id[YAP_V2_MAX_MODEL_ID_BYTES + 1U];
  uint32_t vector_dimensions;
  YAP_V2_VECTOR_METRIC vector_metric;
} YAP_V2_CONFIG;

typedef struct {
  char name[YAP_V2_MAX_COMPONENT_NAME_BYTES + 1U];
  uint32_t file_type;
  uint64_t record_count;
  uint64_t file_bytes;
  unsigned char checksum[32];
} YAP_V2_COMPONENT_DESCRIPTOR;

typedef struct {
  char id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  uint64_t document_count;
  uint64_t passage_count;
  uint64_t tombstone_count;
  YAP_V2_COMPONENT_DESCRIPTOR components[YAP_V2_MAX_COMPONENTS];
  size_t component_count;
  /* Deprecated compatibility mirror for the documents component. */
  uint64_t file_bytes;
  unsigned char checksum[32];
} YAP_V2_SEGMENT_DESCRIPTOR;

typedef struct {
  uint32_t format_version;
  uint64_t generation;
  unsigned char config_fingerprint[32];
  YAP_V2_SEGMENT_DESCRIPTOR *segments;
  size_t segment_count;
} YAP_V2_MANIFEST;

typedef struct {
  uint16_t format_version;
  uint16_t header_bytes;
  uint32_t file_type;
  uint64_t generation;
  uint64_t payload_bytes;
  uint32_t payload_crc32c;
} YAP_V2_FILE_HEADER;

typedef struct {
  char id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  uint64_t generation;
  YAP_V2_DOCUMENT_VIEW *documents;
  size_t document_count;
  YAP_V2_PASSAGE_VIEW *passages;
  size_t passage_count;
  unsigned char *storage;
  size_t storage_bytes;
} YAP_V2_SEGMENT;

typedef struct {
  YAP_V2_BYTES_VIEW *document_ids;
  size_t count;
  unsigned char *storage;
  size_t storage_bytes;
} YAP_V2_TOMBSTONES;

const char *YAP_V2_status_string(YAP_V2_STATUS status);

int YAP_V2_segment_id_validate(const char *value);

int YAP_V2_document_validate(const YAP_V2_DOCUMENT_VIEW *document);
int YAP_V2_passage_validate(const YAP_V2_PASSAGE_VIEW *passage);
int YAP_V2_config_validate(const YAP_V2_CONFIG *config);

void YAP_V2_manifest_init(YAP_V2_MANIFEST *manifest);
void YAP_V2_manifest_free(YAP_V2_MANIFEST *manifest);
int YAP_V2_manifest_add_segment(YAP_V2_MANIFEST *manifest,
                                const YAP_V2_SEGMENT_DESCRIPTOR *segment);
int YAP_V2_segment_descriptor_add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                                            const YAP_V2_COMPONENT_DESCRIPTOR *component);
int YAP_V2_manifest_validate(const YAP_V2_MANIFEST *manifest);

int YAP_V2_file_header_encode(const YAP_V2_FILE_HEADER *header,
                              unsigned char output[YAP_V2_FILE_HEADER_BYTES]);
int YAP_V2_file_header_decode(const unsigned char input[YAP_V2_FILE_HEADER_BYTES],
                              YAP_V2_FILE_HEADER *header);

void YAP_V2_segment_init(YAP_V2_SEGMENT *segment);
void YAP_V2_segment_free(YAP_V2_SEGMENT *segment);

int YAP_V2_segment_write(const char *path, const char *segment_id, uint64_t generation,
                         const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         YAP_V2_SEGMENT_DESCRIPTOR *descriptor);
int YAP_V2_segment_read(const char *path, uint64_t expected_generation, YAP_V2_SEGMENT *segment,
                        YAP_V2_SEGMENT_DESCRIPTOR *descriptor);
int YAP_V2_file_sha256(const char *path, unsigned char digest[32], uint64_t *file_bytes);
int YAP_V2_tombstones_write(const char *path, uint64_t generation,
                            const YAP_V2_BYTES_VIEW *document_ids, size_t document_count,
                            YAP_V2_COMPONENT_DESCRIPTOR *component);
void YAP_V2_tombstones_init(YAP_V2_TOMBSTONES *tombstones);
void YAP_V2_tombstones_free(YAP_V2_TOMBSTONES *tombstones);
int YAP_V2_tombstones_read(const char *path, uint64_t expected_generation,
                           YAP_V2_TOMBSTONES *tombstones);

#endif
