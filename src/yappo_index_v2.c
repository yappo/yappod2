#include "yappo_index_v2.h"

#include <stdlib.h>
#include <string.h>

static int bytes_view_validate(YAP_V2_BYTES_VIEW value, size_t max_bytes, int required) {
  size_t i;

  if (required && (value.data == NULL || value.len == 0U)) {
    return YAP_V2_INVALID_FORMAT;
  }
  if (value.len > max_bytes || (value.len > 0U && value.data == NULL)) {
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0; i < value.len; i++) {
    if (value.data[i] == '\0') {
      return YAP_V2_INVALID_FORMAT;
    }
  }
  return YAP_V2_OK;
}

static int c_string_validate(const char *value, size_t max_bytes, int required) {
  size_t len;

  if (value == NULL) {
    return required ? YAP_V2_INVALID_ARGUMENT : YAP_V2_OK;
  }
  len = 0U;
  while (len <= max_bytes && value[len] != '\0') {
    len++;
  }
  if (len > max_bytes) {
    return YAP_V2_INVALID_FORMAT;
  }
  if (required && len == 0U) {
    return YAP_V2_INVALID_FORMAT;
  }
  return YAP_V2_OK;
}

int YAP_V2_segment_id_validate(const char *value) {
  size_t i;
  int status = c_string_validate(value, YAP_V2_MAX_IDENTIFIER_BYTES, 1);

  if (status != YAP_V2_OK) {
    return status;
  }
  for (i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.')) {
      return YAP_V2_INVALID_FORMAT;
    }
  }
  return YAP_V2_OK;
}

const char *YAP_V2_status_string(YAP_V2_STATUS status) {
  switch (status) {
    case YAP_V2_OK:
      return "ok";
    case YAP_V2_INVALID_ARGUMENT:
      return "invalid argument";
    case YAP_V2_INVALID_FORMAT:
      return "invalid format";
    case YAP_V2_OUT_OF_RANGE:
      return "out of range";
    case YAP_V2_DUPLICATE:
      return "duplicate";
    case YAP_V2_ALLOCATION_FAILED:
      return "allocation failed";
    case YAP_V2_IO_ERROR:
      return "I/O error";
    case YAP_V2_CHECKSUM_MISMATCH:
      return "checksum mismatch";
    case YAP_V2_CONFLICT:
      return "conflict";
    default:
      return "unknown status";
  }
}

