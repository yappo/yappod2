#include "yappo_update_v2.h"

#include "yappo_ann_v2.h"
#include "yappo_config_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_unicode.h"
#include "yappo_vector_v2.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  YAP_V2_CHUNK_SEQUENCE chunks;
} OPERATION_CHUNKS;

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static YAP_V2_BYTES_VIEW view(const char *value) {
  YAP_V2_BYTES_VIEW result = {(const unsigned char *)value, value == NULL ? 0U : strlen(value)};
  return result;
}

static void cleanup_segment_dir(const char *directory) {
  static const char *const names[] = {"documents.yap2", "terms.yap2", "postings.yap2",
    "positions.yap2", "metadata.yap2", "vectors.yap2", "vectors.usearch",
    "tombstones.yap2", NULL};
  char path[4096]; size_t i;
  if (directory == NULL) return;
  for (i = 0U; names[i] != NULL; i++)
    if (join_path(path, sizeof(path), directory, names[i]) == 0) (void)unlink(path);
  (void)rmdir(directory);
}

static int add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                         const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  return YAP_V2_segment_descriptor_add_component(segment, component);
}

static int validate_unique_ids(const YAP_V2_INGEST_OPERATION *operations, size_t count) {
  size_t i, j;
  for (i = 0U; i < count; i++)
    for (j = 0U; j < i; j++)
      if (strcmp(operations[i].id, operations[j].id) == 0) return YAP_V2_DUPLICATE;
  return YAP_V2_OK;
}

static void free_chunks(OPERATION_CHUNKS *chunks, size_t count) {
  size_t i;
  if (chunks == NULL) return;
  for (i = 0U; i < count; i++) YAP_V2_chunk_sequence_free(&chunks[i].chunks);
  free(chunks);
}

