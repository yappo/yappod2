#include "indexing/yappo_update_wal_v2.h"

#include "common/yappo_types_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WAL_HEADER_BYTES 64U
#define WAL_OPERATION_HEADER_BYTES 72U
#define WAL_VERSION 1U
#define WAL_MAX_OPERATIONS 10000U

static const unsigned char wal_magic[8] = {'Y','A','P','W','A','L','2','\0'};

static int join_path(char *output, size_t capacity, const char *left,
                     const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static void put_u32(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)value;
  output[1] = (unsigned char)(value >> 8U);
  output[2] = (unsigned char)(value >> 16U);
  output[3] = (unsigned char)(value >> 24U);
}

static void put_u64(unsigned char *output, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++) output[i] = (unsigned char)(value >> (i * 8U));
}

static uint32_t get_u32(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
         ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

static uint64_t get_u64(const unsigned char *input) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0U; i < 8U; i++) value |= (uint64_t)input[i] << (i * 8U);
  return value;
}

static uint32_t crc32c_update(uint32_t crc, const unsigned char *data,
                              size_t length) {
  size_t i;
  for (i = 0U; i < length; i++) {
    unsigned int bit;
    crc ^= data[i];
    for (bit = 0U; bit < 8U; bit++)
      crc = (crc >> 1U) ^
            (UINT32_C(0x82f63b78) & (uint32_t)-(int32_t)(crc & 1U));
  }
  return crc;
}

static int sync_directory(const char *path) {
  int descriptor = open(path, O_RDONLY | O_DIRECTORY);
  int status = YAP_V2_OK;
  if (descriptor < 0) return YAP_V2_IO_ERROR;
  if (fsync(descriptor) != 0) status = YAP_V2_IO_ERROR;
  if (close(descriptor) != 0) status = YAP_V2_IO_ERROR;
  return status;
}

static int write_bytes(FILE *file, const void *data, size_t length,
                       uint32_t *crc, uint64_t *payload_bytes) {
  if (length != 0U && fwrite(data, 1U, length, file) != length)
    return YAP_V2_IO_ERROR;
  *crc = crc32c_update(*crc, data, length);
  if ((uint64_t)length > UINT64_MAX - *payload_bytes)
    return YAP_V2_OUT_OF_RANGE;
  *payload_bytes += (uint64_t)length;
  return YAP_V2_OK;
}

static size_t text_length(const char *value) {
  return value == NULL ? 0U : strlen(value);
}

void YAP_V2_update_wal_init(YAP_V2_UPDATE_WAL *wal) {
  if (wal != NULL) memset(wal, 0, sizeof(*wal));
}

void YAP_V2_update_wal_free(YAP_V2_UPDATE_WAL *wal) {
  size_t i;
  if (wal == NULL) return;
  for (i = 0U; i < wal->operation_count; i++)
    YAP_V2_ingest_operation_free(&wal->operations[i]);
  free(wal->operations);
  memset(wal, 0, sizeof(*wal));
}

int YAP_V2_update_wal_exists(const char *index_dir) {
  char path[4096];
  if (index_dir == NULL ||
      join_path(path, sizeof(path), index_dir, "update.wal") != 0)
    return 0;
  return access(path, F_OK) == 0;
}

