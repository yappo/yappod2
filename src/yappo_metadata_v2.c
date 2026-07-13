#include "yappo_metadata_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  unsigned char *data;
  size_t len;
  size_t capacity;
} BUFFER;

static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_u64(const unsigned char *p) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0U; i < 8U; i++) value |= (uint64_t)p[i] << (8U * i);
  return value;
}

static void put_u32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16); p[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *p, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++) p[i] = (unsigned char)(value >> (8U * i));
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_MAX;
  size_t i; unsigned int bit;
  for (i = 0U; i < len; i++) {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++)
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ UINT32_C(0x82f63b78) : crc >> 1;
  }
  return ~crc;
}

static int append(BUFFER *buffer, const void *data, size_t len) {
  size_t needed, capacity; unsigned char *next;
  if (len > SIZE_MAX - buffer->len) return YAP_V2_OUT_OF_RANGE;
  needed = buffer->len + len;
  if (needed > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES) return YAP_V2_OUT_OF_RANGE;
  if (needed > buffer->capacity) {
    capacity = buffer->capacity == 0U ? 256U : buffer->capacity;
    while (capacity < needed) {
      if (capacity > YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES / 2U) {
        capacity = YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES;
        break;
      }
      capacity *= 2U;
    }
    next = realloc(buffer->data, capacity);
    if (next == NULL) return YAP_V2_ALLOCATION_FAILED;
    buffer->data = next; buffer->capacity = capacity;
  }
  if (len > 0U) memcpy(buffer->data + buffer->len, data, len);
  buffer->len = needed;
  return YAP_V2_OK;
}

static int append_u32(BUFFER *buffer, uint32_t value) {
  unsigned char data[4]; put_u32(data, value); return append(buffer, data, sizeof(data));
}

static int append_u64(BUFFER *buffer, uint64_t value) {
  unsigned char data[8]; put_u64(data, value); return append(buffer, data, sizeof(data));
}

static yyjson_val *path_value(yyjson_val *root, const char *path) {
  const char *start = path;
  yyjson_val *value = root;
  while (*start != '\0') {
    const char *dot = strchr(start, '.');
    size_t len = dot == NULL ? strlen(start) : (size_t)(dot - start);
    if (!yyjson_is_obj(value)) return NULL;
    value = yyjson_obj_getn(value, start, len);
    if (value == NULL || dot == NULL) return value;
    start = dot + 1;
  }
  return NULL;
}

static int append_scalar(BUFFER *payload, uint32_t field, uint64_t document, yyjson_val *value,
                         uint64_t *records) {
  YAP_V2_METADATA_TYPE type; const char *text = NULL; size_t len = 0U; char number[64];
  unsigned char boolean; int status;
  if (yyjson_is_null(value)) type = YAP_V2_METADATA_NULL;
  else if (yyjson_is_bool(value)) {
    type = YAP_V2_METADATA_BOOL; boolean = yyjson_is_true(value) ? 1U : 0U;
    text = (const char *)&boolean; len = 1U;
  } else if (yyjson_is_num(value)) {
    type = YAP_V2_METADATA_NUMBER;
    if (yyjson_is_sint(value)) len = (size_t)snprintf(number, sizeof(number), "%lld", (long long)yyjson_get_sint(value));
    else if (yyjson_is_uint(value)) len = (size_t)snprintf(number, sizeof(number), "%llu", (unsigned long long)yyjson_get_uint(value));
    else len = (size_t)snprintf(number, sizeof(number), "%.17g", yyjson_get_real(value));
    if (len >= sizeof(number)) return YAP_V2_OUT_OF_RANGE;
    text = number;
  } else if (yyjson_is_str(value)) {
    type = YAP_V2_METADATA_STRING; text = yyjson_get_str(value); len = yyjson_get_len(value);
  } else return YAP_V2_OK;
  if (len > UINT32_MAX) return YAP_V2_OUT_OF_RANGE;
  status = append_u32(payload, field);
  if (status == YAP_V2_OK) status = append_u64(payload, document);
  if (status == YAP_V2_OK) status = append_u32(payload, (uint32_t)type);
  if (status == YAP_V2_OK) status = append_u32(payload, (uint32_t)len);
  if (status == YAP_V2_OK) status = append(payload, text, len);
  if (status != YAP_V2_OK) return status;
  (*records)++;
  return YAP_V2_OK;
}

static int append_value(BUFFER *payload, uint32_t field, uint64_t document, yyjson_val *value,
                        uint64_t *records) {
  yyjson_arr_iter iterator; yyjson_val *item; int status;
  if (!yyjson_is_arr(value)) return append_scalar(payload, field, document, value, records);
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    status = append_scalar(payload, field, document, item, records);
    if (status != YAP_V2_OK) return status;
  }
  return YAP_V2_OK;
}

