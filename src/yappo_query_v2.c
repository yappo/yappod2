#include "yappo_query_v2.h"

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

static const YAP_V2_PASSAGE_VIEW *passage_by_id(const YAP_V2_SEGMENT *segment,
                                                YAP_V2_BYTES_VIEW id, size_t *ordinal) {
  size_t i;
  for (i = 0U; i < segment->passage_count; i++)
    if (bytes_equal(segment->passages[i].id, id)) {
      if (ordinal != NULL) *ordinal = i;
      return &segment->passages[i];
    }
  return NULL;
}

static size_t document_ordinal(const YAP_V2_SEGMENT *segment, YAP_V2_BYTES_VIEW id) {
  size_t i;
  for (i = 0U; i < segment->document_count; i++)
    if (bytes_equal(segment->documents[i].id, id)) return i;
  return SIZE_MAX;
}

static int filter_matches(const YAP_V2_FILTER *filter, int enabled, size_t ordinal) {
  int matches = 1;
  return !enabled || (YAP_V2_filter_matches(filter, ordinal, &matches) == YAP_V2_OK && matches);
}

static int candidate_add(CANDIDATE *items, size_t capacity, size_t *count,
                         const CANDIDATE *candidate) {
  size_t i;
  for (i = 0U; i < *count; i++)
    if (bytes_equal(items[i].id, candidate->id)) {
      if (candidate->score > items[i].score) items[i] = *candidate;
      return YAP_V2_OK;
    }
  if (*count >= capacity) return YAP_V2_OUT_OF_RANGE;
  items[(*count)++] = *candidate;
  return YAP_V2_OK;
}

void YAP_V2_query_request_init(YAP_V2_QUERY_REQUEST *request) {
  if (request == NULL) return;
  memset(request, 0, sizeof(*request)); request->mode = YAP_V2_SEARCH_HYBRID;
  request->scope = YAP_V2_SEARCH_DOCUMENTS; request->query_operator = YAP_V2_QUERY_OR;
  request->top_k = 20U; request->candidate_k = 100U;
  request->lexical_weight = 1.0; request->vector_weight = 1.0;
}

static int collect_lexical(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                           const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                           const YAP_V2_QUERY_REQUEST *request, CANDIDATE *candidates,
                           size_t capacity, size_t *candidate_count) {
  size_t s;
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    YAP_V2_LEXICAL_SEARCH_OPTIONS options;
    YAP_V2_LEXICAL_HIT *local;
    YAP_V2_FILTER filter;
    size_t local_count, local_limit, accepted = 0U, i;
    int status, filter_enabled = request->filter_json.len > 0U;
    if (segments[s].lexical == NULL || documents == NULL) return YAP_V2_INVALID_ARGUMENT;
    YAP_V2_filter_init(&filter);
    if (filter_enabled) {
      if (segments[s].metadata == NULL) return YAP_V2_INVALID_ARGUMENT;
      status = YAP_V2_filter_compile(request->filter_json, segments[s].metadata, &filter);
      if (status != YAP_V2_OK) return status;
    }
    local_limit = request->scope == YAP_V2_SEARCH_DOCUMENTS ? documents->document_count :
                  documents->passage_count;
    if (local_limit == 0U) { YAP_V2_filter_free(&filter); continue; }
    local = (YAP_V2_LEXICAL_HIT *)malloc(sizeof(*local) * local_limit);
    if (local == NULL) { YAP_V2_filter_free(&filter); return YAP_V2_ALLOCATION_FAILED; }
    YAP_V2_lexical_search_options_init(&options);
    options.object_type = request->scope == YAP_V2_SEARCH_DOCUMENTS ?
                          YAP_V2_LEXICAL_DOCUMENT : YAP_V2_LEXICAL_PASSAGE;
    options.query_operator = request->query_operator; options.phrase = request->phrase;
    options.top_k = local_limit;
    status = YAP_V2_lexical_search(segments[s].lexical, request->query, &options, local,
                                   local_limit, &local_count);
    for (i = 0U; status == YAP_V2_OK && i < local_count; i++) {
      CANDIDATE candidate;
      size_t doc_ordinal;
      if (local[i].object_type == YAP_V2_LEXICAL_DOCUMENT) {
        if (local[i].object_ordinal >= documents->document_count) { status = YAP_V2_CONFLICT; break; }
        doc_ordinal = (size_t)local[i].object_ordinal;
        candidate.id = documents->documents[doc_ordinal].id; candidate.parent = candidate.id;
        candidate.ordinal = doc_ordinal;
      } else {
        const YAP_V2_PASSAGE_VIEW *passage;
        if (local[i].object_ordinal >= documents->passage_count) { status = YAP_V2_CONFLICT; break; }
        passage = &documents->passages[local[i].object_ordinal];
        doc_ordinal = document_ordinal(documents, passage->parent_document_id);
        if (doc_ordinal == SIZE_MAX) { status = YAP_V2_CONFLICT; break; }
        candidate.id = passage->id; candidate.parent = passage->parent_document_id;
        candidate.ordinal = (size_t)local[i].object_ordinal;
      }
      if (!YAP_V2_snapshot_document_visible(snapshot, s, candidate.parent) ||
          !filter_matches(&filter, filter_enabled, doc_ordinal)) continue;
      candidate.segment = s; candidate.score = local[i].score;
      status = candidate_add(candidates, capacity, candidate_count, &candidate);
      accepted++;
      if (accepted == request->candidate_k) break;
    }
    free(local); YAP_V2_filter_free(&filter);
    if (status != YAP_V2_OK) return status;
  }
  return YAP_V2_OK;
}

