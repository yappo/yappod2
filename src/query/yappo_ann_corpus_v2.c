#include "query/yappo_ann_corpus_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define YAP_V2_ANN_CONNECTIVITY 16U
#define YAP_V2_ANN_EXPANSION_ADD 128U
#define YAP_V2_ANN_EXPANSION_SEARCH 128U
#define YAP_V2_ANN_CACHE_PAYLOAD_VERSION 1U
#define YAP_V2_ANN_CACHE_META_NAME "ann-base.yap2"
#define YAP_V2_ANN_CACHE_INDEX_NAME "ann-base.usearch"
#define YAP_V2_ANN_CACHE_LOCK_NAME "ann-base.lock"

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len && left.data != NULL && right.data != NULL &&
         memcmp(left.data, right.data, left.len) == 0;
}

static uint64_t string_hash(const char *value) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const unsigned char *cursor = (const unsigned char *)value;
  while (*cursor != '\0') {
    hash ^= *cursor++;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t bytes_hash_update(uint64_t hash, const void *data, size_t bytes) {
  const unsigned char *cursor = data;
  size_t i;
  for (i = 0U; i < bytes; i++) {
    hash ^= cursor[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t descriptor_fingerprint(const YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t i;
  hash = bytes_hash_update(hash, segment->id, strlen(segment->id));
  hash = bytes_hash_update(hash, &segment->document_count, sizeof(segment->document_count));
  hash = bytes_hash_update(hash, &segment->passage_count, sizeof(segment->passage_count));
  hash = bytes_hash_update(hash, &segment->tombstone_count, sizeof(segment->tombstone_count));
  hash = bytes_hash_update(hash, &segment->component_count, sizeof(segment->component_count));
  for (i = 0U; i < segment->component_count; i++) {
    const YAP_V2_COMPONENT_DESCRIPTOR *component = &segment->components[i];
    hash = bytes_hash_update(hash, component->name, strlen(component->name));
    hash = bytes_hash_update(hash, &component->file_type, sizeof(component->file_type));
    hash = bytes_hash_update(hash, &component->record_count, sizeof(component->record_count));
    hash = bytes_hash_update(hash, &component->file_bytes, sizeof(component->file_bytes));
    hash = bytes_hash_update(hash, component->checksum, sizeof(component->checksum));
  }
  return hash;
}

static void put_u32_le(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)(value & 0xffU);
  output[1] = (unsigned char)((value >> 8U) & 0xffU);
  output[2] = (unsigned char)((value >> 16U) & 0xffU);
  output[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void put_u64_le(unsigned char *output, uint64_t value) {
  size_t i;
  for (i = 0U; i < 8U; i++) output[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
}

static uint32_t get_u32_le(const unsigned char *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
         ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

static uint64_t get_u64_le(const unsigned char *input) {
  uint64_t value = 0U;
  size_t i;
  for (i = 0U; i < 8U; i++) value |= (uint64_t)input[i] << (i * 8U);
  return value;
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
  uint32_t crc = UINT32_MAX;
  size_t i;
  for (i = 0U; i < len; i++) {
    uint32_t current = (crc ^ data[i]) & 0xffU;
    unsigned int bit;
    for (bit = 0U; bit < 8U; bit++)
      current = (current >> 1U) ^ ((current & 1U) ? UINT32_C(0x82f63b78) : 0U);
    crc = (crc >> 8U) ^ current;
  }
  return ~crc;
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int sync_file(const char *path) {
  FILE *file = fopen(path, "rb");
  int status;
  if (file == NULL) return -1;
  status = fsync(fileno(file));
  if (fclose(file) != 0) status = -1;
  return status;
}

static int sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY), status;
  if (fd < 0) return -1;
  status = fsync(fd);
  if (close(fd) != 0) status = -1;
  return status;
}

static int write_file(const char *path, const unsigned char *data, size_t bytes) {
  FILE *file = fopen(path, "wb");
  int status = 0;
  if (file == NULL) return -1;
  if (fwrite(data, 1U, bytes, file) != bytes || fflush(file) != 0 ||
      fsync(fileno(file)) != 0) status = -1;
  if (fclose(file) != 0) status = -1;
  return status;
}

static int read_file(const char *path, unsigned char **data, size_t *bytes) {
  FILE *file;
  struct stat info;
  unsigned char *loaded;
  if (path == NULL || data == NULL || bytes == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (stat(path, &info) != 0)
    return errno == ENOENT ? YAP_V2_NOT_FOUND : YAP_V2_IO_ERROR;
  if (info.st_size < 0 || (uint64_t)info.st_size > YAP_V2_MAX_MANIFEST_BYTES)
    return YAP_V2_OUT_OF_RANGE;
  file = fopen(path, "rb");
  if (file == NULL) return YAP_V2_IO_ERROR;
  loaded = malloc((size_t)info.st_size);
  if (loaded == NULL) { fclose(file); return YAP_V2_ALLOCATION_FAILED; }
  if (fread(loaded, 1U, (size_t)info.st_size, file) != (size_t)info.st_size ||
      fgetc(file) != EOF || ferror(file) || fclose(file) != 0) {
    free(loaded); return YAP_V2_IO_ERROR;
  }
  *data = loaded; *bytes = (size_t)info.st_size;
  return YAP_V2_OK;
}

static int read_ann_cache_generation(const char *path, uint64_t *generation) {
  unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  YAP_V2_FILE_HEADER header;
  FILE *file;
  size_t read_bytes;
  int close_status;
  if (path == NULL || generation == NULL) return YAP_V2_INVALID_ARGUMENT;
  file = fopen(path, "rb");
  if (file == NULL) return errno == ENOENT ? YAP_V2_NOT_FOUND : YAP_V2_IO_ERROR;
  read_bytes = fread(encoded, 1U, sizeof(encoded), file);
  close_status = fclose(file);
  if (read_bytes != sizeof(encoded) || close_status != 0)
    return YAP_V2_IO_ERROR;
  if (YAP_V2_file_header_decode(encoded, &header) != YAP_V2_OK ||
      header.file_type != YAP_V2_FILE_ANN_BASE)
    return YAP_V2_INVALID_FORMAT;
  *generation = header.generation;
  return YAP_V2_OK;
}

void YAP_V2_ann_corpus_init(YAP_V2_ANN_CORPUS *corpus) {
  if (corpus == NULL) return;
  memset(corpus, 0, sizeof(*corpus));
  YAP_V2_ann_index_init(&corpus->index);
}

void YAP_V2_ann_corpus_free(YAP_V2_ANN_CORPUS *corpus) {
  if (corpus == NULL) return;
  YAP_V2_ann_index_close(&corpus->index);
  free(corpus->segments);
  free(corpus->segment_fingerprints);
  memset(corpus, 0, sizeof(*corpus));
}

static int vector_is_visible(const YAP_V2_SEARCH_SNAPSHOT *snapshot, size_t segment_ordinal,
                             const YAP_V2_SEGMENT *documents, size_t passage_ordinal) {
  YAP_V2_DOCUMENT_HIT hit;
  const YAP_V2_PASSAGE_VIEW *passage;
  if (documents == NULL || passage_ordinal >= documents->passage_count) return 0;
  passage = &documents->passages[passage_ordinal];
  return YAP_V2_snapshot_lookup_document(snapshot, passage->parent_document_id, &hit) == YAP_V2_OK &&
         hit.segment_ordinal == segment_ordinal;
}

int YAP_V2_ann_corpus_build(const YAP_V2_MANIFEST *manifest,
                            const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                            const YAP_V2_ANN_SEGMENT *segments,
                            size_t segment_count, YAP_V2_ANN_CORPUS *corpus) {
  const YAP_V2_VECTOR_SEGMENT *representative = NULL;
  size_t visible_count = 0U, s, i;
  int status = YAP_V2_OK;
  YAP_V2_ANN_CORPUS built;
  if (manifest == NULL || snapshot == NULL || segments == NULL || corpus == NULL ||
      segment_count == 0U || segment_count != manifest->segment_count ||
      segment_count != YAP_V2_snapshot_segment_count(snapshot) ||
      manifest->generation != YAP_V2_snapshot_generation(snapshot))
    return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_ann_corpus_init(&built);
  if (segment_count > SIZE_MAX / sizeof(*built.segments)) return YAP_V2_OUT_OF_RANGE;
  built.segments = malloc(segment_count * sizeof(*built.segments));
  built.segment_fingerprints = malloc(segment_count * sizeof(*built.segment_fingerprints));
  if (built.segments == NULL || built.segment_fingerprints == NULL) {
    YAP_V2_ann_corpus_free(&built); return YAP_V2_ALLOCATION_FAILED;
  }
  memcpy(built.segments, manifest->segments, segment_count * sizeof(*built.segments));
  for (s = 0U; s < segment_count; s++)
    built.segment_fingerprints[s] = descriptor_fingerprint(&manifest->segments[s]);
  built.segment_count = segment_count;
  built.generation = manifest->generation;
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_VECTOR_SEGMENT *vectors = segments[s].vectors;
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    if (vectors == NULL || vectors->entry_count == 0U) continue;
    if (representative == NULL) representative = vectors;
    if (documents == NULL || vectors->entry_count != documents->passage_count) {
      status = YAP_V2_CONFLICT;
      goto done;
    }
    for (i = 0U; i < vectors->entry_count; i++) {
      if (!bytes_equal(vectors->entries[i].id, documents->passages[i].id)) {
        status = YAP_V2_CONFLICT;
        goto done;
      }
      if (vector_is_visible(snapshot, s, documents, i)) visible_count++;
    }
  }
  if (visible_count == 0U) {
    built.vector_count = 0U;
    goto publish;
  }
  if (representative == NULL) { status = YAP_V2_CONFLICT; goto done; }
  status = YAP_V2_ann_index_create(representative->metric,
                                    representative->dimensions,
                                    visible_count, YAP_V2_ANN_CONNECTIVITY,
                                    YAP_V2_ANN_EXPANSION_ADD, YAP_V2_ANN_EXPANSION_SEARCH,
                                    &built.index);
  if (status != YAP_ANN_OK) { status = YAP_V2_CONFLICT; goto done; }
  for (s = 0U; status == YAP_V2_OK && s < segment_count; s++) {
    const YAP_V2_VECTOR_SEGMENT *vectors = segments[s].vectors;
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    if (vectors == NULL) continue;
    if (s > UINT32_MAX) { status = YAP_V2_OUT_OF_RANGE; break; }
    for (i = 0U; i < vectors->entry_count; i++) {
      uint64_t key;
      if (!vector_is_visible(snapshot, s, documents, i)) continue;
      if (i > UINT32_MAX) { status = YAP_V2_OUT_OF_RANGE; break; }
      key = ((uint64_t)s << 32U) | (uint64_t)i;
      if (YAP_V2_ann_index_add(&built.index, key, vectors->entries[i].values) != YAP_ANN_OK) {
        status = YAP_V2_CONFLICT;
        break;
      }
    }
  }
  if (status != YAP_V2_OK) goto done;
  built.vector_count = visible_count;
publish:
  YAP_V2_ann_corpus_free(corpus);
  *corpus = built;
  return YAP_V2_OK;
done:
  YAP_V2_ann_corpus_free(&built);
  return status;
}

int YAP_V2_ann_corpus_search(const YAP_V2_ANN_CORPUS *corpus, const float *query,
                             size_t dimensions, size_t top_k, uint64_t *keys,
                             size_t key_capacity, size_t *key_count) {
  if (corpus == NULL || query == NULL || keys == NULL || key_count == NULL || top_k == 0U)
    return YAP_V2_INVALID_ARGUMENT;
  if (corpus->vector_count == 0U) { *key_count = 0U; return YAP_V2_OK; }
  return YAP_V2_ann_index_search(&corpus->index, query, dimensions, top_k,
                                 keys, key_capacity, key_count);
}

int YAP_V2_ann_corpus_save_cache(const char *index_dir,
                                 const YAP_V2_ANN_CORPUS *corpus) {
  unsigned char *file_data = NULL, *payload;
  unsigned char ann_checksum[32];
  YAP_V2_FILE_HEADER header;
  char ann_path[4096], ann_tmp[4096], meta_path[4096], meta_tmp[4096], lock_path[4096];
  uint64_t ann_bytes = 0U;
  size_t payload_bytes = 64U, file_bytes, offset = 0U, i;
  int lock_fd = -1;
  int status = YAP_V2_IO_ERROR;
  if (index_dir == NULL || corpus == NULL || corpus->index.index == NULL ||
      corpus->vector_count == 0U || corpus->segment_count == 0U ||
      corpus->segment_fingerprints == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < corpus->segment_count; i++) {
    size_t id_bytes = strlen(corpus->segments[i].id);
    if (id_bytes == 0U || id_bytes > YAP_V2_MAX_IDENTIFIER_BYTES ||
        payload_bytes > SIZE_MAX - 12U - id_bytes) return YAP_V2_OUT_OF_RANGE;
    payload_bytes += 12U + id_bytes;
  }
  if (payload_bytes > YAP_V2_MAX_MANIFEST_BYTES - YAP_V2_FILE_HEADER_BYTES ||
      join_path(ann_path, sizeof(ann_path), index_dir, YAP_V2_ANN_CACHE_INDEX_NAME) != 0 ||
      join_path(meta_path, sizeof(meta_path), index_dir, YAP_V2_ANN_CACHE_META_NAME) != 0 ||
      join_path(lock_path, sizeof(lock_path), index_dir, YAP_V2_ANN_CACHE_LOCK_NAME) != 0)
    return YAP_V2_OUT_OF_RANGE;
  {
    int ann_written = snprintf(ann_tmp, sizeof(ann_tmp), "%s.tmp.%ld.%p", ann_path,
                               (long)getpid(), (const void *)corpus);
    int meta_written = snprintf(meta_tmp, sizeof(meta_tmp), "%s.tmp.%ld.%p", meta_path,
                                (long)getpid(), (const void *)corpus);
    if (ann_written < 0 || (size_t)ann_written >= sizeof(ann_tmp) ||
        meta_written < 0 || (size_t)meta_written >= sizeof(meta_tmp))
      return YAP_V2_OUT_OF_RANGE;
  }
  (void)unlink(ann_tmp); (void)unlink(meta_tmp);
  if (YAP_V2_ann_index_save(&corpus->index, ann_tmp) != YAP_ANN_OK ||
      sync_file(ann_tmp) != 0 ||
      YAP_V2_file_sha256(ann_tmp, ann_checksum, &ann_bytes) != YAP_V2_OK) goto done;
  if (payload_bytes > SIZE_MAX - YAP_V2_FILE_HEADER_BYTES) { status = YAP_V2_OUT_OF_RANGE; goto done; }
  file_bytes = YAP_V2_FILE_HEADER_BYTES + payload_bytes;
  file_data = calloc(1U, file_bytes);
  if (file_data == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  payload = file_data + YAP_V2_FILE_HEADER_BYTES;
  put_u32_le(payload + offset, YAP_V2_ANN_CACHE_PAYLOAD_VERSION); offset += 4U;
  put_u32_le(payload + offset, (uint32_t)corpus->index.dimensions); offset += 4U;
  put_u32_le(payload + offset, (uint32_t)corpus->index.metric); offset += 4U;
  put_u32_le(payload + offset, (uint32_t)corpus->segment_count); offset += 4U;
  put_u64_le(payload + offset, corpus->vector_count); offset += 8U;
  put_u64_le(payload + offset, ann_bytes); offset += 8U;
  memcpy(payload + offset, ann_checksum, sizeof(ann_checksum)); offset += sizeof(ann_checksum);
  for (i = 0U; i < corpus->segment_count; i++) {
    size_t id_bytes = strlen(corpus->segments[i].id);
    put_u32_le(payload + offset, (uint32_t)id_bytes); offset += 4U;
    memcpy(payload + offset, corpus->segments[i].id, id_bytes); offset += id_bytes;
    put_u64_le(payload + offset, corpus->segment_fingerprints[i]); offset += 8U;
  }
  if (offset != payload_bytes) { status = YAP_V2_CONFLICT; goto done; }
  memset(&header, 0, sizeof(header));
  header.format_version = YAP_V2_FORMAT_VERSION;
  header.header_bytes = YAP_V2_FILE_HEADER_BYTES;
  header.file_type = YAP_V2_FILE_ANN_BASE;
  header.generation = corpus->generation;
  header.payload_bytes = payload_bytes;
  header.payload_crc32c = crc32c(payload, payload_bytes);
  status = YAP_V2_file_header_encode(&header, file_data);
  if (status != YAP_V2_OK || write_file(meta_tmp, file_data, file_bytes) != 0) {
    status = YAP_V2_IO_ERROR; goto done;
  }
  lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
    status = YAP_V2_IO_ERROR; goto done;
  }
  {
    uint64_t published_generation = 0U;
    if (read_ann_cache_generation(meta_path, &published_generation) == YAP_V2_OK &&
        published_generation > corpus->generation) {
      status = YAP_V2_OK; goto done;
    }
  }
  if (rename(ann_tmp, ann_path) != 0 || rename(meta_tmp, meta_path) != 0 ||
      sync_directory(index_dir) != 0) { status = YAP_V2_IO_ERROR; goto done; }
  status = YAP_V2_OK;
done:
  if (lock_fd >= 0) {
    (void)flock(lock_fd, LOCK_UN);
    (void)close(lock_fd);
  }
  (void)unlink(ann_tmp); (void)unlink(meta_tmp);
  free(file_data);
  return status;
}

int YAP_V2_ann_corpus_load_cache(const char *index_dir,
                                 const YAP_V2_CONFIG *config,
                                 const YAP_V2_MANIFEST *manifest,
                                 YAP_V2_ANN_CORPUS *corpus) {
  unsigned char *file_data = NULL, *payload, checksum[32];
  YAP_V2_FILE_HEADER header;
  YAP_V2_ANN_CORPUS loaded;
  char ann_path[4096], meta_path[4096];
  uint64_t ann_bytes, actual_ann_bytes;
  size_t file_bytes = 0U, payload_bytes, offset = 0U, i;
  uint32_t dimensions, metric, segment_count;
  uint64_t vector_count;
  int status;
  if (index_dir == NULL || config == NULL || manifest == NULL || corpus == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  if (join_path(ann_path, sizeof(ann_path), index_dir, YAP_V2_ANN_CACHE_INDEX_NAME) != 0 ||
      join_path(meta_path, sizeof(meta_path), index_dir, YAP_V2_ANN_CACHE_META_NAME) != 0)
    return YAP_V2_OUT_OF_RANGE;
  status = read_file(meta_path, &file_data, &file_bytes);
  if (status != YAP_V2_OK) return status;
  YAP_V2_ann_corpus_init(&loaded);
  if (file_bytes < YAP_V2_FILE_HEADER_BYTES + 64U ||
      YAP_V2_file_header_decode(file_data, &header) != YAP_V2_OK ||
      header.file_type != YAP_V2_FILE_ANN_BASE || header.generation > manifest->generation ||
      header.payload_bytes != file_bytes - YAP_V2_FILE_HEADER_BYTES) {
    status = YAP_V2_INVALID_FORMAT; goto done;
  }
  payload = file_data + YAP_V2_FILE_HEADER_BYTES;
  payload_bytes = (size_t)header.payload_bytes;
  if (crc32c(payload, payload_bytes) != header.payload_crc32c ||
      get_u32_le(payload) != YAP_V2_ANN_CACHE_PAYLOAD_VERSION) {
    status = YAP_V2_CHECKSUM_MISMATCH; goto done;
  }
  offset = 4U;
  dimensions = get_u32_le(payload + offset); offset += 4U;
  metric = get_u32_le(payload + offset); offset += 4U;
  segment_count = get_u32_le(payload + offset); offset += 4U;
  vector_count = get_u64_le(payload + offset); offset += 8U;
  ann_bytes = get_u64_le(payload + offset); offset += 8U;
  memcpy(checksum, payload + offset, sizeof(checksum)); offset += sizeof(checksum);
  if (dimensions != config->vector_dimensions || metric != (uint32_t)config->vector_metric ||
      segment_count == 0U || segment_count > YAP_V2_MAX_SEGMENTS || vector_count == 0U ||
      vector_count > SIZE_MAX) {
    status = YAP_V2_CONFLICT; goto done;
  }
  loaded.segments = calloc(segment_count, sizeof(*loaded.segments));
  loaded.segment_fingerprints = malloc(segment_count * sizeof(*loaded.segment_fingerprints));
  if (loaded.segments == NULL || loaded.segment_fingerprints == NULL) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  for (i = 0U; i < segment_count; i++) {
    uint32_t id_bytes;
    if (offset > payload_bytes || payload_bytes - offset < 4U) {
      status = YAP_V2_INVALID_FORMAT; goto done;
    }
    id_bytes = get_u32_le(payload + offset); offset += 4U;
    if (id_bytes == 0U || id_bytes > YAP_V2_MAX_IDENTIFIER_BYTES ||
        payload_bytes - offset < (size_t)id_bytes + 8U) {
      status = YAP_V2_INVALID_FORMAT; goto done;
    }
    memcpy(loaded.segments[i].id, payload + offset, id_bytes);
    loaded.segments[i].id[id_bytes] = '\0'; offset += id_bytes;
    if (YAP_V2_segment_id_validate(loaded.segments[i].id) != YAP_V2_OK) {
      status = YAP_V2_INVALID_FORMAT; goto done;
    }
    loaded.segment_fingerprints[i] = get_u64_le(payload + offset); offset += 8U;
  }
  if (offset != payload_bytes) { status = YAP_V2_INVALID_FORMAT; goto done; }
  {
    unsigned char actual_checksum[32];
    if (YAP_V2_file_sha256(ann_path, actual_checksum, &actual_ann_bytes) != YAP_V2_OK ||
        actual_ann_bytes != ann_bytes || memcmp(actual_checksum, checksum, sizeof(checksum)) != 0) {
      status = YAP_V2_CHECKSUM_MISMATCH; goto done;
    }
  }
  if (YAP_V2_ann_index_view(ann_path, config->vector_metric, config->vector_dimensions,
                            (size_t)vector_count, YAP_V2_ANN_EXPANSION_SEARCH,
                            &loaded.index) != YAP_ANN_OK) {
    status = YAP_V2_CONFLICT; goto done;
  }
  loaded.segment_count = segment_count;
  loaded.vector_count = (size_t)vector_count;
  loaded.generation = header.generation;
  YAP_V2_ann_corpus_free(corpus);
  *corpus = loaded;
  free(file_data);
  return YAP_V2_OK;
done:
  YAP_V2_ann_corpus_free(&loaded);
  free(file_data);
  return status;
}

void YAP_V2_ann_query_plan_init(YAP_V2_ANN_QUERY_PLAN *plan) {
  if (plan != NULL) memset(plan, 0, sizeof(*plan));
}

void YAP_V2_ann_query_plan_free(YAP_V2_ANN_QUERY_PLAN *plan) {
  if (plan == NULL) return;
  free(plan->base_to_current);
  free(plan->current_is_delta);
  memset(plan, 0, sizeof(*plan));
}

int YAP_V2_ann_query_plan_build(const YAP_V2_ANN_CORPUS *corpus,
                                const YAP_V2_MANIFEST *manifest,
                                YAP_V2_ANN_QUERY_PLAN *plan) {
  size_t *slots = NULL;
  size_t capacity = 1U, i;
  YAP_V2_ANN_QUERY_PLAN built;
  if (corpus == NULL || manifest == NULL || plan == NULL || corpus->segment_count == 0U ||
      manifest->segment_count == 0U) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_ann_query_plan_init(&built);
  while (capacity < corpus->segment_count * 2U) {
    if (capacity > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
    capacity *= 2U;
  }
  slots = malloc(capacity * sizeof(*slots));
  built.base_to_current = malloc(corpus->segment_count * sizeof(*built.base_to_current));
  built.current_is_delta = calloc(manifest->segment_count, sizeof(*built.current_is_delta));
  if (slots == NULL || built.base_to_current == NULL || built.current_is_delta == NULL) {
    free(slots); YAP_V2_ann_query_plan_free(&built); return YAP_V2_ALLOCATION_FAILED;
  }
  for (i = 0U; i < capacity; i++) slots[i] = SIZE_MAX;
  for (i = 0U; i < corpus->segment_count; i++) {
    size_t slot = (size_t)(string_hash(corpus->segments[i].id) & (uint64_t)(capacity - 1U));
    while (slots[slot] != SIZE_MAX) slot = (slot + 1U) & (capacity - 1U);
    slots[slot] = i;
    built.base_to_current[i] = SIZE_MAX;
  }
  for (i = 0U; i < manifest->segment_count; i++) {
    size_t slot = (size_t)(string_hash(manifest->segments[i].id) & (uint64_t)(capacity - 1U));
    size_t probes;
    int found = 0;
    for (probes = 0U; probes < capacity; probes++) {
      size_t base = slots[slot];
      if (base == SIZE_MAX) break;
      if (strcmp(corpus->segments[base].id, manifest->segments[i].id) == 0) {
        if (corpus->segment_fingerprints[base] ==
            descriptor_fingerprint(&manifest->segments[i])) {
          built.base_to_current[base] = i;
          found = 1;
        }
        break;
      }
      slot = (slot + 1U) & (capacity - 1U);
    }
    if (!found) {
      built.current_is_delta[i] = 1U;
      built.delta_segment_count++;
    }
  }
  free(slots);
  for (i = 0U; i < corpus->segment_count; i++)
    if (built.base_to_current[i] == SIZE_MAX) built.missing_base_segment_count++;
  built.base_segment_count = corpus->segment_count;
  built.current_segment_count = manifest->segment_count;
  YAP_V2_ann_query_plan_free(plan);
  *plan = built;
  return YAP_V2_OK;
}
