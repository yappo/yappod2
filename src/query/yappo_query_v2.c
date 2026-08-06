#include "query/yappo_query_v2.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  YAP_V2_BYTES_VIEW id;
  YAP_V2_BYTES_VIEW parent;
  size_t segment;
  size_t ordinal;
  double score;
} CANDIDATE;

typedef struct {
  size_t candidate_index;
  unsigned char state;
} CANDIDATE_HASH_SLOT;

typedef struct {
  CANDIDATE *items;
  size_t count;
  size_t capacity;
  size_t *heap;
  size_t *heap_positions;
  CANDIDATE_HASH_SLOT *hash;
  size_t hash_capacity;
} CANDIDATE_SET;

static int bytes_equal(YAP_V2_BYTES_VIEW a, YAP_V2_BYTES_VIEW b) {
  return a.len == b.len && a.data != NULL && b.data != NULL && memcmp(a.data, b.data, a.len) == 0;
}

static int bytes_compare(YAP_V2_BYTES_VIEW a, YAP_V2_BYTES_VIEW b) {
  size_t common = a.len < b.len ? a.len : b.len;
  int compared = memcmp(a.data, b.data, common);
  if (compared != 0) return compared;
  return a.len < b.len ? -1 : a.len > b.len;
}

static int candidate_compare(const void *left, const void *right) {
  const CANDIDATE *a = (const CANDIDATE *)left, *b = (const CANDIDATE *)right;
  if (a->score > b->score) return -1;
  if (a->score < b->score) return 1;
  return bytes_compare(a->id, b->id);
}