static int collect_vector(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                          const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                          const YAP_V2_QUERY_REQUEST *request, CANDIDATE *candidates,
                          size_t capacity, size_t *candidate_count) {
  size_t s;
  for (s = 0U; s < segment_count; s++) {
    const YAP_V2_SEGMENT *documents = YAP_V2_snapshot_segment_documents(snapshot, s);
    YAP_VECTOR_HIT *local;
    YAP_V2_FILTER filter;
    size_t local_count, accepted = 0U, i, request_count;
    int status, filter_enabled = request->filter_json.len > 0U;
    if (segments[s].vector == NULL || segments[s].vector->vectors == NULL || documents == NULL)
      return YAP_V2_INVALID_ARGUMENT;
    request_count = segments[s].vector->vectors->entry_count;
    if (request_count == 0U) continue;
    YAP_V2_filter_init(&filter);
    if (filter_enabled) {
      if (segments[s].metadata == NULL) return YAP_V2_INVALID_ARGUMENT;
      status = YAP_V2_filter_compile(request->filter_json, segments[s].metadata, &filter);
      if (status != YAP_V2_OK) return status;
    }
    local = (YAP_VECTOR_HIT *)malloc(sizeof(*local) * request_count);
    if (local == NULL) { YAP_V2_filter_free(&filter); return YAP_V2_ALLOCATION_FAILED; }
    status = YAP_V2_ann_search(segments[s].vector, request->query_vector,
                               request->query_dimensions, request_count, local, request_count,
                               &local_count);
    for (i = 0U; status == YAP_VECTOR_OK && i < local_count; i++) {
      const YAP_V2_PASSAGE_VIEW *passage;
      CANDIDATE candidate;
      size_t passage_ordinal, doc_ordinal;
      passage = passage_by_id(documents, local[i].id, &passage_ordinal);
      if (passage == NULL) { status = YAP_V2_CONFLICT; break; }
      doc_ordinal = document_ordinal(documents, passage->parent_document_id);
      if (doc_ordinal == SIZE_MAX) { status = YAP_V2_CONFLICT; break; }
      if (!YAP_V2_snapshot_document_visible(snapshot, s, passage->parent_document_id) ||
          !filter_matches(&filter, filter_enabled, doc_ordinal)) continue;
      candidate.id = request->scope == YAP_V2_SEARCH_DOCUMENTS ? passage->parent_document_id : passage->id;
      candidate.parent = passage->parent_document_id; candidate.segment = s;
      candidate.ordinal = request->scope == YAP_V2_SEARCH_DOCUMENTS ? doc_ordinal : passage_ordinal;
      candidate.score = local[i].score;
      status = candidate_add(candidates, capacity, candidate_count, &candidate);
      accepted++;
      if (accepted == request->candidate_k) break;
    }
    free(local); YAP_V2_filter_free(&filter);
    if (status != YAP_VECTOR_OK && status != YAP_V2_OK) return status;
  }
  return YAP_V2_OK;
}

