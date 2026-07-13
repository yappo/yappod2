#include "yappo_vector_v2.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unicode/utf8.h>

#define VECTOR_PAYLOAD_VERSION UINT32_C(1)
#define VECTOR_FIXED_HEADER_BYTES 40U
#define VECTOR_RECORD_BYTES 16U

typedef struct {
  unsigned char *data;
  size_t len;
  size_t capacity;
} BUFFER;

static void put_u32(unsigned char *data, uint32_t value) {
  data[0] = (unsigned char)value; data[1] = (unsigned char)(value >> 8);
  data[2] = (unsigned char)(value >> 16); data[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *data, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++) data[i] = (unsigned char)(value >> (i * 8U));
}

static uint32_t get_u32(const unsigned char *data) {
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
         (uint32_t)data[3] << 24;
}

static uint64_t get_u64(const unsigned char *data) {
  uint64_t value = 0U; size_t i;
  for (i = 0U; i < 8U; i++) value |= (uint64_t)data[i] << (i * 8U);
  return value;
}

static int utf8_valid(const unsigned char *data, size_t len) {
  int32_t offset = 0; UChar32 codepoint;
  if (len > INT32_MAX) return 0;
  while (offset < (int32_t)len) {
    U8_NEXT(data, offset, (int32_t)len, codepoint);
    if (codepoint < 0) return 0;
  }
  return 1;
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_MAX; size_t i; unsigned int bit;
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
        capacity = YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES; break;
      }
      capacity *= 2U;
    }
    next = realloc(buffer->data, capacity);
    if (next == NULL) return YAP_V2_ALLOCATION_FAILED;
    buffer->data = next; buffer->capacity = capacity;
  }
  if (len > 0U) memcpy(buffer->data + buffer->len, data, len);
  buffer->len = needed; return YAP_V2_OK;
}

static int append_u32(BUFFER *buffer, uint32_t value) {
  unsigned char data[4]; put_u32(data, value); return append(buffer, data, sizeof(data));
}

static int append_u64(BUFFER *buffer, uint64_t value) {
  unsigned char data[8]; put_u64(data, value); return append(buffer, data, sizeof(data));
}

static int append_float(BUFFER *buffer, float value) {
  uint32_t bits; unsigned char data[4];
  memcpy(&bits, &value, sizeof(bits)); put_u32(data, bits); return append(buffer, data, sizeof(data));
}

static int fsync_parent(const char *path) {
  char *parent = strdup(path); char *slash; int fd, status;
  if (parent == NULL) return -1;
  slash = strrchr(parent, '/');
  if (slash == NULL) strcpy(parent, "."); else if (slash == parent) slash[1] = '\0'; else *slash = '\0';
  fd = open(parent, O_RDONLY); free(parent); if (fd < 0) return -1;
  status = fsync(fd); if (close(fd) != 0) status = -1; return status;
}

static int write_atomic(const char *path, uint64_t generation, const BUFFER *payload,
                        uint64_t records, YAP_V2_COMPONENT_DESCRIPTOR *component) {
  YAP_V2_FILE_HEADER header; unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  char *temporary; FILE *file; size_t path_len; int failed = 0; uint64_t bytes; int status;
  memset(&header, 0, sizeof(header)); header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES; header.file_type = YAP_V2_FILE_VECTORS;
  header.generation = generation; header.payload_bytes = payload->len;
  header.payload_crc32c = crc32c(payload->data, payload->len);
  status = YAP_V2_file_header_encode(&header, encoded); if (status != YAP_V2_OK) return status;
  path_len = strlen(path); temporary = malloc(path_len + 5U);
  if (temporary == NULL) return YAP_V2_ALLOCATION_FAILED;
  snprintf(temporary, path_len + 5U, "%s.tmp", path);
  file = fopen(temporary, "wb"); if (file == NULL) { free(temporary); return YAP_V2_IO_ERROR; }
  if (fwrite(encoded, 1U, sizeof(encoded), file) != sizeof(encoded) ||
      fwrite(payload->data, 1U, payload->len, file) != payload->len || fflush(file) != 0 ||
      fsync(fileno(file)) != 0) failed = 1;
  if (fclose(file) != 0) failed = 1;
  if (failed || rename(temporary, path) != 0 || fsync_parent(path) != 0) {
    unlink(temporary); free(temporary); return YAP_V2_IO_ERROR;
  }
  free(temporary); memset(component, 0, sizeof(*component)); strcpy(component->name, "vectors.yap2");
  component->file_type = YAP_V2_FILE_VECTORS; component->record_count = records;
  status = YAP_V2_file_sha256(path, component->checksum, &bytes); component->file_bytes = bytes;
  return status;
}

