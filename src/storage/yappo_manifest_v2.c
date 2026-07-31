#include "storage/yappo_manifest_v2.h"
#include "config/yappo_config_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

#define YAP_V2_MAX_MANIFEST_BYTES (16U * 1024U * 1024U)

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

static int exact_keys(yyjson_val *object, const char *const *names, size_t expected) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t seen = 0U;
  if (!yyjson_is_obj(object) || yyjson_obj_size(object) != expected)
    return 0;
  iterator = yyjson_obj_iter_with(object);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key);
    size_t i;
    int found = 0;
    for (i = 0U; names[i] != NULL; i++)
      if (strcmp(name, names[i]) == 0) {
        found = 1;
        break;
      }
    if (!found)
      return 0;
    seen++;
  }
  return seen == expected;
}

static int hex_decode(yyjson_val *value, unsigned char output[32]) {
  const char *text;
  size_t i;
  if (!yyjson_is_str(value) || yyjson_get_len(value) != 64U)
    return YAP_V2_INVALID_FORMAT;
  text = yyjson_get_str(value);
  for (i = 0U; i < 32U; i++) {
    unsigned char a = (unsigned char)text[i * 2U], b = (unsigned char)text[i * 2U + 1U];
    int high = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
    int low = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
    if (high < 0 || low < 0)
      return YAP_V2_INVALID_FORMAT;
    output[i] = (unsigned char)((high << 4) | low);
  }
  return YAP_V2_OK;
}

static int value_u64(yyjson_val *object, const char *key, uint64_t *output) {
  yyjson_val *value = yyjson_obj_get(object, key);
  if (!yyjson_is_uint(value))
    return YAP_V2_INVALID_FORMAT;
  *output = yyjson_get_uint(value);
  return YAP_V2_OK;
}

static int parse_component(yyjson_val *value, YAP_V2_COMPONENT_DESCRIPTOR *component) {
  static const char *const keys[] = {"name", "file_type", "records", "file_bytes", "sha256", NULL};
  yyjson_val *name, *type;
  int status;
  if (!exact_keys(value, keys, 5U))
    return YAP_V2_INVALID_FORMAT;
  name = yyjson_obj_get(value, "name");
  type = yyjson_obj_get(value, "file_type");
  if (!yyjson_is_str(name) || yyjson_get_len(name) == 0U ||
      yyjson_get_len(name) > YAP_V2_MAX_COMPONENT_NAME_BYTES || !yyjson_is_uint(type) ||
      yyjson_get_uint(type) > UINT32_MAX)
    return YAP_V2_INVALID_FORMAT;
  memset(component, 0, sizeof(*component));
  memcpy(component->name, yyjson_get_str(name), yyjson_get_len(name));
  component->file_type = (uint32_t)yyjson_get_uint(type);
  status = value_u64(value, "records", &component->record_count);
  if (status == YAP_V2_OK)
    status = value_u64(value, "file_bytes", &component->file_bytes);
  if (status == YAP_V2_OK)
    status = hex_decode(yyjson_obj_get(value, "sha256"), component->checksum);
  return status;
}

static int parse_segment(yyjson_val *value, YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  static const char *const keys[] = {"id",         "documents",  "passages",
                                     "tombstones", "components", NULL};
  yyjson_val *id, *components, *item;
  yyjson_arr_iter iterator;
  int status = YAP_V2_OK;
  if (!exact_keys(value, keys, 5U))
    return YAP_V2_INVALID_FORMAT;
  id = yyjson_obj_get(value, "id");
  components = yyjson_obj_get(value, "components");
  if (!yyjson_is_str(id) || yyjson_get_len(id) == 0U ||
      yyjson_get_len(id) > YAP_V2_MAX_IDENTIFIER_BYTES || !yyjson_is_arr(components) ||
      yyjson_arr_size(components) == 0U || yyjson_arr_size(components) > YAP_V2_MAX_COMPONENTS)
    return YAP_V2_INVALID_FORMAT;
  memset(segment, 0, sizeof(*segment));
  memcpy(segment->id, yyjson_get_str(id), yyjson_get_len(id));
  status = value_u64(value, "documents", &segment->document_count);
  if (status == YAP_V2_OK)
    status = value_u64(value, "passages", &segment->passage_count);
  if (status == YAP_V2_OK)
    status = value_u64(value, "tombstones", &segment->tombstone_count);
  yyjson_arr_iter_init(components, &iterator);
  while (status == YAP_V2_OK && (item = yyjson_arr_iter_next(&iterator)) != NULL) {
    YAP_V2_COMPONENT_DESCRIPTOR component;
    status = parse_component(item, &component);
    if (status == YAP_V2_OK)
      status = YAP_V2_segment_descriptor_add_component(segment, &component);
  }
  return status;
}