int YAP_V2_query_execute(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                         const YAP_V2_QUERY_SEGMENT *segments, size_t segment_count,
                         const YAP_V2_QUERY_REQUEST *request, YAP_V2_QUERY_HIT *hits,
                         size_t hit_capacity, size_t *hit_count) {
  CANDIDATE *lexical = NULL, *vector = NULL;
  YAP_HYBRID_CANDIDATE *lexical_rrf = NULL, *vector_rrf = NULL;
  YAP_HYBRID_HIT *fused = NULL;
  size_t capacity, lexical_count = 0U, vector_count = 0U, fused_count = 0U, i, j;
  int status = YAP_V2_OK;
  if (snapshot == NULL || segments == NULL || request == NULL || hits == NULL || hit_count == NULL ||
      segment_count == 0U || segment_count != YAP_V2_snapshot_segment_count(snapshot) ||
      request->top_k == 0U || request->candidate_k < request->top_k || hit_capacity < request->top_k ||
      request->mode < YAP_V2_SEARCH_LEXICAL || request->mode > YAP_V2_SEARCH_HYBRID ||
      request->scope < YAP_V2_SEARCH_DOCUMENTS || request->scope > YAP_V2_SEARCH_PASSAGES ||
      segment_count > SIZE_MAX / request->candidate_k) return YAP_V2_INVALID_ARGUMENT;
  if ((request->mode == YAP_V2_SEARCH_LEXICAL || request->mode == YAP_V2_SEARCH_HYBRID) &&
      (request->query.data == NULL || request->query.len == 0U)) return YAP_V2_INVALID_ARGUMENT;
  if ((request->mode == YAP_V2_SEARCH_VECTOR || request->mode == YAP_V2_SEARCH_HYBRID) &&
      (request->query_vector == NULL || request->query_dimensions == 0U)) return YAP_V2_INVALID_ARGUMENT;
  capacity = segment_count * request->candidate_k;
  lexical = (CANDIDATE *)calloc(capacity, sizeof(*lexical));
  vector = (CANDIDATE *)calloc(capacity, sizeof(*vector));
  lexical_rrf = (YAP_HYBRID_CANDIDATE *)calloc(capacity, sizeof(*lexical_rrf));
  vector_rrf = (YAP_HYBRID_CANDIDATE *)calloc(capacity, sizeof(*vector_rrf));
  fused = (YAP_HYBRID_HIT *)calloc(request->top_k, sizeof(*fused));
  if (lexical == NULL || vector == NULL || lexical_rrf == NULL || vector_rrf == NULL || fused == NULL) {
    status = YAP_V2_ALLOCATION_FAILED; goto done;
  }
  if (request->mode != YAP_V2_SEARCH_VECTOR)
    status = collect_lexical(snapshot, segments, segment_count, request, lexical, capacity, &lexical_count);
  if (status == YAP_V2_OK && request->mode != YAP_V2_SEARCH_LEXICAL)
    status = collect_vector(snapshot, segments, segment_count, request, vector, capacity, &vector_count);
  if (status != YAP_V2_OK) goto done;
  qsort(lexical, lexical_count, sizeof(*lexical), candidate_compare);
  qsort(vector, vector_count, sizeof(*vector), candidate_compare);
  for (i = 0U; i < lexical_count; i++) { lexical_rrf[i].id = lexical[i].id; lexical_rrf[i].score = lexical[i].score; }
  for (i = 0U; i < vector_count; i++) { vector_rrf[i].id = vector[i].id; vector_rrf[i].score = vector[i].score; }
  if (lexical_count + vector_count == 0U) { *hit_count = 0U; goto done; }
  status = YAP_Hybrid_fuse_rrf(lexical_rrf, lexical_count, vector_rrf, vector_count,
                               request->mode == YAP_V2_SEARCH_VECTOR ? 0.0 : request->lexical_weight,
                               request->mode == YAP_V2_SEARCH_LEXICAL ? 0.0 : request->vector_weight,
                               request->top_k, fused, request->top_k, &fused_count);
  if (status != YAP_HYBRID_OK) { status = YAP_V2_INVALID_ARGUMENT; goto done; }
  for (i = 0U; i < fused_count; i++) {
    const CANDIDATE *source = NULL;
    for (j = 0U; j < lexical_count; j++) if (bytes_equal(lexical[j].id, fused[i].id)) { source = &lexical[j]; break; }
    if (source == NULL) for (j = 0U; j < vector_count; j++) if (bytes_equal(vector[j].id, fused[i].id)) { source = &vector[j]; break; }
    if (source == NULL) { status = YAP_V2_CONFLICT; goto done; }
    hits[i].id = source->id; hits[i].parent_document_id = source->parent;
    hits[i].segment_ordinal = source->segment; hits[i].object_ordinal = source->ordinal;
    hits[i].lexical_score = fused[i].lexical_score; hits[i].vector_score = fused[i].vector_score;
    hits[i].fused_score = fused[i].fused_score;
  }
  *hit_count = fused_count; status = YAP_V2_OK;
done:
  free(lexical); free(vector); free(lexical_rrf); free(vector_rrf); free(fused); return status;
}
