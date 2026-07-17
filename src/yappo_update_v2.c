#include "yappo_update_v2.h"

#include "yappo_config_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_segment_planner_v2.h"
#include "yappo_unicode.h"
#include "yappo_writer_lock_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  YAP_V2_CHUNK_SEQUENCE chunks;
} OPERATION_CHUNKS;

static const char *testing_failpoint;

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

static int failpoint(const char *name) {
  return testing_failpoint != NULL && strcmp(testing_failpoint, name) == 0;
}

void YAP_V2_update_set_failpoint_for_testing(const char *name) {
  testing_failpoint = name;
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY), status;
  if (fd < 0) return YAP_V2_IO_ERROR;
  status = fsync(fd) == 0 ? YAP_V2_OK : YAP_V2_IO_ERROR;
  if (close(fd) != 0) status = YAP_V2_IO_ERROR;
  return status;
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

static int compare_ids(const void *left, const void *right) {
  const char *const *a = left;
  const char *const *b = right;
  return strcmp(*a, *b);
}

static int validate_unique_ids(const YAP_V2_INGEST_OPERATION *operations, size_t count) {
  const char **ids;
  size_t i;
  int status = YAP_V2_OK;
  if (count > SIZE_MAX / sizeof(*ids)) return YAP_V2_OUT_OF_RANGE;
  ids = malloc(count * sizeof(*ids));
  if (ids == NULL) return YAP_V2_ALLOCATION_FAILED;
  for (i = 0U; i < count; i++) ids[i] = operations[i].id;
  qsort(ids, count, sizeof(*ids), compare_ids);
  for (i = 1U; i < count; i++)
    if (strcmp(ids[i - 1U], ids[i]) == 0) {
      status = YAP_V2_DUPLICATE;
      break;
    }
  free(ids);
  return status;
}

static void free_chunks(OPERATION_CHUNKS *chunks, size_t count) {
  size_t i;
  if (chunks == NULL) return;
  for (i = 0U; i < count; i++) YAP_V2_chunk_sequence_free(&chunks[i].chunks);
  free(chunks);
}

void YAP_V2_update_result_init(YAP_V2_UPDATE_RESULT *result) {
  if (result != NULL) memset(result, 0, sizeof(*result));
}

void YAP_V2_update_result_free(YAP_V2_UPDATE_RESULT *result) {
  if (result == NULL) return;
  YAP_V2_segment_id_list_free(&result->segment_ids);
  memset(result, 0, sizeof(*result));
}