int YAP_V2_manifest_load(const char *path, YAP_V2_MANIFEST *manifest) {
  static const char *const keys[] = {"format_version", "generation", "config_fingerprint",
                                     "segments", NULL};
  FILE *file;
  struct stat stat_buffer;
  char *data;
  size_t read_bytes;
  yyjson_doc *document;
  yyjson_read_err error;
  yyjson_val *root, *segments, *item, *version;
  yyjson_arr_iter iterator;
  YAP_V2_MANIFEST parsed;
  int status = YAP_V2_OK;
  if (path == NULL || manifest == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  file = fopen(path, "rb");
  if (file == NULL)
    return YAP_V2_IO_ERROR;
  if (fstat(fileno(file), &stat_buffer) != 0 || stat_buffer.st_size < 0 ||
      (uint64_t)stat_buffer.st_size > YAP_V2_MAX_MANIFEST_BYTES) {
    fclose(file);
    return YAP_V2_OUT_OF_RANGE;
  }
  data = (char *)malloc((size_t)stat_buffer.st_size + 1U);
  if (data == NULL) {
    fclose(file);
    return YAP_V2_ALLOCATION_FAILED;
  }
  read_bytes = fread(data, 1U, (size_t)stat_buffer.st_size, file);
  if (read_bytes != (size_t)stat_buffer.st_size || fclose(file) != 0) {
    free(data);
    return YAP_V2_IO_ERROR;
  }
  data[read_bytes] = '\0';
  document = yyjson_read_opts(data, read_bytes, YYJSON_READ_NOFLAG, NULL, &error);
  free(data);
  if (document == NULL)
    return YAP_V2_INVALID_FORMAT;
  root = yyjson_doc_get_root(document);
  YAP_V2_manifest_init(&parsed);
  if (!exact_keys(root, keys, 4U)) {
    status = YAP_V2_INVALID_FORMAT;
    goto done;
  }
  version = yyjson_obj_get(root, "format_version");
  segments = yyjson_obj_get(root, "segments");
  if (!yyjson_is_uint(version) || yyjson_get_uint(version) != YAP_V2_FORMAT_VERSION ||
      !yyjson_is_arr(segments)) {
    status = YAP_V2_INVALID_FORMAT;
    goto done;
  }
  status = value_u64(root, "generation", &parsed.generation);
  if (status == YAP_V2_OK)
    status = hex_decode(yyjson_obj_get(root, "config_fingerprint"), parsed.config_fingerprint);
  yyjson_arr_iter_init(segments, &iterator);
  while (status == YAP_V2_OK && (item = yyjson_arr_iter_next(&iterator)) != NULL) {
    YAP_V2_SEGMENT_DESCRIPTOR segment;
    status = parse_segment(item, &segment);
    if (status == YAP_V2_OK)
      status = YAP_V2_manifest_add_segment(&parsed, &segment);
  }
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_validate(&parsed);
done:
  yyjson_doc_free(document);
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

static int write_hex(FILE *file, const unsigned char bytes[32]) {
  static const char digits[] = "0123456789abcdef";
  size_t i;
  if (fputc('"', file) == EOF)
    return -1;
  for (i = 0U; i < 32U; i++)
    if (fputc(digits[bytes[i] >> 4], file) == EOF || fputc(digits[bytes[i] & 15U], file) == EOF)
      return -1;
  return fputc('"', file) == EOF ? -1 : 0;
}

static int write_json(FILE *file, const YAP_V2_MANIFEST *manifest) {
  size_t i, j;
  if (fprintf(file, "{\"format_version\":%u,\"generation\":%" PRIu64 ",\"config_fingerprint\":",
              manifest->format_version, manifest->generation) < 0 ||
      write_hex(file, manifest->config_fingerprint) != 0 || fputs(",\"segments\":[", file) == EOF)
    return -1;
  for (i = 0U; i < manifest->segment_count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *segment = &manifest->segments[i];
    if (i > 0U && fputc(',', file) == EOF)
      return -1;
    if (fprintf(file,
                "{\"id\":\"%s\",\"documents\":%" PRIu64 ",\"passages\":%" PRIu64
                ",\"tombstones\":%" PRIu64 ",\"components\":[",
                segment->id, segment->document_count, segment->passage_count,
                segment->tombstone_count) < 0)
      return -1;
    for (j = 0U; j < segment->component_count; j++) {
      const YAP_V2_COMPONENT_DESCRIPTOR *component = &segment->components[j];
      if (j > 0U && fputc(',', file) == EOF)
        return -1;
      if (fprintf(file,
                  "{\"name\":\"%s\",\"file_type\":%u,\"records\":%" PRIu64
                  ",\"file_bytes\":%" PRIu64 ",\"sha256\":",
                  component->name, component->file_type, component->record_count,
                  component->file_bytes) < 0 ||
          write_hex(file, component->checksum) != 0 || fputc('}', file) == EOF)
        return -1;
    }
    if (fputs("]}", file) == EOF)
      return -1;
  }
  return fputs("]}", file) == EOF ? -1 : 0;
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
  char *temporary;
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
  length = strlen(path);
  if (length > SIZE_MAX - 5U) {
    return YAP_V2_OUT_OF_RANGE;
  }
  temporary = (char *)malloc(length + 5U);
  if (temporary == NULL) {
    return YAP_V2_ALLOCATION_FAILED;
  }
  (void)snprintf(temporary, length + 5U, "%s.tmp", path);
  file = fopen(temporary, "wb");
  if (file == NULL) {
    free(temporary);
    return YAP_V2_IO_ERROR;
  }
  if (write_json(file, manifest) != 0 || fflush(file) != 0 || fsync(fileno(file)) != 0) {
    write_failed = 1;
  }
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
