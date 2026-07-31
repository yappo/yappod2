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
                           const YAP_V2_QUERY_REQUEST *request, CANDIDATE_SET *candidates) {
  YAP_V2_LEXICAL_QUERY_PLAN plan;
  size_t s;
  int status;
  YAP_V2_lexical_query_plan_init(&plan);
  status = YAP_V2_lexical_query_plan_prepare(request->query, &plan);
  if (status != YAP_V2_OK)
    return status;
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
    status = YAP_V2_lexical_search_prepared(segments[s].lexical, &plan, &options,
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

static int collect_vector(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                          const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                          const YAP_V2_QUERY_REQUEST *request, CANDIDATE_SET *candidates) {
  size_t s;
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    YAP_VECTOR_HIT *local = NULL;
    YAP_V2_FILTER filter;
    CANDIDATE_SET segment_candidates;
    size_t local_count, i, request_count, entry_count;
    int status, filter_enabled = request->filter_json.len > 0U;
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
      for (i = 0U; status == YAP_VECTOR_OK && i < local_count; i++) {
        const YAP_V2_PASSAGE_VIEW *passage;
        YAP_V2_DOCUMENT_HIT document_hit;
        CANDIDATE candidate;
        size_t passage_ordinal;
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
          continue;
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

int YAP_V2_query_execute(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                         const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                         const YAP_V2_QUERY_REQUEST *request, YAP_V2_QUERY_HIT *hits,
                         size_t hit_capacity, size_t *hit_count) {
  CANDIDATE_SET lexical, vector;
  YAP_HYBRID_CANDIDATE *lexical_rrf = NULL, *vector_rrf = NULL;
  YAP_HYBRID_HIT *fused = NULL;
  size_t fused_count = 0U, i, j;
  int status = YAP_V2_OK;
  memset(&lexical, 0, sizeof(lexical));
  memset(&vector, 0, sizeof(vector));
  if (snapshot == NULL || segments == NULL || request == NULL || hits == NULL || hit_count == NULL ||
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
    status = collect_lexical(snapshot, segments, segment_count, request, &lexical);
  if (status == YAP_V2_OK && request->mode != YAP_V2_SEARCH_LEXICAL)
    status = collect_vector(snapshot, segments, segment_count, request, &vector);
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