static int filter_matches(const YAP_V2_FILTER *filter, int enabled, size_t ordinal) {
  int matches = 1;
  return !enabled || (YAP_V2_filter_matches(filter, ordinal, &matches) == YAP_V2_OK && matches);
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

static int candidate_better(const CANDIDATE *left, const CANDIDATE *right) {
  if (left->score != right->score) return left->score > right->score;
  return bytes_compare(left->id, right->id) < 0;
}

static int candidate_worse(const CANDIDATE *left, const CANDIDATE *right) {
  return candidate_better(right, left);
}

static void candidate_set_free(CANDIDATE_SET *set) {
  if (set == NULL) return;
  free(set->items);
  free(set->heap);
  free(set->heap_positions);
  free(set->hash);
  memset(set, 0, sizeof(*set));
}

static int candidate_set_init(CANDIDATE_SET *set, size_t capacity) {
  size_t hash_capacity = 1U;
  if (set == NULL || capacity == 0U || capacity > SIZE_MAX / 2U)
    return YAP_V2_INVALID_ARGUMENT;
  memset(set, 0, sizeof(*set));
  while (hash_capacity < capacity * 2U) {
    if (hash_capacity > SIZE_MAX / 2U) return YAP_V2_OUT_OF_RANGE;
    hash_capacity *= 2U;
  }
  set->items = (CANDIDATE *)calloc(capacity, sizeof(*set->items));
  set->heap = (size_t *)calloc(capacity, sizeof(*set->heap));
  set->heap_positions = (size_t *)calloc(capacity, sizeof(*set->heap_positions));
  set->hash = (CANDIDATE_HASH_SLOT *)calloc(hash_capacity, sizeof(*set->hash));
  if (set->items == NULL || set->heap == NULL || set->heap_positions == NULL ||
      set->hash == NULL) {
    candidate_set_free(set);
    return YAP_V2_ALLOCATION_FAILED;
  }
  set->capacity = capacity;
  set->hash_capacity = hash_capacity;
  return YAP_V2_OK;
}

static size_t candidate_hash_slot(const CANDIDATE_SET *set, YAP_V2_BYTES_VIEW id, int *found) {
  size_t index, probes, deleted = SIZE_MAX;
  index = (size_t)(bytes_hash(id) & (uint64_t)(set->hash_capacity - 1U));
  for (probes = 0U; probes < set->hash_capacity; probes++) {
    const CANDIDATE_HASH_SLOT *slot = &set->hash[index];
    if (slot->state == 0U) {
      *found = 0;
      return deleted == SIZE_MAX ? index : deleted;
    }
    if (slot->state == 1U && bytes_equal(set->items[slot->candidate_index].id, id)) {
      *found = 1;
      return index;
    }
    if (slot->state == 2U && deleted == SIZE_MAX) deleted = index;
    index = (index + 1U) & (set->hash_capacity - 1U);
  }
  *found = 0;
  return deleted;
}

static void candidate_hash_insert(CANDIDATE_SET *set, size_t candidate_index) {
  int found;
  size_t slot = candidate_hash_slot(set, set->items[candidate_index].id, &found);
  set->hash[slot].candidate_index = candidate_index;
  set->hash[slot].state = 1U;
}

static void candidate_hash_remove(CANDIDATE_SET *set, YAP_V2_BYTES_VIEW id) {
  int found;
  size_t slot = candidate_hash_slot(set, id, &found);
  if (found) set->hash[slot].state = 2U;
}

static void candidate_heap_swap(CANDIDATE_SET *set, size_t left, size_t right) {
  size_t candidate = set->heap[left];
  set->heap[left] = set->heap[right];
  set->heap[right] = candidate;
  set->heap_positions[set->heap[left]] = left;
  set->heap_positions[set->heap[right]] = right;
}

static void candidate_heap_up(CANDIDATE_SET *set, size_t position) {
  while (position > 0U) {
    size_t parent = (position - 1U) / 2U;
    if (!candidate_worse(&set->items[set->heap[position]],
                         &set->items[set->heap[parent]])) break;
    candidate_heap_swap(set, position, parent);
    position = parent;
  }
}

static void candidate_heap_down(CANDIDATE_SET *set, size_t position) {
  for (;;) {
    size_t left = position * 2U + 1U, right = left + 1U, worst = position;
    if (left < set->count &&
        candidate_worse(&set->items[set->heap[left]], &set->items[set->heap[worst]]))
      worst = left;
    if (right < set->count &&
        candidate_worse(&set->items[set->heap[right]], &set->items[set->heap[worst]]))
      worst = right;
    if (worst == position) break;
    candidate_heap_swap(set, position, worst);
    position = worst;
  }
}

static int candidate_set_add(CANDIDATE_SET *set, const CANDIDATE *candidate) {
  int found;
  size_t hash_slot = candidate_hash_slot(set, candidate->id, &found);
  if (found) {
    size_t candidate_index = set->hash[hash_slot].candidate_index;
    size_t heap_position;
    if (!candidate_better(candidate, &set->items[candidate_index])) return YAP_V2_OK;
    set->items[candidate_index] = *candidate;
    heap_position = set->heap_positions[candidate_index];
    candidate_heap_down(set, heap_position);
    return YAP_V2_OK;
  }
  if (set->count < set->capacity) {
    size_t candidate_index = set->count;
    set->items[candidate_index] = *candidate;
    set->heap[candidate_index] = candidate_index;
    set->heap_positions[candidate_index] = candidate_index;
    set->count++;
    candidate_hash_insert(set, candidate_index);
    candidate_heap_up(set, candidate_index);
    return YAP_V2_OK;
  }
  if (!candidate_better(candidate, &set->items[set->heap[0]])) return YAP_V2_OK;
  candidate_hash_remove(set, set->items[set->heap[0]].id);
  set->items[set->heap[0]] = *candidate;
  candidate_hash_insert(set, set->heap[0]);
  candidate_heap_down(set, 0U);
  return YAP_V2_OK;
}

void YAP_V2_query_request_init(YAP_V2_QUERY_REQUEST *request) {
  if (request == NULL) return;
  memset(request, 0, sizeof(*request)); request->mode = YAP_V2_SEARCH_HYBRID;
  request->scope = YAP_V2_SEARCH_DOCUMENTS; request->query_operator = YAP_V2_QUERY_OR;
  request->top_k = 20U; request->candidate_k = 100U;
  request->lexical_weight = 1.0; request->vector_weight = 1.0;
}

typedef struct {
  const YAP_V2_SEARCH_SNAPSHOT *snapshot;
  const YAP_V2_SEGMENT *documents;
  const YAP_V2_FILTER *filter;
  size_t segment_ordinal;
  int filter_enabled;
} LEXICAL_ACCEPT_CONTEXT;

static int lexical_accept(void *opaque, uint32_t object_type, uint64_t object_ordinal) {
  LEXICAL_ACCEPT_CONTEXT *context = (LEXICAL_ACCEPT_CONTEXT *)opaque;
  YAP_V2_BYTES_VIEW document_id;
  YAP_V2_DOCUMENT_HIT hit;
  if (object_type == YAP_V2_LEXICAL_DOCUMENT) {
    if (object_ordinal >= context->documents->document_count) return 0;
    document_id = context->documents->documents[object_ordinal].id;
  } else if (object_type == YAP_V2_LEXICAL_PASSAGE) {
    if (object_ordinal >= context->documents->passage_count) return 0;
    document_id = context->documents->passages[object_ordinal].parent_document_id;
  } else {
    return 0;
  }
  if (YAP_V2_snapshot_lookup_document(context->snapshot, document_id, &hit) != YAP_V2_OK ||
      hit.segment_ordinal != context->segment_ordinal)
    return 0;
  return filter_matches(context->filter, context->filter_enabled, hit.document_ordinal);
}

static int collect_lexical(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                           const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                           const YAP_V2_LEXICAL_CORPUS_STATS *corpus_stats,
                           const YAP_V2_QUERY_REQUEST *request, CANDIDATE_SET *candidates) {
  YAP_V2_LEXICAL_QUERY_PLAN plan;
  const YAP_V2_LEXICAL_SEGMENT **lexical_segments;
  size_t s;
  int status;
  YAP_V2_lexical_query_plan_init(&plan);
  status = YAP_V2_lexical_query_plan_prepare(request->query, &plan);
  if (status != YAP_V2_OK)
    return status;
  lexical_segments = (const YAP_V2_LEXICAL_SEGMENT **)calloc(
    segment_count, sizeof(*lexical_segments));
  if (lexical_segments == NULL) {
    YAP_V2_lexical_query_plan_free(&plan);
    return YAP_V2_ALLOCATION_FAILED;
  }
  for (s = 0U; s < segment_count; s++)
    lexical_segments[s] = segments[s].lexical;
  status = YAP_V2_lexical_query_plan_bind(&plan, lexical_segments, segment_count);
  free(lexical_segments);
  if (status != YAP_V2_OK) {
    YAP_V2_lexical_query_plan_free(&plan);
    return status;
  }
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    YAP_V2_LEXICAL_SEARCH_OPTIONS options;
    YAP_V2_LEXICAL_HIT *local;
    YAP_V2_FILTER filter;
    LEXICAL_ACCEPT_CONTEXT accept_context;
    size_t local_count, local_limit, i;
    int filter_enabled = request->filter_json.len > 0U;
    if (documents == NULL) { status = YAP_V2_INVALID_ARGUMENT; break; }
    local_limit = request->scope == YAP_V2_SEARCH_DOCUMENTS ? documents->document_count :
                  documents->passage_count;
    if (local_limit > request->candidate_k) local_limit = request->candidate_k;
    if (local_limit == 0U) continue;
    if (segments[s].lexical == NULL) { status = YAP_V2_INVALID_ARGUMENT; break; }
    YAP_V2_filter_init(&filter);
    if (filter_enabled) {
      if (segments[s].metadata == NULL) {
        YAP_V2_filter_free(&filter); status = YAP_V2_INVALID_ARGUMENT; break;
      }
      status = YAP_V2_filter_compile(request->filter_json, segments[s].metadata, &filter);
      if (status != YAP_V2_OK) { YAP_V2_filter_free(&filter); break; }
    }
    local = (YAP_V2_LEXICAL_HIT *)malloc(sizeof(*local) * local_limit);
    if (local == NULL) {
      YAP_V2_filter_free(&filter); status = YAP_V2_ALLOCATION_FAILED; break;
    }
    YAP_V2_lexical_search_options_init(&options);
    options.object_type = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                          YAP_V2_LEXICAL_DOCUMENT : YAP_V2_LEXICAL_PASSAGE;
    options.query_operator = request->query_operator; options.phrase = request->phrase;
    options.top_k = local_limit;
    accept_context.snapshot = snapshot;
    accept_context.documents = documents;
    accept_context.filter = &filter;
    accept_context.segment_ordinal = s;
    accept_context.filter_enabled = filter_enabled;
    options.accept = lexical_accept;
    options.accept_context = &accept_context;
    status = YAP_V2_lexical_search_prepared(&plan, s, corpus_stats, &options,
                                            local, local_limit, &local_count);
    for (i = 0U; status == YAP_V2_OK && i < local_count; i++) {
      CANDIDATE candidate;
      if (local[i].object_type == YAP_V2_LEXICAL_DOCUMENT) {
        if (local[i].object_ordinal >= documents->document_count) { status = YAP_V2_CONFLICT; break; }
        candidate.ordinal = (size_t)local[i].object_ordinal;
        candidate.id = documents->documents[candidate.ordinal].id; candidate.parent = candidate.id;
      } else {
        const YAP_V2_PASSAGE_VIEW *passage;
        if (local[i].object_ordinal >= documents->passage_count) { status = YAP_V2_CONFLICT; break; }
        passage = &documents->passages[local[i].object_ordinal];
        candidate.id = passage->id; candidate.parent = passage->parent_document_id;
        candidate.ordinal = (size_t)local[i].object_ordinal;
      }
      candidate.segment = s; candidate.score = local[i].score;
      status = candidate_set_add(candidates, &candidate);
    }
    free(local); YAP_V2_filter_free(&filter);
    if (status != YAP_V2_OK) break;
  }
  YAP_V2_lexical_query_plan_free(&plan);
  return status;
}

