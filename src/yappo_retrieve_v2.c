#include "yappo_retrieve_v2.h"

#include <stdint.h>
#include <string.h>

static int bytes_equal(YAP_V2_BYTES_VIEW a, YAP_V2_BYTES_VIEW b) {
  return a.len == b.len && a.data != NULL && b.data != NULL && memcmp(a.data, b.data, a.len) == 0;
}

static const YAP_V2_DOCUMENT_VIEW *find_document(const YAP_V2_SEGMENT *segment,
                                                 YAP_V2_BYTES_VIEW id) {
  size_t i;
  for (i = 0U; i < segment->document_count; i++)
    if (bytes_equal(segment->documents[i].id, id)) return &segment->documents[i];
  return NULL;
}

static size_t selected_for_document(const YAP_V2_CITATION *citations, size_t count,
                                    YAP_V2_BYTES_VIEW document_id) {
  size_t i, selected = 0U;
  for (i = 0U; i < count; i++)
    if (bytes_equal(citations[i].document_id, document_id)) selected++;
  return selected;
}

static int passage_selected(const YAP_V2_CITATION *citations, size_t count,
                            YAP_V2_BYTES_VIEW passage_id) {
  size_t i;
  for (i = 0U; i < count; i++)
    if (bytes_equal(citations[i].passage_id, passage_id)) return 1;
  return 0;
}

void YAP_V2_retrieve_options_init(YAP_V2_RETRIEVE_OPTIONS *options) {
  if (options == NULL) return;
  options->max_passages = 10U;
  options->max_passages_per_document = 3U;
  options->max_context_bytes = 16384U;
}

int YAP_V2_retrieve_context(const YAP_V2_SEARCH_SNAPSHOT *snapshot,
                            const YAP_V2_QUERY_HIT *ranked_passages, size_t ranked_count,
                            const YAP_V2_RETRIEVE_OPTIONS *options,
                            unsigned char *context, size_t context_capacity,
                            size_t *context_bytes, YAP_V2_CITATION *citations,
                            size_t citation_capacity, size_t *citation_count) {
  size_t used = 0U, selected = 0U, i;
  if (context_bytes != NULL) *context_bytes = 0U;
  if (citation_count != NULL) *citation_count = 0U;
  if (snapshot == NULL || (ranked_count != 0U && ranked_passages == NULL) || options == NULL ||
      context == NULL || context_bytes == NULL || citations == NULL || citation_count == NULL ||
      options->max_passages == 0U || options->max_passages_per_document == 0U ||
      options->max_context_bytes == 0U || context_capacity < options->max_context_bytes ||
      citation_capacity < options->max_passages)
    return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < ranked_count && selected < options->max_passages; i++) {
    const YAP_V2_QUERY_HIT *hit = &ranked_passages[i];
    const YAP_V2_SEGMENT *segment;
    const YAP_V2_PASSAGE_VIEW *passage;
    const YAP_V2_DOCUMENT_VIEW *document;
    size_t separator = selected == 0U ? 0U : 2U;
    if (hit->segment_ordinal >= YAP_V2_snapshot_segment_count(snapshot)) return YAP_V2_CONFLICT;
    segment = YAP_V2_snapshot_segment_documents(snapshot, hit->segment_ordinal);
    if (segment == NULL || hit->object_ordinal >= segment->passage_count) return YAP_V2_CONFLICT;
    passage = &segment->passages[hit->object_ordinal];
    if (!bytes_equal(passage->id, hit->id) ||
        !bytes_equal(passage->parent_document_id, hit->parent_document_id)) return YAP_V2_CONFLICT;
    document = find_document(segment, passage->parent_document_id);
    if (document == NULL) return YAP_V2_CONFLICT;
    if (!YAP_V2_snapshot_document_visible(snapshot, hit->segment_ordinal, document->id) ||
        passage_selected(citations, selected, passage->id) ||
        selected_for_document(citations, selected, document->id) >= options->max_passages_per_document)
      continue;
    if (passage->text.len > options->max_context_bytes - used ||
        separator > options->max_context_bytes - used - passage->text.len) continue;
    if (separator != 0U) { context[used++] = '\n'; context[used++] = '\n'; }
    citations[selected].context_start = used;
    memcpy(context + used, passage->text.data, passage->text.len); used += passage->text.len;
    citations[selected].context_end = used;
    citations[selected].passage_id = passage->id;
    citations[selected].document_id = document->id;
    citations[selected].url = document->url;
    citations[selected].title = document->title;
    citations[selected].text = passage->text;
    citations[selected].start_char = passage->start_char;
    citations[selected].end_char = passage->end_char;
    citations[selected].lexical_score = hit->lexical_score;
    citations[selected].vector_score = hit->vector_score;
    citations[selected].fused_score = hit->fused_score;
    selected++;
  }
  *context_bytes = used; *citation_count = selected; return YAP_V2_OK;
}
