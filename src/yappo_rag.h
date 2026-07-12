/* Borrowed passage lookup helpers for RAG context assembly. */
#ifndef YAPPO_RAG_H
#define YAPPO_RAG_H

#include <stddef.h>

#include "yappo_index_v2.h"

typedef enum {
  YAP_RAG_OK = 0,
  YAP_RAG_INVALID_ARGUMENT = -1,
  YAP_RAG_NOT_FOUND = -2,
  YAP_RAG_BUFFER_TOO_SMALL = -3,
  YAP_RAG_ALLOCATION_FAILED = -4
} YAP_RAG_STATUS;

const char *YAP_RAG_status_string(YAP_RAG_STATUS status);

/* The returned passage is borrowed from segment and remains valid until segment_free. */
int YAP_RAG_find_passage(const YAP_V2_SEGMENT *segment, YAP_V2_BYTES_VIEW passage_id,
                         const YAP_V2_PASSAGE_VIEW **passage_out);

/* Returns passages for one document in ordinal order, without copying their text. */
int YAP_RAG_list_passages(const YAP_V2_SEGMENT *segment, YAP_V2_BYTES_VIEW document_id,
                          const YAP_V2_PASSAGE_VIEW **passages, size_t passage_capacity,
                          size_t *passage_count_out);

#endif
