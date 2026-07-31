#include "indexing/yappo_compact_v2.h"
#include "config/yappo_application_config.h"

#include "storage/yappo_compaction_status_v2.h"
#include "config/yappo_config_v2.h"
#include "storage/yappo_manifest_v2.h"
#include "indexing/yappo_segment_planner_v2.h"
#include "components/yappo_vector_v2.h"
#include "storage/yappo_writer_lock_v2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define YAP_V2_COMPACTION_MAX_SOURCE_SEGMENTS 8U
#define YAP_V2_COMPACTION_MAX_SOURCE_BYTES (512U * 1024U * 1024U)

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
  YAP_V2_MANIFEST_SEGMENT_MAP referenced;
  size_t removed = 0U;
  int status = YAP_V2_OK;
  YAP_V2_manifest_segment_map_init(&referenced);
  if (index_dir == NULL || manifest == NULL || removed_segments == NULL ||
      YAP_V2_manifest_validate(manifest) != YAP_V2_OK) return YAP_V2_INVALID_ARGUMENT;
  *removed_segments = 0U;
  status = YAP_V2_manifest_segment_map_build(&referenced, manifest);
  if (status != YAP_V2_OK) return status;
  if (join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0)
    status = YAP_V2_OUT_OF_RANGE;
  if (status != YAP_V2_OK) goto done;
  directory = opendir(segments_path);
  if (directory == NULL) {
    status = errno == ENOENT ? YAP_V2_OK : YAP_V2_IO_ERROR;
    goto done;
  }
  while ((entry = readdir(directory)) != NULL) {
    struct stat info;
    size_t segment_index;
    int found;
    if (strcmp(entry->d_name, ".") == 0 ||
        strcmp(entry->d_name, "..") == 0)
      continue;
    found = YAP_V2_manifest_segment_map_find(
      &referenced, entry->d_name, &segment_index);
    if (found == YAP_V2_OK) continue;
    if (found != YAP_V2_NOT_FOUND) { status = found; break; }
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
done:
  YAP_V2_manifest_segment_map_free(&referenced);
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

typedef struct {
  YAP_V2_SEGMENT documents;
  YAP_V2_TOMBSTONES tombstones;
  YAP_V2_VECTOR_SEGMENT vectors;
} COMPACTION_SOURCE;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  size_t source;
  size_t ordinal;
  int document;
  int occupied;
} COMPACTION_EVENT;

typedef struct {
  COMPACTION_EVENT *slots;
  size_t capacity;
} COMPACTION_EVENT_MAP;

typedef struct {
  COMPACTION_SOURCE *sources;
  size_t source_count;
  YAP_V2_SEGMENT_UNIT *units;
  size_t unit_count;
  size_t document_count;
  size_t passage_count;
  float *vector_values;
} COMPACTION_INPUT;

static YAP_V2_COMPACTION_TEST_HOOK testing_hook;
static void *testing_hook_context;

void YAP_V2_compaction_set_hook_for_testing(
  YAP_V2_COMPACTION_TEST_HOOK hook, void *context) {
  testing_hook = hook;
  testing_hook_context = context;
}

static void call_testing_hook(const char *point) {
  if (testing_hook != NULL) testing_hook(point, testing_hook_context);
}

static const YAP_V2_COMPONENT_DESCRIPTOR *segment_component(
  const YAP_V2_SEGMENT_DESCRIPTOR *segment, uint32_t file_type) {
  size_t i;
  for (i = 0U; i < segment->component_count; i++)
    if (segment->components[i].file_type == file_type)
      return &segment->components[i];
  return NULL;
}

static size_t segment_storage_bytes(
  const YAP_V2_SEGMENT_DESCRIPTOR *segment) {
  size_t total = 0U;
  size_t i;
  for (i = 0U; i < segment->component_count; i++) {
    if (segment->components[i].file_bytes > SIZE_MAX)
      return SIZE_MAX;
    if (segment->components[i].file_bytes > SIZE_MAX - total)
      return SIZE_MAX;
    total += (size_t)segment->components[i].file_bytes;
  }
  return total;
}