static int apply_operations(const char *index_dir,
                            const YAP_V2_INGEST_OPERATION *operations,
                            size_t operation_count, size_t operation_limit,
                            YAP_V2_UPDATE_RESULT *result,
                            char *error, size_t error_size) {
  YAP_V2_CONFIG config; YAP_V2_MANIFEST manifest;
  YAP_V2_DOCUMENT_VIEW *documents = NULL; YAP_V2_PASSAGE_VIEW *passages = NULL;
  YAP_V2_BYTES_VIEW *tombstones = NULL; OPERATION_CHUNKS *chunks = NULL;
  YAP_V2_SEGMENT_UNIT *units = NULL; YAP_V2_SEGMENT_DESCRIPTOR *descriptors = NULL;
  YAP_V2_SEGMENT_PLAN plan; YAP_V2_SEGMENT_CAPACITY_ERROR capacity_error;
  float *vector_values = NULL; char (*segment_paths)[4096] = NULL;
  char config_path[4096], manifest_path[4096], segments_path[4096];
  char config_error[256];
  size_t i, j, document_count = 0U, passage_count = 0U, tombstone_count = 0U;
  size_t document_index = 0U, passage_index = 0U, tombstone_index = 0U;
  size_t failed_slice = SIZE_MAX;
  uint64_t next_generation; int status = YAP_V2_OK, published = 0;
  YAP_V2_WRITER_LOCK writer_lock;
  YAP_V2_manifest_init(&manifest); YAP_V2_segment_plan_init(&plan);
  YAP_V2_writer_lock_init(&writer_lock);
  if (index_dir == NULL || operations == NULL || result == NULL || operation_count == 0U ||
      operation_count > operation_limit) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_update_result_init(result);
  if (join_path(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir, "manifest.json") != 0 ||
      join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0) {
    set_error(error, error_size, "index path is too long"); return YAP_V2_OUT_OF_RANGE;
  }
  status = YAP_V2_writer_lock_acquire(&writer_lock, index_dir);
  if (status != YAP_V2_OK) { set_error(error, error_size, "cannot acquire index writer lock"); goto done; }
  status = validate_unique_ids(operations, operation_count);
  if (status != YAP_V2_OK) { set_error(error, error_size, "batch contains duplicate document IDs"); goto done; }
  status = YAP_V2_config_load(config_path, &config, config_error, sizeof(config_error));
  if (status != YAP_V2_OK) { set_error(error, error_size, config_error); goto done; }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &manifest);
  if (status != YAP_V2_OK) { set_error(error, error_size, "current index snapshot is invalid"); goto done; }
  if (manifest.generation == UINT64_MAX ||
      YAP_V2_segment_count_validate(manifest.segment_count, 1U) != YAP_V2_OK) {
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
  units = calloc(operation_count, sizeof(*units));
  if ((document_count != 0U && documents == NULL) || (passage_count != 0U && passages == NULL) ||
      (tombstone_count != 0U && tombstones == NULL) || units == NULL) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  if (config.vector_metric != YAP_V2_VECTOR_DISABLED && passage_count != 0U) {
    if (passage_count > SIZE_MAX / config.vector_dimensions ||
        passage_count * config.vector_dimensions > SIZE_MAX / sizeof(float)) { status = YAP_V2_OUT_OF_RANGE; goto done; }
    vector_values = malloc(passage_count * config.vector_dimensions * sizeof(float));
    if (vector_values == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  }
  for (i = 0U; i < operation_count; i++) {
    const YAP_V2_INGEST_OPERATION *operation = &operations[i];
    if (operation->kind == YAP_V2_INGEST_DELETE) {
      tombstones[tombstone_index] = view(operation->id);
      units[i].tombstone = tombstones[tombstone_index++];
      continue;
    }
    units[i].document = &documents[document_index];
    units[i].passages = &passages[passage_index];
    units[i].passage_count = chunks[i].chunks.chunk_count;
    units[i].vectors = vector_values == NULL ? NULL :
      vector_values + passage_index * config.vector_dimensions;
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
  status = YAP_V2_segment_plan(&config, units, operation_count, 31U,
                               YAP_V2_segment_planner_payload_limit(), &plan, &capacity_error);
  if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED) {
    (void)snprintf(error, error_size,
      "document '%.*s' requires %zu bytes in %s (limit %zu)",
      (int)capacity_error.document_id.len, capacity_error.document_id.data,
      capacity_error.required_bytes, capacity_error.component, capacity_error.limit_bytes);
    goto done;
  }
  if (status != YAP_V2_OK) { set_error(error, error_size, "segment planning failed"); goto done; }
write_segments:
  failed_slice = SIZE_MAX;
  if (YAP_V2_segment_count_validate(manifest.segment_count, plan.count) != YAP_V2_OK) {
    status = YAP_V2_OUT_OF_RANGE; set_error(error, error_size, "index segment limit reached"); goto done;
  }
  descriptors = calloc(plan.count, sizeof(*descriptors));
  segment_paths = calloc(plan.count, sizeof(*segment_paths));
  if (descriptors == NULL || segment_paths == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++) {
    const char *segment_id;
    if (snprintf(segment_paths[i], sizeof(segment_paths[i]), "%s/seg-%020llu-XXXXXX",
                 segments_path, (unsigned long long)next_generation) < 0 ||
        mkdtemp(segment_paths[i]) == NULL) {
      status = YAP_V2_IO_ERROR; set_error(error, error_size, "cannot create segment directory"); break;
    }
    segment_id = strrchr(segment_paths[i], '/');
    segment_id = segment_id == NULL ? segment_paths[i] : segment_id + 1;
    status = YAP_V2_segment_slice_write(segment_paths[i], segment_id, next_generation, &config,
                                        units, &plan, plan.slices[i], &descriptors[i]);
    if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED) failed_slice = i;
    if (status == YAP_V2_OK) status = YAP_V2_segment_id_list_add(&result->segment_ids, segment_id);
    if (status == YAP_V2_OK && i == 0U && failpoint("after_first_segment")) {
      status = YAP_V2_IO_ERROR;
      set_error(error, error_size, "injected failure after first segment");
    }
  }
  if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED && failed_slice < plan.count &&
      plan.slices[failed_slice].count > 1U) {
    for (i = 0U; i < plan.count; i++)
      if (segment_paths[i][0] != '\0') cleanup_segment_dir(segment_paths[i]);
    free(descriptors); free(segment_paths); descriptors = NULL; segment_paths = NULL;
    YAP_V2_segment_id_list_free(&result->segment_ids);
    YAP_V2_segment_id_list_init(&result->segment_ids);
    status = YAP_V2_segment_plan_bisect(&plan, failed_slice);
    if (status == YAP_V2_OK) goto write_segments;
  }
  if (status != YAP_V2_OK) {
    if (error != NULL && error_size > 0U && error[0] == '\0')
      set_error(error, error_size, "segment creation failed");
    goto done;
  }
  if (status == YAP_V2_OK) status = sync_directory(segments_path);
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++) {
    status = YAP_V2_manifest_verify_segment_components(
      index_dir, next_generation, &descriptors[i]);
  }
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++)
    status = YAP_V2_manifest_add_segment(&manifest, &descriptors[i]);
  if (status == YAP_V2_OK) {
    manifest.generation = next_generation;
  }
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_publish_if_generation(manifest_path, next_generation - 1U, &manifest);
  if (status != YAP_V2_OK) {
    set_error(error, error_size,
              status == YAP_V2_CONFLICT ? "generation changed during update" :
                                          "segment publish failed");
    goto done;
  }
  published = 1; result->generation = next_generation; result->accepted = operation_count;
  result->upserts = document_count; result->deletes = tombstone_count;