int YAP_V2_update_apply(const char *index_dir, const YAP_V2_INGEST_OPERATION *operations,
                        size_t operation_count, YAP_V2_UPDATE_RESULT *result,
                        char *error, size_t error_size) {
  YAP_V2_CONFIG config; YAP_V2_MANIFEST manifest; YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_DOCUMENT_VIEW *documents = NULL; YAP_V2_PASSAGE_VIEW *passages = NULL;
  YAP_V2_BYTES_VIEW *tombstones = NULL; OPERATION_CHUNKS *chunks = NULL;
  YAP_EMBEDDING_RESULT embeddings; float *vector_values = NULL;
  char config_path[4096], manifest_path[4096], segments_path[4096], segment_template[4096];
  char path[4096], config_error[256]; const char *segment_id = NULL;
  size_t i, j, document_count = 0U, passage_count = 0U, tombstone_count = 0U;
  size_t document_index = 0U, passage_index = 0U, tombstone_index = 0U;
  uint64_t next_generation; int status = YAP_V2_OK, published = 0;
  YAP_V2_manifest_init(&manifest); YAP_Embedding_result_init(&embeddings);
  if (index_dir == NULL || operations == NULL || result == NULL || operation_count == 0U ||
      operation_count > YAP_V2_UPDATE_MAX_OPERATIONS) return YAP_V2_INVALID_ARGUMENT;
  memset(result, 0, sizeof(*result));
  if (join_path(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir, "manifest.json") != 0 ||
      join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0) {
    set_error(error, error_size, "index path is too long"); return YAP_V2_OUT_OF_RANGE;
  }
  status = validate_unique_ids(operations, operation_count);
  if (status != YAP_V2_OK) { set_error(error, error_size, "batch contains duplicate document IDs"); goto done; }
  status = YAP_V2_config_load(config_path, &config, config_error, sizeof(config_error));
  if (status != YAP_V2_OK) { set_error(error, error_size, config_error); goto done; }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &manifest);
  if (status == YAP_V2_OK) status = YAP_V2_manifest_verify_components(index_dir, &manifest);
  if (status != YAP_V2_OK) { set_error(error, error_size, "current index snapshot is invalid"); goto done; }
  if (manifest.generation == UINT64_MAX || manifest.segment_count >= YAP_V2_MAX_SEGMENTS) {
    status = YAP_V2_OUT_OF_RANGE; set_error(error, error_size, "index generation or segment limit reached"); goto done;
  }
  next_generation = manifest.generation + 1U;
  chunks = calloc(operation_count, sizeof(*chunks));
  if (chunks == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; i < operation_count; i++) {
    if (operations[i].kind == YAP_V2_INGEST_DELETE) { tombstone_count++; continue; }
    document_count++;
    status = YAP_V2_unicode_chunk(operations[i].id, operations[i].body, strlen(operations[i].body),
                                  config.chunk_max_chars, config.chunk_overlap_chars,
                                  &chunks[i].chunks);
    if (status != YAP_V2_OK) { set_error(error, error_size, "document chunking failed"); goto done; }
    if (chunks[i].chunks.chunk_count > SIZE_MAX - passage_count) { status = YAP_V2_OUT_OF_RANGE; goto done; }
    passage_count += chunks[i].chunks.chunk_count;
    if (config.vector_metric != YAP_V2_VECTOR_DISABLED &&
        (operations[i].vectors == NULL ||
         operations[i].vector_count != chunks[i].chunks.chunk_count ||
         operations[i].vector_dimensions != config.vector_dimensions)) {
      status = YAP_V2_INVALID_FORMAT;
      set_error(error, error_size, "vectors must match generated passage count and configured dimensions");
      goto done;
    }
    if (config.vector_metric == YAP_V2_VECTOR_DISABLED && operations[i].vectors != NULL) {
      status = YAP_V2_INVALID_FORMAT; set_error(error, error_size, "vectors are disabled by config"); goto done;
    }
  }
  documents = document_count == 0U ? NULL : calloc(document_count, sizeof(*documents));
  passages = passage_count == 0U ? NULL : calloc(passage_count, sizeof(*passages));
  tombstones = tombstone_count == 0U ? NULL : calloc(tombstone_count, sizeof(*tombstones));
  if ((document_count != 0U && documents == NULL) || (passage_count != 0U && passages == NULL) ||
      (tombstone_count != 0U && tombstones == NULL)) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  if (config.vector_metric != YAP_V2_VECTOR_DISABLED && passage_count != 0U) {
    if (passage_count > SIZE_MAX / config.vector_dimensions ||
        passage_count * config.vector_dimensions > SIZE_MAX / sizeof(float)) { status = YAP_V2_OUT_OF_RANGE; goto done; }
    vector_values = malloc(passage_count * config.vector_dimensions * sizeof(float));
    if (vector_values == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  }
  for (i = 0U; i < operation_count; i++) {
    const YAP_V2_INGEST_OPERATION *operation = &operations[i];
    if (operation->kind == YAP_V2_INGEST_DELETE) {
      tombstones[tombstone_index++] = view(operation->id); continue;
    }
    documents[document_index].id = view(operation->id); documents[document_index].url = view(operation->url);
    documents[document_index].title = view(operation->title); documents[document_index].body = view(operation->body);
    documents[document_index].metadata_json = view(operation->metadata_json);
    documents[document_index].updated_at_unix_ms = operation->updated_at_unix_ms;
    for (j = 0U; j < chunks[i].chunks.chunk_count; j++) {
      YAP_V2_CHUNK *chunk = &chunks[i].chunks.chunks[j];
      passages[passage_index].id = view(chunk->id); passages[passage_index].parent_document_id = view(operation->id);
      passages[passage_index].text = view(chunk->text); passages[passage_index].ordinal = chunk->ordinal;
      passages[passage_index].start_char = chunk->start_char; passages[passage_index].end_char = chunk->end_char;
      if (vector_values != NULL)
        memcpy(vector_values + passage_index * config.vector_dimensions,
               operation->vectors + j * config.vector_dimensions,
               config.vector_dimensions * sizeof(float));
      passage_index++;
    }
    document_index++;
  }
  if (mkdir(segments_path, 0700) != 0 && errno != EEXIST) { status = YAP_V2_IO_ERROR; goto done; }
  if (snprintf(segment_template, sizeof(segment_template), "%s/seg-%020llu-XXXXXX", segments_path,
               (unsigned long long)next_generation) < 0 || strlen(segment_template) >= sizeof(segment_template) - 1U ||
      mkdtemp(segment_template) == NULL) { status = YAP_V2_IO_ERROR; set_error(error, error_size, "cannot create segment directory"); goto done; }
  segment_id = strrchr(segment_template, '/'); segment_id = segment_id == NULL ? segment_template : segment_id + 1;
  if (join_path(path, sizeof(path), segment_template, "documents.yap2") != 0) { status = YAP_V2_OUT_OF_RANGE; goto done; }
  status = YAP_V2_segment_write(path, segment_id, next_generation, documents, document_count,
                                passages, passage_count, &descriptor);
  if (status != YAP_V2_OK) goto done;
  if (document_count != 0U) {
    YAP_V2_COMPONENT_DESCRIPTOR lexical[3], metadata;
    status = YAP_V2_lexical_write(segment_template, next_generation, documents, document_count,
                                  passages, passage_count, lexical);
    for (i = 0U; status == YAP_V2_OK && i < 3U; i++) status = add_component(&descriptor, &lexical[i]);
    if (status == YAP_V2_OK && join_path(path, sizeof(path), segment_template, "metadata.yap2") == 0)
      status = YAP_V2_metadata_write(path, next_generation, &config, documents, document_count, &metadata);
    else if (status == YAP_V2_OK) status = YAP_V2_OUT_OF_RANGE;
    if (status == YAP_V2_OK) status = add_component(&descriptor, &metadata);
  }
  if (status == YAP_V2_OK && vector_values != NULL) {
    YAP_V2_COMPONENT_DESCRIPTOR vectors, ann; YAP_V2_VECTOR_SEGMENT vector_segment;
    embeddings.values = vector_values; embeddings.input_count = passage_count; embeddings.dimensions = config.vector_dimensions;
    if (join_path(path, sizeof(path), segment_template, "vectors.yap2") != 0) status = YAP_V2_OUT_OF_RANGE;
    else status = YAP_V2_vectors_write(path, next_generation, &config, passages, passage_count, &embeddings, &vectors);
    if (status == YAP_V2_OK) status = add_component(&descriptor, &vectors);
    YAP_V2_vector_segment_init(&vector_segment);
    if (status == YAP_V2_OK) status = YAP_V2_vector_segment_open(path, next_generation, &config, &vector_segment, NULL);
    if (status == YAP_V2_OK && join_path(path, sizeof(path), segment_template, "vectors.usearch") == 0) {
      if (YAP_V2_ann_build_save(path, &vector_segment, 16U, 128U, 64U, &ann) != YAP_ANN_OK)
        status = YAP_V2_CONFLICT;
    } else if (status == YAP_V2_OK) status = YAP_V2_OUT_OF_RANGE;
    YAP_V2_vector_segment_close(&vector_segment);
    if (status == YAP_V2_OK) status = add_component(&descriptor, &ann);
  }
  if (status == YAP_V2_OK && tombstone_count != 0U) {
    YAP_V2_COMPONENT_DESCRIPTOR component;
    if (join_path(path, sizeof(path), segment_template, "tombstones.yap2") != 0) status = YAP_V2_OUT_OF_RANGE;
    else status = YAP_V2_tombstones_write(path, next_generation, tombstones, tombstone_count, &component);
    if (status == YAP_V2_OK) status = add_component(&descriptor, &component);
    descriptor.tombstone_count = tombstone_count;
  }
  if (status == YAP_V2_OK) status = YAP_V2_manifest_add_segment(&manifest, &descriptor);
  if (status == YAP_V2_OK) {
    manifest.generation = next_generation;
    status = YAP_V2_manifest_verify_components(index_dir, &manifest);
  }
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_publish_if_generation(manifest_path, next_generation - 1U, &manifest);
  if (status != YAP_V2_OK) { set_error(error, error_size, status == YAP_V2_CONFLICT ? "generation changed during update" : "segment publish failed"); goto done; }
  published = 1; result->generation = next_generation; result->accepted = operation_count;
  result->upserts = document_count; result->deletes = tombstone_count;
  (void)snprintf(result->segment_id, sizeof(result->segment_id), "%s", segment_id);
done:
  if (!published && segment_id != NULL) cleanup_segment_dir(segment_template);
  free(documents); free(passages); free(tombstones); free(vector_values);
  free_chunks(chunks, operation_count); YAP_V2_manifest_free(&manifest);
  return status;
}