static int write_atomic(const char *path, uint64_t generation, BUFFER *payload, uint64_t records,
                        YAP_V2_COMPONENT_DESCRIPTOR *component) {
  YAP_V2_FILE_HEADER header; unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  char *temporary; FILE *file; size_t path_len = strlen(path); int failed = 0; uint64_t bytes;
  memset(&header, 0, sizeof(header)); header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES; header.file_type = YAP_V2_FILE_METADATA;
  header.generation = generation; header.payload_bytes = payload->len;
  header.payload_crc32c = crc32c(payload->data, payload->len);
  if (YAP_V2_file_header_encode(&header, encoded) != YAP_V2_OK) return YAP_V2_INVALID_FORMAT;
  temporary = malloc(path_len + 5U); if (temporary == NULL) return YAP_V2_ALLOCATION_FAILED;
  snprintf(temporary, path_len + 5U, "%s.tmp", path);
  file = fopen(temporary, "wb"); if (file == NULL) { free(temporary); return YAP_V2_IO_ERROR; }
  if (fwrite(encoded, 1U, sizeof(encoded), file) != sizeof(encoded) ||
      fwrite(payload->data, 1U, payload->len, file) != payload->len || fflush(file) != 0 ||
      fsync(fileno(file)) != 0) failed = 1;
  if (fclose(file) != 0) failed = 1;
  if (failed || rename(temporary, path) != 0) { unlink(temporary); free(temporary); return YAP_V2_IO_ERROR; }
  {
    char *parent = strdup(path); char *slash; int parent_fd;
    if (parent == NULL) { free(temporary); return YAP_V2_ALLOCATION_FAILED; }
    slash = strrchr(parent, '/');
    if (slash == NULL) strcpy(parent, "."); else if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    parent_fd = open(parent, O_RDONLY); free(parent);
    if (parent_fd < 0) { free(temporary); return YAP_V2_IO_ERROR; }
    failed = fsync(parent_fd) != 0;
    if (close(parent_fd) != 0) failed = 1;
    if (failed) { free(temporary); return YAP_V2_IO_ERROR; }
  }
  free(temporary); memset(component, 0, sizeof(*component)); strcpy(component->name, "metadata.yap2");
  component->file_type = YAP_V2_FILE_METADATA; component->record_count = records;
  if (YAP_V2_file_sha256(path, component->checksum, &bytes) != YAP_V2_OK) return YAP_V2_IO_ERROR;
  component->file_bytes = bytes; return YAP_V2_OK;
}

void YAP_V2_metadata_index_init(YAP_V2_METADATA_INDEX *index) { if (index != NULL) memset(index, 0, sizeof(*index)); }
void YAP_V2_metadata_index_free(YAP_V2_METADATA_INDEX *index) {
  if (index == NULL) return; free(index->entries); free(index->storage); memset(index, 0, sizeof(*index));
}

int YAP_V2_metadata_write(const char *path, uint64_t generation, const YAP_V2_CONFIG *config,
                          const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                          YAP_V2_COMPONENT_DESCRIPTOR *component) {
  BUFFER payload = {0}; uint64_t records = 0U; size_t i, field; int status = YAP_V2_OK;
  if (path == NULL || config == NULL || component == NULL || (document_count > 0U && documents == NULL) ||
      YAP_V2_config_validate(config) != YAP_V2_OK) return YAP_V2_INVALID_ARGUMENT;
  if (document_count > YAP_V2_MAX_SEGMENT_DOCUMENTS) return YAP_V2_OUT_OF_RANGE;
  status = append_u32(&payload, 1U); if (status == YAP_V2_OK) status = append_u32(&payload, (uint32_t)config->filterable_field_count);
  if (status == YAP_V2_OK) status = append_u64(&payload, document_count);
  if (status == YAP_V2_OK) status = append_u64(&payload, 0U);
  for (field = 0U; status == YAP_V2_OK && field < config->filterable_field_count; field++) {
    size_t len = strlen(config->filterable_fields[field]);
    status = append_u32(&payload, (uint32_t)len); if (status == YAP_V2_OK) status = append(&payload, config->filterable_fields[field], len);
  }
  for (i = 0U; status == YAP_V2_OK && i < document_count; i++) {
    yyjson_doc *json; yyjson_val *root;
    if (YAP_V2_document_validate(&documents[i]) != YAP_V2_OK) { status = YAP_V2_INVALID_FORMAT; break; }
    json = yyjson_read((const char *)documents[i].metadata_json.data, documents[i].metadata_json.len, 0U);
    if (json == NULL || !yyjson_is_obj(yyjson_doc_get_root(json))) { yyjson_doc_free(json); status = YAP_V2_INVALID_FORMAT; break; }
    root = yyjson_doc_get_root(json);
    for (field = 0U; status == YAP_V2_OK && field < config->filterable_field_count; field++) {
      yyjson_val *value = path_value(root, config->filterable_fields[field]);
      if (value != NULL) status = append_value(&payload, (uint32_t)field, i, value, &records);
    }
    yyjson_doc_free(json);
  }
  if (status == YAP_V2_OK) put_u64(payload.data + 16U, records);
  if (status == YAP_V2_OK) status = write_atomic(path, generation, &payload, records, component);
  free(payload.data); return status;
}