static int collect_vector_base(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                               const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                               const YAP_V2_ANN_CORPUS *corpus,
                               const YAP_V2_ANN_QUERY_PLAN *plan,
                               const YAP_V2_QUERY_REQUEST *request,
                               CANDIDATE_SET *candidates, YAP_V2_QUERY_STATS *stats) {
  CANDIDATE_SET base_candidates;
  YAP_V2_FILTER *filters = NULL;
  unsigned char *filter_states = NULL;
  uint64_t *keys = NULL;
  size_t request_count, key_count = 0U, i;
  int status = YAP_V2_OK;
  int filter_enabled = request->filter_json.len > 0U;
  memset(&base_candidates, 0, sizeof(base_candidates));
  if (corpus == NULL || plan == NULL || corpus->vector_count == 0U) return YAP_V2_OK;
  if (plan->base_segment_count != corpus->segment_count ||
      plan->current_segment_count != segment_count) return YAP_V2_INVALID_ARGUMENT;
  request_count = request->candidate_k > SIZE_MAX / 4U ?
                  corpus->vector_count : request->candidate_k * 4U;
  if (request_count > corpus->vector_count) request_count = corpus->vector_count;
  filters = calloc(segment_count, sizeof(*filters));
  filter_states = calloc(segment_count, sizeof(*filter_states));
  if (filters == NULL || filter_states == NULL) { status = YAP_V2_ALLOCATION_FAILED; goto done; }
  for (i = 0U; i < segment_count; i++) YAP_V2_filter_init(&filters[i]);
  status = candidate_set_init(&base_candidates, request->candidate_k);
  if (status != YAP_V2_OK) goto done;
  for (;;) {
    uint64_t *resized = realloc(keys, sizeof(*keys) * request_count);
    if (resized == NULL) { status = YAP_V2_ALLOCATION_FAILED; break; }
    keys = resized;
    memset(base_candidates.hash, 0,
           sizeof(*base_candidates.hash) * base_candidates.hash_capacity);
    base_candidates.count = 0U;
    status = YAP_V2_ann_corpus_search(corpus, request->query_vector,
                                      request->query_dimensions, request_count,
                                      keys, request_count, &key_count);
    if (stats != NULL) stats->base_search_calls++;
    if (status != YAP_VECTOR_OK && status != YAP_V2_OK) break;
    for (i = 0U; status == YAP_V2_OK && i < key_count; i++) {
      size_t base_segment = (size_t)(keys[i] >> 32U);
      size_t passage_ordinal = (size_t)(keys[i] & UINT64_C(0xffffffff));
      size_t current_segment;
      const YAP_V2_SEGMENT *documents;
      const YAP_V2_VECTOR_SEGMENT *vectors;
      const YAP_V2_PASSAGE_VIEW *passage;
      YAP_V2_DOCUMENT_HIT document_hit;
      CANDIDATE candidate;
      double score;
      if (stats != NULL) stats->candidates_examined++;
      if (base_segment >= plan->base_segment_count ||
          plan->base_to_current[base_segment] == SIZE_MAX) {
        if (stats != NULL) stats->candidates_rejected++;
        continue;
      }
      current_segment = plan->base_to_current[base_segment];
      documents = YAP_V2_snapshot_segment_documents(snapshot, current_segment);
      vectors = segments[current_segment].vector == NULL ? NULL :
                segments[current_segment].vector->vectors;
      if (documents == NULL || vectors == NULL || passage_ordinal >= documents->passage_count ||
          passage_ordinal >= vectors->entry_count ||
          !bytes_equal(documents->passages[passage_ordinal].id,
                       vectors->entries[passage_ordinal].id)) {
        status = YAP_V2_CONFLICT;
        break;
      }
      passage = &documents->passages[passage_ordinal];
      if (YAP_V2_snapshot_lookup_document(snapshot, passage->parent_document_id,
                                          &document_hit) != YAP_V2_OK ||
          document_hit.segment_ordinal != current_segment) {
        if (stats != NULL) stats->candidates_rejected++;
        continue;
      }
      if (filter_enabled && filter_states[current_segment] == 0U) {
        if (segments[current_segment].metadata == NULL) { status = YAP_V2_INVALID_ARGUMENT; break; }
        status = YAP_V2_filter_compile(request->filter_json,
                                       segments[current_segment].metadata,
                                       &filters[current_segment]);
        if (status != YAP_V2_OK) break;
        filter_states[current_segment] = 1U;
      }
      if (!filter_matches(&filters[current_segment], filter_enabled,
                          document_hit.document_ordinal)) {
        if (stats != NULL) stats->candidates_rejected++;
        continue;
      }
      status = YAP_Vector_score(vectors->metric, request->query_vector,
                                vectors->entries[passage_ordinal].values,
                                request->query_dimensions, &score);
      if (status != YAP_VECTOR_OK) break;
      candidate.id = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                     passage->parent_document_id : passage->id;
      candidate.parent = passage->parent_document_id;
      candidate.segment = current_segment;
      candidate.ordinal = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                          document_hit.document_ordinal : passage_ordinal;
      candidate.score = score;
      status = candidate_set_add(&base_candidates, &candidate);
    }
    if (status != YAP_V2_OK) break;
    if (base_candidates.count >= request->candidate_k || request_count == corpus->vector_count)
      break;
    request_count = request_count > corpus->vector_count / 2U ?
                    corpus->vector_count : request_count * 2U;
    if (stats != NULL) stats->retry_search_calls++;
  }
  for (i = 0U; status == YAP_V2_OK && i < base_candidates.count; i++)
    status = candidate_set_add(candidates, &base_candidates.items[i]);
done:
  if (filters != NULL)
    for (i = 0U; i < segment_count; i++) YAP_V2_filter_free(&filters[i]);
  free(filters); free(filter_states); free(keys);
  candidate_set_free(&base_candidates);
  return status;
}

