#include "yappo_core_protocol_v2.h"

#include <stdlib.h>
#include <string.h>

#include "yappo_io.h"

static const unsigned char frame_magic[4] = {'Y', 'A', 'P', '2'};

static void put_u16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)(value >> 8); p[1] = (unsigned char)value;
}

static void put_u32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}

static void put_u64(unsigned char *p, uint64_t value) {
  put_u32(p, (uint32_t)(value >> 32)); put_u32(p + 4, (uint32_t)value);
}

static uint16_t get_u16(const unsigned char *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_u64(const unsigned char *p) {
  return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

static int valid_type(uint16_t type) {
  return type >= YAP_V2_CORE_SEARCH_REQUEST && type <= YAP_V2_CORE_ERROR_RESPONSE;
}

static int valid_limit(uint32_t max_payload_bytes) {
  return max_payload_bytes != 0U && max_payload_bytes <= YAP_V2_CORE_MAX_PAYLOAD_BYTES;
}

static int parse_header(const unsigned char *header, uint32_t max_payload_bytes,
                        uint16_t *type, uint64_t *request_id, uint32_t *payload_bytes) {
  if (memcmp(header, frame_magic, 4U) != 0 ||
      get_u16(header + 4) != YAP_V2_CORE_PROTOCOL_VERSION ||
      !valid_type(get_u16(header + 6)) || get_u16(header + 8) != 0U ||
      get_u16(header + 10) != 0U || get_u64(header + 12) == 0U)
    return YAP_V2_CORE_FRAME_INVALID;
  *payload_bytes = get_u32(header + 20);
  if (*payload_bytes > max_payload_bytes) return YAP_V2_CORE_FRAME_TOO_LARGE;
  *type = get_u16(header + 6);
  *request_id = get_u64(header + 12);
  return YAP_V2_CORE_FRAME_OK;
}

void YAP_V2_core_frame_init(YAP_V2_CORE_FRAME *frame) {
  if (frame != NULL) memset(frame, 0, sizeof(*frame));
}

void YAP_V2_core_frame_free(YAP_V2_CORE_FRAME *frame) {
  if (frame == NULL) return;
  free(frame->payload); YAP_V2_core_frame_init(frame);
}

int YAP_V2_core_frame_validate(const YAP_V2_CORE_FRAME *frame, uint32_t max_payload_bytes) {
  if (frame == NULL || !valid_limit(max_payload_bytes))
    return YAP_V2_CORE_FRAME_INVALID_ARGUMENT;
  if (!valid_type(frame->type) || frame->flags != 0U || frame->request_id == 0U)
    return YAP_V2_CORE_FRAME_INVALID;
  if (frame->payload_bytes > max_payload_bytes) return YAP_V2_CORE_FRAME_TOO_LARGE;
  if (frame->payload_bytes != 0U && frame->payload == NULL) return YAP_V2_CORE_FRAME_INVALID;
  return YAP_V2_CORE_FRAME_OK;
}

int YAP_V2_core_frame_encode(const YAP_V2_CORE_FRAME *frame, uint32_t max_payload_bytes,
                             unsigned char *output, size_t output_capacity, size_t *output_bytes) {
  size_t total; int status;
  if (output_bytes == NULL) return YAP_V2_CORE_FRAME_INVALID_ARGUMENT;
  *output_bytes = 0U; status = YAP_V2_core_frame_validate(frame, max_payload_bytes);
  if (status != YAP_V2_CORE_FRAME_OK) return status;
  total = YAP_V2_CORE_FRAME_HEADER_BYTES + (size_t)frame->payload_bytes;
  if (output == NULL || output_capacity < total) return YAP_V2_CORE_FRAME_NEED_MORE;
  memcpy(output, frame_magic, 4U); put_u16(output + 4, YAP_V2_CORE_PROTOCOL_VERSION);
  put_u16(output + 6, frame->type); put_u16(output + 8, frame->flags); put_u16(output + 10, 0U);
  put_u64(output + 12, frame->request_id); put_u32(output + 20, frame->payload_bytes);
  if (frame->payload_bytes != 0U) memcpy(output + YAP_V2_CORE_FRAME_HEADER_BYTES,
                                         frame->payload, frame->payload_bytes);
  *output_bytes = total; return YAP_V2_CORE_FRAME_OK;
}

int YAP_V2_core_frame_decode(const unsigned char *input, size_t input_bytes,
                             uint32_t max_payload_bytes, YAP_V2_CORE_FRAME *frame,
                             size_t *consumed_bytes) {
  uint16_t type; uint64_t request_id; uint32_t payload_bytes;
  size_t total; unsigned char *payload = NULL; int status;
  if (frame == NULL || consumed_bytes == NULL || !valid_limit(max_payload_bytes) ||
      (input_bytes != 0U && input == NULL)) return YAP_V2_CORE_FRAME_INVALID_ARGUMENT;
  *consumed_bytes = 0U;
  if (input_bytes < YAP_V2_CORE_FRAME_HEADER_BYTES) return YAP_V2_CORE_FRAME_NEED_MORE;
  status = parse_header(input, max_payload_bytes, &type, &request_id, &payload_bytes);
  if (status != YAP_V2_CORE_FRAME_OK) return status;
  total = YAP_V2_CORE_FRAME_HEADER_BYTES + (size_t)payload_bytes;
  if (input_bytes < total) return YAP_V2_CORE_FRAME_NEED_MORE;
  if (payload_bytes != 0U) {
    payload = (unsigned char *)malloc(payload_bytes);
    if (payload == NULL) return YAP_V2_CORE_FRAME_NO_MEMORY;
    memcpy(payload, input + YAP_V2_CORE_FRAME_HEADER_BYTES, payload_bytes);
  }
  YAP_V2_core_frame_free(frame); frame->type = type;
  frame->flags = 0U; frame->request_id = request_id;
  frame->payload_bytes = payload_bytes; frame->payload = payload;
  *consumed_bytes = total; return YAP_V2_CORE_FRAME_OK;
}

int YAP_V2_core_frame_write(FILE *stream, const YAP_V2_CORE_FRAME *frame,
                            uint32_t max_payload_bytes) {
  unsigned char header[YAP_V2_CORE_FRAME_HEADER_BYTES]; int status;
  status = YAP_V2_core_frame_validate(frame, max_payload_bytes);
  if (stream == NULL || status != YAP_V2_CORE_FRAME_OK)
    return stream == NULL ? YAP_V2_CORE_FRAME_INVALID_ARGUMENT : status;
  memcpy(header, frame_magic, 4U); put_u16(header + 4, YAP_V2_CORE_PROTOCOL_VERSION);
  put_u16(header + 6, frame->type); put_u16(header + 8, frame->flags); put_u16(header + 10, 0U);
  put_u64(header + 12, frame->request_id); put_u32(header + 20, frame->payload_bytes);
  if (YAP_fwrite_exact(stream, header, 1U, sizeof(header)) != 0 ||
      (frame->payload_bytes != 0U &&
       YAP_fwrite_exact(stream, frame->payload, 1U, frame->payload_bytes) != 0))
    return YAP_V2_CORE_FRAME_IO_ERROR;
  return YAP_V2_CORE_FRAME_OK;
}

int YAP_V2_core_frame_read(FILE *stream, uint32_t max_payload_bytes, YAP_V2_CORE_FRAME *frame) {
  unsigned char header[YAP_V2_CORE_FRAME_HEADER_BYTES], *payload = NULL;
  uint16_t type; uint64_t request_id; uint32_t payload_bytes; int status;
  if (stream == NULL || frame == NULL || !valid_limit(max_payload_bytes))
    return YAP_V2_CORE_FRAME_INVALID_ARGUMENT;
  if (YAP_fread_exact(stream, header, 1U, sizeof(header)) != 0) return YAP_V2_CORE_FRAME_IO_ERROR;
  status = parse_header(header, max_payload_bytes, &type, &request_id, &payload_bytes);
  if (status != YAP_V2_CORE_FRAME_OK) return status;
  if (payload_bytes != 0U) {
    payload = (unsigned char *)malloc(payload_bytes);
    if (payload == NULL) return YAP_V2_CORE_FRAME_NO_MEMORY;
    if (YAP_fread_exact(stream, payload, 1U, payload_bytes) != 0) {
      free(payload); return YAP_V2_CORE_FRAME_IO_ERROR;
    }
  }
  YAP_V2_core_frame_free(frame);
  frame->type = type; frame->request_id = request_id;
  frame->payload_bytes = payload_bytes; frame->payload = payload;
  return YAP_V2_CORE_FRAME_OK;
}