static int select_range(const YAP_V2_MANIFEST *manifest, size_t *first,
                        size_t *count) {
  size_t best_first = 0U, best_count = 0U, best_bytes = SIZE_MAX;
  size_t start;
  if (manifest->segment_count == 0U) return YAP_V2_NOT_FOUND;
  for (start = 0U; start < manifest->segment_count; start++) {
    size_t total = 0U;
    size_t selected = 0U;
    while (start + selected < manifest->segment_count &&
           selected < YAP_V2_COMPACTION_MAX_SOURCE_SEGMENTS) {
      size_t bytes = segment_storage_bytes(
        &manifest->segments[start + selected]);
      if (selected > 0U &&
          (total >= YAP_V2_COMPACTION_MAX_SOURCE_BYTES ||
           bytes > YAP_V2_COMPACTION_MAX_SOURCE_BYTES - total))
        break;
      if (bytes > SIZE_MAX - total) break;
      total += bytes;
      selected++;
    }
    if (selected > best_count ||
        (selected == best_count && total < best_bytes)) {
      best_first = start;
      best_count = selected;
      best_bytes = total;
    }
  }
  if (best_count == 0U) {
    best_first = 0U;
    best_count = 1U;
  }
  *first = best_first;
  *count = best_count;
  return YAP_V2_OK;
}