static int collect_vector_segments(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                   const YAP_V2_QUERY_SEGMENT *segments,
                                   size_t segment_count,
                                   const YAP_V2_ANN_QUERY_PLAN *plan,
                                   const YAP_V2_QUERY_REQUEST *request,
                                   CANDIDATE_SET *candidates,
                                   YAP_V2_QUERY_STATS *stats) {
  size_t s;
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    YAP_VECTOR_HIT *local = NULL;
    YAP_V2_FILTER filter;
    CANDIDATE_SET segment_candidates;
    size_t local_count, i, request_count, entry_count;
    int status, filter_enabled = request->filter_json.len > 0U;
    if (plan != NULL && !plan->current_is_delta[s]) continue;
    if (documents == NULL)
      return YAP_V2_INVALID_ARGUMENT;
    if (documents->passage_count == 0U) continue;
    if (segments[s].vector == NULL || segments[s].vector->vectors == NULL)
      return YAP_V2_INVALID_ARGUMENT;
    entry_count = segments[s].vector->vectors->entry_count;
    if (entry_count == 0U) continue;
    request_count = request->candidate_k > SIZE_MAX / 4U ?
                    entry_count : request->candidate_k * 4U;
    if (request_count > entry_count) request_count = entry_count;
    YAP_V2_filter_init(&filter);
    if (filter_enabled) {
      if (segments[s].metadata == NULL) return YAP_V2_INVALID_ARGUMENT;
      status = YAP_V2_filter_compile(request->filter_json, segments[s].metadata, &filter);
      if (status != YAP_V2_OK) return status;
    }
    status = candidate_set_init(&segment_candidates, request->candidate_k);
    if (status != YAP_V2_OK) {
      YAP_V2_filter_free(&filter);
      return status;
    }
    for (;;) {
      YAP_VECTOR_HIT *resized = (YAP_VECTOR_HIT *)realloc(local, sizeof(*local) * request_count);
      if (resized == NULL) {
        status = YAP_V2_ALLOCATION_FAILED;
        break;
      }
      local = resized;
      memset(segment_candidates.hash, 0,
             sizeof(*segment_candidates.hash) * segment_candidates.hash_capacity);
      segment_candidates.count = 0U;
      status = YAP_V2_ann_search(segments[s].vector, request->query_vector,
                                 request->query_dimensions, request_count, local, request_count,
                                 &local_count);
      if (stats != NULL) stats->delta_search_calls++;
      for (i = 0U; status == YAP_VECTOR_OK && i < local_count; i++) {
        const YAP_V2_PASSAGE_VIEW *passage;
        YAP_V2_DOCUMENT_HIT document_hit;
        CANDIDATE candidate;
        size_t passage_ordinal;
        if (stats != NULL) stats->candidates_examined++;
        passage_ordinal = local[i].ordinal;
        if (passage_ordinal >= documents->passage_count ||
            !bytes_equal(documents->passages[passage_ordinal].id, local[i].id)) {
          status = YAP_V2_CONFLICT;
          break;
        }
        passage = &documents->passages[passage_ordinal];
        if (YAP_V2_snapshot_lookup_document(snapshot, passage->parent_document_id,
                                            &document_hit) != YAP_V2_OK ||
            document_hit.segment_ordinal != s ||
            !filter_matches(&filter, filter_enabled, document_hit.document_ordinal))
          { if (stats != NULL) stats->candidates_rejected++; continue; }
        candidate.id = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                       passage->parent_document_id : passage->id;
        candidate.parent = passage->parent_document_id;
        candidate.segment = s;
        candidate.ordinal = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                            document_hit.document_ordinal : passage_ordinal;
        candidate.score = local[i].score;
        status = candidate_set_add(&segment_candidates, &candidate);
      }
      if (status != YAP_VECTOR_OK && status != YAP_V2_OK) break;
      if (segment_candidates.count >= request->candidate_k || request_count == entry_count) break;
      request_count = request_count > entry_count / 2U ? entry_count : request_count * 2U;
      if (stats != NULL) stats->retry_search_calls++;
    }
    for (i = 0U; status == YAP_V2_OK && i < segment_candidates.count; i++)
      status = candidate_set_add(candidates, &segment_candidates.items[i]);
    candidate_set_free(&segment_candidates);
    free(local);
    YAP_V2_filter_free(&filter);
    if (status != YAP_VECTOR_OK && status != YAP_V2_OK) return status;
  }
  return YAP_V2_OK;
}

