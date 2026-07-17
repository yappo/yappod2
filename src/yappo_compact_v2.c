#include "yappo_compact_v2.h"

#include "yappo_ann_v2.h"
#include "yappo_config_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_observability_v2.h"
#include "yappo_segment_planner_v2.h"
#include "yappo_snapshot_v2.h"
#include "yappo_vector_v2.h"
#include "yappo_writer_lock_v2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len && left.data != NULL && right.data != NULL &&
         memcmp(left.data, right.data, left.len) == 0;
}

static int segment_referenced(const YAP_V2_MANIFEST *manifest, const char *id) {
  size_t i;
  for (i = 0U; i < manifest->segment_count; i++)
    if (strcmp(manifest->segments[i].id, id) == 0) return 1;
  return 0;
}

static int remove_segment_directory(const char *path) {
  DIR *directory = opendir(path); struct dirent *entry; int status = YAP_V2_OK;
  if (directory == NULL) return errno == ENOENT ? YAP_V2_OK : YAP_V2_IO_ERROR;
  while ((entry = readdir(directory)) != NULL) {
    char child[4096]; struct stat info;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (join_path(child, sizeof(child), path, entry->d_name) != 0 || lstat(child, &info) != 0 ||
        S_ISDIR(info.st_mode) || unlink(child) != 0) { status = YAP_V2_IO_ERROR; break; }
  }
  if (closedir(directory) != 0) status = YAP_V2_IO_ERROR;
  if (status == YAP_V2_OK && rmdir(path) != 0 && errno != ENOENT) status = YAP_V2_IO_ERROR;
  return status;
}

int YAP_V2_compact_gc(const char *index_dir, const YAP_V2_MANIFEST *manifest,
                      size_t *removed_segments) {
  char segments_path[4096], candidate[4096]; DIR *directory; struct dirent *entry;
  size_t removed = 0U; int status = YAP_V2_OK;
  if (index_dir == NULL || manifest == NULL || removed_segments == NULL ||
      YAP_V2_manifest_validate(manifest) != YAP_V2_OK) return YAP_V2_INVALID_ARGUMENT;
  *removed_segments = 0U;
  if (join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0)
    return YAP_V2_OUT_OF_RANGE;
  directory = opendir(segments_path);
  if (directory == NULL) return errno == ENOENT ? YAP_V2_OK : YAP_V2_IO_ERROR;
  while ((entry = readdir(directory)) != NULL) {
    struct stat info;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        segment_referenced(manifest, entry->d_name)) continue;
    if (join_path(candidate, sizeof(candidate), segments_path, entry->d_name) != 0) {
      status = YAP_V2_OUT_OF_RANGE; break;
    }
    if (lstat(candidate, &info) != 0) { if (errno == ENOENT) continue; status = YAP_V2_IO_ERROR; break; }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) continue;
    status = remove_segment_directory(candidate);
    if (status != YAP_V2_OK) break;
    removed++;
  }
  if (closedir(directory) != 0 && status == YAP_V2_OK) status = YAP_V2_IO_ERROR;
  if (status == YAP_V2_OK) *removed_segments = removed;
  return status;
}

static const char *testing_failpoint;

void YAP_V2_compaction_set_failpoint_for_testing(const char *name) {
  testing_failpoint = name;
}

static int failpoint(const char *name) {
  if (testing_failpoint != NULL && strcmp(testing_failpoint, name) == 0) _exit(86);
  return YAP_V2_OK;
}

static int sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY), status;
  if (fd < 0) return YAP_V2_IO_ERROR;
  status = fsync(fd) == 0 ? YAP_V2_OK : YAP_V2_IO_ERROR;
  if (close(fd) != 0) status = YAP_V2_IO_ERROR;
  return status;
}