static size_t event_hash(YAP_V2_BYTES_VIEW id) {
  size_t hash = (size_t)1469598103934665603ULL;
  size_t i;
  for (i = 0U; i < id.len; i++) {
    hash ^= id.data[i];
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static int event_map_create(COMPACTION_EVENT_MAP *map, size_t records) {
  size_t capacity = 1U;
  memset(map, 0, sizeof(*map));
  if (records == 0U) return YAP_V2_OK;
  if (records > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
  while (capacity < records * 2U) {
    if (capacity > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
    capacity *= 2U;
  }
  map->slots = calloc(capacity, sizeof(*map->slots));
  if (map->slots == NULL) return YAP_V2_ALLOCATION_FAILED;
  map->capacity = capacity;
  return YAP_V2_OK;
}

static COMPACTION_EVENT *event_map_slot(COMPACTION_EVENT_MAP *map,
                                        YAP_V2_BYTES_VIEW id) {
  size_t slot;
  if (map->capacity == 0U) return NULL;
  slot = event_hash(id) & (map->capacity - 1U);
  while (map->slots[slot].occupied &&
         !bytes_equal(map->slots[slot].id, id))
    slot = (slot + 1U) & (map->capacity - 1U);
  return &map->slots[slot];
}

static int event_is_latest(COMPACTION_EVENT_MAP *map, YAP_V2_BYTES_VIEW id,
                           size_t source, size_t ordinal, int document) {
  COMPACTION_EVENT *event = event_map_slot(map, id);
  return event != NULL && event->occupied && event->source == source &&
         event->ordinal == ordinal && event->document == document;
}

static void compaction_input_free(COMPACTION_INPUT *input) {
  size_t i;
  if (input == NULL) return;
  for (i = 0U; i < input->source_count; i++) {
    YAP_V2_vector_segment_close(&input->sources[i].vectors);
    YAP_V2_tombstones_free(&input->sources[i].tombstones);
    YAP_V2_segment_free(&input->sources[i].documents);
  }
  free(input->sources);
  free(input->units);
  free(input->vector_values);
  memset(input, 0, sizeof(*input));
}

static int load_source(const char *index_dir, const YAP_V2_CONFIG *config,
                       uint64_t manifest_generation,
                       const YAP_V2_SEGMENT_DESCRIPTOR *descriptor,
                       COMPACTION_SOURCE *source) {
  const YAP_V2_COMPONENT_DESCRIPTOR *documents;
  const YAP_V2_COMPONENT_DESCRIPTOR *tombstones;
  const YAP_V2_COMPONENT_DESCRIPTOR *vectors;
  char directory[4096], path[4096];
  int written;
  int status;
  YAP_V2_segment_init(&source->documents);
  YAP_V2_tombstones_init(&source->tombstones);
  YAP_V2_vector_segment_init(&source->vectors);
  status = YAP_V2_manifest_verify_segment_components(
    index_dir, manifest_generation, descriptor);
  if (status != YAP_V2_OK) return status;
  written = snprintf(directory, sizeof(directory), "%s/segments/%s",
                     index_dir, descriptor->id);
  if (written < 0 || (size_t)written >= sizeof(directory))
    return YAP_V2_OUT_OF_RANGE;
  documents = segment_component(descriptor, YAP_V2_FILE_DOCUMENTS);
  tombstones = segment_component(descriptor, YAP_V2_FILE_TOMBSTONES);
  vectors = segment_component(descriptor, YAP_V2_FILE_VECTORS);
  if (documents == NULL ||
      join_path(path, sizeof(path), directory, documents->name) != 0)
    return YAP_V2_INVALID_FORMAT;
  status = YAP_V2_segment_read(path, 0U, &source->documents, NULL);
  if (status == YAP_V2_OK &&
      (strcmp(source->documents.id, descriptor->id) != 0 ||
       source->documents.document_count != descriptor->document_count ||
       source->documents.passage_count != descriptor->passage_count))
    status = YAP_V2_CONFLICT;
  if (status == YAP_V2_OK && tombstones != NULL) {
    if (join_path(path, sizeof(path), directory, tombstones->name) != 0)
      status = YAP_V2_OUT_OF_RANGE;
    else
      status = YAP_V2_tombstones_read(path, 0U, &source->tombstones);
  }
  if (status == YAP_V2_OK &&
      source->tombstones.count != descriptor->tombstone_count)
    status = YAP_V2_CONFLICT;
  if (status == YAP_V2_OK && config->vector_metric != YAP_V2_VECTOR_DISABLED &&
      descriptor->passage_count > 0U) {
    if (vectors == NULL ||
        join_path(path, sizeof(path), directory, vectors->name) != 0)
      status = YAP_V2_INVALID_FORMAT;
    else
      status = YAP_V2_vector_segment_open(path, 0U, config,
                                          &source->vectors, NULL);
  }
  return status;
}

static int collect_range(const char *index_dir, const YAP_V2_CONFIG *config,
                         const YAP_V2_MANIFEST *manifest, size_t first,
                         size_t count, int drop_tombstones,
                         COMPACTION_INPUT *input) {
  COMPACTION_EVENT_MAP events;
  size_t records = 0U, i, j, unit_index = 0U, vector_index = 0U;
  int status = YAP_V2_OK;
  memset(input, 0, sizeof(*input));
  memset(&events, 0, sizeof(events));
  input->sources = calloc(count, sizeof(*input->sources));
  if (input->sources == NULL) return YAP_V2_ALLOCATION_FAILED;
  for (i = 0U; status == YAP_V2_OK && i < count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *descriptor =
      &manifest->segments[first + i];
    status = load_source(index_dir, config, manifest->generation,
                         descriptor, &input->sources[i]);
    input->source_count = i + 1U;
    if (descriptor->document_count > SIZE_MAX ||
        descriptor->tombstone_count > SIZE_MAX ||
        (size_t)descriptor->document_count > SIZE_MAX - records ||
        (size_t)descriptor->tombstone_count >
          SIZE_MAX - records - (size_t)descriptor->document_count)
      status = YAP_V2_OUT_OF_RANGE;
    else
      records += (size_t)descriptor->document_count +
                 (size_t)descriptor->tombstone_count;
  }
  if (status == YAP_V2_OK) status = event_map_create(&events, records);
  for (i = 0U; status == YAP_V2_OK && i < count; i++) {
    COMPACTION_SOURCE *source = &input->sources[i];
    for (j = 0U; j < source->tombstones.count; j++) {
      COMPACTION_EVENT *event = event_map_slot(
        &events, source->tombstones.document_ids[j]);
      if (event == NULL) { status = YAP_V2_CONFLICT; break; }
      event->id = source->tombstones.document_ids[j];
      event->source = i; event->ordinal = j;
      event->document = 0; event->occupied = 1;
    }
    for (j = 0U; status == YAP_V2_OK &&
                 j < source->documents.document_count; j++) {
      COMPACTION_EVENT *event = event_map_slot(
        &events, source->documents.documents[j].id);
      if (event == NULL) { status = YAP_V2_CONFLICT; break; }
      event->id = source->documents.documents[j].id;
      event->source = i; event->ordinal = j;
      event->document = 1; event->occupied = 1;
    }
  }
  for (i = 0U; status == YAP_V2_OK && i < events.capacity; i++) {
    if (!events.slots[i].occupied) continue;
    if (events.slots[i].document) input->document_count++;
    else if (!drop_tombstones) input->unit_count++;
  }
  if (input->document_count > SIZE_MAX - input->unit_count)
    status = YAP_V2_OUT_OF_RANGE;
  else
    input->unit_count += input->document_count;
  for (i = 0U; status == YAP_V2_OK && i < count; i++) {
    COMPACTION_SOURCE *source = &input->sources[i];
    size_t passage = 0U;
    for (j = 0U; j < source->documents.document_count; j++) {
      size_t start = passage;
      while (passage < source->documents.passage_count &&
             bytes_equal(source->documents.passages[passage].parent_document_id,
                         source->documents.documents[j].id))
        passage++;
      if (event_is_latest(&events, source->documents.documents[j].id,
                          i, j, 1)) {
        if (passage - start > SIZE_MAX - input->passage_count) {
          status = YAP_V2_OUT_OF_RANGE;
          break;
        }
        input->passage_count += passage - start;
      }
    }
    if (passage != source->documents.passage_count)
      status = YAP_V2_CONFLICT;
  }
  input->units = input->unit_count == 0U ? NULL :
                 calloc(input->unit_count, sizeof(*input->units));
  if (status == YAP_V2_OK && input->unit_count > 0U &&
      input->units == NULL)
    status = YAP_V2_ALLOCATION_FAILED;
  if (status == YAP_V2_OK &&
      config->vector_metric != YAP_V2_VECTOR_DISABLED &&
      input->passage_count > 0U) {
    if (input->passage_count > SIZE_MAX / config->vector_dimensions ||
        input->passage_count * config->vector_dimensions >
          SIZE_MAX / sizeof(*input->vector_values))
      status = YAP_V2_OUT_OF_RANGE;
    else {
      input->vector_values = malloc(input->passage_count *
                                    config->vector_dimensions *
                                    sizeof(*input->vector_values));
      if (input->vector_values == NULL) status = YAP_V2_ALLOCATION_FAILED;
    }
  }
  for (i = 0U; status == YAP_V2_OK && i < count; i++) {
    COMPACTION_SOURCE *source = &input->sources[i];
    size_t passage = 0U;
    if (!drop_tombstones)
      for (j = 0U; j < source->tombstones.count; j++)
        if (event_is_latest(&events, source->tombstones.document_ids[j],
                            i, j, 0))
          input->units[unit_index++].tombstone =
            source->tombstones.document_ids[j];
    for (j = 0U; status == YAP_V2_OK &&
                 j < source->documents.document_count; j++) {
      size_t start = passage;
      size_t k;
      while (passage < source->documents.passage_count &&
             bytes_equal(source->documents.passages[passage].parent_document_id,
                         source->documents.documents[j].id))
        passage++;
      if (!event_is_latest(&events, source->documents.documents[j].id,
                           i, j, 1))
        continue;
      input->units[unit_index].document =
        &source->documents.documents[j];
      input->units[unit_index].passages =
        source->documents.passages + start;
      input->units[unit_index].passage_count = passage - start;
      input->units[unit_index].vectors =
        input->vector_values == NULL ? NULL :
        input->vector_values +
          vector_index * config->vector_dimensions;
      for (k = start; input->vector_values != NULL && k < passage; k++) {
        if (k >= source->vectors.entry_count ||
            !bytes_equal(source->vectors.entries[k].id,
                         source->documents.passages[k].id)) {
          status = YAP_V2_CONFLICT;
          break;
        }
        memcpy(input->vector_values +
                 vector_index * config->vector_dimensions,
               source->vectors.entries[k].values,
               config->vector_dimensions * sizeof(float));
        vector_index++;
      }
      unit_index++;
    }
  }
  if (status == YAP_V2_OK &&
      (unit_index != input->unit_count ||
       (input->vector_values != NULL &&
        vector_index != input->passage_count)))
    status = YAP_V2_CONFLICT;
  free(events.slots);
  if (status != YAP_V2_OK) compaction_input_free(input);
  return status;
}

static int selected_range_unchanged(const YAP_V2_MANIFEST *base,
                                    const YAP_V2_MANIFEST *current,
                                    size_t first, size_t count) {
  size_t i;
  if (first > current->segment_count ||
      count > current->segment_count - first ||
      first > base->segment_count || count > base->segment_count - first)
    return 0;
  for (i = 0U; i < count; i++)
    if (!YAP_V2_segment_descriptor_equal(&base->segments[first + i],
                                         &current->segments[first + i]))
      return 0;
  return 1;
}

void YAP_V2_compaction_result_init(YAP_V2_COMPACTION_RESULT *result) {
  if (result != NULL) memset(result, 0, sizeof(*result));
}

void YAP_V2_compaction_result_free(YAP_V2_COMPACTION_RESULT *result) {
  if (result == NULL) return;
  YAP_V2_segment_id_list_free(&result->segment_ids);
  memset(result, 0, sizeof(*result));
}

int YAP_V2_manifest_needs_compaction(
    const YAP_V2_MANIFEST *manifest,
    const YAP_V2_COMPACTION_POLICY *policy, int *needed,
    size_t *small_segment_count) {
  size_t i, count = 0U, longest = 0U;
  if (manifest == NULL || policy == NULL || needed == NULL ||
      YAP_V2_compaction_policy_validate(policy) != YAP_V2_OK)
    return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < manifest->segment_count; i++) {
    size_t bytes = segment_storage_bytes(&manifest->segments[i]);
    if (bytes < policy->small_segment_bytes) {
      count++;
      if (count > longest) longest = count;
    } else {
      count = 0U;
    }
  }
  *needed = policy->enabled &&
    longest >= policy->min_small_segments;
  if (small_segment_count != NULL) *small_segment_count = longest;
  return YAP_V2_OK;
}

int YAP_V2_compaction_needed(
    const char *index_dir, const YAP_V2_COMPACTION_POLICY *policy,
    int *needed, size_t *small_segment_count, char *error,
    size_t error_size) {
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  char config_path[4096], manifest_path[4096], config_error[256];
  int status;
  if (index_dir == NULL || policy == NULL || needed == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  *needed = 0;
  if (small_segment_count != NULL) *small_segment_count = 0U;
  if (join_path(config_path, sizeof(config_path), index_dir,
                "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir,
                "manifest.json") != 0)
    return YAP_V2_OUT_OF_RANGE;
  YAP_V2_manifest_init(&manifest);
  status = YAP_V2_config_load(config_path, &config, config_error,
                              sizeof(config_error));
  if (status != YAP_V2_OK) {
    set_error(error, error_size, config_error);
    goto done;
  }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config,
                                           &manifest);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "current index manifest is invalid");
    goto done;
  }
  status = YAP_V2_manifest_needs_compaction(
    &manifest, policy, needed, small_segment_count);
  if (status != YAP_V2_OK)
    set_error(error, error_size, "automatic compaction policy is invalid");
done:
  YAP_V2_manifest_free(&manifest);
  return status;
}

static int compact_internal(
    const char *index_dir, const YAP_V2_COMPACTION_POLICY *policy,
    YAP_V2_COMPACTION_RESULT *result, int *compacted,
    size_t *small_segment_count, char *error, size_t error_size) {
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest, current, candidate;
  COMPACTION_INPUT input;
  YAP_V2_SEGMENT_DESCRIPTOR *descriptors = NULL;
  YAP_V2_SEGMENT_PLAN plan;
  YAP_V2_SEGMENT_CAPACITY_ERROR capacity_error;
  char (*segment_paths)[4096] = NULL;
  char config_path[4096], manifest_path[4096], segments_path[4096];
  char config_error[256];
  size_t range_first = 0U, range_count = 0U;
  size_t removed_before = 0U, removed_after = 0U, i;
  size_t failed_slice = SIZE_MAX;
  uint64_t output_generation = 0U;
  uint64_t status_generation = 0U;
  int status = YAP_V2_OK, published = 0, status_started = 0;
  int needed = 1;
  YAP_V2_WRITER_LOCK writer_lock, compaction_lock;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_manifest_init(&current);
  YAP_V2_manifest_init(&candidate);
  memset(&input, 0, sizeof(input));
  YAP_V2_segment_plan_init(&plan);
  YAP_V2_writer_lock_init(&writer_lock);
  YAP_V2_writer_lock_init(&compaction_lock);
  if (index_dir == NULL || result == NULL) return YAP_V2_INVALID_ARGUMENT;
  if (policy != NULL &&
      YAP_V2_compaction_policy_validate(policy) != YAP_V2_OK)
    return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_compaction_result_init(result);
  if (compacted != NULL) *compacted = 0;
  if (small_segment_count != NULL) *small_segment_count = 0U;
  if (join_path(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir, "manifest.json") != 0 ||
      join_path(segments_path, sizeof(segments_path), index_dir, "segments") != 0)
    return YAP_V2_OUT_OF_RANGE;
  status = YAP_V2_compaction_lock_acquire(&compaction_lock, index_dir);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "cannot acquire compaction lock");
    goto done;
  }
  status = YAP_V2_writer_lock_acquire(&writer_lock, index_dir);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "cannot acquire index writer lock");
    goto done;
  }
  status = YAP_V2_config_load(config_path, &config, config_error, sizeof(config_error));
  if (status != YAP_V2_OK) { set_error(error, error_size, config_error); goto done; }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &manifest);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "current index manifest is invalid");
    goto done;
  }
  if (policy != NULL) {
    status = YAP_V2_manifest_needs_compaction(
      &manifest, policy, &needed, small_segment_count);
    if (status != YAP_V2_OK) {
      set_error(error, error_size, "automatic compaction policy is invalid");
      goto done;
    }
    if (!needed) goto done;
  }
  if (compacted != NULL) *compacted = 1;
  status_started = 1;
  (void)YAP_V2_compaction_status_write(index_dir,
                                        YAP_V2_COMPACTION_RUNNING, 0U);
  status_generation = manifest.generation;
  status = YAP_V2_compact_gc(index_dir, &manifest, &removed_before);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "orphan segment cleanup failed");
    goto done;
  }
  status = select_range(&manifest, &range_first, &range_count);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "no segment is available for compaction");
    goto done;
  }
  if (manifest.generation == UINT64_MAX) {
    status = YAP_V2_OUT_OF_RANGE;
    goto done;
  }
  output_generation = manifest.generation + 1U;
  YAP_V2_writer_lock_release(&writer_lock);

  status = collect_range(index_dir, &config, &manifest, range_first,
                         range_count,
                         range_first == 0U &&
                         range_count == manifest.segment_count,
                         &input);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "cannot collect selected segment range");
    goto done;
  }
  status = YAP_V2_segment_plan_with_policy(
    &config, input.units, input.unit_count, 35U,
    YAP_V2_segment_planner_size_policy(), &plan, &capacity_error);
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
  descriptors = calloc(plan.count, sizeof(*descriptors));
  segment_paths = calloc(plan.count, sizeof(*segment_paths));
  if (descriptors == NULL || segment_paths == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++) {
    const char *segment_id;
    int written = snprintf(
      segment_paths[i], sizeof(segment_paths[i]),
      "%s/compact-%020llu-XXXXXX", segments_path,
      (unsigned long long)output_generation);
    if (written < 0 || (size_t)written >= sizeof(segment_paths[i]) ||
        mkdtemp(segment_paths[i]) == NULL) { status = YAP_V2_IO_ERROR; break; }
    segment_id = strrchr(segment_paths[i], '/');
    segment_id = segment_id == NULL ? segment_paths[i] : segment_id + 1;
    status = YAP_V2_segment_slice_write(
      segment_paths[i], segment_id, output_generation, &config,
      input.units, &plan, plan.slices[i], &descriptors[i]);
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
  call_testing_hook("before_publish_lock");
  (void)failpoint("before_publish");

  status = YAP_V2_writer_lock_acquire(&writer_lock, index_dir);
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "cannot reacquire index writer lock");
    goto done;
  }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &current);
  if (status != YAP_V2_OK ||
      !selected_range_unchanged(&manifest, &current, range_first,
                                range_count)) {
    status = YAP_V2_CONFLICT;
    set_error(error, error_size, "selected segment range changed");
    goto done;
  }
  if (current.generation == UINT64_MAX) {
    status = YAP_V2_OUT_OF_RANGE;
    goto done;
  }
  status_generation = current.generation;
  if (current.segment_count - range_count >
        YAP_V2_MAX_SEGMENTS - plan.count) {
    status = YAP_V2_OUT_OF_RANGE;
    set_error(error, error_size, "index segment limit reached");
    goto done;
  }
  candidate.generation = current.generation + 1U;
  candidate.format_version = current.format_version;
  memcpy(candidate.config_fingerprint, current.config_fingerprint,
         sizeof(candidate.config_fingerprint));
  for (i = 0U; status == YAP_V2_OK && i < range_first; i++)
    status = YAP_V2_manifest_add_segment(&candidate, &current.segments[i]);
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++)
    status = YAP_V2_manifest_add_segment(&candidate, &descriptors[i]);
  for (i = range_first + range_count;
       status == YAP_V2_OK && i < current.segment_count; i++)
    status = YAP_V2_manifest_add_segment(&candidate, &current.segments[i]);
  if (status == YAP_V2_OK) status = YAP_V2_manifest_validate(&candidate);
  for (i = 0U; status == YAP_V2_OK && i < plan.count; i++)
    status = YAP_V2_manifest_verify_segment_components(
      index_dir, candidate.generation, &descriptors[i]);
  if (status == YAP_V2_OK)
    status = YAP_V2_manifest_publish_if_generation(
      manifest_path, current.generation, &candidate);
  if (status != YAP_V2_OK) { set_error(error, error_size, "compaction publish failed"); goto done; }
  published = 1;
  status_generation = candidate.generation;
  (void)failpoint("after_publish");
  for (i = 0U; status == YAP_V2_OK && i < range_count; i++) {
    char obsolete[4096];
    int written = snprintf(obsolete, sizeof(obsolete), "%s/%s",
                           segments_path,
                           manifest.segments[range_first + i].id);
    if (written < 0 || (size_t)written >= sizeof(obsolete))
      status = YAP_V2_OUT_OF_RANGE;
    else
      status = remove_segment_directory(obsolete);
    if (status == YAP_V2_OK) removed_after++;
  }
  if (status != YAP_V2_OK) {
    set_error(error, error_size, "obsolete segment cleanup failed");
    goto done;
  }
  YAP_V2_writer_lock_release(&writer_lock);
  result->generation = candidate.generation;
  result->documents = input.document_count;
  result->passages = input.passage_count;
  result->removed_segments = removed_before + removed_after;
