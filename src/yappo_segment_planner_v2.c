#include "yappo_segment_planner_v2.h"

#include "yappo_ann_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_vector_v2.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  char *term;
  size_t term_bytes;
  size_t occurrences;
  size_t postings;
  uint64_t last_object;
  int used;
} TERM_SLOT;

typedef struct {
  TERM_SLOT *slots;
  size_t capacity;
  size_t count;
} TERM_MAP;

typedef struct {
  size_t documents;
  size_t passages;
  size_t tombstones;
  size_t document_payload;
  size_t metadata_payload;
  size_t vector_id_bytes;
  size_t term_payload;
  size_t posting_payload;
  size_t position_payload;
  size_t tombstone_payload;
  TERM_MAP terms;
} SIZER;

typedef struct {
  TERM_MAP terms;
  size_t document_bytes;
  size_t passage_bytes;
  size_t metadata_bytes;
  size_t passage_count;
  size_t vector_id_bytes;
} UNIT_SIZE;

typedef struct {
  YAP_V2_LEXICAL_PREPARED lexical;
  yyjson_doc *metadata_document;
} PREPARED_UNIT;

static size_t test_payload_limit;

size_t YAP_V2_segment_planner_payload_limit(void) {
  return test_payload_limit == 0U ? YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES : test_payload_limit;
}

void YAP_V2_segment_planner_set_payload_limit_for_testing(size_t payload_limit) {
  test_payload_limit = payload_limit;
}

int YAP_V2_segment_count_validate(size_t existing_count, size_t added_count) {
  if (existing_count > YAP_V2_MAX_SEGMENTS ||
      added_count > YAP_V2_MAX_SEGMENTS - existing_count) return YAP_V2_OUT_OF_RANGE;
  return YAP_V2_OK;
}