int YAP_V2_update_wal_write(
    const char *index_dir, uint64_t base_generation,
    const YAP_V2_INGEST_OPERATION *operations, size_t operation_count) {
  char path[4096], temporary[4096];
  unsigned char header[WAL_HEADER_BYTES] = {0};
  uint32_t crc = UINT32_MAX;
  uint64_t payload_bytes = 0U;
  FILE *file = NULL;
  int descriptor = -1, status = YAP_V2_OK, renamed = 0;
  size_t i;
  if (index_dir == NULL || operations == NULL || operation_count == 0U ||
      operation_count > WAL_MAX_OPERATIONS || base_generation == UINT64_MAX)
    return YAP_V2_INVALID_ARGUMENT;
  if (join_path(path, sizeof(path), index_dir, "update.wal") != 0) 
    return YAP_V2_OUT_OF_RANGE;
  {
    int written = snprintf(temporary, sizeof(temporary),
                           "%s/update.wal.tmp-XXXXXX", index_dir);
    if (written < 0 || (size_t)written >= sizeof(temporary))
      return YAP_V2_OUT_OF_RANGE;
  }
  descriptor = mkstemp(temporary);
  if (descriptor < 0) return YAP_V2_IO_ERROR;
  file = fdopen(descriptor, "w+b");
  if (file == NULL) {
    (void)close(descriptor); (void)unlink(temporary);
    return YAP_V2_IO_ERROR;
  }
  descriptor = -1;
  if (fwrite(header, 1U, sizeof(header), file) != sizeof(header))
    status = YAP_V2_IO_ERROR;
  for (i = 0U; status == YAP_V2_OK && i < operation_count; i++) {
    const YAP_V2_INGEST_OPERATION *operation = &operations[i];
    const char *texts[5] = {operation->id, operation->url, operation->title,
                            operation->body, operation->metadata_json};
    size_t lengths[5], j, vector_values;
    unsigned char record[WAL_OPERATION_HEADER_BYTES] = {0};
    if (operation->id == NULL ||
        (operation->kind != YAP_V2_INGEST_UPSERT &&
         operation->kind != YAP_V2_INGEST_DELETE)) {
      status = YAP_V2_INVALID_ARGUMENT; break;
    }
    if (operation->vector_count != 0U &&
        operation->vector_dimensions > SIZE_MAX / operation->vector_count) {
      status = YAP_V2_OUT_OF_RANGE; break;
    }
    vector_values = operation->vector_count * operation->vector_dimensions;
    if (vector_values != 0U && operation->vectors == NULL) {
      status = YAP_V2_INVALID_ARGUMENT; break;
    }
    put_u32(record, (uint32_t)operation->kind);
    put_u64(record + 8U, (uint64_t)operation->updated_at_unix_ms);
    put_u64(record + 16U, (uint64_t)operation->vector_count);
    put_u64(record + 24U, (uint64_t)operation->vector_dimensions);
    for (j = 0U; j < 5U; j++) {
      lengths[j] = text_length(texts[j]);
      put_u64(record + 32U + j * 8U, (uint64_t)lengths[j]);
    }
    status = write_bytes(file, record, sizeof(record), &crc, &payload_bytes);
    for (j = 0U; status == YAP_V2_OK && j < 5U; j++)
      status = write_bytes(file, texts[j], lengths[j], &crc, &payload_bytes);
    for (j = 0U; status == YAP_V2_OK && j < vector_values; j++) {
      uint32_t bits;
      unsigned char encoded[4];
      memcpy(&bits, &operation->vectors[j], sizeof(bits));
      put_u32(encoded, bits);
      status = write_bytes(file, encoded, sizeof(encoded), &crc,
                           &payload_bytes);
    }
  }
  if (status == YAP_V2_OK) {
    memcpy(header, wal_magic, sizeof(wal_magic));
    put_u32(header + 8U, WAL_VERSION);
    put_u32(header + 12U, WAL_HEADER_BYTES);
    put_u64(header + 16U, base_generation);
    put_u64(header + 24U, base_generation + 1U);
    put_u64(header + 32U, (uint64_t)operation_count);
    put_u64(header + 40U, payload_bytes);
    put_u32(header + 48U, ~crc);
    if (fseek(file, 0L, SEEK_SET) != 0 ||
        fwrite(header, 1U, sizeof(header), file) != sizeof(header) ||
        fflush(file) != 0 || fsync(fileno(file)) != 0)
      status = YAP_V2_IO_ERROR;
  }
  if (fclose(file) != 0 && status == YAP_V2_OK) status = YAP_V2_IO_ERROR;
  if (status == YAP_V2_OK) {
    if (rename(temporary, path) != 0) status = YAP_V2_IO_ERROR;
    else renamed = 1;
  }
  if (status == YAP_V2_OK) status = sync_directory(index_dir);
  if (!renamed) (void)unlink(temporary);
  return status;
}

static int read_exact(FILE *file, void *data, size_t length,
                      uint32_t *crc, uint64_t *remaining) {
  if ((uint64_t)length > *remaining) return YAP_V2_INVALID_FORMAT;
  if (length != 0U && fread(data, 1U, length, file) != length)
    return YAP_V2_INVALID_FORMAT;
  *crc = crc32c_update(*crc, data, length);
  *remaining -= (uint64_t)length;
  return YAP_V2_OK;
}