done:
  if (!published && segment_paths != NULL)
    for (i = 0U; i < plan.count; i++)
      if (segment_paths[i][0] != '\0') cleanup_segment_dir(segment_paths[i]);
  if (status != YAP_V2_OK) YAP_V2_update_result_free(result);
  free(documents); free(passages); free(tombstones); free(vector_values);
  free(units); free(descriptors); free(segment_paths); YAP_V2_segment_plan_free(&plan);
  free_chunks(chunks, operation_count); YAP_V2_manifest_free(&manifest);
  YAP_V2_writer_lock_release(&writer_lock);
  return status;
}

int YAP_V2_update_apply(const char *index_dir, const YAP_V2_INGEST_OPERATION *operations,
                        size_t operation_count, YAP_V2_UPDATE_RESULT *result,
                        char *error, size_t error_size) {
  return apply_operations(index_dir, operations, operation_count,
                          YAP_V2_UPDATE_MAX_OPERATIONS, result, error, error_size);
}

int YAP_V2_build_apply(const char *index_dir, const YAP_V2_INGEST_OPERATION *operations,
                       size_t operation_count, YAP_V2_UPDATE_RESULT *result,
                       char *error, size_t error_size) {
  return apply_operations(index_dir, operations, operation_count,
                          YAP_V2_BUILD_BATCH_OPERATIONS, result, error, error_size);
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
  YAP_V2_update_result_init(&result);
  status = YAP_V2_update_ndjson(index_dir, data, bytes, &result, error, sizeof(error)); free(data);
  if (status != YAP_V2_OK) { fprintf(stderr, "Update failed: %s (%s)\n", error, YAP_V2_status_string(status)); return EXIT_FAILURE; }
  printf("{\"generation\":%llu,\"accepted\":%zu,\"upserts\":%zu,\"deletes\":%zu,\"segment_ids\":[",
         (unsigned long long)result.generation, result.accepted, result.upserts, result.deletes);
  for (i = 0; i < (int)result.segment_ids.count; i++)
    printf("%s\"%s\"", i == 0 ? "" : ",", result.segment_ids.items[i]);
  puts("]}");
  YAP_V2_update_result_free(&result);
  return EXIT_SUCCESS;
}