done:
  YAP_V2_writer_lock_release(&writer_lock);
  if (status_started)
    (void)YAP_V2_compaction_status_write(index_dir,
      status == YAP_V2_OK ? YAP_V2_COMPACTION_SUCCEEDED : YAP_V2_COMPACTION_FAILED,
      status == YAP_V2_OK ? result->generation : status_generation);
  if (!published && segment_paths != NULL)
    for (i = 0U; i < plan.count; i++)
      if (segment_paths[i][0] != '\0') (void)remove_segment_directory(segment_paths[i]);
  if (status != YAP_V2_OK) YAP_V2_compaction_result_free(result);
  compaction_input_free(&input);
  free(descriptors);
  free(segment_paths);
  YAP_V2_segment_plan_free(&plan);
  YAP_V2_manifest_free(&candidate);
  YAP_V2_manifest_free(&current);
  YAP_V2_manifest_free(&manifest);
  YAP_V2_writer_lock_release(&compaction_lock);
  return status;
}

int YAP_V2_compact_if_needed(
    const char *index_dir, const YAP_V2_COMPACTION_POLICY *policy,
    YAP_V2_COMPACTION_RESULT *result, int *compacted,
    size_t *small_segment_count, char *error, size_t error_size) {
  if (policy == NULL || compacted == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  return compact_internal(index_dir, policy, result, compacted,
                          small_segment_count, error, error_size);
}

int YAP_V2_compact(const char *index_dir, YAP_V2_COMPACTION_RESULT *result,
                   char *error, size_t error_size) {
  return compact_internal(index_dir, NULL, result, NULL, NULL,
                          error, error_size);
}

int YAP_V2_compact_main(int argc, char **argv) {
  const char *index_dir = NULL, *config_path = NULL; YAP_APPLICATION_CONFIG application;
  YAP_V2_COMPACTION_RESULT result; char error[256] = {0}; int i, status;
  for (i = 1; i < argc; i++) {
    const char **target;
    if (strcmp(argv[i], "--index") == 0) target = &index_dir;
    else if (strcmp(argv[i], "--config") == 0) target = &config_path;
    else { fprintf(stderr, "Unknown compact option: %s\n", argv[i]); return EXIT_FAILURE; }
    if (++i >= argc) { fputs("Missing --index value\n", stderr); return EXIT_FAILURE; }
    *target = argv[i];
  }
  if ((index_dir == NULL) == (config_path == NULL)) { fputs("Usage: yappo_compact (--config CONFIG | --index INDEX_DIR)\n", stderr); return EXIT_FAILURE; }
  if (config_path != NULL) {
    status = YAP_application_config_load(config_path, &application, error, sizeof(error));
    if (status != YAP_V2_OK) { fprintf(stderr, "Config error: %s\n", error); return EXIT_FAILURE; }
    index_dir = application.index_directory;
  }
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