void YAP_V2_vector_segment_init(YAP_V2_VECTOR_SEGMENT *segment) {
  if (segment != NULL) memset(segment, 0, sizeof(*segment));
}

void YAP_V2_vector_segment_close(YAP_V2_VECTOR_SEGMENT *segment) {
  if (segment == NULL) return;
  free(segment->entries);
  if (segment->map != NULL && segment->map_bytes > 0U) munmap(segment->map, segment->map_bytes);
  memset(segment, 0, sizeof(*segment));
}

int YAP_V2_vectors_write(const char *path, uint64_t generation, const YAP_V2_CONFIG *config,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         const YAP_EMBEDDING_RESULT *embeddings,
                         YAP_V2_COMPONENT_DESCRIPTOR *component) {
  BUFFER payload = {0}, ids = {0}, records = {0}, vectors = {0};
  size_t i, j, model_len, vector_offset; int status = YAP_V2_OK;
  if (path == NULL || config == NULL || embeddings == NULL || component == NULL ||
      (passage_count > 0U && passages == NULL) || YAP_V2_config_validate(config) != YAP_V2_OK ||
      config->vector_metric == YAP_V2_VECTOR_DISABLED || embeddings->input_count != passage_count ||
      embeddings->dimensions != config->vector_dimensions ||
      (passage_count > 0U && embeddings->values == NULL)) return YAP_V2_INVALID_ARGUMENT;
  model_len = strlen(config->vector_model_id);
  status = append_u32(&payload, VECTOR_PAYLOAD_VERSION);
  if (status == YAP_V2_OK) status = append_u32(&payload, (uint32_t)config->vector_metric);
  if (status == YAP_V2_OK) status = append_u32(&payload, config->vector_dimensions);
  if (status == YAP_V2_OK) status = append_u32(&payload, (uint32_t)model_len);
  if (status == YAP_V2_OK) status = append_u64(&payload, passage_count);
  if (status == YAP_V2_OK) status = append_u64(&payload, 0U);
  if (status == YAP_V2_OK) status = append_u64(&payload, 0U);
  if (status == YAP_V2_OK) status = append(&payload, config->vector_model_id, model_len);
  for (i = 0U; status == YAP_V2_OK && i < passage_count; i++) {
    if (YAP_V2_passage_validate(&passages[i]) != YAP_V2_OK || passages[i].id.len > UINT32_MAX) {
      status = YAP_V2_INVALID_FORMAT; break;
    }
    for (j = 0U; j < i; j++) {
      if (passages[j].id.len == passages[i].id.len &&
          memcmp(passages[j].id.data, passages[i].id.data, passages[i].id.len) == 0) {
        status = YAP_V2_DUPLICATE; break;
      }
    }
    if (status != YAP_V2_OK) break;
    status = append_u64(&records, ids.len);
    if (status == YAP_V2_OK) status = append_u32(&records, (uint32_t)passages[i].id.len);
    if (status == YAP_V2_OK) status = append_u32(&records, 0U);
    if (status == YAP_V2_OK) status = append(&ids, passages[i].id.data, passages[i].id.len);
    for (j = 0U; status == YAP_V2_OK && j < embeddings->dimensions; j++) {
      float value = embeddings->values[i * embeddings->dimensions + j];
      if (!isfinite((double)value)) { status = YAP_V2_INVALID_FORMAT; break; }
      status = append_float(&vectors, value);
    }
  }
  if (status == YAP_V2_OK) status = append(&payload, records.data, records.len);
  if (status == YAP_V2_OK) status = append(&payload, ids.data, ids.len);
  while (status == YAP_V2_OK && payload.len % sizeof(float) != 0U) status = append(&payload, "\0", 1U);
  vector_offset = payload.len;
  if (status == YAP_V2_OK) status = append(&payload, vectors.data, vectors.len);
  if (status == YAP_V2_OK) {
    put_u64(payload.data + 24U, ids.len); put_u64(payload.data + 32U, vector_offset);
    status = write_atomic(path, generation, &payload, passage_count, component);
  }
  free(payload.data); free(ids.data); free(records.data); free(vectors.data); return status;
}