static void free_operations(YAP_V2_INGEST_OPERATION *operations, size_t count) {
  size_t i; for (i = 0U; i < count; i++) YAP_V2_ingest_operation_free(&operations[i]); free(operations);
}

static int parse_lines(const unsigned char *input, size_t input_bytes,
                       YAP_V2_INGEST_OPERATION **operations_out, size_t *count_out,
                       char *error, size_t error_size) {
  YAP_V2_INGEST_OPERATION *operations = NULL; size_t count = 0U, offset = 0U;
  while (offset < input_bytes) {
    size_t end = offset; int status;
    while (end < input_bytes && input[end] != '\n') end++;
    if (end > offset && input[end - 1U] == '\r') end--;
    if (end == offset) { set_error(error, error_size, "empty NDJSON records are not allowed"); free_operations(operations, count); return YAP_V2_INVALID_FORMAT; }
    if (count == YAP_V2_UPDATE_MAX_OPERATIONS) { set_error(error, error_size, "batch exceeds 100 operations"); free_operations(operations, count); return YAP_V2_OUT_OF_RANGE; }
    {
      YAP_V2_INGEST_OPERATION *next = realloc(operations, (count + 1U) * sizeof(*operations));
      if (next == NULL) { free_operations(operations, count); return YAP_V2_ALLOCATION_FAILED; }
      operations = next;
    }
    status = YAP_V2_ingest_parse_ndjson((const char *)input + offset, end - offset, &operations[count], error, error_size);
    if (status != YAP_V2_OK) { free_operations(operations, count); return status; }
    count++; offset = end;
    if (offset < input_bytes && input[offset] == '\r') offset++;
    if (offset < input_bytes && input[offset] == '\n') offset++;
  }
  if (count == 0U) { free(operations); set_error(error, error_size, "batch must contain at least one operation"); return YAP_V2_INVALID_FORMAT; }
  *operations_out = operations; *count_out = count; return YAP_V2_OK;
}

