#include "storage/yappo_snapshot_v2.h"

#include "storage/yappo_manifest_v2.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  pthread_mutex_t references_lock;
  size_t references;
  YAP_V2_SEGMENT documents;
  YAP_V2_TOMBSTONES tombstones;
} SNAPSHOT_SEGMENT;

typedef struct {
  YAP_V2_BYTES_VIEW id;
  size_t segment_ordinal;
  size_t document_ordinal;
  const YAP_V2_DOCUMENT_VIEW *document;
  int occupied;
} VISIBILITY_ENTRY;

struct YAP_V2_SEARCH_SNAPSHOT {
  pthread_mutex_t references_lock;
  size_t references;
  YAP_V2_MANIFEST manifest;
  SNAPSHOT_SEGMENT **segments;
  size_t segment_count;
  VISIBILITY_ENTRY *visibility;
  size_t visibility_capacity;
};

typedef struct {
  pthread_mutex_t lock;
  char *index_dir;
  char *manifest_path;
  YAP_V2_CONFIG config;
  YAP_V2_SEARCH_SNAPSHOT *current;
} MANAGER_STATE;

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len && left.data != NULL && right.data != NULL &&
         memcmp(left.data, right.data, left.len) == 0;
}

static uint64_t bytes_hash(YAP_V2_BYTES_VIEW value) {
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t i;
  for (i = 0U; i < value.len; i++) {
    hash ^= value.data[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static char *copy_string(const char *value) {
  size_t length;
  char *copy;
  if (value == NULL) return NULL;
  length = strlen(value);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1U);
  if (copy != NULL) memcpy(copy, value, length + 1U);
  return copy;
}

static void snapshot_destroy(YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  size_t i;
  if (snapshot == NULL) return;
  free(snapshot->visibility);
  for (i = 0U; i < snapshot->segment_count; i++) {
    SNAPSHOT_SEGMENT *segment = snapshot->segments[i];
    int destroy = 0;
    if (segment == NULL) continue;
    pthread_mutex_lock(&segment->references_lock);
    if (segment->references > 0U) {
      segment->references--;
      destroy = segment->references == 0U;
    }
    pthread_mutex_unlock(&segment->references_lock);
    if (destroy) {
      YAP_V2_segment_free(&segment->documents);
      YAP_V2_tombstones_free(&segment->tombstones);
      pthread_mutex_destroy(&segment->references_lock);
      free(segment);
    }
  }
  free(snapshot->segments);
  YAP_V2_manifest_free(&snapshot->manifest);
  pthread_mutex_destroy(&snapshot->references_lock);
  free(snapshot);
}

static VISIBILITY_ENTRY *visibility_entry(YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                          YAP_V2_BYTES_VIEW id) {
  size_t index, probes;
  if (snapshot->visibility_capacity == 0U) return NULL;
  index = (size_t)(bytes_hash(id) & (uint64_t)(snapshot->visibility_capacity - 1U));
  for (probes = 0U; probes < snapshot->visibility_capacity; probes++) {
    VISIBILITY_ENTRY *entry = &snapshot->visibility[index];
    if (!entry->occupied || bytes_equal(entry->id, id)) return entry;
    index = (index + 1U) & (snapshot->visibility_capacity - 1U);
  }
  return NULL;
}

static const VISIBILITY_ENTRY *visibility_lookup(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                                 YAP_V2_BYTES_VIEW id) {
  size_t index, probes;
  if (snapshot->visibility_capacity == 0U) return NULL;
  index = (size_t)(bytes_hash(id) & (uint64_t)(snapshot->visibility_capacity - 1U));
  for (probes = 0U; probes < snapshot->visibility_capacity; probes++) {
    const VISIBILITY_ENTRY *entry = &snapshot->visibility[index];
    if (!entry->occupied) return NULL;
    if (bytes_equal(entry->id, id)) return entry;
    index = (index + 1U) & (snapshot->visibility_capacity - 1U);
  }
  return NULL;
}

static int snapshot_build_visibility(YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  size_t record_count = 0U, capacity = 1U, s, i;
  for (s = 0U; s < snapshot->segment_count; s++) {
    const SNAPSHOT_SEGMENT *segment = snapshot->segments[s];
    if (segment->documents.document_count > SIZE_MAX - record_count)
      return YAP_V2_OUT_OF_RANGE;
    record_count += segment->documents.document_count;
    if (segment->tombstones.count > SIZE_MAX - record_count)
      return YAP_V2_OUT_OF_RANGE;
    record_count += segment->tombstones.count;
  }
  if (record_count == 0U) return YAP_V2_OK;
  if (record_count > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
  while (capacity < record_count * 2U) {
    if (capacity > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
    capacity *= 2U;
  }
  snapshot->visibility = (VISIBILITY_ENTRY *)calloc(capacity, sizeof(*snapshot->visibility));
  if (snapshot->visibility == NULL) return YAP_V2_ALLOCATION_FAILED;
  snapshot->visibility_capacity = capacity;
  for (s = 0U; s < snapshot->segment_count; s++) {
    SNAPSHOT_SEGMENT *segment = snapshot->segments[s];
    for (i = 0U; i < segment->tombstones.count; i++) {
      VISIBILITY_ENTRY *entry = visibility_entry(snapshot, segment->tombstones.document_ids[i]);
      if (entry == NULL) return YAP_V2_CONFLICT;
      entry->id = segment->tombstones.document_ids[i];
      entry->segment_ordinal = s;
      entry->document_ordinal = SIZE_MAX;
      entry->document = NULL;
      entry->occupied = 1;
    }
    for (i = 0U; i < segment->documents.document_count; i++) {
      VISIBILITY_ENTRY *entry = visibility_entry(snapshot, segment->documents.documents[i].id);
      if (entry == NULL) return YAP_V2_CONFLICT;
      entry->id = segment->documents.documents[i].id;
      entry->segment_ordinal = s;
      entry->document_ordinal = i;
      entry->document = &segment->documents.documents[i];
      entry->occupied = 1;
    }
  }
  return YAP_V2_OK;
}

static void snapshot_retain(YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  pthread_mutex_lock(&snapshot->references_lock);
  snapshot->references++;
  pthread_mutex_unlock(&snapshot->references_lock);
}

void YAP_V2_snapshot_release(YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  int destroy = 0;
  if (snapshot == NULL) return;
  pthread_mutex_lock(&snapshot->references_lock);
  if (snapshot->references > 0U) {
    snapshot->references--;
    destroy = snapshot->references == 0U;
  }
  pthread_mutex_unlock(&snapshot->references_lock);
  if (destroy) snapshot_destroy(snapshot);
}

static const YAP_V2_COMPONENT_DESCRIPTOR *component(const YAP_V2_SEGMENT_DESCRIPTOR *segment,
                                                     uint32_t file_type) {
  size_t i;
  for (i = 0U; i < segment->component_count; i++)
    if (segment->components[i].file_type == file_type) return &segment->components[i];
  return NULL;
}

static int same_descriptor(const YAP_V2_SEGMENT_DESCRIPTOR *left,
                           const YAP_V2_SEGMENT_DESCRIPTOR *right) {
  size_t i;
  if (strcmp(left->id, right->id) != 0 ||
      left->document_count != right->document_count ||
      left->passage_count != right->passage_count ||
      left->tombstone_count != right->tombstone_count ||
      left->component_count != right->component_count) return 0;
  for (i = 0U; i < left->component_count; i++) {
    const YAP_V2_COMPONENT_DESCRIPTOR *a = &left->components[i];
    const YAP_V2_COMPONENT_DESCRIPTOR *b = &right->components[i];
    if (strcmp(a->name, b->name) != 0 || a->file_type != b->file_type ||
        a->record_count != b->record_count || a->file_bytes != b->file_bytes ||
        memcmp(a->checksum, b->checksum, sizeof(a->checksum)) != 0) return 0;
  }
  return 1;
}

static int component_path(const char *index_dir, const char *segment_id, const char *name,
                          char **path_out) {
  size_t length;
  char *path;
  if (strlen(index_dir) > SIZE_MAX - strlen(segment_id) - strlen(name) - 12U)
    return YAP_V2_OUT_OF_RANGE;
  length = strlen(index_dir) + strlen(segment_id) + strlen(name) + 12U;
  path = (char *)malloc(length);
  if (path == NULL) return YAP_V2_ALLOCATION_FAILED;
  (void)snprintf(path, length, "%s/segments/%s/%s", index_dir, segment_id, name);
  *path_out = path;
  return YAP_V2_OK;
}

static int snapshot_load(const MANAGER_STATE *state,
                         const YAP_V2_SEARCH_SNAPSHOT *previous,
                         YAP_V2_SEARCH_SNAPSHOT **snapshot_out) {
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  size_t i;
  int status;
  snapshot = (YAP_V2_SEARCH_SNAPSHOT *)calloc(1U, sizeof(*snapshot));
  if (snapshot == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_mutex_init(&snapshot->references_lock, NULL) != 0) { free(snapshot); return YAP_V2_IO_ERROR; }
  snapshot->references = 1U;
  YAP_V2_manifest_init(&snapshot->manifest);
  status = YAP_V2_manifest_load_for_config(state->manifest_path, &state->config,
                                           &snapshot->manifest);
  if (status == YAP_V2_OK) snapshot->segment_count = snapshot->manifest.segment_count;
  if (status == YAP_V2_OK && snapshot->segment_count > 0U) {
    snapshot->segments = (SNAPSHOT_SEGMENT **)calloc(snapshot->segment_count,
                                                     sizeof(*snapshot->segments));
    if (snapshot->segments == NULL) status = YAP_V2_ALLOCATION_FAILED;
  }
  for (i = 0U; status == YAP_V2_OK && i < snapshot->segment_count; i++) {
    const YAP_V2_SEGMENT_DESCRIPTOR *descriptor = &snapshot->manifest.segments[i];
    const YAP_V2_COMPONENT_DESCRIPTOR *documents = component(descriptor, YAP_V2_FILE_DOCUMENTS);
    const YAP_V2_COMPONENT_DESCRIPTOR *tombstones = component(descriptor, YAP_V2_FILE_TOMBSTONES);
    char *path = NULL;
    size_t previous_index;
    if (previous != NULL) {
      for (previous_index = 0U; previous_index < previous->segment_count; previous_index++) {
        if (same_descriptor(&previous->manifest.segments[previous_index], descriptor)) {
          snapshot->segments[i] = previous->segments[previous_index];
          pthread_mutex_lock(&snapshot->segments[i]->references_lock);
          snapshot->segments[i]->references++;
          pthread_mutex_unlock(&snapshot->segments[i]->references_lock);
          break;
        }
      }
      if (previous_index < previous->segment_count) continue;
    }
    status = YAP_V2_manifest_verify_segment_components(
      state->index_dir, snapshot->manifest.generation, descriptor);
    if (status != YAP_V2_OK) break;
    snapshot->segments[i] = calloc(1U, sizeof(*snapshot->segments[i]));
    if (snapshot->segments[i] == NULL) { status = YAP_V2_ALLOCATION_FAILED; break; }
    if (pthread_mutex_init(&snapshot->segments[i]->references_lock, NULL) != 0) {
      free(snapshot->segments[i]); snapshot->segments[i] = NULL;
      status = YAP_V2_IO_ERROR; break;
    }
    snapshot->segments[i]->references = 1U;
    YAP_V2_segment_init(&snapshot->segments[i]->documents);
    YAP_V2_tombstones_init(&snapshot->segments[i]->tombstones);
    if (documents == NULL) { status = YAP_V2_INVALID_FORMAT; break; }
    status = component_path(state->index_dir, descriptor->id, documents->name, &path);
    if (status == YAP_V2_OK)
      status = YAP_V2_segment_read(path, 0U,
                                   &snapshot->segments[i]->documents, NULL);
    free(path); path = NULL;
    if (status == YAP_V2_OK &&
        (strcmp(snapshot->segments[i]->documents.id, descriptor->id) != 0 ||
         snapshot->segments[i]->documents.document_count != descriptor->document_count ||
         snapshot->segments[i]->documents.passage_count != descriptor->passage_count))
      status = YAP_V2_CONFLICT;
    if (status == YAP_V2_OK && tombstones != NULL) {
      status = component_path(state->index_dir, descriptor->id, tombstones->name, &path);
      if (status == YAP_V2_OK)
        status = YAP_V2_tombstones_read(path, 0U,
                                        &snapshot->segments[i]->tombstones);
      free(path);
    }
    if (status == YAP_V2_OK &&
        snapshot->segments[i]->tombstones.count != descriptor->tombstone_count)
      status = YAP_V2_CONFLICT;
  }
  if (status == YAP_V2_OK) status = snapshot_build_visibility(snapshot);
  if (status != YAP_V2_OK) { snapshot_destroy(snapshot); return status; }
  *snapshot_out = snapshot;
  return YAP_V2_OK;
}

void YAP_V2_snapshot_manager_init(YAP_V2_SNAPSHOT_MANAGER *manager) {
  if (manager != NULL) manager->state = NULL;
}

void YAP_V2_snapshot_manager_close(YAP_V2_SNAPSHOT_MANAGER *manager) {
  MANAGER_STATE *state;
  YAP_V2_SEARCH_SNAPSHOT *current;
  if (manager == NULL || manager->state == NULL) return;
  state = (MANAGER_STATE *)manager->state;
  pthread_mutex_lock(&state->lock); current = state->current; state->current = NULL;
  pthread_mutex_unlock(&state->lock);
  YAP_V2_snapshot_release(current);
  pthread_mutex_destroy(&state->lock);
  free(state->index_dir); free(state->manifest_path); free(state); manager->state = NULL;
}

int YAP_V2_snapshot_manager_open(YAP_V2_SNAPSHOT_MANAGER *manager, const char *index_dir,
                                 const char *manifest_path, const YAP_V2_CONFIG *config) {
  MANAGER_STATE *state;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  int status;
  if (manager == NULL || manager->state != NULL || index_dir == NULL || manifest_path == NULL ||
      config == NULL || YAP_V2_config_validate(config) != YAP_V2_OK)
    return YAP_V2_INVALID_ARGUMENT;
  state = (MANAGER_STATE *)calloc(1U, sizeof(*state));
  if (state == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (pthread_mutex_init(&state->lock, NULL) != 0) { free(state); return YAP_V2_IO_ERROR; }
  state->index_dir = copy_string(index_dir); state->manifest_path = copy_string(manifest_path);
  state->config = *config;
  if (state->index_dir == NULL || state->manifest_path == NULL) {
    pthread_mutex_destroy(&state->lock); free(state->index_dir); free(state->manifest_path);
    free(state); return YAP_V2_ALLOCATION_FAILED;
  }
  status = snapshot_load(state, NULL, &snapshot);
  if (status != YAP_V2_OK) {
    pthread_mutex_destroy(&state->lock); free(state->index_dir); free(state->manifest_path);
    free(state); return status;
  }
  state->current = snapshot; manager->state = state; return YAP_V2_OK;
}

int YAP_V2_snapshot_manager_reload(YAP_V2_SNAPSHOT_MANAGER *manager, int *changed) {
  MANAGER_STATE *state;
  YAP_V2_SEARCH_SNAPSHOT *candidate, *previous, *base;
  int status;
  if (manager == NULL || manager->state == NULL || changed == NULL) return YAP_V2_INVALID_ARGUMENT;
  *changed = 0; state = (MANAGER_STATE *)manager->state;
  pthread_mutex_lock(&state->lock); base = state->current;
  if (base != NULL) snapshot_retain(base);
  pthread_mutex_unlock(&state->lock);
  status = snapshot_load(state, base, &candidate);
  YAP_V2_snapshot_release(base);
  if (status != YAP_V2_OK) return status;
  pthread_mutex_lock(&state->lock);
  previous = state->current;
  if (previous != NULL && candidate->manifest.generation < previous->manifest.generation) {
    pthread_mutex_unlock(&state->lock); YAP_V2_snapshot_release(candidate); return YAP_V2_CONFLICT;
  }
  if (previous != NULL && candidate->manifest.generation == previous->manifest.generation) {
    pthread_mutex_unlock(&state->lock); YAP_V2_snapshot_release(candidate); return YAP_V2_OK;
  }
  state->current = candidate; *changed = 1;
  pthread_mutex_unlock(&state->lock);
  YAP_V2_snapshot_release(previous);
  return YAP_V2_OK;
}

YAP_V2_SEARCH_SNAPSHOT *YAP_V2_snapshot_acquire(YAP_V2_SNAPSHOT_MANAGER *manager) {
  MANAGER_STATE *state;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  if (manager == NULL || manager->state == NULL) return NULL;
  state = (MANAGER_STATE *)manager->state;
  pthread_mutex_lock(&state->lock); snapshot = state->current;
  if (snapshot != NULL) snapshot_retain(snapshot);
  pthread_mutex_unlock(&state->lock); return snapshot;
}

uint64_t YAP_V2_snapshot_generation(const YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  return snapshot == NULL ? 0U : snapshot->manifest.generation;
}

size_t YAP_V2_snapshot_segment_count(const YAP_V2_SEARCH_SNAPSHOT *snapshot) {
  return snapshot == NULL ? 0U : snapshot->segment_count;
}

const YAP_V2_SEGMENT *YAP_V2_snapshot_segment_documents(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                                        size_t segment_ordinal) {
  if (snapshot == NULL || segment_ordinal >= snapshot->segment_count) return NULL;
  return &snapshot->segments[segment_ordinal]->documents;
}

int YAP_V2_snapshot_document_visible(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                     size_t segment_ordinal, YAP_V2_BYTES_VIEW document_id) {
  const VISIBILITY_ENTRY *entry;
  if (snapshot == NULL || segment_ordinal >= snapshot->segment_count ||
      document_id.data == NULL || document_id.len == 0U) return 0;
  entry = visibility_lookup(snapshot, document_id);
  return entry != NULL && entry->document != NULL && entry->segment_ordinal == segment_ordinal;
}

int YAP_V2_snapshot_lookup_document(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                    YAP_V2_BYTES_VIEW document_id, YAP_V2_DOCUMENT_HIT *hit) {
  const VISIBILITY_ENTRY *entry;
  if (snapshot == NULL || document_id.data == NULL || document_id.len == 0U || hit == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  entry = visibility_lookup(snapshot, document_id);
  if (entry == NULL || entry->document == NULL) return YAP_V2_NOT_FOUND;
  hit->segment_ordinal = entry->segment_ordinal;
  hit->document_ordinal = entry->document_ordinal;
  hit->document = entry->document;
  return YAP_V2_OK;
}
