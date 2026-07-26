#include "storage/yappo_storage_v2.h"

#include <stdlib.h>
#include <string.h>

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
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '.')) {
      return YAP_V2_INVALID_FORMAT;
    }
  }
  return YAP_V2_OK;
}

void YAP_V2_segment_id_list_init(YAP_V2_SEGMENT_ID_LIST *list) {
  if (list != NULL) memset(list, 0, sizeof(*list));
}

void YAP_V2_segment_id_list_free(YAP_V2_SEGMENT_ID_LIST *list) {
  if (list == NULL) return;
  free(list->items);
  memset(list, 0, sizeof(*list));
}

int YAP_V2_segment_id_list_add(YAP_V2_SEGMENT_ID_LIST *list, const char *segment_id) {
  char (*next)[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  size_t length, capacity;
  int status;
  if (list == NULL || segment_id == NULL) return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_segment_id_validate(segment_id);
  if (status != YAP_V2_OK) return status;
  if (list->count >= YAP_V2_MAX_SEGMENTS) return YAP_V2_OUT_OF_RANGE;
  if (list->count == list->capacity) {
    capacity = list->capacity == 0U ? 4U : list->capacity * 2U;
    if (capacity < list->capacity || capacity > YAP_V2_MAX_SEGMENTS)
      capacity = YAP_V2_MAX_SEGMENTS;
    if (capacity > SIZE_MAX / sizeof(*next)) return YAP_V2_OUT_OF_RANGE;
    next = realloc(list->items, capacity * sizeof(*next));
    if (next == NULL) return YAP_V2_ALLOCATION_FAILED;
    list->items = next;
    list->capacity = capacity;
  }
  length = strlen(segment_id);
  memcpy(list->items[list->count], segment_id, length + 1U);
  list->count++;
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
  next = (YAP_V2_SEGMENT_DESCRIPTOR *)realloc(manifest->segments, sizeof(*manifest->segments) *
                                                                    (manifest->segment_count + 1U));
  if (next == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  manifest->segments = next;
  manifest->segments[manifest->segment_count] = *segment;
  manifest->segment_count++;
  return YAP_V2_OK;
}

int YAP_V2_segment_descriptor_add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                                            const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  size_t i;
  size_t length;
  if (segment == NULL || component == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  length = strnlen(component->name, sizeof(component->name));
  if (length == 0U || length >= sizeof(component->name) || component->file_type == 0U ||
      component->file_type > YAP_V2_FILE_ANN || component->file_bytes == 0U ||
      (component->file_type != YAP_V2_FILE_ANN &&
       component->file_bytes < YAP_V2_FILE_HEADER_BYTES) || strchr(component->name, '/') != NULL ||
      strchr(component->name, '\\') != NULL) {
    return YAP_V2_INVALID_FORMAT;
  }
  if (segment->component_count >= YAP_V2_MAX_COMPONENTS)
    return YAP_V2_OUT_OF_RANGE;
  for (i = 0U; i < segment->component_count; i++) {
    if (strcmp(segment->components[i].name, component->name) == 0 ||
        segment->components[i].file_type == component->file_type)
      return YAP_V2_DUPLICATE;
  }
  segment->components[segment->component_count++] = *component;
  return YAP_V2_OK;
}

int YAP_V2_manifest_validate(const YAP_V2_MANIFEST *manifest) {
  size_t i;
  size_t j;
  int fingerprint_present = 0;

  if (manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  if (manifest->format_version != YAP_V2_FORMAT_VERSION || manifest->generation == 0U ||
      manifest->segment_count > YAP_V2_MAX_SEGMENTS ||
      (manifest->segment_count > 0U && manifest->segments == NULL)) {
    return YAP_V2_INVALID_FORMAT;
  }
  for (i = 0U; i < sizeof(manifest->config_fingerprint); i++) {
    if (manifest->config_fingerprint[i] != 0U) {
      fingerprint_present = 1;
      break;
    }
  }
  if (!fingerprint_present)
    return YAP_V2_INVALID_FORMAT;
  for (i = 0; i < manifest->segment_count; i++) {
    int status = YAP_V2_segment_id_validate(manifest->segments[i].id);
    if (status != YAP_V2_OK) {
      return status;
    }
    if (manifest->segments[i].component_count == 0U ||
        manifest->segments[i].component_count > YAP_V2_MAX_COMPONENTS) {
      return YAP_V2_INVALID_FORMAT;
    }
    for (j = 0U; j < manifest->segments[i].component_count; j++) {
      YAP_V2_SEGMENT_DESCRIPTOR copy = manifest->segments[i];
      YAP_V2_COMPONENT_DESCRIPTOR component = copy.components[j];
      copy.component_count = j;
      status = YAP_V2_segment_descriptor_add_component(&copy, &component);
      if (status != YAP_V2_OK)
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
      header->file_type > YAP_V2_FILE_TOMBSTONES || header->generation == 0U) {
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
  if (input == NULL || header == NULL || input[0] != YAP_V2_MAGIC_0 || input[1] != YAP_V2_MAGIC_1 ||
      input[2] != YAP_V2_MAGIC_2 || input[3] != YAP_V2_MAGIC_3) {
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
      header->file_type > YAP_V2_FILE_TOMBSTONES || header->generation == 0U) {
    return YAP_V2_INVALID_FORMAT;
  }
  return YAP_V2_OK;
}