static int collect_vector(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                          const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                          const YAP_V2_ANN_CORPUS *corpus,
                          const YAP_V2_ANN_QUERY_PLAN *plan,
                          const YAP_V2_QUERY_REQUEST *request, CANDIDATE_SET *candidates,
                          YAP_V2_QUERY_STATS *stats) {
  int status = YAP_V2_OK;
  if (corpus != NULL && plan != NULL)
    status = collect_vector_base(snapshot, segments, segment_count, corpus, plan,
                                 request, candidates, stats);
  if (status == YAP_V2_OK)
    status = collect_vector_segments(snapshot, segments, segment_count,
                                     corpus == NULL ? NULL : plan,
                                     request, candidates, stats);
  return status;
}

static int add_u64(uint64_t *total, uint64_t value) {
  if (*total > UINT64_MAX - value)
    return YAP_V2_OUT_OF_RANGE;
  *total += value;
  return YAP_V2_OK;
}

int YAP_V2_query_corpus_stats_build(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                    const YAP_V2_QUERY_SEGMENT *segments,
                                    size_t segment_count,
                                    YAP_V2_QUERY_CORPUS_STATS *stats) {
  size_t i, field;
  int status = YAP_V2_OK;
  if (snapshot == NULL || segments == NULL || stats == NULL || segment_count == 0U ||
      segment_count != YAP_V2_snapshot_segment_count(snapshot))
    return YAP_V2_INVALID_ARGUMENT;
  memset(stats, 0, sizeof(*stats));
  stats->generation = YAP_V2_snapshot_generation(snapshot);
  for (i = 0U; status == YAP_V2_OK && i < segment_count; i++) {
    const YAP_V2_LEXICAL_SEGMENT *lexical = segments[i].lexical;
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, i);
    if (lexical == NULL)
      continue;
    if (documents == NULL || lexical->document_count != documents->document_count ||
        lexical->passage_count != documents->passage_count)
      return YAP_V2_CONFLICT;
    status = add_u64(&stats->lexical.document_count, lexical->document_count);
    if (status == YAP_V2_OK)
      status = add_u64(&stats->lexical.passage_count, lexical->passage_count);
    for (field = 0U; status == YAP_V2_OK && field < 3U; field++)
      status = add_u64(&stats->lexical.field_token_count[field],
                       lexical->field_token_count[field]);
  }
  if (status != YAP_V2_OK)
    memset(stats, 0, sizeof(*stats));
  return status;
}