static int collect_live(const char *index_dir, const YAP_V2_CONFIG *config,
                        const YAP_V2_MANIFEST *manifest, YAP_V2_SEARCH_SNAPSHOT *snapshot,
                        YAP_V2_DOCUMENT_VIEW **documents_out, size_t *document_count_out,
                        YAP_V2_PASSAGE_VIEW **passages_out, size_t *passage_count_out,
                        float **vectors_out) {
  YAP_V2_DOCUMENT_VIEW *documents = NULL; YAP_V2_PASSAGE_VIEW *passages = NULL;
  YAP_V2_VECTOR_SEGMENT *vectors = NULL; float *values = NULL;
  size_t document_count = 0U, passage_count = 0U, i, j, document_index = 0U, passage_index = 0U;
  int status = YAP_V2_OK;
  for (i = 0U; i < YAP_V2_snapshot_segment_count(snapshot); i++) {
    const YAP_V2_SEGMENT *segment = YAP_V2_snapshot_segment_documents(snapshot, i);
    for (j = 0U; j < segment->document_count; j++)
      if (YAP_V2_snapshot_document_visible(snapshot, i, segment->documents[j].id)) document_count++;
    for (j = 0U; j < segment->passage_count; j++)
      if (YAP_V2_snapshot_document_visible(snapshot, i, segment->passages[j].parent_document_id)) passage_count++;
  }
  documents = document_count == 0U ? NULL : calloc(document_count, sizeof(*documents));
  passages = passage_count == 0U ? NULL : calloc(passage_count, sizeof(*passages));
  if ((document_count != 0U && documents == NULL) || (passage_count != 0U && passages == NULL)) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  if (config->vector_metric != YAP_V2_VECTOR_DISABLED && passage_count != 0U) {
    if (passage_count > SIZE_MAX / config->vector_dimensions ||
        passage_count * config->vector_dimensions > SIZE_MAX / sizeof(float)) {
      status = YAP_V2_OUT_OF_RANGE; goto done;
    }
    values = malloc(passage_count * config->vector_dimensions * sizeof(float));
    vectors = calloc(manifest->segment_count, sizeof(*vectors));
    if (values == NULL || vectors == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
    for (i = 0U; i < manifest->segment_count; i++) {
      char directory[4096], path[4096];
      YAP_V2_vector_segment_init(&vectors[i]);
      if (manifest->segments[i].passage_count == 0U) continue;
      if (snprintf(directory, sizeof(directory), "%s/segments/%s", index_dir,
                   manifest->segments[i].id) < 0 ||
          join_path(path, sizeof(path), directory, "vectors.yap2") != 0) {
        status = YAP_V2_OUT_OF_RANGE; goto done;
      }
      status = YAP_V2_vector_segment_open(path, 0U, config, &vectors[i], NULL);
      if (status != YAP_V2_OK) goto done;
    }
  }
  for (i = 0U; i < YAP_V2_snapshot_segment_count(snapshot); i++) {
    const YAP_V2_SEGMENT *segment = YAP_V2_snapshot_segment_documents(snapshot, i);
    for (j = 0U; j < segment->document_count; j++)
      if (YAP_V2_snapshot_document_visible(snapshot, i, segment->documents[j].id))
        documents[document_index++] = segment->documents[j];
    for (j = 0U; j < segment->passage_count; j++) {
      if (!YAP_V2_snapshot_document_visible(snapshot, i, segment->passages[j].parent_document_id)) continue;
      passages[passage_index] = segment->passages[j];
      if (values != NULL) {
        if (j >= vectors[i].entry_count || !bytes_equal(vectors[i].entries[j].id, segment->passages[j].id)) {
          status = YAP_V2_CONFLICT; goto done;
        }
        memcpy(values + passage_index * config->vector_dimensions, vectors[i].entries[j].values,
               config->vector_dimensions * sizeof(float));
      }
      passage_index++;
    }
  }
  *documents_out = documents; *document_count_out = document_count;
  *passages_out = passages; *passage_count_out = passage_count; *vectors_out = values;
  documents = NULL; passages = NULL; values = NULL;
done:
  if (vectors != NULL) {
    for (i = 0U; i < manifest->segment_count; i++) YAP_V2_vector_segment_close(&vectors[i]);
  }
  free(vectors); free(documents); free(passages); free(values); return status;
}

void YAP_V2_compaction_result_init(YAP_V2_COMPACTION_RESULT *result) {
  if (result != NULL) memset(result, 0, sizeof(*result));
}

void YAP_V2_compaction_result_free(YAP_V2_COMPACTION_RESULT *result) {
  if (result == NULL) return;
  YAP_V2_segment_id_list_free(&result->segment_ids);
  memset(result, 0, sizeof(*result));
}

int YAP_V2_compact(const char *index_dir, YAP_V2_COMPACTION_RESULT *result,
                   char *error, size_t error_size) {
  YAP_V2_CONFIG config; YAP_V2_MANIFEST manifest, candidate; YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot = NULL;
  YAP_V2_DOCUMENT_VIEW *documents = NULL; YAP_V2_PASSAGE_VIEW *passages = NULL; float *vectors = NULL;
  YAP_V2_SEGMENT_UNIT *units = NULL; YAP_V2_SEGMENT_DESCRIPTOR *descriptors = NULL;
  YAP_V2_SEGMENT_PLAN plan; YAP_V2_SEGMENT_CAPACITY_ERROR capacity_error;
  char (*segment_paths)[4096] = NULL;
  char config_path[4096], manifest_path[4096], segments_path[4096];
  char config_error[256]; size_t documents_count = 0U, passages_count = 0U;
  size_t removed_before = 0U, removed_after = 0U, i, passage_index = 0U;
  size_t failed_slice = SIZE_MAX;
  uint64_t next_generation; int status = YAP_V2_OK, published = 0, status_started = 0;
  YAP_V2_WRITER_LOCK writer_lock;
  YAP_V2_manifest_init(&manifest); YAP_V2_manifest_init(&candidate); YAP_V2_snapshot_manager_init(&manager);
  YAP_V2_segment_plan_init(&plan);
  YAP_V2_writer_lock_init(&writer_lock);
  if (index_dir == NULL || result == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_compaction_result_init(result);
  if (join_path(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir, "manifest.json") != 0 ||
      join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0) return YAP_V2_OUT_OF_RANGE;
  status = YAP_V2_writer_lock_acquire(&writer_lock, index_dir);
  if (status != YAP_V2_OK) { set_error(error, error_size, "cannot acquire index writer lock"); goto done; }
  status_started = 1;
  (void)YAP_V2_compaction_status_write(index_dir, YAP_V2_COMPACTION_RUNNING, 0U);
  status = YAP_V2_config_load(config_path, &config, config_error, sizeof(config_error));
  if (status != YAP_V2_OK) { set_error(error, error_size, config_error); goto done; }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &manifest);
  if (status == YAP_V2_OK) status = YAP_V2_manifest_verify_components(index_dir, &manifest);
  if (status != YAP_V2_OK) { set_error(error, error_size, "current index snapshot is invalid"); goto done; }
  status = YAP_V2_compact_gc(index_dir, &manifest, &removed_before);
  if (status != YAP_V2_OK) { set_error(error, error_size, "orphan segment cleanup failed"); goto done; }
  if (manifest.generation == UINT64_MAX) { status = YAP_V2_OUT_OF_RANGE; goto done; }
  next_generation = manifest.generation + 1U;
  status = YAP_V2_snapshot_manager_open(&manager, index_dir, manifest_path, &config);
  if (status != YAP_V2_OK) goto done;
  snapshot = YAP_V2_snapshot_acquire(&manager);
  if (snapshot == NULL) { status = YAP_V2_IO_ERROR; goto done; }
  status = collect_live(index_dir, &config, &manifest, snapshot, &documents, &documents_count,
                        &passages, &passages_count, &vectors);
  if (status != YAP_V2_OK) { set_error(error, error_size, "cannot collect live records"); goto done; }
  units = documents_count == 0U ? NULL : calloc(documents_count, sizeof(*units));
  if (documents_count > 0U && units == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; i < documents_count; i++) {
    size_t start = passage_index;
    while (passage_index < passages_count &&
           bytes_equal(passages[passage_index].parent_document_id, documents[i].id))
      passage_index++;
    units[i].document = &documents[i];
    units[i].passages = passages + start;
    units[i].passage_count = passage_index - start;
    units[i].vectors = vectors == NULL ? NULL : vectors + start * config.vector_dimensions;
  }
  if (passage_index != passages_count) { status = YAP_V2_CONFLICT; set_error(error, error_size, "passages are not grouped with their parent documents"); goto done; }
  status = YAP_V2_segment_plan(&config, units, documents_count, 35U,
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
  if (YAP_V2_segment_count_validate(0U, plan.count) != YAP_V2_OK) {
    status = YAP_V2_OUT_OF_RANGE; set_error(error, error_size, "index segment limit reached"); goto done;
  }
  descriptors = calloc(plan.count, sizeof(*descriptors));
  segment_paths = calloc(plan.count, sizeof(*segment_paths));
  if (descriptors == NULL || segment_paths == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++) {
    const char *segment_id;
    if (snprintf(segment_paths[i], sizeof(segment_paths[i]), "%s/compact-%020llu-XXXXXX",
                 segments_path, (unsigned long long)next_generation) < 0 ||
        mkdtemp(segment_paths[i]) == NULL) { status = YAP_V2_IO_ERROR; break; }
    segment_id = strrchr(segment_paths[i], '/');
    segment_id = segment_id == NULL ? segment_paths[i] : segment_id + 1;
    status = YAP_V2_segment_slice_write(segment_paths[i], segment_id, next_generation, &config,
                                        units, &plan, plan.slices[i], &descriptors[i]);
    if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED) failed_slice = i;
    if (status == YAP_V2_OK) status = YAP_V2_segment_id_list_add(&result->segment_ids, segment_id);
  }
  if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED && failed_slice < plan.count &&
      plan.slices[failed_slice].count > 1U) {
    for (i = 0U; i < plan.count; i++)
      if (segment_paths[i][0] != '\0') (void)remove_segment_directory(segment_paths[i]);
    free(descriptors); free(segment_paths); descriptors = NULL; segment_paths = NULL;
    YAP_V2_segment_id_list_free(&result->segment_ids);
    YAP_V2_segment_id_list_init(&result->segment_ids);
    status = YAP_V2_segment_plan_bisect(&plan, failed_slice);
    if (status == YAP_V2_OK) goto write_segments;
  }
  if (status == YAP_V2_OK) status = sync_directory(segments_path);
  if (status != YAP_V2_OK) { set_error(error, error_size, "compacted segment creation failed"); goto done; }
  (void)failpoint("before_publish");
  candidate.generation = next_generation; candidate.format_version = manifest.format_version;
  memcpy(candidate.config_fingerprint, manifest.config_fingerprint, sizeof(candidate.config_fingerprint));
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++)
    status = YAP_V2_manifest_add_segment(&candidate, &descriptors[i]);
  if (status == YAP_V2_OK) status = YAP_V2_manifest_verify_components(index_dir, &candidate);
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_publish_if_generation(manifest_path, next_generation - 1U, &candidate);
  if (status != YAP_V2_OK) { set_error(error, error_size, "compaction publish failed"); goto done; }
  published = 1; (void)failpoint("after_publish");
  status = YAP_V2_compact_gc(index_dir, &candidate, &removed_after);
  if (status != YAP_V2_OK) { set_error(error, error_size, "obsolete segment cleanup failed"); goto done; }
  result->generation = next_generation; result->documents = documents_count;
  result->passages = passages_count; result->removed_segments = removed_before + removed_after;