int YAP_V2_update_ndjson(const char *index_dir, const unsigned char *input, size_t input_bytes,
                         YAP_V2_UPDATE_RESULT *result, char *error, size_t error_size) {
  YAP_V2_INGEST_OPERATION *operations = NULL; size_t count = 0U; int status;
  if (index_dir == NULL || input == NULL || result == NULL) return YAP_V2_INVALID_ARGUMENT;
  status = parse_lines(input, input_bytes, &operations, &count, error, error_size);
  if (status == YAP_V2_OK) status = YAP_V2_update_apply(index_dir, operations, count, result, error, error_size);
  free_operations(operations, count); return status;
}

int YAP_V2_update_json_batch(const char *index_dir, const unsigned char *input,
                             size_t input_bytes, YAP_V2_UPDATE_RESULT *result,
                             char *error, size_t error_size) {
  yyjson_doc *document; yyjson_val *root, *array, *item; yyjson_arr_iter iterator;
  YAP_V2_INGEST_OPERATION *operations = NULL; size_t count = 0U; int status = YAP_V2_INVALID_FORMAT;
  if (index_dir == NULL || input == NULL || result == NULL) return YAP_V2_INVALID_ARGUMENT;
  document = yyjson_read((const char *)input, input_bytes, YYJSON_READ_NOFLAG);
  if (document == NULL) { set_error(error, error_size, "invalid JSON"); return status; }
  root = yyjson_doc_get_root(document); array = yyjson_is_obj(root) ? yyjson_obj_get(root, "operations") : NULL;
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 1U || !yyjson_is_arr(array) ||
      (count = yyjson_arr_size(array)) == 0U || count > YAP_V2_UPDATE_MAX_OPERATIONS) {
    set_error(error, error_size, "request must contain 1 to 100 operations"); goto done;
  }
  operations = calloc(count, sizeof(*operations));
  if (operations == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  yyjson_arr_iter_init(array, &iterator); count = 0U;
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    char *json; size_t bytes;
    if (!yyjson_is_obj(item)) { set_error(error, error_size, "each operation must be an object"); goto done; }
    json = yyjson_val_write(item, YYJSON_WRITE_NOFLAG, &bytes);
    if (json == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
    status = YAP_V2_ingest_parse_ndjson(json, bytes, &operations[count], error, error_size); free(json);
    if (status != YAP_V2_OK) goto done;
    count++;
  }
  status = YAP_V2_update_apply(index_dir, operations, count, result, error, error_size);
done:
  free_operations(operations, count); yyjson_doc_free(document); return status;
}

