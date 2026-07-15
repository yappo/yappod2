#ifndef YAPPO_LEXICAL_V2_H
#define YAPPO_LEXICAL_V2_H

#include "yappo_index_v2.h"

#include <stddef.h>
#include <stdint.h>

#define YAP_V2_LEXICAL_PAYLOAD_VERSION UINT32_C(1)
#define YAP_V2_POSTINGS_BLOCK_SIZE 128U

typedef enum { YAP_V2_LEXICAL_DOCUMENT = 1, YAP_V2_LEXICAL_PASSAGE = 2 } YAP_V2_LEXICAL_OBJECT_TYPE;

typedef enum {
  YAP_V2_FIELD_TITLE = 1,
  YAP_V2_FIELD_BODY = 2,
  YAP_V2_FIELD_PASSAGE = 3
} YAP_V2_LEXICAL_FIELD;

typedef struct {
  char *term;
  size_t term_len;
  uint32_t object_type;
  uint64_t object_ordinal;
  uint32_t field;
  uint32_t position;
  uint32_t field_length;
} YAP_V2_LEXICAL_OCCURRENCE;

typedef struct {
  YAP_V2_LEXICAL_OCCURRENCE *items;
  size_t count;
  size_t capacity;
  uint64_t field_token_count[3];
  size_t document_count;
  size_t passage_count;
} YAP_V2_LEXICAL_PREPARED;

typedef struct {
  YAP_V2_BYTES_VIEW term;
  uint64_t document_frequency;
  uint64_t postings_offset;
  uint64_t postings_bytes;
  uint64_t positions_offset;
  uint64_t positions_bytes;
} YAP_V2_TERM_ENTRY;

typedef struct {
  uint32_t object_type;
  uint64_t object_ordinal;
  uint32_t term_frequency[3];
  uint32_t field_length[3];
  uint64_t position_offset;
  uint32_t position_count;
} YAP_V2_POSTING;

typedef struct {
  uint32_t first_posting;
  uint32_t posting_count;
  uint32_t max_term_frequency;
  uint32_t min_field_length;
} YAP_V2_POSTINGS_BLOCK;

typedef struct {
  uint32_t field;
  uint32_t position;
} YAP_V2_POSITION;

typedef struct {
  void *maps[3];
  size_t map_bytes[3];
  uint64_t generation;
  uint64_t document_count;
  uint64_t passage_count;
  uint64_t posting_count;
  uint64_t position_count;
  uint64_t field_token_count[3];
  YAP_V2_TERM_ENTRY *terms;
  size_t term_count;
} YAP_V2_LEXICAL_SEGMENT;

typedef struct {
  const YAP_V2_LEXICAL_SEGMENT *segment;
  const YAP_V2_TERM_ENTRY *term;
  size_t offset;
  size_t index;
  size_t blocks_offset;
} YAP_V2_POSTING_ITERATOR;

typedef struct {
  const YAP_V2_LEXICAL_SEGMENT *segment;
  const YAP_V2_TERM_ENTRY *term;
  size_t offset;
  size_t index;
} YAP_V2_POSITION_ITERATOR;

int YAP_V2_lexical_write(const char *segment_dir, uint64_t generation,
                         const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                         const YAP_V2_PASSAGE_VIEW *passages, size_t passage_count,
                         YAP_V2_COMPONENT_DESCRIPTOR components[3]);
void YAP_V2_lexical_prepared_init(YAP_V2_LEXICAL_PREPARED *prepared);
void YAP_V2_lexical_prepared_free(YAP_V2_LEXICAL_PREPARED *prepared);
int YAP_V2_lexical_prepare_unit(const YAP_V2_DOCUMENT_VIEW *document,
                                const YAP_V2_PASSAGE_VIEW *passages,
                                size_t passage_count,
                                YAP_V2_LEXICAL_PREPARED *prepared);
int YAP_V2_lexical_write_prepared(const char *segment_dir, uint64_t generation,
                                  const YAP_V2_LEXICAL_PREPARED *const *prepared,
                                  size_t prepared_count,
                                  YAP_V2_COMPONENT_DESCRIPTOR components[3]);
void YAP_V2_lexical_segment_init(YAP_V2_LEXICAL_SEGMENT *segment);
void YAP_V2_lexical_segment_close(YAP_V2_LEXICAL_SEGMENT *segment);
int YAP_V2_lexical_segment_open(const char *segment_dir, uint64_t expected_generation,
                                YAP_V2_LEXICAL_SEGMENT *segment);
const YAP_V2_TERM_ENTRY *YAP_V2_lexical_term_find(const YAP_V2_LEXICAL_SEGMENT *segment,
                                                  YAP_V2_BYTES_VIEW term);
int YAP_V2_posting_iterator_init(const YAP_V2_LEXICAL_SEGMENT *segment,
                                 const YAP_V2_TERM_ENTRY *term, YAP_V2_POSTING_ITERATOR *iterator);
int YAP_V2_posting_iterator_next(YAP_V2_POSTING_ITERATOR *iterator, YAP_V2_POSTING *posting);
int YAP_V2_posting_iterator_block(const YAP_V2_POSTING_ITERATOR *iterator, size_t block_index,
                                  YAP_V2_POSTINGS_BLOCK *block);
int YAP_V2_position_iterator_init(const YAP_V2_LEXICAL_SEGMENT *segment,
                                  const YAP_V2_TERM_ENTRY *term,
                                  YAP_V2_POSITION_ITERATOR *iterator);
int YAP_V2_position_iterator_next(YAP_V2_POSITION_ITERATOR *iterator, YAP_V2_POSITION *position);
int YAP_V2_posting_position_at(const YAP_V2_LEXICAL_SEGMENT *segment, const YAP_V2_TERM_ENTRY *term,
                               const YAP_V2_POSTING *posting, size_t index,
                               YAP_V2_POSITION *position);

#endif