int YAP_V2_metadata_read(const char *path, uint64_t expected_generation,
                         const YAP_V2_CONFIG *config, YAP_V2_METADATA_INDEX *index,
                         YAP_V2_COMPONENT_DESCRIPTOR *component) {
  FILE *file; long size; unsigned char header_data[YAP_V2_FILE_HEADER_BYTES]; YAP_V2_FILE_HEADER header;
  unsigned char *payload = NULL; size_t offset = 0U, i; uint64_t entry_count; int status = YAP_V2_INVALID_FORMAT;
  if (path == NULL || config == NULL || index == NULL || YAP_V2_config_validate(config) != YAP_V2_OK) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_metadata_index_init(index); file = fopen(path, "rb"); if (file == NULL) return YAP_V2_IO_ERROR;
  if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < (long)YAP_V2_FILE_HEADER_BYTES || fseek(file, 0L, SEEK_SET) != 0 ||
      fread(header_data, 1U, sizeof(header_data), file) != sizeof(header_data) || YAP_V2_file_header_decode(header_data, &header) != YAP_V2_OK ||
      header.file_type != YAP_V2_FILE_METADATA || header.generation != expected_generation ||
      header.payload_bytes != (uint64_t)size - YAP_V2_FILE_HEADER_BYTES || header.payload_bytes > SIZE_MAX) goto done;
  payload = malloc((size_t)header.payload_bytes); if (payload == NULL && header.payload_bytes > 0U) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  if (fread(payload, 1U, (size_t)header.payload_bytes, file) != header.payload_bytes || crc32c(payload, (size_t)header.payload_bytes) != header.payload_crc32c) { status = YAP_V2_CHECKSUM_MISMATCH; goto done; }
  if (header.payload_bytes < 24U || get_u32(payload) != 1U || get_u32(payload + 4U) != config->filterable_field_count) goto done;
  index->field_count = config->filterable_field_count; index->document_count = get_u64(payload + 8U); entry_count = get_u64(payload + 16U); offset = 24U;
  if (entry_count > SIZE_MAX / sizeof(*index->entries)) { status = YAP_V2_OUT_OF_RANGE; goto done; }
  for (i = 0U; i < index->field_count; i++) {
    uint32_t len; if (offset > header.payload_bytes || header.payload_bytes - offset < 4U) goto done; len = get_u32(payload + offset); offset += 4U;
    if (len > YAP_V2_MAX_FILTER_FIELD_BYTES || offset > header.payload_bytes || len > header.payload_bytes - offset || len != strlen(config->filterable_fields[i]) || memcmp(payload + offset, config->filterable_fields[i], len) != 0) goto done;
    memcpy(index->fields[i], payload + offset, len); index->fields[i][len] = '\0'; offset += len;
  }
  index->entries = calloc((size_t)entry_count, sizeof(*index->entries)); if (index->entries == NULL && entry_count > 0U) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; i < entry_count; i++) {
    uint32_t len; YAP_V2_METADATA_ENTRY *entry = &index->entries[i];
    if (offset > header.payload_bytes || header.payload_bytes - offset < 20U) goto done;
    entry->field_ordinal = get_u32(payload + offset); entry->document_ordinal = get_u64(payload + offset + 4U);
    entry->type = (YAP_V2_METADATA_TYPE)get_u32(payload + offset + 12U); len = get_u32(payload + offset + 16U); offset += 20U;
    if (entry->field_ordinal >= index->field_count || entry->document_ordinal >= index->document_count || entry->type < YAP_V2_METADATA_NULL || entry->type > YAP_V2_METADATA_STRING || offset > header.payload_bytes || len > header.payload_bytes - offset) goto done;
    entry->value.data = payload + offset; entry->value.len = len; offset += len;
  }
  if (offset != header.payload_bytes) goto done;
  index->entry_count = (size_t)entry_count; index->storage = payload; index->storage_bytes = (size_t)header.payload_bytes; payload = NULL;
  if (component != NULL) { uint64_t bytes; memset(component, 0, sizeof(*component)); strcpy(component->name, "metadata.yap2"); component->file_type = YAP_V2_FILE_METADATA; component->record_count = entry_count; if (YAP_V2_file_sha256(path, component->checksum, &bytes) != YAP_V2_OK) { status = YAP_V2_IO_ERROR; goto done; } component->file_bytes = bytes; }
  status = YAP_V2_OK;
done:
  fclose(file); free(payload); if (status != YAP_V2_OK) YAP_V2_metadata_index_free(index); return status;
}

int YAP_V2_metadata_field_ordinal(const YAP_V2_METADATA_INDEX *index, const char *field,
                                  uint32_t *ordinal) {
  size_t i; if (index == NULL || field == NULL || ordinal == NULL) return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < index->field_count; i++) if (strcmp(index->fields[i], field) == 0) { *ordinal = (uint32_t)i; return YAP_V2_OK; }
  return YAP_V2_INVALID_FORMAT;
}