int YAP_V2_query_execute_with_ann(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                                  const YAP_V2_QUERY_SEGMENT *segments,
                                  size_t segment_count,
                                  const YAP_V2_QUERY_CORPUS_STATS *stats,
                                  const YAP_V2_ANN_CORPUS *ann_corpus,
                                  const YAP_V2_ANN_QUERY_PLAN *ann_plan,
                                  const YAP_V2_QUERY_REQUEST *request,
                                  YAP_V2_QUERY_HIT *hits, size_t hit_capacity,
                                  size_t *hit_count, YAP_V2_QUERY_STATS *query_stats) {
  CANDIDATE_SET lexical, vector;
  YAP_HYBRID_CANDIDATE *lexical_rrf = NULL, *vector_rrf = NULL;
  YAP_HYBRID_HIT *fused = NULL;
  size_t fused_count = 0U, i, j;
  int status = YAP_V2_OK;
  memset(&lexical, 0, sizeof(lexical));
  memset(&vector, 0, sizeof(vector));
  if (query_stats != NULL) memset(query_stats, 0, sizeof(*query_stats));
  if (snapshot == NULL || segments == NULL || stats == NULL || request == NULL || hits == NULL ||
      hit_count == NULL || stats->generation != YAP_V2_snapshot_generation(snapshot) ||
      segment_count == 0U || segment_count != YAP_V2_snapshot_segment_count(snapshot) ||
      request->top_k == 0U || request->candidate_k < request->top_k || hit_capacity < request->top_k ||
      request->mode < YAP_V2_SEARCH_LEXICAL || request->mode > YAP_V2_SEARCH_HYBRID ||
      request->scope < YAP_V2_SEARCH_DOCUMENTS || request->scope > YAP_V2_SEARCH_PASSAGES)
    return YAP_V2_INVALID_ARGUMENT;
  if ((request->mode == YAP_V2_SEARCH_LEXICAL || request->mode == YAP_V2_SEARCH_HYBRID) &&
      (request->query.data == NULL || request->query.len == 0U)) return YAP_V2_INVALID_ARGUMENT;
  if ((request->mode == YAP_V2_SEARCH_VECTOR || request->mode == YAP_V2_SEARCH_HYBRID) &&
      (request->query_vector == NULL || request->query_dimensions == 0U)) return YAP_V2_INVALID_ARGUMENT;
  status = candidate_set_init(&lexical, request->candidate_k);
  if (status == YAP_V2_OK) status = candidate_set_init(&vector, request->candidate_k);
  lexical_rrf = (YAP_HYBRID_CANDIDATE *)calloc(request->candidate_k, sizeof(*lexical_rrf));
  vector_rrf = (YAP_HYBRID_CANDIDATE *)calloc(request->candidate_k, sizeof(*vector_rrf));
  fused = (YAP_HYBRID_HIT *)calloc(request->top_k, sizeof(*fused));
  if (status != YAP_V2_OK || lexical_rrf == NULL || vector_rrf == NULL || fused == NULL) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  if (request->mode != YAP_V2_SEARCH_VECTOR)
    status = collect_lexical(snapshot, segments, segment_count, &stats->lexical,
                             request, &lexical);
  if (status == YAP_V2_OK && request->mode != YAP_V2_SEARCH_LEXICAL)
    status = collect_vector(snapshot, segments, segment_count, ann_corpus, ann_plan,
                            request, &vector, query_stats);
  if (status != YAP_V2_OK) goto done;
  qsort(lexical.items, lexical.count, sizeof(*lexical.items), candidate_compare);
  qsort(vector.items, vector.count, sizeof(*vector.items), candidate_compare);
  for (i = 0U; i < lexical.count; i++) {
    lexical_rrf[i].id = lexical.items[i].id;
    lexical_rrf[i].score = lexical.items[i].score;
  }
  for (i = 0U; i < vector.count; i++) {
    vector_rrf[i].id = vector.items[i].id;
    vector_rrf[i].score = vector.items[i].score;
  }
  if (lexical.count + vector.count == 0U) { *hit_count = 0U; goto done; }
  status = YAP_Hybrid_fuse_rrf(lexical_rrf, lexical.count, vector_rrf, vector.count,
                               request->mode == YAP_V2_SEARCH_VECTOR ? 0.0 : request->lexical_weight,
                               request->mode == YAP_V2_SEARCH_LEXICAL ? 0.0 : request->vector_weight,
                               request->top_k, fused, request->top_k, &fused_count);
  if (status != YAP_HYBRID_OK) { status = YAP_V2_INVALID_ARGUMENT; goto done; }
  for (i = 0U; i < fused_count; i++) {
    const CANDIDATE *source = NULL;
    for (j = 0U; j < lexical.count; j++)
      if (bytes_equal(lexical.items[j].id, fused[i].id)) {
        source = &lexical.items[j];
        break;
      }
    if (source == NULL)
      for (j = 0U; j < vector.count; j++)
        if (bytes_equal(vector.items[j].id, fused[i].id)) {
          source = &vector.items[j];
          break;
        }
    if (source == NULL) { status = YAP_V2_CONFLICT; goto done; }
    hits[i].id = source->id; hits[i].parent_document_id = source->parent;
    hits[i].segment_ordinal = source->segment; hits[i].object_ordinal = source->ordinal;
    hits[i].lexical_score = fused[i].lexical_score; hits[i].vector_score = fused[i].vector_score;
    hits[i].fused_score = fused[i].fused_score;
  }
  *hit_count = fused_count; status = YAP_V2_OK;
done:
  candidate_set_free(&lexical);
  candidate_set_free(&vector);
  free(lexical_rrf);
  free(vector_rrf);
  free(fused);
  return status;
}

int YAP_V2_query_execute(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                         const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                         const YAP_V2_QUERY_CORPUS_STATS *stats,
                         const YAP_V2_QUERY_REQUEST *request, YAP_V2_QUERY_HIT *hits,
                         size_t hit_capacity, size_t *hit_count) {
  return YAP_V2_query_execute_with_ann(snapshot, segments, segment_count, stats,
                                       NULL, NULL, request, hits, hit_capacity,
                                       hit_count, NULL);
}
