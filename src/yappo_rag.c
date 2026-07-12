/* Borrowed passage lookup helpers for RAG context assembly. */
#include "yappo_rag.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int bytes_equal(YAP_V2_BYTES_VIEW left, YAP_V2_BYTES_VIEW right) {
  return left.len == right.len && (left.len == 0U ||
                                   (left.data != NULL && right.data != NULL &&
                                    memcmp(left.data, right.data, left.len) == 0));
}

const char *YAP_RAG_status_string(YAP_RAG_STATUS status) {
  switch (status) {
    case YAP_RAG_OK:
      return "ok";
    case YAP_RAG_INVALID_ARGUMENT:
      return "invalid argument";
    case YAP_RAG_NOT_FOUND:
      return "not found";
    case YAP_RAG_BUFFER_TOO_SMALL:
      return "buffer too small";
    case YAP_RAG_ALLOCATION_FAILED:
      return "allocation failed";
    default:
      return "unknown status";
  }
}

int YAP_RAG_find_passage(const YAP_V2_SEGMENT *segment, YAP_V2_BYTES_VIEW passage_id,
                         const YAP_V2_PASSAGE_VIEW **passage_out) {
  size_t i;

  if (segment == NULL || passage_out == NULL || passage_id.data == NULL || passage_id.len == 0U ||
      (segment->passage_count > 0U && segment->passages == NULL)) {
    return YAP_RAG_INVALID_ARGUMENT;
  }
  *passage_out = NULL;
  for (i = 0U; i < segment->passage_count; i++) {
    if (bytes_equal(segment->passages[i].id, passage_id)) {
      *passage_out = &segment->passages[i];
      return YAP_RAG_OK;
    }
  }
  return YAP_RAG_NOT_FOUND;
}

static int compare_passages(const void *left, const void *right) {
  const YAP_V2_PASSAGE_VIEW *const *left_passage = (const YAP_V2_PASSAGE_VIEW *const *)left;
  const YAP_V2_PASSAGE_VIEW *const *right_passage = (const YAP_V2_PASSAGE_VIEW *const *)right;

  if ((*left_passage)->ordinal < (*right_passage)->ordinal) return -1;
  if ((*left_passage)->ordinal > (*right_passage)->ordinal) return 1;
  return 0;
}

int YAP_RAG_list_passages(const YAP_V2_SEGMENT *segment, YAP_V2_BYTES_VIEW document_id,
                          const YAP_V2_PASSAGE_VIEW **passages, size_t passage_capacity,
                          size_t *passage_count_out) {
  const YAP_V2_PASSAGE_VIEW **selected;
  size_t matching = 0U;
  size_t i;

  if (segment == NULL || passage_count_out == NULL || document_id.data == NULL ||
      document_id.len == 0U || (passage_capacity > 0U && passages == NULL)) {
    return YAP_RAG_INVALID_ARGUMENT;
  }
  for (i = 0U; i < segment->passage_count; i++) {
    if (bytes_equal(segment->passages[i].parent_document_id, document_id)) {
      matching++;
    }
  }
  *passage_count_out = matching;
  if (matching == 0U) {
    return YAP_RAG_OK;
  }
  if (passage_capacity < matching) {
    return YAP_RAG_BUFFER_TOO_SMALL;
  }
  if (matching > SIZE_MAX / sizeof(*selected)) {
    return YAP_RAG_ALLOCATION_FAILED;
  }
  selected = (const YAP_V2_PASSAGE_VIEW **)malloc(sizeof(*selected) * matching);
  if (selected == NULL) {
    return YAP_RAG_ALLOCATION_FAILED;
  }
  matching = 0U;
  for (i = 0U; i < segment->passage_count; i++) {
    if (bytes_equal(segment->passages[i].parent_document_id, document_id)) {
      selected[matching++] = &segment->passages[i];
    }
  }
  qsort(selected, matching, sizeof(*selected), compare_passages);
  memcpy(passages, selected, sizeof(*selected) * matching);
  free(selected);
  return YAP_RAG_OK;
}