done:
  if (status_started)
    (void)YAP_V2_compaction_status_write(index_dir,
      status == YAP_V2_OK ? YAP_V2_COMPACTION_SUCCEEDED : YAP_V2_COMPACTION_FAILED,
      status == YAP_V2_OK ? result->generation : manifest.generation);
  if (!published && segment_paths != NULL)
    for (i = 0U; i < plan.count; i++)
      if (segment_paths[i][0] != '\0') (void)remove_segment_directory(segment_paths[i]);
  if (status != YAP_V2_OK) YAP_V2_compaction_result_free(result);
  free(documents); free(passages); free(vectors); YAP_V2_snapshot_release(snapshot);
  free(units); free(descriptors); free(segment_paths); YAP_V2_segment_plan_free(&plan);
  YAP_V2_snapshot_manager_close(&manager); YAP_V2_manifest_free(&candidate); YAP_V2_manifest_free(&manifest);
  YAP_V2_writer_lock_release(&writer_lock);
  return status;
}

int YAP_V2_compact_main(int argc, char **argv) {
  const char *index_dir = NULL; YAP_V2_COMPACTION_RESULT result; char error[256] = {0}; int i, status;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--index") != 0) { fprintf(stderr, "Unknown compact option: %s\n", argv[i]); return EXIT_FAILURE; }
    if (++i >= argc) { fputs("Missing --index value\n", stderr); return EXIT_FAILURE; }
    index_dir = argv[i];
  }
  if (index_dir == NULL) { fputs("Usage: yappo_compact --index INDEX_DIR\n", stderr); return EXIT_FAILURE; }
  YAP_V2_compaction_result_init(&result);
  status = YAP_V2_compact(index_dir, &result, error, sizeof(error));
  if (status != YAP_V2_OK) {
    fprintf(stderr, "Compaction failed: %s (%s)\n", error, YAP_V2_status_string(status)); return EXIT_FAILURE;
  }
  printf("{\"generation\":%llu,\"documents\":%zu,\"passages\":%zu,"
         "\"removed_segments\":%zu,\"segment_ids\":[",
         (unsigned long long)result.generation, result.documents, result.passages,
         result.removed_segments);
  for (i = 0; i < (int)result.segment_ids.count; i++)
    printf("%s\"%s\"", i == 0 ? "" : ",", result.segment_ids.items[i]);
  puts("]}");
  YAP_V2_compaction_result_free(&result);
  return EXIT_SUCCESS;
}
