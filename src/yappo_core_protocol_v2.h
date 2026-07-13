#ifndef YAPPO_CORE_PROTOCOL_V2_H
#define YAPPO_CORE_PROTOCOL_V2_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define YAP_V2_CORE_PROTOCOL_VERSION 1U
#define YAP_V2_CORE_FRAME_HEADER_BYTES 24U
#define YAP_V2_CORE_MAX_PAYLOAD_BYTES (16U * 1024U * 1024U)

typedef enum {
  YAP_V2_CORE_SEARCH_REQUEST = 1,
  YAP_V2_CORE_SEARCH_RESPONSE = 2,
  YAP_V2_CORE_RETRIEVE_REQUEST = 3,
  YAP_V2_CORE_RETRIEVE_RESPONSE = 4,
  YAP_V2_CORE_INGEST_REQUEST = 5,
  YAP_V2_CORE_INGEST_RESPONSE = 6,
  YAP_V2_CORE_HEALTH_REQUEST = 7,
  YAP_V2_CORE_HEALTH_RESPONSE = 8,
  YAP_V2_CORE_ERROR_RESPONSE = 9
} YAP_V2_CORE_MESSAGE_TYPE;

typedef enum {
  YAP_V2_CORE_FRAME_OK = 0,
  YAP_V2_CORE_FRAME_NEED_MORE = 1,
  YAP_V2_CORE_FRAME_INVALID_ARGUMENT = -1,
  YAP_V2_CORE_FRAME_INVALID = -2,
  YAP_V2_CORE_FRAME_TOO_LARGE = -3,
  YAP_V2_CORE_FRAME_IO_ERROR = -4,
  YAP_V2_CORE_FRAME_NO_MEMORY = -5
} YAP_V2_CORE_FRAME_STATUS;

typedef struct {
  uint16_t type;
  uint16_t flags;
  uint64_t request_id;
  uint32_t payload_bytes;
  unsigned char *payload;
} YAP_V2_CORE_FRAME;

/* Initialize before first decode/read. Decoded payload is owned by the frame and
 * released by free; encode/write only borrow the caller-provided payload. */
void YAP_V2_core_frame_init(YAP_V2_CORE_FRAME *frame);
void YAP_V2_core_frame_free(YAP_V2_CORE_FRAME *frame);
int YAP_V2_core_frame_validate(const YAP_V2_CORE_FRAME *frame, uint32_t max_payload_bytes);
int YAP_V2_core_frame_encode(const YAP_V2_CORE_FRAME *frame, uint32_t max_payload_bytes,
                             unsigned char *output, size_t output_capacity, size_t *output_bytes);
int YAP_V2_core_frame_decode(const unsigned char *input, size_t input_bytes,
                             uint32_t max_payload_bytes, YAP_V2_CORE_FRAME *frame,
                             size_t *consumed_bytes);
int YAP_V2_core_frame_write(FILE *stream, const YAP_V2_CORE_FRAME *frame,
                            uint32_t max_payload_bytes);
int YAP_V2_core_frame_read(FILE *stream, uint32_t max_payload_bytes, YAP_V2_CORE_FRAME *frame);

#endif
