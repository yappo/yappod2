#include "storage/yappo_manifest_v2.h"
#include "config/yappo_config_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define YAP_V2_MANIFEST_PAYLOAD_VERSION 1U
#define YAP_V2_MANIFEST_PAYLOAD_HEADER_BYTES 40U
#define YAP_V2_MANIFEST_SEGMENT_FIXED_BYTES 32U
#define YAP_V2_MANIFEST_COMPONENT_FIXED_BYTES 56U

static size_t segment_id_hash(const char *value) {
  size_t hash = (size_t)1469598103934665603ULL;
  const unsigned char *cursor = (const unsigned char *)value;
  while (*cursor != '\0') {
    hash ^= *cursor++;
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

int YAP_V2_segment_descriptor_equal(
  const YAP_V2_SEGMENT_DESCRIPTOR *left,
  const YAP_V2_SEGMENT_DESCRIPTOR *right) {
  size_t i;
  if (left == NULL || right == NULL ||
      strcmp(left->id, right->id) != 0 ||
      left->document_count != right->document_count ||
      left->passage_count != right->passage_count ||
      left->tombstone_count != right->tombstone_count ||
      left->component_count != right->component_count)
    return 0;
  for (i = 0U; i < left->component_count; i++) {
    const YAP_V2_COMPONENT_DESCRIPTOR *a = &left->components[i];
    const YAP_V2_COMPONENT_DESCRIPTOR *b = &right->components[i];
    if (strcmp(a->name, b->name) != 0 ||
        a->file_type != b->file_type ||
        a->record_count != b->record_count ||
        a->file_bytes != b->file_bytes ||
        memcmp(a->checksum, b->checksum, sizeof(a->checksum)) != 0)
      return 0;
  }
  return 1;
}

void YAP_V2_manifest_segment_map_init(YAP_V2_MANIFEST_SEGMENT_MAP *map) {
  if (map != NULL) memset(map, 0, sizeof(*map));
}

void YAP_V2_manifest_segment_map_free(YAP_V2_MANIFEST_SEGMENT_MAP *map) {
  if (map == NULL) return;
  free(map->slots);
  memset(map, 0, sizeof(*map));
}

int YAP_V2_manifest_segment_map_build(YAP_V2_MANIFEST_SEGMENT_MAP *map,
                                      const YAP_V2_MANIFEST *manifest) {
  size_t capacity = 1U;
  size_t i;
  if (map == NULL || manifest == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_manifest_segment_map_free(map);
  map->manifest = manifest;
  if (manifest->segment_count == 0U) return YAP_V2_OK;
  if (manifest->segment_count > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
  while (capacity < manifest->segment_count * 2U) {
    if (capacity > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
    capacity *= 2U;
  }
  map->slots = calloc(capacity, sizeof(*map->slots));
  if (map->slots == NULL) {
    map->manifest = NULL;
    return YAP_V2_ALLOCATION_FAILED;
  }
  map->capacity = capacity;
  for (i = 0U; i < manifest->segment_count; i++) {
    size_t slot = segment_id_hash(manifest->segments[i].id) & (capacity - 1U);
    while (map->slots[slot] != 0U) {
      size_t existing = map->slots[slot] - 1U;
      if (strcmp(manifest->segments[existing].id, manifest->segments[i].id) == 0) {
        YAP_V2_manifest_segment_map_free(map);
        return YAP_V2_CONFLICT;
      }
      slot = (slot + 1U) & (capacity - 1U);
    }
    map->slots[slot] = i + 1U;
  }
  return YAP_V2_OK;
}

int YAP_V2_manifest_segment_map_find(const YAP_V2_MANIFEST_SEGMENT_MAP *map,
                                     const char *segment_id,
                                     size_t *segment_index) {
  size_t slot;
  size_t probes;
  if (map == NULL || segment_id == NULL || segment_index == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (map->manifest == NULL || map->capacity == 0U) return YAP_V2_NOT_FOUND;
  slot = segment_id_hash(segment_id) & (map->capacity - 1U);
  for (probes = 0U; probes < map->capacity; probes++) {
    size_t encoded = map->slots[slot];
    if (encoded == 0U) return YAP_V2_NOT_FOUND;
    if (strcmp(map->manifest->segments[encoded - 1U].id, segment_id) == 0) {
      *segment_index = encoded - 1U;
      return YAP_V2_OK;
    }
    slot = (slot + 1U) & (map->capacity - 1U);
  }
  return YAP_V2_NOT_FOUND;
}

typedef struct {
  const unsigned char *data;
  size_t len;
  size_t offset;
} MANIFEST_READER;

static void put_u32_le(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value & 0xffU);
  output[1] = (unsigned char)((value >> 8) & 0xffU);
  output[2] = (unsigned char)((value >> 16) & 0xffU);
  output[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void put_u64_le(unsigned char *output, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++)
    output[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
}

static uint32_t get_u32_le(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) |
         ((uint32_t)input[3] << 24);
}

static uint64_t get_u64_le(const unsigned char *input) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0U; i < 8U; i++) value |= (uint64_t)input[i] << (i * 8U);
  return value;
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_C(0xffffffff);
  size_t i;
  for (i = 0U; i < len; i++) {
    unsigned int bit;
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++)
      crc = (crc >> 1U) ^ (UINT32_C(0x82f63b78) & (uint32_t)-(int32_t)(crc & 1U));
  }
  return ~crc;
}

static int reader_take(MANIFEST_READER *reader, size_t bytes, const unsigned char **value) {
  if (reader->offset > reader->len || bytes > reader->len - reader->offset)
    return YAP_V2_INVALID_FORMAT;
  *value = reader->data + reader->offset;
  reader->offset += bytes;
  return YAP_V2_OK;
}

static int reader_u32(MANIFEST_READER *reader, uint32_t *value) {
  const unsigned char *bytes;
  int status = reader_take(reader, 4U, &bytes);
  if (status == YAP_V2_OK) *value = get_u32_le(bytes);
  return status;
}

static int reader_u64(MANIFEST_READER *reader, uint64_t *value) {
  const unsigned char *bytes;
  int status = reader_take(reader, 8U, &bytes);
  if (status == YAP_V2_OK) *value = get_u64_le(bytes);
  return status;
}

static int parse_component(MANIFEST_READER *reader,
                           YAP_V2_COMPONENT_DESCRIPTOR *component) {
  const unsigned char *checksum;
  const unsigned char *name;
  uint32_t name_bytes;
  int status;
  memset(component, 0, sizeof(*component));
  status = reader_u32(reader, &name_bytes);
  if (status == YAP_V2_OK) status = reader_u32(reader, &component->file_type);
  if (status == YAP_V2_OK) status = reader_u64(reader, &component->record_count);
  if (status == YAP_V2_OK) status = reader_u64(reader, &component->file_bytes);
  if (status == YAP_V2_OK) status = reader_take(reader, sizeof(component->checksum), &checksum);
  if (status == YAP_V2_OK && (name_bytes == 0U || name_bytes > YAP_V2_MAX_COMPONENT_NAME_BYTES))
    status = YAP_V2_INVALID_FORMAT;
  if (status == YAP_V2_OK) status = reader_take(reader, name_bytes, &name);
  if (status == YAP_V2_OK && memchr(name, '\0', name_bytes) != NULL)
    status = YAP_V2_INVALID_FORMAT;
  if (status != YAP_V2_OK) return status;
  memcpy(component->checksum, checksum, sizeof(component->checksum));
  memcpy(component->name, name, name_bytes);
  return YAP_V2_OK;
}

static int parse_segment(MANIFEST_READER *reader, YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  const unsigned char *id;
  uint32_t id_bytes;
  uint32_t component_count;
  uint32_t i;
  int status;
  memset(segment, 0, sizeof(*segment));
  status = reader_u32(reader, &id_bytes);
  if (status == YAP_V2_OK) status = reader_u32(reader, &component_count);
  if (status == YAP_V2_OK) status = reader_u64(reader, &segment->document_count);
  if (status == YAP_V2_OK) status = reader_u64(reader, &segment->passage_count);
  if (status == YAP_V2_OK) status = reader_u64(reader, &segment->tombstone_count);
  if (status == YAP_V2_OK &&
      (id_bytes == 0U || id_bytes > YAP_V2_MAX_IDENTIFIER_BYTES || component_count == 0U ||
       component_count > YAP_V2_MAX_COMPONENTS))
    status = YAP_V2_INVALID_FORMAT;
  if (status == YAP_V2_OK) status = reader_take(reader, id_bytes, &id);
  if (status == YAP_V2_OK && memchr(id, '\0', id_bytes) != NULL)
    status = YAP_V2_INVALID_FORMAT;
  if (status != YAP_V2_OK) return status;
  memcpy(segment->id, id, id_bytes);
  for (i = 0U; status == YAP_V2_OK && i < component_count; i++) {
    YAP_V2_COMPONENT_DESCRIPTOR component;
    status = parse_component(reader, &component);
    if (status == YAP_V2_OK)
      status = YAP_V2_segment_descriptor_add_component(segment, &component);
  }
  return status;
}

int YAP_V2_manifest_load(const char *path, YAP_V2_MANIFEST *manifest) {
  FILE *file;
  struct stat stat_buffer;
  unsigned char *data;
  size_t read_bytes;
  size_t file_size;
  const unsigned char *fingerprint;
  MANIFEST_READER reader;
  YAP_V2_FILE_HEADER header;
  YAP_V2_MANIFEST parsed;
  uint32_t payload_version;
  uint32_t segment_count;
  uint32_t i;
  int status = YAP_V2_OK;
  if (path == NULL || manifest == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  file = fopen(path, "rb");
  if (file == NULL)
    return YAP_V2_IO_ERROR;
  if (fstat(fileno(file), &stat_buffer) != 0 || stat_buffer.st_size < 0) {
    fclose(file);
    return YAP_V2_IO_ERROR;
  }
  if (stat_buffer.st_size < (off_t)(YAP_V2_FILE_HEADER_BYTES +
                                    YAP_V2_MANIFEST_PAYLOAD_HEADER_BYTES)) {
    fclose(file);
    return YAP_V2_INVALID_FORMAT;
  }
  if ((uint64_t)stat_buffer.st_size > YAP_V2_MAX_MANIFEST_BYTES) {
    fclose(file);
    return YAP_V2_OUT_OF_RANGE;
  }
  file_size = (size_t)stat_buffer.st_size;
  data = malloc(file_size);
  if (data == NULL) {
    fclose(file);
    return YAP_V2_ALLOCATION_FAILED;
  }
  read_bytes = fread(data, 1U, file_size, file);
  if (read_bytes != file_size) {
    fclose(file);
    free(data);
    return YAP_V2_IO_ERROR;
  }
  if (fclose(file) != 0) {
    free(data);
    return YAP_V2_IO_ERROR;
  }
  YAP_V2_manifest_init(&parsed);
  status = YAP_V2_file_header_decode(data, &header);
  if (status == YAP_V2_OK &&
      (header.file_type != YAP_V2_FILE_MANIFEST ||
       header.payload_bytes != file_size - YAP_V2_FILE_HEADER_BYTES))
    status = YAP_V2_INVALID_FORMAT;
  if (status == YAP_V2_OK &&
      crc32c(data + YAP_V2_FILE_HEADER_BYTES, (size_t)header.payload_bytes) !=
        header.payload_crc32c)
    status = YAP_V2_CHECKSUM_MISMATCH;
  reader.data = data + YAP_V2_FILE_HEADER_BYTES;
  reader.len = file_size - YAP_V2_FILE_HEADER_BYTES;
  reader.offset = 0U;
  if (status == YAP_V2_OK) status = reader_u32(&reader, &payload_version);
  if (status == YAP_V2_OK) status = reader_u32(&reader, &segment_count);
  if (status == YAP_V2_OK) status = reader_take(&reader, 32U, &fingerprint);
  if (status == YAP_V2_OK &&
      (payload_version != YAP_V2_MANIFEST_PAYLOAD_VERSION ||
       segment_count > YAP_V2_MAX_SEGMENTS))
    status = YAP_V2_INVALID_FORMAT;
  if (status == YAP_V2_OK) {
    parsed.generation = header.generation;
    memcpy(parsed.config_fingerprint, fingerprint, sizeof(parsed.config_fingerprint));
  }
  for (i = 0U; status == YAP_V2_OK && i < segment_count; i++) {
    YAP_V2_SEGMENT_DESCRIPTOR segment;
    status = parse_segment(&reader, &segment);
    if (status == YAP_V2_OK)
      status = YAP_V2_manifest_add_segment(&parsed, &segment);
  }
  if (status == YAP_V2_OK && reader.offset != reader.len) status = YAP_V2_INVALID_FORMAT;
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_validate(&parsed);
  free(data);
  if (status == YAP_V2_OK) {
    YAP_V2_manifest_free(manifest);
    *manifest = parsed;
  } else
    YAP_V2_manifest_free(&parsed);
  return status;
}

int YAP_V2_manifest_load_for_config(const char *path, const YAP_V2_CONFIG *config,
                                    YAP_V2_MANIFEST *manifest) {
  unsigned char fingerprint[32];
  int status;
  if (path == NULL || config == NULL || manifest == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_config_fingerprint(config, fingerprint);
  if (status != YAP_V2_OK)
    return status;
  status = YAP_V2_manifest_load(path, manifest);
  if (status == YAP_V2_OK && memcmp(fingerprint, manifest->config_fingerprint, 32U) != 0) {
    YAP_V2_manifest_free(manifest);
    return YAP_V2_CONFLICT;
  }
  return status;
}

static int add_size(size_t *total, size_t additional) {
  if (additional > SIZE_MAX - *total) return YAP_V2_OUT_OF_RANGE;
  *total += additional;
  return YAP_V2_OK;
}

static int manifest_payload_size(const YAP_V2_MANIFEST *manifest, size_t *payload_bytes) {
  size_t total = YAP_V2_MANIFEST_PAYLOAD_HEADER_BYTES;
  size_t i;
  for (i = 0U; i < manifest->segment_count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *segment = &manifest->segments[i];
    size_t j;
    int status = add_size(&total, YAP_V2_MANIFEST_SEGMENT_FIXED_BYTES + strlen(segment->id));
    if (status != YAP_V2_OK) return status;
    for (j = 0U; j < segment->component_count; j++) {
      const YAP_V2_COMPONENT_DESCRIPTOR *component = &segment->components[j];
      status = add_size(&total,
                        YAP_V2_MANIFEST_COMPONENT_FIXED_BYTES + strlen(component->name));
      if (status != YAP_V2_OK) return status;
    }
  }
  if (total > YAP_V2_MAX_MANIFEST_BYTES - YAP_V2_FILE_HEADER_BYTES)
    return YAP_V2_OUT_OF_RANGE;
  *payload_bytes = total;
  return YAP_V2_OK;
}

static int encode_manifest(const YAP_V2_MANIFEST *manifest, unsigned char *file_data,
                           size_t file_size) {
  YAP_V2_FILE_HEADER header;
  unsigned char *payload = file_data + YAP_V2_FILE_HEADER_BYTES;
  size_t offset = 0U;
  size_t i;
  put_u32_le(payload + offset, YAP_V2_MANIFEST_PAYLOAD_VERSION);
  offset += 4U;
  put_u32_le(payload + offset, (uint32_t)manifest->segment_count);
  offset += 4U;
  memcpy(payload + offset, manifest->config_fingerprint, 32U);
  offset += 32U;
  for (i = 0U; i < manifest->segment_count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *segment = &manifest->segments[i];
    size_t id_bytes = strlen(segment->id);
    size_t j;
    put_u32_le(payload + offset, (uint32_t)id_bytes);
    offset += 4U;
    put_u32_le(payload + offset, (uint32_t)segment->component_count);
    offset += 4U;
    put_u64_le(payload + offset, segment->document_count);
    offset += 8U;
    put_u64_le(payload + offset, segment->passage_count);
    offset += 8U;
    put_u64_le(payload + offset, segment->tombstone_count);
    offset += 8U;
    memcpy(payload + offset, segment->id, id_bytes);
    offset += id_bytes;
    for (j = 0U; j < segment->component_count; j++) {
      const YAP_V2_COMPONENT_DESCRIPTOR *component = &segment->components[j];
      size_t name_bytes = strlen(component->name);
      put_u32_le(payload + offset, (uint32_t)name_bytes);
      offset += 4U;
      put_u32_le(payload + offset, component->file_type);
      offset += 4U;
      put_u64_le(payload + offset, component->record_count);
      offset += 8U;
      put_u64_le(payload + offset, component->file_bytes);
      offset += 8U;
      memcpy(payload + offset, component->checksum, 32U);
      offset += 32U;
      memcpy(payload + offset, component->name, name_bytes);
      offset += name_bytes;
    }
  }
  if (offset != file_size - YAP_V2_FILE_HEADER_BYTES) return YAP_V2_CONFLICT;
  memset(&header, 0, sizeof(header));
  header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES;
  header.file_type = YAP_V2_FILE_MANIFEST;
  header.generation = manifest->generation;
  header.payload_bytes = offset;
  header.payload_crc32c = crc32c(payload, offset);
  return YAP_V2_file_header_encode(&header, file_data);
}

static int fsync_parent(const char *path) {
  char *copy, *slash;
  int fd, result;
  if (path == NULL)
    return -1;
  copy = strdup(path);
  if (copy == NULL)
    return -1;
  slash = strrchr(copy, '/');
  if (slash == NULL)
    strcpy(copy, ".");
  else if (slash == copy)
    slash[1] = '\0';
  else
    *slash = '\0';
  fd = open(copy, O_RDONLY);
  free(copy);
  if (fd < 0)
    return -1;
  result = fsync(fd);
  if (close(fd) != 0)
    result = -1;
  return result;
}

int YAP_V2_manifest_save_atomic(const char *path, const YAP_V2_MANIFEST *manifest) {
  FILE *file = NULL;
  unsigned char *file_data = NULL;
  char *temporary;
  size_t payload_bytes;
  size_t file_size;
  size_t length;
  int status;
  int write_failed = 0;

  if (path == NULL || manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  status = YAP_V2_manifest_validate(manifest);
  if (status != YAP_V2_OK) {
    return status;
  }
  status = manifest_payload_size(manifest, &payload_bytes);
  if (status != YAP_V2_OK) return status;
  file_size = YAP_V2_FILE_HEADER_BYTES + payload_bytes;
  file_data = malloc(file_size);
  if (file_data == NULL) return YAP_V2_ALLOCATION_FAILED;
  status = encode_manifest(manifest, file_data, file_size);
  if (status != YAP_V2_OK) {
    free(file_data);
    return status;
  }
  length = strlen(path);
  if (length > SIZE_MAX - 5U) {
    free(file_data);
    return YAP_V2_OUT_OF_RANGE;
  }
  temporary = (char *)malloc(length + 5U);
  if (temporary == NULL) {
    free(file_data);
    return YAP_V2_ALLOCATION_FAILED;
  }
  (void)snprintf(temporary, length + 5U, "%s.tmp", path);
  file = fopen(temporary, "wb");
  if (file == NULL) {
    free(file_data);
    free(temporary);
    return YAP_V2_IO_ERROR;
  }
  if (fwrite(file_data, 1U, file_size, file) != file_size || fflush(file) != 0 ||
      fsync(fileno(file)) != 0) {
    write_failed = 1;
  }
  free(file_data);
  if (fclose(file) != 0) {
    write_failed = 1;
  }
  file = NULL;
  if (write_failed || rename(temporary, path) != 0 || fsync_parent(path) != 0) {
    unlink(temporary);
    free(temporary);
    return YAP_V2_IO_ERROR;
  }
  free(temporary);
  return YAP_V2_OK;
}

int YAP_V2_manifest_publish_next(const char *path, YAP_V2_MANIFEST *manifest) {
  YAP_V2_MANIFEST current;
  char *lock_path;
  size_t length;
  int lock_fd;
  int status;

  if (path == NULL || manifest == NULL) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  length = strlen(path);
  if (length > SIZE_MAX - 6U) {
    return YAP_V2_OUT_OF_RANGE;
  }
  lock_path = (char *)malloc(length + 6U);
  if (lock_path == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  (void)snprintf(lock_path, length + 6U, "%s.lock", path);
  lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0) {
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }
  if (flock(lock_fd, LOCK_EX) != 0) {
    close(lock_fd);
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }
  YAP_V2_manifest_init(&current);
  status = YAP_V2_manifest_load(path, &current);
  if (status == YAP_V2_IO_ERROR && access(path, F_OK) != 0 && errno == ENOENT) {
    current.generation = 0U;
    status = YAP_V2_OK;
  }
  if (status == YAP_V2_OK && current.generation > 0U &&
      memcmp(current.config_fingerprint, manifest->config_fingerprint, 32U) != 0) {
    status = YAP_V2_CONFLICT;
  }
  if (status == YAP_V2_OK) {
    if (current.generation == UINT64_MAX) {
      status = YAP_V2_OUT_OF_RANGE;
    } else {
      manifest->generation = current.generation + 1U;
      status = YAP_V2_manifest_save_atomic(path, manifest);
    }
  }
  YAP_V2_manifest_free(&current);
  (void)flock(lock_fd, LOCK_UN);
  if (close(lock_fd) != 0 && status == YAP_V2_OK) {
    status = YAP_V2_IO_ERROR;
  }
  free(lock_path);
  return status;
}

int YAP_V2_manifest_publish_if_generation(const char *path, uint64_t expected_generation,
                                          YAP_V2_MANIFEST *manifest) {
  YAP_V2_MANIFEST current;
  char *lock_path;
  size_t length;
  int lock_fd;
  int status;
  if (path == NULL || manifest == NULL || expected_generation == UINT64_MAX)
    return YAP_V2_INVALID_ARGUMENT;
  length = strlen(path);
  if (length > SIZE_MAX - 6U) return YAP_V2_OUT_OF_RANGE;
  lock_path = malloc(length + 6U);
  if (lock_path == NULL) return YAP_V2_ALLOCATION_FAILED;
  (void)snprintf(lock_path, length + 6U, "%s.lock", path);
  lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
    if (lock_fd >= 0) close(lock_fd);
    free(lock_path);
    return YAP_V2_IO_ERROR;
  }
  YAP_V2_manifest_init(&current);
  status = YAP_V2_manifest_load(path, &current);
  if (status == YAP_V2_OK && current.generation != expected_generation)
    status = YAP_V2_CONFLICT;
  if (status == YAP_V2_OK &&
      memcmp(current.config_fingerprint, manifest->config_fingerprint, 32U) != 0)
    status = YAP_V2_CONFLICT;
  if (status == YAP_V2_OK) {
    manifest->generation = expected_generation + 1U;
    status = YAP_V2_manifest_save_atomic(path, manifest);
  }
  YAP_V2_manifest_free(&current);
  (void)flock(lock_fd, LOCK_UN);
  if (close(lock_fd) != 0 && status == YAP_V2_OK) status = YAP_V2_IO_ERROR;
  free(lock_path);
  return status;
}

int YAP_V2_manifest_verify_segment_components(
  const char *index_dir, uint64_t manifest_generation,
  const YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  size_t j;
  int status;

  if (index_dir == NULL || segment == NULL || manifest_generation == 0U) {
    return YAP_V2_INVALID_ARGUMENT;
  }
  for (j = 0U; j < segment->component_count; j++) {
      const YAP_V2_COMPONENT_DESCRIPTOR *component = &segment->components[j];
      size_t length =
        strlen(index_dir) + strlen(segment->id) + strlen(component->name) + 12U;
      char *path = (char *)malloc(length);
      unsigned char checksum[32];
      unsigned char header_data[YAP_V2_FILE_HEADER_BYTES];
      uint64_t bytes;
      FILE *file;
      YAP_V2_FILE_HEADER header;

      if (path == NULL) {
        return YAP_V2_ALLOCATION_FAILED;
      }
      (void)snprintf(path, length, "%s/segments/%s/%s", index_dir, segment->id,
                     component->name);
      status = YAP_V2_file_sha256(path, checksum, &bytes);
      if (status == YAP_V2_OK &&
          (bytes != component->file_bytes || memcmp(checksum, component->checksum, 32U) != 0)) {
        status = YAP_V2_CHECKSUM_MISMATCH;
      }
      if (status == YAP_V2_OK && component->file_type != YAP_V2_FILE_ANN) {
        file = fopen(path, "rb");
        if (file == NULL ||
            fread(header_data, 1U, sizeof(header_data), file) != sizeof(header_data)) {
          status = YAP_V2_IO_ERROR;
        }
        if (file != NULL && fclose(file) != 0) {
          status = YAP_V2_IO_ERROR;
        }
      }
      free(path);
      if (status == YAP_V2_OK && component->file_type != YAP_V2_FILE_ANN) {
        status = YAP_V2_file_header_decode(header_data, &header);
      }
      if (status == YAP_V2_OK && component->file_type != YAP_V2_FILE_ANN &&
          (header.generation == 0U || header.generation > manifest_generation ||
           header.file_type != component->file_type ||
           header.payload_bytes + YAP_V2_FILE_HEADER_BYTES != bytes)) {
        status = YAP_V2_INVALID_FORMAT;
      }
      if (status != YAP_V2_OK) {
        return status;
      }
  }
  return YAP_V2_OK;
}

int YAP_V2_manifest_verify_components(const char *index_dir, const YAP_V2_MANIFEST *manifest) {
  size_t i;
  int status;
  if (index_dir == NULL || manifest == NULL) return YAP_V2_INVALID_ARGUMENT;
  status = YAP_V2_manifest_validate(manifest);
  if (status != YAP_V2_OK) return status;
  for (i = 0U; i < manifest->segment_count; i++) {
    status = YAP_V2_manifest_verify_segment_components(
      index_dir, manifest->generation, &manifest->segments[i]);
    if (status != YAP_V2_OK) return status;
  }
  return YAP_V2_OK;
}