static int read_text(FILE *file, uint64_t length, char **output,
                     uint32_t *crc, uint64_t *remaining) {
  char *value;
  if (length > SIZE_MAX - 1U) return YAP_V2_OUT_OF_RANGE;
  value = malloc((size_t)length + 1U);
  if (value == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (read_exact(file, value, (size_t)length, crc, remaining) != YAP_V2_OK) {
    free(value); return YAP_V2_INVALID_FORMAT;
  }
  value[length] = '\0';
  *output = value;
  return YAP_V2_OK;
}

int YAP_V2_update_wal_load(const char *index_dir, YAP_V2_UPDATE_WAL *wal) {
  char path[4096];
  unsigned char header[WAL_HEADER_BYTES];
  struct stat info;
  FILE *file;
  uint64_t operation_count, payload_bytes, remaining;
  uint32_t expected_crc, crc = UINT32_MAX;
  size_t i;
  int status = YAP_V2_OK;
  if (index_dir == NULL || wal == NULL ||
      join_path(path, sizeof(path), index_dir, "update.wal") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_update_wal_init(wal);
  file = fopen(path, "rb");
  if (file == NULL) return errno == ENOENT ? YAP_V2_NOT_FOUND : YAP_V2_IO_ERROR;
  if (fstat(fileno(file), &info) != 0 || info.st_size < (off_t)sizeof(header) ||
      fread(header, 1U, sizeof(header), file) != sizeof(header)) {
    status = YAP_V2_INVALID_FORMAT; goto done;
  }
  operation_count = get_u64(header + 32U);
  payload_bytes = get_u64(header + 40U);
  expected_crc = get_u32(header + 48U);
  if (memcmp(header, wal_magic, sizeof(wal_magic)) != 0 ||
      get_u32(header + 8U) != WAL_VERSION ||
      get_u32(header + 12U) != WAL_HEADER_BYTES || operation_count == 0U ||
      operation_count > WAL_MAX_OPERATIONS ||
      payload_bytes != (uint64_t)info.st_size - WAL_HEADER_BYTES) {
    status = YAP_V2_INVALID_FORMAT; goto done;
  }
  wal->base_generation = get_u64(header + 16U);
  wal->target_generation = get_u64(header + 24U);
  if (wal->base_generation == UINT64_MAX ||
      wal->target_generation != wal->base_generation + 1U) {
    status = YAP_V2_INVALID_FORMAT; goto done;
  }
  wal->operations = calloc((size_t)operation_count, sizeof(*wal->operations));
  if (wal->operations == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  wal->operation_count = (size_t)operation_count;
  remaining = payload_bytes;
  for (i = 0U; status == YAP_V2_OK && i < wal->operation_count; i++) {
    YAP_V2_INGEST_OPERATION *operation = &wal->operations[i];
    unsigned char record[WAL_OPERATION_HEADER_BYTES];
    uint64_t lengths[5], vector_count, vector_dimensions, vector_values;
    char **texts[5] = {&operation->id, &operation->url, &operation->title,
                       &operation->body, &operation->metadata_json};
    size_t j;
    status = read_exact(file, record, sizeof(record), &crc, &remaining);
    if (status != YAP_V2_OK) break;
    operation->kind = (YAP_V2_INGEST_KIND)get_u32(record);
    operation->updated_at_unix_ms = (int64_t)get_u64(record + 8U);
    vector_count = get_u64(record + 16U);
    vector_dimensions = get_u64(record + 24U);
    if ((operation->kind != YAP_V2_INGEST_UPSERT &&
         operation->kind != YAP_V2_INGEST_DELETE) ||
        vector_count > SIZE_MAX || vector_dimensions > SIZE_MAX ||
        (vector_count != 0U && vector_dimensions > SIZE_MAX / vector_count)) {
      status = YAP_V2_INVALID_FORMAT; break;
    }
    operation->vector_count = (size_t)vector_count;
    operation->vector_dimensions = (size_t)vector_dimensions;
    for (j = 0U; j < 5U; j++) lengths[j] = get_u64(record + 32U + j * 8U);
    for (j = 0U; status == YAP_V2_OK && j < 5U; j++)
      status = read_text(file, lengths[j], texts[j], &crc, &remaining);
    vector_values = vector_count * vector_dimensions;
    if (status == YAP_V2_OK && vector_values != 0U) {
      if (vector_values > SIZE_MAX / sizeof(float)) {
        status = YAP_V2_OUT_OF_RANGE; break;
      }
      operation->vectors = malloc((size_t)vector_values * sizeof(float));
      if (operation->vectors == NULL) {
        status = YAP_V2_ALLOCATION_FAILED; break;
      }
      for (j = 0U; status == YAP_V2_OK && j < (size_t)vector_values; j++) {
        unsigned char encoded[4];
        uint32_t bits;
        status = read_exact(file, encoded, sizeof(encoded), &crc, &remaining);
        bits = get_u32(encoded);
        memcpy(&operation->vectors[j], &bits, sizeof(bits));
      }
    }
    if (status == YAP_V2_OK && operation->id[0] == '\0')
      status = YAP_V2_INVALID_FORMAT;
  }
  if (status == YAP_V2_OK && (remaining != 0U || ~crc != expected_crc ||
                              fgetc(file) != EOF))
    status = YAP_V2_INVALID_FORMAT;
done:
  if (fclose(file) != 0 && status == YAP_V2_OK) status = YAP_V2_IO_ERROR;
  if (status != YAP_V2_OK) YAP_V2_update_wal_free(wal);
  return status;
}

int YAP_V2_update_wal_clear(const char *index_dir) {
  char path[4096];
  if (index_dir == NULL ||
      join_path(path, sizeof(path), index_dir, "update.wal") != 0)
    return YAP_V2_INVALID_ARGUMENT;
  if (unlink(path) != 0 && errno != ENOENT) return YAP_V2_IO_ERROR;
  return sync_directory(index_dir);
}