int YAP_V2_document_validate(const YAP_V2_DOCUMENT_VIEW *document) {
  int status;

  if (document == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = bytes_view_validate(document->id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->url, YAP_V2_MAX_IDENTIFIER_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->title, YAP_V2_MAX_IDENTIFIER_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->body, YAP_V2_MAX_METADATA_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(document->metadata_json, YAP_V2_MAX_METADATA_BYTES, 0);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (document->updated_at_unix_ms < 0) {
    return YAP_V2_OUT_OF_RANGE;
  }
  return YAP_V2_OK;
}

int YAP_V2_passage_validate(const YAP_V2_PASSAGE_VIEW *passage) {
  int status;

  if (passage == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = bytes_view_validate(passage->id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(passage->parent_document_id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = bytes_view_validate(passage->text, YAP_V2_MAX_METADATA_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (passage->end_char < passage->start_char || passage->start_char > YAP_V2_MAX_CHUNK_CHARS ||
      passage->end_char > YAP_V2_MAX_CHUNK_CHARS) {
    return YAP_V2_OUT_OF_RANGE;
  }
  return YAP_V2_OK;
}

int YAP_V2_config_validate(const YAP_V2_CONFIG *config) {
  int status;

  if (config == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  if (config->format_version != YAP_V2_FORMAT_VERSION) {
    return YAP_V2_INVALID_FORMAT;
  }
  status = c_string_validate(config->tokenizer_id, YAP_V2_MAX_IDENTIFIER_BYTES, 1);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (config->chunk_max_chars == 0U || config->chunk_max_chars > YAP_V2_MAX_CHUNK_CHARS ||
      config->chunk_overlap_chars >= config->chunk_max_chars) {
    return YAP_V2_OUT_OF_RANGE;
  }
  status = c_string_validate(config->vector_model_id, YAP_V2_MAX_MODEL_ID_BYTES,
                             config->vector_metric != YAP_V2_VECTOR_DISABLED);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (config->vector_metric == YAP_V2_VECTOR_DISABLED) {
    if (config->vector_dimensions != 0U || config->vector_model_id[0] != '\0') {
      return YAP_V2_INVALID_FORMAT;
    }
  } else {
    if (config->vector_dimensions == 0U ||
        config->vector_dimensions > YAP_V2_MAX_VECTOR_DIMENSIONS ||
        config->vector_metric < YAP_V2_VECTOR_COSINE ||
        config->vector_metric > YAP_V2_VECTOR_L2) {
      return YAP_V2_OUT_OF_RANGE;
    }
  }
  return YAP_V2_OK;
}

void YAP_V2_manifest_init(YAP_V2_MANIFEST *manifest) {
  if (manifest == NULL) {
    return;
  }
  memset(manifest, 0, sizeof(*manifest));
  manifest->format_version = YAP_V2_FORMAT_VERSION;
  manifest->generation = 1U;
}

void YAP_V2_manifest_free(YAP_V2_MANIFEST *manifest) {
  if (manifest == NULL) {
    return;
  }
  free(manifest->segments);
  YAP_V2_manifest_init(manifest);
}

int YAP_V2_manifest_add_segment(YAP_V2_MANIFEST *manifest,
                                const YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  YAP_V2_SEGMENT_DESCRIPTOR *next;
  size_t i;
  int status;

  if (manifest == NULL || segment == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = YAP_V2_segment_id_validate(segment->id);
  if (status != YAP_V2_OK) {
    return status;
  }
  if (manifest->segment_count >= YAP_V2_MAX_SEGMENTS) {
    return YAP_V2_OUT_OF_RANGE;
  }
  for (i = 0; i < manifest->segment_count; i++) {
    if (strcmp(manifest->segments[i].id, segment->id) == 0) {
      return YAP_V2_DUPLICATE;
    }
  }
  next = (YAP_V2_SEGMENT_DESCRIPTOR *)realloc(
    manifest->segments, sizeof(*manifest->segments) * (manifest->segment_count + 1U));
  if (next == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  manifest->segments = next;
  manifest->segments[manifest->segment_count] = *segment;
  manifest->segment_count++;
  return YAP_V2_OK;
}

int YAP_V2_manifest_validate(const YAP_V2_MANIFEST *manifest) {
  size_t i;
  size_t j;

  if (manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  if (manifest->format_version != YAP_V2_FORMAT_VERSION || manifest->generation == 0U ||
      manifest->segment_count > YAP_V2_MAX_SEGMENTS ||
      (manifest->segment_count > 0U && manifest->segments == NULL)) {
    return YAP_V2_INVALID_FORMAT;
  }
  for (i = 0; i < manifest->segment_count; i++) {
    int status = YAP_V2_segment_id_validate(manifest->segments[i].id);
    if (status != YAP_V2_OK) {
      return status;
    }
    for (j = 0; j < i; j++) {
      if (strcmp(manifest->segments[i].id, manifest->segments[j].id) == 0) {
        return YAP_V2_DUPLICATE;
      }
    }
  }
  return YAP_V2_OK;
}

static void put_u16_le(unsigned char *output, uint16_t value) {
  output[0] = (unsigned char)(value & 0xffU);
  output[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void put_u32_le(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value & 0xffU);
  output[1] = (unsigned char)((value >> 8) & 0xffU);
  output[2] = (unsigned char)((value >> 16) & 0xffU);
  output[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void put_u64_le(unsigned char *output, uint64_t value) {
  size_t i;
  for (i = 0; i < 8U; i++) {
    output[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
  }
}

static uint16_t get_u16_le(const unsigned char *input) {
  return (uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8);
}

static uint32_t get_u32_le(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) |
         ((uint32_t)input[3] << 24);
}

static uint64_t get_u64_le(const unsigned char *input) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0; i < 8U; i++) {
    value |= (uint64_t)input[i] << (i * 8U);
  }
  return value;
}

int YAP_V2_file_header_encode(const YAP_V2_FILE_HEADER *header,
                              unsigned char output[YAP_V2_FILE_HEADER_BYTES]) {
  if (header == NULL || output == NULL || header->format_version != YAP_V2_FORMAT_VERSION ||
      header->header_bytes != YAP_V2_FILE_HEADER_BYTES || header->file_type == 0U ||
      header->file_type > YAP_V2_FILE_VECTORS || header->generation == 0U) {
    return YAP_V2_INVALID_FORMAT;
  }
  memset(output, 0, YAP_V2_FILE_HEADER_BYTES);
  output[0] = YAP_V2_MAGIC_0;
  output[1] = YAP_V2_MAGIC_1;
  output[2] = YAP_V2_MAGIC_2;
  output[3] = YAP_V2_MAGIC_3;
  put_u16_le(output + 4U, header->format_version);
  put_u16_le(output + 6U, header->header_bytes);
  put_u32_le(output + 8U, header->file_type);
  put_u64_le(output + 12U, header->generation);
  put_u64_le(output + 20U, header->payload_bytes);
  put_u32_le(output + 28U, header->payload_crc32c);
  return YAP_V2_OK;
}

int YAP_V2_file_header_decode(const unsigned char input[YAP_V2_FILE_HEADER_BYTES],
                              YAP_V2_FILE_HEADER *header) {
  if (input == NULL || header == NULL || input[0] != YAP_V2_MAGIC_0 ||
      input[1] != YAP_V2_MAGIC_1 || input[2] != YAP_V2_MAGIC_2 || input[3] != YAP_V2_MAGIC_3) {
    return YAP_V2_INVALID_FORMAT;
  }
  header->format_version = get_u16_le(input + 4U);
  header->header_bytes = get_u16_le(input + 6U);
  header->file_type = get_u32_le(input + 8U);
  header->generation = get_u64_le(input + 12U);
  header->payload_bytes = get_u64_le(input + 20U);
  header->payload_crc32c = get_u32_le(input + 28U);
  if (header->format_version != YAP_V2_FORMAT_VERSION ||
      header->header_bytes != YAP_V2_FILE_HEADER_BYTES || header->file_type == 0U ||
      header->file_type > YAP_V2_FILE_VECTORS || header->generation == 0U) {
    return YAP_V2_INVALID_FORMAT;
  }
  return YAP_V2_OK;
}
