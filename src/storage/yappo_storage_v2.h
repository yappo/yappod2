#ifndef YAPPO_STORAGE_V2_H
#define YAPPO_STORAGE_V2_H

#include "config/yappo_config_v2.h"
#include "common/yappo_types_v2.h"

#define YAP_V2_FILE_HEADER_BYTES 32U
#define YAP_V2_MAGIC_0 'Y'
#define YAP_V2_MAGIC_1 'A'
#define YAP_V2_MAGIC_2 'P'
#define YAP_V2_MAGIC_3 '2'

#define YAP_V2_MAX_SEGMENTS 100000U
#define YAP_V2_MAX_SEGMENT_DOCUMENTS 1000000U
#define YAP_V2_MAX_SEGMENT_PASSAGES 4000000U
#define YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES (256U * 1024U * 1024U)
#define YAP_V2_MAX_MANIFEST_BYTES (128U * 1024U * 1024U)
#define YAP_V2_MAX_COMPONENTS 8U
#define YAP_V2_MAX_COMPONENT_NAME_BYTES 63U

typedef enum {
  YAP_V2_FILE_TERMS = 1,
  YAP_V2_FILE_POSTINGS = 2,
  YAP_V2_FILE_POSITIONS = 3,
  YAP_V2_FILE_DOCUMENTS = 4,
  YAP_V2_FILE_METADATA = 5,
  YAP_V2_FILE_VECTORS = 6,
  YAP_V2_FILE_TOMBSTONES = 7,
  YAP_V2_FILE_ANN = 8,
  YAP_V2_FILE_MANIFEST = 9,
  YAP_V2_FILE_ANN_BASE = 10
} YAP_V2_FILE_TYPE;

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
} YAP_V2_SEGMENT_DESCRIPTOR;

typedef struct {
  uint32_t format_version;
  uint64_t generation;
  unsigned char config_fingerprint[32];
  YAP_V2_SEGMENT_DESCRIPTOR *segments;
  size_t segment_count;
  size_t segment_capacity;
  size_t *segment_slots;
  size_t segment_slot_capacity;
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

typedef struct {
  char (*items)[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  size_t count;
  size_t capacity;
} YAP_V2_SEGMENT_ID_LIST;

void YAP_V2_segment_id_list_init(YAP_V2_SEGMENT_ID_LIST *list);
void YAP_V2_segment_id_list_free(YAP_V2_SEGMENT_ID_LIST *list);
int YAP_V2_segment_id_list_add(YAP_V2_SEGMENT_ID_LIST *list, const char *segment_id);

int YAP_V2_segment_id_validate(const char *value);

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