int YAP_V2_vector_segment_open(const char *path, uint64_t expected_generation,
                               const YAP_V2_CONFIG *config, YAP_V2_VECTOR_SEGMENT *segment,
                               YAP_V2_COMPONENT_DESCRIPTOR *component) {
  int fd = -1, status = YAP_V2_INVALID_FORMAT; struct stat info; unsigned char *map = NULL, *payload;
  YAP_V2_FILE_HEADER header; size_t payload_bytes, model_len, count, records_offset, ids_offset;
  size_t ids_bytes, vector_offset, vectors_bytes, i, expected_id_offset = 0U;
  uint64_t count64, ids64, vector64; YAP_VECTOR_ENTRY *entries = NULL;
  uint16_t endian = UINT16_C(1);
  if (path == NULL || config == NULL || segment == NULL || YAP_V2_config_validate(config) != YAP_V2_OK ||
      config->vector_metric == YAP_V2_VECTOR_DISABLED) return YAP_V2_INVALID_ARGUMENT;
  if (*(const unsigned char *)(const void *)&endian != 1U) return YAP_V2_CONFLICT;
  fd = open(path, O_RDONLY); if (fd < 0 || fstat(fd, &info) != 0 || info.st_size < (off_t)YAP_V2_FILE_HEADER_BYTES ||
      (uint64_t)info.st_size > SIZE_MAX) goto done;
  map = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) { map = NULL; status = YAP_V2_IO_ERROR; goto done; }
  status = YAP_V2_file_header_decode(map, &header);
  if (status != YAP_V2_OK || header.file_type != YAP_V2_FILE_VECTORS ||
      header.generation != expected_generation || header.payload_bytes != (uint64_t)info.st_size - YAP_V2_FILE_HEADER_BYTES) {
    status = YAP_V2_INVALID_FORMAT; goto done;
  }
  payload = map + YAP_V2_FILE_HEADER_BYTES; payload_bytes = (size_t)header.payload_bytes;
  if (crc32c(payload, payload_bytes) != header.payload_crc32c) { status = YAP_V2_CHECKSUM_MISMATCH; goto done; }
  if (payload_bytes < VECTOR_FIXED_HEADER_BYTES || get_u32(payload) != VECTOR_PAYLOAD_VERSION)
    goto done;
  if (get_u32(payload + 4U) != (uint32_t)config->vector_metric ||
      get_u32(payload + 8U) != config->vector_dimensions) { status = YAP_V2_CONFLICT; goto done; }
  model_len = get_u32(payload + 12U); count64 = get_u64(payload + 16U);
  ids64 = get_u64(payload + 24U); vector64 = get_u64(payload + 32U);
  if (count64 > SIZE_MAX || ids64 > SIZE_MAX || vector64 > SIZE_MAX) goto done;
  count = (size_t)count64; ids_bytes = (size_t)ids64; vector_offset = (size_t)vector64;
  if (model_len != strlen(config->vector_model_id)) { status = YAP_V2_CONFLICT; goto done; }
  if (model_len > payload_bytes - VECTOR_FIXED_HEADER_BYTES ||
      memcmp(payload + VECTOR_FIXED_HEADER_BYTES, config->vector_model_id, model_len) != 0) {
    status = YAP_V2_CONFLICT; goto done;
  }
  if (
      count > YAP_V2_MAX_SEGMENT_PASSAGES || count > SIZE_MAX / VECTOR_RECORD_BYTES ||
      count > SIZE_MAX / (config->vector_dimensions * sizeof(float))) goto done;
  records_offset = VECTOR_FIXED_HEADER_BYTES + model_len;
  if (records_offset > payload_bytes || count * VECTOR_RECORD_BYTES > payload_bytes - records_offset) goto done;
  ids_offset = records_offset + count * VECTOR_RECORD_BYTES;
  vectors_bytes = count * config->vector_dimensions * sizeof(float);
  if (ids_bytes > payload_bytes - ids_offset || vector_offset < ids_offset + ids_bytes ||
      vector_offset % sizeof(float) != 0U || vector_offset > payload_bytes ||
      vectors_bytes != payload_bytes - vector_offset) goto done;
  for (i = ids_offset + ids_bytes; i < vector_offset; i++) if (payload[i] != 0U) goto done;
  entries = calloc(count, sizeof(*entries)); if (entries == NULL && count > 0U) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; i < count; i++) {
    const unsigned char *record = payload + records_offset + i * VECTOR_RECORD_BYTES;
    size_t id_offset = (size_t)get_u64(record); size_t id_len = get_u32(record + 8U), j;
    if (get_u32(record + 12U) != 0U || id_len == 0U || id_len > YAP_V2_MAX_IDENTIFIER_BYTES ||
        id_offset != expected_id_offset || id_offset > ids_bytes || id_len > ids_bytes - id_offset ||
        memchr(payload + ids_offset + id_offset, '\0', id_len) != NULL ||
        !utf8_valid(payload + ids_offset + id_offset, id_len)) goto done;
    entries[i].id.data = payload + ids_offset + id_offset; entries[i].id.len = id_len;
    entries[i].values = (const float *)(const void *)(payload + vector_offset + i * config->vector_dimensions * sizeof(float));
    entries[i].dimensions = config->vector_dimensions;
    for (j = 0U; j < config->vector_dimensions; j++) if (!isfinite((double)entries[i].values[j])) goto done;
    for (j = 0U; j < i; j++) if (entries[j].id.len == id_len &&
        memcmp(entries[j].id.data, entries[i].id.data, id_len) == 0) goto done;
    expected_id_offset += id_len;
  }
  if (expected_id_offset != ids_bytes) goto done;
  YAP_V2_vector_segment_close(segment); segment->map = map; segment->map_bytes = (size_t)info.st_size;
  segment->generation = expected_generation; segment->metric = config->vector_metric;
  segment->dimensions = config->vector_dimensions; strcpy(segment->model_id, config->vector_model_id);
  segment->entries = entries; segment->entry_count = count; map = NULL; entries = NULL;
  if (component != NULL) {
    uint64_t bytes; memset(component, 0, sizeof(*component)); strcpy(component->name, "vectors.yap2");
    component->file_type = YAP_V2_FILE_VECTORS; component->record_count = count;
    status = YAP_V2_file_sha256(path, component->checksum, &bytes); component->file_bytes = bytes;
    if (status != YAP_V2_OK) { YAP_V2_vector_segment_close(segment); goto done; }
  }
  status = YAP_V2_OK;
done:
  if (fd >= 0) close(fd); free(entries); if (map != NULL) munmap(map, (size_t)info.st_size); return status;
}

int YAP_V2_vector_segment_search(const YAP_V2_VECTOR_SEGMENT *segment, const float *query,
                                 size_t query_dimensions, size_t top_k, YAP_VECTOR_HIT *hits,
                                 size_t hit_capacity, size_t *hit_count) {
  if (segment == NULL || segment->map == NULL || query_dimensions != segment->dimensions)
    return query_dimensions != (segment == NULL ? 0U : segment->dimensions) ?
           YAP_VECTOR_DIMENSION_MISMATCH : YAP_VECTOR_INVALID_ARGUMENT;
  return YAP_Vector_search(segment->entries, segment->entry_count, query, query_dimensions,
                           segment->metric, top_k, hits, hit_capacity, hit_count);
}