static int read_file(const char *path, unsigned char **data_out, size_t *bytes_out) {
  FILE *file; long length; unsigned char *data;
  file = fopen(path, "rb"); if (file == NULL) return -1;
  if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0L || fseek(file, 0L, SEEK_SET) != 0) { fclose(file); return -1; }
  data = length == 0L ? malloc(1U) : malloc((size_t)length);
  if (data == NULL || (length > 0L && fread(data, 1U, (size_t)length, file) != (size_t)length) || fclose(file) != 0) { free(data); return -1; }
  *data_out = data; *bytes_out = (size_t)length; return 0;
}

int YAP_V2_update_main(int argc, char **argv) {
  const char *input_path = NULL, *index_dir = NULL; unsigned char *data = NULL; size_t bytes = 0U;
  YAP_V2_UPDATE_RESULT result; char error[256] = {0}; int i, status;
  for (i = 1; i < argc; i++) {
    const char **target;
    if (strcmp(argv[i], "--input") == 0) target = &input_path;
    else if (strcmp(argv[i], "--index") == 0) target = &index_dir;
    else { fprintf(stderr, "Unknown update option: %s\n", argv[i]); return EXIT_FAILURE; }
    if (++i >= argc) { fputs("Missing update option value\n", stderr); return EXIT_FAILURE; }
    *target = argv[i];
  }
  if (input_path == NULL || index_dir == NULL) {
    fputs("Usage: yappo_makeindex update --input operations.ndjson --index INDEX_DIR\n", stderr);
    return EXIT_FAILURE;
  }
  if (read_file(input_path, &data, &bytes) != 0) { perror("update input"); return EXIT_FAILURE; }
  status = YAP_V2_update_ndjson(index_dir, data, bytes, &result, error, sizeof(error)); free(data);
  if (status != YAP_V2_OK) { fprintf(stderr, "Update failed: %s (%s)\n", error, YAP_V2_status_string(status)); return EXIT_FAILURE; }
  printf("{\"generation\":%llu,\"accepted\":%zu,\"upserts\":%zu,\"deletes\":%zu,\"segment_id\":\"%s\"}\n",
         (unsigned long long)result.generation, result.accepted, result.upserts, result.deletes, result.segment_id);
  return EXIT_SUCCESS;
}