static size_t hash_bytes(const char *data, size_t length) {
  size_t hash = (size_t)1469598103934665603ULL;
  size_t i;
  for (i = 0U; i < length; i++) {
    hash ^= (unsigned char)data[i];
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static void term_map_free(TERM_MAP *map) {
  size_t i;
  if (map == NULL) return;
  for (i = 0U; i < map->capacity; i++) free(map->slots[i].term);
  free(map->slots);
  memset(map, 0, sizeof(*map));
}

static int term_map_grow(TERM_MAP *map) {
  TERM_SLOT *old = map->slots;
  size_t old_capacity = map->capacity;
  size_t capacity = old_capacity == 0U ? 256U : old_capacity * 2U;
  size_t i;
  if (capacity < old_capacity || capacity > SIZE_MAX / sizeof(*map->slots))
    return YAP_V2_OUT_OF_RANGE;
  map->slots = calloc(capacity, sizeof(*map->slots));
  if (map->slots == NULL) {
    map->slots = old;
    return YAP_V2_ALLOCATION_FAILED;
  }
  map->capacity = capacity;
  map->count = 0U;
  for (i = 0U; i < old_capacity; i++) {
    if (old[i].used) {
      size_t index = hash_bytes(old[i].term, old[i].term_bytes) & (capacity - 1U);
      while (map->slots[index].used) index = (index + 1U) & (capacity - 1U);
      map->slots[index] = old[i];
      map->count++;
    }
  }
  free(old);
  return YAP_V2_OK;
}

static int term_map_slot(TERM_MAP *map, const char *term, size_t term_bytes,
                         int create, TERM_SLOT **slot) {
  size_t index;
  int status;
  if (map->capacity == 0U || (create && (map->count + 1U) * 10U >= map->capacity * 7U)) {
    status = term_map_grow(map);
    if (status != YAP_V2_OK) return status;
  }
  index = hash_bytes(term, term_bytes) & (map->capacity - 1U);
  while (map->slots[index].used) {
    if (map->slots[index].term_bytes == term_bytes &&
        memcmp(map->slots[index].term, term, term_bytes) == 0) {
      *slot = &map->slots[index];
      return YAP_V2_OK;
    }
    index = (index + 1U) & (map->capacity - 1U);
  }
  if (!create) {
    *slot = NULL;
    return YAP_V2_OK;
  }
  map->slots[index].term = malloc(term_bytes);
  if (map->slots[index].term == NULL) return YAP_V2_ALLOCATION_FAILED;
  memcpy(map->slots[index].term, term, term_bytes);
  map->slots[index].term_bytes = term_bytes;
  map->slots[index].used = 1;
  map->slots[index].last_object = UINT64_MAX;
  map->count++;
  *slot = &map->slots[index];
  return YAP_V2_OK;
}

static const TERM_SLOT *term_map_find(const TERM_MAP *map, const char *term,
                                      size_t term_bytes) {
  size_t index;
  if (map->capacity == 0U) return NULL;
  index = hash_bytes(term, term_bytes) & (map->capacity - 1U);
  while (map->slots[index].used) {
    if (map->slots[index].term_bytes == term_bytes &&
        memcmp(map->slots[index].term, term, term_bytes) == 0)
      return &map->slots[index];
    index = (index + 1U) & (map->capacity - 1U);
  }
  return NULL;
}

static int checked_add(size_t *value, size_t additional) {
  if (additional > SIZE_MAX - *value) return YAP_V2_OUT_OF_RANGE;
  *value += additional;
  return YAP_V2_OK;
}

static int bytes_view_size(YAP_V2_BYTES_VIEW value, size_t *bytes) {
  if (value.len > SIZE_MAX - 4U) return YAP_V2_OUT_OF_RANGE;
  return checked_add(bytes, 4U + value.len);
}

static yyjson_val *path_value(yyjson_val *root, const char *path) {
  const char *start = path;
  yyjson_val *value = root;
  while (*start != '\0') {
    const char *dot = strchr(start, '.');
    size_t length = dot == NULL ? strlen(start) : (size_t)(dot - start);
    if (!yyjson_is_obj(value)) return NULL;
    value = yyjson_obj_getn(value, start, length);
    if (value == NULL || dot == NULL) return value;
    start = dot + 1;
  }
  return NULL;
}

static int scalar_size(yyjson_val *value, size_t *bytes) {
  size_t length = 0U;
  char number[64];
  if (yyjson_is_null(value)) length = 0U;
  else if (yyjson_is_bool(value)) length = 1U;
  else if (yyjson_is_sint(value))
    length = (size_t)snprintf(number, sizeof(number), "%lld", (long long)yyjson_get_sint(value));
  else if (yyjson_is_uint(value))
    length = (size_t)snprintf(number, sizeof(number), "%llu", (unsigned long long)yyjson_get_uint(value));
  else if (yyjson_is_real(value))
    length = (size_t)snprintf(number, sizeof(number), "%.17g", yyjson_get_real(value));
  else if (yyjson_is_str(value)) length = yyjson_get_len(value);
  else return YAP_V2_OK;
  if (length >= sizeof(number) && yyjson_is_num(value)) return YAP_V2_OUT_OF_RANGE;
  return checked_add(bytes, 20U + length);
}

static int value_size(yyjson_val *value, size_t *bytes) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  int status = YAP_V2_OK;
  if (!yyjson_is_arr(value)) return scalar_size(value, bytes);
  yyjson_arr_iter_init(value, &iterator);
  while (status == YAP_V2_OK && (item = yyjson_arr_iter_next(&iterator)) != NULL)
    status = scalar_size(item, bytes);
  return status;
}

static int prepare_unit(const YAP_V2_CONFIG *config, const YAP_V2_SEGMENT_UNIT *unit,
                        UNIT_SIZE *size, PREPARED_UNIT *prepared) {
  size_t i;
  int status = YAP_V2_OK;
  memset(size, 0, sizeof(*size));
  memset(prepared, 0, sizeof(*prepared));
  if (unit->document == NULL) return YAP_V2_OK;
  size->document_bytes = 8U + 8U;
  status = bytes_view_size(unit->document->id, &size->document_bytes);
  if (status == YAP_V2_OK) status = bytes_view_size(unit->document->url, &size->document_bytes);
  if (status == YAP_V2_OK) status = bytes_view_size(unit->document->title, &size->document_bytes);
  if (status == YAP_V2_OK) status = bytes_view_size(unit->document->body, &size->document_bytes);
  if (status == YAP_V2_OK) status = bytes_view_size(unit->document->metadata_json, &size->document_bytes);
  if (status == YAP_V2_OK)
    status = YAP_V2_lexical_prepare_unit(unit->document, unit->passages,
                                         unit->passage_count, &prepared->lexical);
  for (i = 0U; status == YAP_V2_OK && i < prepared->lexical.count; i++) {
    const YAP_V2_LEXICAL_OCCURRENCE *occurrence = &prepared->lexical.items[i];
    uint64_t object_key = occurrence->object_type == YAP_V2_LEXICAL_DOCUMENT ? 0U :
                          occurrence->object_ordinal + 1U;
    TERM_SLOT *slot;
    status = term_map_slot(&size->terms, occurrence->term, occurrence->term_len, 1, &slot);
    if (status == YAP_V2_OK) {
      slot->occurrences++;
      if (slot->last_object != object_key) {
        slot->postings++;
        slot->last_object = object_key;
      }
    }
  }
  for (i = 0U; status == YAP_V2_OK && i < unit->passage_count; i++) {
    size_t record = 8U + 12U;
    status = bytes_view_size(unit->passages[i].id, &record);
    if (status == YAP_V2_OK) status = bytes_view_size(unit->passages[i].parent_document_id, &record);
    if (status == YAP_V2_OK) status = bytes_view_size(unit->passages[i].text, &record);
    if (status == YAP_V2_OK) status = checked_add(&size->passage_bytes, record);
    if (status == YAP_V2_OK) status = checked_add(&size->vector_id_bytes, unit->passages[i].id.len);
  }
  size->passage_count = unit->passage_count;
  if (status == YAP_V2_OK) {
    yyjson_val *root;
    prepared->metadata_document = yyjson_read((const char *)unit->document->metadata_json.data,
                                              unit->document->metadata_json.len, 0U);
    root = prepared->metadata_document == NULL ? NULL :
           yyjson_doc_get_root(prepared->metadata_document);
    size->metadata_bytes = 0U;
    if (root == NULL || !yyjson_is_obj(root)) status = YAP_V2_INVALID_FORMAT;
    for (i = 0U; status == YAP_V2_OK && i < config->filterable_field_count; i++) {
      yyjson_val *value = path_value(root, config->filterable_fields[i]);
      if (value != NULL) status = value_size(value, &size->metadata_bytes);
    }
  }
  if (status != YAP_V2_OK) {
    term_map_free(&size->terms);
    YAP_V2_lexical_prepared_free(&prepared->lexical);
    yyjson_doc_free(prepared->metadata_document);
    prepared->metadata_document = NULL;
  }
  return status;
}

static size_t metadata_base(const YAP_V2_CONFIG *config) {
  size_t bytes = 24U;
  size_t i;
  for (i = 0U; i < config->filterable_field_count; i++)
    bytes += 4U + strlen(config->filterable_fields[i]);
  return bytes;
}

static int component_sizes(const YAP_V2_CONFIG *config, const SIZER *sizer,
                           size_t segment_id_bytes, size_t output[7]) {
  size_t vector_bytes;
  memset(output, 0, sizeof(size_t) * 7U);
  output[0] = 24U + segment_id_bytes + sizer->document_payload;
  if (sizer->documents > 0U) {
    output[1] = sizer->term_payload;
    output[2] = sizer->posting_payload;
    output[3] = sizer->position_payload;
    output[4] = metadata_base(config) + sizer->metadata_payload;
  }
  if (config->vector_metric != YAP_V2_VECTOR_DISABLED && sizer->passages > 0U) {
    vector_bytes = 40U + strlen(config->vector_model_id) + sizer->passages * 16U +
                   sizer->vector_id_bytes;
    vector_bytes = (vector_bytes + sizeof(float) - 1U) & ~(sizeof(float) - 1U);
    vector_bytes += sizer->passages * config->vector_dimensions * sizeof(float);
    output[5] = vector_bytes;
  }
  if (sizer->tombstones > 0U) output[6] = sizer->tombstone_payload;
  return YAP_V2_OK;
}

static int projected_fits(const YAP_V2_CONFIG *config, const SIZER *sizer,
                          const YAP_V2_SEGMENT_UNIT *unit, const UNIT_SIZE *unit_size,
                          size_t segment_id_bytes, size_t limit,
                          size_t *largest_required, const char **largest_component) {
  static const char *const names[] = {"documents.yap2", "terms.yap2", "postings.yap2",
                                      "positions.yap2", "metadata.yap2", "vectors.yap2",
                                      "tombstones.yap2"};
  SIZER projected = *sizer;
  size_t sizes[7];
  size_t i;
  int status;
  if (unit->document != NULL) {
    if (projected.documents == 0U) {
      projected.term_payload = 12U;
      projected.posting_payload = 56U;
      projected.position_payload = 12U;
    }
    projected.documents++;
    projected.passages += unit_size->passage_count;
    projected.document_payload += unit_size->document_bytes + unit_size->passage_bytes;
    projected.metadata_payload += unit_size->metadata_bytes;
    projected.vector_id_bytes += unit_size->vector_id_bytes;
    for (i = 0U; i < unit_size->terms.capacity; i++) {
      const TERM_SLOT *target;
      const TERM_SLOT *source = &unit_size->terms.slots[i];
      size_t old_postings = 0U;
      size_t old_blocks = 0U, new_blocks;
      if (!source->used) continue;
      target = term_map_find(&sizer->terms, source->term, source->term_bytes);
      if (target == NULL) {
        projected.term_payload += 44U + source->term_bytes;
        projected.posting_payload += 20U;
        projected.position_payload += 16U;
      } else {
        old_postings = target->postings;
        old_blocks = (old_postings + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                     YAP_V2_POSTINGS_BLOCK_SIZE;
      }
      new_blocks = (old_postings + source->postings + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                   YAP_V2_POSTINGS_BLOCK_SIZE;
      projected.posting_payload += source->postings * 48U + (new_blocks - old_blocks) * 16U;
      projected.position_payload += source->occurrences * 8U;
    }
  } else {
    projected.tombstones++;
    if (projected.tombstones == 1U) projected.tombstone_payload = 12U;
    projected.tombstone_payload += 4U + unit->tombstone.len;
  }
  status = component_sizes(config, &projected, segment_id_bytes, sizes);
  if (sizer->documents + (unit->document != NULL) > YAP_V2_MAX_SEGMENT_DOCUMENTS ||
      sizer->passages + unit_size->passage_count > YAP_V2_MAX_SEGMENT_PASSAGES) {
    *largest_required = limit + 1U;
    *largest_component = unit->document != NULL ? "documents.yap2" : "tombstones.yap2";
    return YAP_V2_SEGMENT_CAPACITY_EXCEEDED;
  }
  *largest_required = 0U;
  *largest_component = NULL;
  for (i = 0U; i < 7U; i++) {
    if (sizes[i] > *largest_required) {
      *largest_required = sizes[i];
      *largest_component = names[i];
    }
    if (sizes[i] > limit) status = YAP_V2_SEGMENT_CAPACITY_EXCEEDED;
  }
  return status;
}

static int sizer_add(SIZER *sizer, const YAP_V2_SEGMENT_UNIT *unit,
                     const UNIT_SIZE *unit_size) {
  size_t i;
  int status = YAP_V2_OK;
  if (unit->document == NULL) {
    sizer->tombstones++;
    if (sizer->tombstones == 1U) sizer->tombstone_payload = 12U;
    sizer->tombstone_payload += 4U + unit->tombstone.len;
    return YAP_V2_OK;
  }
  if (sizer->documents == 0U) {
    sizer->term_payload = 12U;
    sizer->posting_payload = 56U;
    sizer->position_payload = 12U;
  }
  sizer->documents++;
  sizer->passages += unit_size->passage_count;
  sizer->document_payload += unit_size->document_bytes + unit_size->passage_bytes;
  sizer->metadata_payload += unit_size->metadata_bytes;
  sizer->vector_id_bytes += unit_size->vector_id_bytes;
  for (i = 0U; status == YAP_V2_OK && i < unit_size->terms.capacity; i++) {
    TERM_SLOT *target;
    const TERM_SLOT *source = &unit_size->terms.slots[i];
    size_t old_postings;
    size_t old_blocks;
    size_t new_blocks;
    if (!source->used) continue;
    old_postings = 0U;
    status = term_map_slot(&sizer->terms, source->term, source->term_bytes, 1, &target);
    if (status == YAP_V2_OK) {
      if (target->occurrences == 0U && target->postings == 0U) {
        sizer->term_payload += 44U + source->term_bytes;
        sizer->posting_payload += 20U;
        sizer->position_payload += 16U;
      } else {
        old_postings = target->postings;
      }
      old_blocks = (old_postings + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                   YAP_V2_POSTINGS_BLOCK_SIZE;
      new_blocks = (old_postings + source->postings + YAP_V2_POSTINGS_BLOCK_SIZE - 1U) /
                   YAP_V2_POSTINGS_BLOCK_SIZE;
      sizer->posting_payload += source->postings * 48U + (new_blocks - old_blocks) * 16U;
      sizer->position_payload += source->occurrences * 8U;
      target->occurrences += source->occurrences;
      target->postings += source->postings;
    }
  }
  return status;
}

static void sizer_free(SIZER *sizer) {
  term_map_free(&sizer->terms);
  memset(sizer, 0, sizeof(*sizer));
}

static int plan_add(YAP_V2_SEGMENT_PLAN *plan, size_t first, size_t count,
                    const YAP_V2_CONFIG *config, const SIZER *sizer,
                    size_t segment_id_bytes) {
  YAP_V2_SEGMENT_SLICE *next;
  int status;
  if (plan->count >= YAP_V2_MAX_SEGMENTS) return YAP_V2_OUT_OF_RANGE;
  next = realloc(plan->slices, (plan->count + 1U) * sizeof(*next));
  if (next == NULL) return YAP_V2_ALLOCATION_FAILED;
  plan->slices = next;
  memset(&plan->slices[plan->count], 0, sizeof(plan->slices[plan->count]));
  plan->slices[plan->count].first = first;
  plan->slices[plan->count].count = count;
  status = component_sizes(config, sizer, segment_id_bytes,
                           plan->slices[plan->count].payload_bytes);
  if (status != YAP_V2_OK) return status;
  plan->count++;
  return YAP_V2_OK;
}

void YAP_V2_segment_plan_init(YAP_V2_SEGMENT_PLAN *plan) {
  if (plan != NULL) memset(plan, 0, sizeof(*plan));
}

void YAP_V2_segment_plan_free(YAP_V2_SEGMENT_PLAN *plan) {
  PREPARED_UNIT *prepared;
  size_t i;
  if (plan == NULL) return;
  prepared = plan->prepared_units;
  for (i = 0U; i < plan->prepared_unit_count; i++) {
    YAP_V2_lexical_prepared_free(&prepared[i].lexical);
    yyjson_doc_free(prepared[i].metadata_document);
  }
  free(prepared);
  free(plan->slices);
  memset(plan, 0, sizeof(*plan));
}

int YAP_V2_segment_plan_bisect(YAP_V2_SEGMENT_PLAN *plan, size_t slice_index) {
  YAP_V2_SEGMENT_SLICE *next;
  YAP_V2_SEGMENT_SLICE original;
  size_t left_count;
  if (plan == NULL || slice_index >= plan->count ||
      plan->slices[slice_index].count < 2U) return YAP_V2_INVALID_ARGUMENT;
  if (plan->count >= YAP_V2_MAX_SEGMENTS ||
      plan->count == SIZE_MAX / sizeof(*next)) return YAP_V2_OUT_OF_RANGE;
  original = plan->slices[slice_index];
  next = realloc(plan->slices, (plan->count + 1U) * sizeof(*next));
  if (next == NULL) return YAP_V2_ALLOCATION_FAILED;
  plan->slices = next;
  memmove(&plan->slices[slice_index + 2U], &plan->slices[slice_index + 1U],
          (plan->count - slice_index - 1U) * sizeof(*plan->slices));
  left_count = original.count / 2U;
  memset(&plan->slices[slice_index], 0, sizeof(plan->slices[slice_index]));
  plan->slices[slice_index].first = original.first;
  plan->slices[slice_index].count = left_count;
  memset(&plan->slices[slice_index + 1U], 0, sizeof(plan->slices[slice_index + 1U]));
  plan->slices[slice_index + 1U].first = original.first + left_count;
  plan->slices[slice_index + 1U].count = original.count - left_count;
  plan->count++;
  return YAP_V2_OK;
}

int YAP_V2_segment_plan(const YAP_V2_CONFIG *config,
                        const YAP_V2_SEGMENT_UNIT *units, size_t unit_count,
                        size_t segment_id_bytes, size_t payload_limit,
                        YAP_V2_SEGMENT_PLAN *plan,
                        YAP_V2_SEGMENT_CAPACITY_ERROR *capacity_error) {
  SIZER sizer = {0};
  size_t first = 0U;
  size_t i;
  int status = YAP_V2_OK;
  if (config == NULL || (unit_count > 0U && units == NULL) || segment_id_bytes == 0U ||
      payload_limit == 0U || plan == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_segment_plan_init(plan);
  if (capacity_error != NULL) memset(capacity_error, 0, sizeof(*capacity_error));
  plan->prepared_units = unit_count == 0U ? NULL : calloc(unit_count, sizeof(PREPARED_UNIT));
  if (unit_count > 0U && plan->prepared_units == NULL) return YAP_V2_ALLOCATION_FAILED;
  plan->prepared_unit_count = unit_count;
  for (i = 0U; status == YAP_V2_OK && i < unit_count; i++) {
    UNIT_SIZE unit_size;
    PREPARED_UNIT *prepared = &((PREPARED_UNIT *)plan->prepared_units)[i];
    size_t required = 0U;
    const char *component = NULL;
    status = prepare_unit(config, &units[i], &unit_size, prepared);
    if (status != YAP_V2_OK) break;
    status = projected_fits(config, &sizer, &units[i], &unit_size, segment_id_bytes,
                            payload_limit, &required, &component);
    if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED && i > first) {
      status = plan_add(plan, first, i - first, config, &sizer, segment_id_bytes);
      sizer_free(&sizer);
      first = i;
      if (status == YAP_V2_OK)
        status = projected_fits(config, &sizer, &units[i], &unit_size, segment_id_bytes,
                                payload_limit, &required, &component);
    }
    if (status == YAP_V2_SEGMENT_CAPACITY_EXCEEDED) {
      if (capacity_error != NULL) {
        capacity_error->component = component;
        capacity_error->required_bytes = required;
        capacity_error->limit_bytes = payload_limit;
        capacity_error->document_id = units[i].document != NULL ? units[i].document->id
                                                                 : units[i].tombstone;
      }
    } else if (status == YAP_V2_OK) {
      status = sizer_add(&sizer, &units[i], &unit_size);
    }
    term_map_free(&unit_size.terms);
  }
  if (status == YAP_V2_OK)
    status = plan_add(plan, first, unit_count - first, config, &sizer, segment_id_bytes);
  sizer_free(&sizer);
  if (status != YAP_V2_OK) YAP_V2_segment_plan_free(plan);
  return status;
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int sync_directory(const char *path) {
  int descriptor = open(path, O_RDONLY | O_DIRECTORY);
  int status;
  if (descriptor < 0) return YAP_V2_IO_ERROR;
  status = fsync(descriptor) == 0 ? YAP_V2_OK : YAP_V2_IO_ERROR;
  if (close(descriptor) != 0) status = YAP_V2_IO_ERROR;
  return status;
}

int YAP_V2_segment_slice_write(const char *directory, const char *segment_id,
                               uint64_t generation, const YAP_V2_CONFIG *config,
                               const YAP_V2_SEGMENT_UNIT *units,
                               const YAP_V2_SEGMENT_PLAN *plan,
                               YAP_V2_SEGMENT_SLICE slice,
                               YAP_V2_SEGMENT_DESCRIPTOR *descriptor) {
  YAP_V2_DOCUMENT_VIEW *documents = NULL;
  YAP_V2_PASSAGE_VIEW *passages = NULL;
  YAP_V2_BYTES_VIEW *tombstones = NULL;
  const YAP_V2_LEXICAL_PREPARED **lexical_prepared = NULL;
  const void **metadata_roots = NULL;
  float *vectors = NULL;
  size_t document_count = 0U, passage_count = 0U, tombstone_count = 0U;
  size_t d = 0U, p = 0U, t = 0U, i, j;
  char path[4096];
  int status = YAP_V2_OK;
  const PREPARED_UNIT *prepared;
  if (directory == NULL || segment_id == NULL || generation == 0U || config == NULL ||
      descriptor == NULL || (slice.count > 0U && units == NULL) || plan == NULL ||
      (plan->prepared_unit_count > 0U && plan->prepared_units == NULL) ||
      slice.first > plan->prepared_unit_count ||
      slice.count > plan->prepared_unit_count - slice.first) return YAP_V2_INVALID_ARGUMENT;
  prepared = plan->prepared_units;
  for (i = 0U; i < slice.count; i++) {
    const YAP_V2_SEGMENT_UNIT *unit = &units[slice.first + i];
    if (unit->document != NULL) { document_count++; passage_count += unit->passage_count; }
    else tombstone_count++;
  }
  documents = document_count == 0U ? NULL : calloc(document_count, sizeof(*documents));
  passages = passage_count == 0U ? NULL : calloc(passage_count, sizeof(*passages));
  tombstones = tombstone_count == 0U ? NULL : calloc(tombstone_count, sizeof(*tombstones));
  lexical_prepared = document_count == 0U ? NULL : calloc(document_count, sizeof(*lexical_prepared));
  metadata_roots = document_count == 0U ? NULL : calloc(document_count, sizeof(*metadata_roots));
  if (config->vector_metric != YAP_V2_VECTOR_DISABLED && passage_count > 0U) {
    if (config->vector_dimensions == 0U || passage_count > SIZE_MAX / config->vector_dimensions ||
        passage_count * config->vector_dimensions > SIZE_MAX / sizeof(*vectors)) {
      status = YAP_V2_OUT_OF_RANGE;
      goto done;
    }
    vectors = malloc(passage_count * config->vector_dimensions * sizeof(*vectors));
  }
  if ((document_count > 0U && documents == NULL) || (passage_count > 0U && passages == NULL) ||
      (tombstone_count > 0U && tombstones == NULL) ||
      (document_count > 0U && (lexical_prepared == NULL || metadata_roots == NULL)) ||
      (config->vector_metric != YAP_V2_VECTOR_DISABLED && passage_count > 0U && vectors == NULL)) {
    status = YAP_V2_ALLOCATION_FAILED;
    goto done;
  }
  for (i = 0U; i < slice.count; i++) {
    const YAP_V2_SEGMENT_UNIT *unit = &units[slice.first + i];
    if (unit->document == NULL) { tombstones[t++] = unit->tombstone; continue; }
    documents[d] = *unit->document;
    lexical_prepared[d] = &prepared[slice.first + i].lexical;
    metadata_roots[d] = yyjson_doc_get_root(prepared[slice.first + i].metadata_document);
    d++;
    for (j = 0U; j < unit->passage_count; j++) {
      passages[p] = unit->passages[j];
      if (vectors != NULL)
        memcpy(vectors + p * config->vector_dimensions,
               unit->vectors + j * config->vector_dimensions,
               config->vector_dimensions * sizeof(*vectors));
      p++;
    }
  }
  if (join_path(path, sizeof(path), directory, "documents.yap2") != 0) status = YAP_V2_OUT_OF_RANGE;
  else status = YAP_V2_segment_write(path, segment_id, generation, documents, document_count,
                                     passages, passage_count, descriptor);
  if (status == YAP_V2_OK && document_count > 0U) {
    YAP_V2_COMPONENT_DESCRIPTOR lexical[3], metadata;
    status = YAP_V2_lexical_write_prepared(directory, generation, lexical_prepared,
                                           document_count, lexical);
    for (i = 0U; status == YAP_V2_OK && i < 3U; i++)
      status = YAP_V2_segment_descriptor_add_component(descriptor, &lexical[i]);
    if (status == YAP_V2_OK && join_path(path, sizeof(path), directory, "metadata.yap2") == 0)
      status = YAP_V2_metadata_write_preparsed(path, generation, config, documents,
                                               metadata_roots, document_count, &metadata);
    else if (status == YAP_V2_OK) status = YAP_V2_OUT_OF_RANGE;
    if (status == YAP_V2_OK) status = YAP_V2_segment_descriptor_add_component(descriptor, &metadata);
  }
  if (status == YAP_V2_OK && vectors != NULL) {
    YAP_V2_COMPONENT_DESCRIPTOR vector_component, ann_component;
    YAP_EMBEDDING_RESULT embeddings;
    YAP_V2_VECTOR_SEGMENT vector_segment;
    embeddings.values = vectors; embeddings.input_count = passage_count;
    embeddings.dimensions = config->vector_dimensions;
    if (join_path(path, sizeof(path), directory, "vectors.yap2") != 0) status = YAP_V2_OUT_OF_RANGE;
    else status = YAP_V2_vectors_write(path, generation, config, passages, passage_count,
                                       &embeddings, &vector_component);
    if (status == YAP_V2_OK) status = YAP_V2_segment_descriptor_add_component(descriptor, &vector_component);
    YAP_V2_vector_segment_init(&vector_segment);
    if (status == YAP_V2_OK) status = YAP_V2_vector_segment_open(path, generation, config, &vector_segment, NULL);
    if (status == YAP_V2_OK && join_path(path, sizeof(path), directory, "vectors.usearch") == 0) {
      if (YAP_V2_ann_build_save(path, &vector_segment, 16U, 128U, 64U, &ann_component) != YAP_ANN_OK)
        status = YAP_V2_CONFLICT;
    } else if (status == YAP_V2_OK) status = YAP_V2_OUT_OF_RANGE;
    YAP_V2_vector_segment_close(&vector_segment);
    if (status == YAP_V2_OK) status = YAP_V2_segment_descriptor_add_component(descriptor, &ann_component);
  }
  if (status == YAP_V2_OK && tombstone_count > 0U) {
    YAP_V2_COMPONENT_DESCRIPTOR component;
    if (join_path(path, sizeof(path), directory, "tombstones.yap2") != 0) status = YAP_V2_OUT_OF_RANGE;
    else status = YAP_V2_tombstones_write(path, generation, tombstones, tombstone_count, &component);
    if (status == YAP_V2_OK) status = YAP_V2_segment_descriptor_add_component(descriptor, &component);
    descriptor->tombstone_count = tombstone_count;
  }
  if (status == YAP_V2_OK) status = sync_directory(directory);
done:
  free(documents); free(passages); free(tombstones); free(vectors);
  free(lexical_prepared); free(metadata_roots);
  return status;
}
