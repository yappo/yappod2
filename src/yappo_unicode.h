#ifndef YAPPO_UNICODE_H
#define YAPPO_UNICODE_H

#include "yappo_index_v2.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t byte_start;
  size_t byte_end;
  uint32_t char_start;
  uint32_t char_end;
} YAP_V2_TOKEN;

typedef struct {
  char *normalized_utf8;
  size_t normalized_bytes;
  YAP_V2_TOKEN *tokens;
  size_t token_count;
} YAP_V2_TOKEN_SEQUENCE;

typedef struct {
  char id[24];
  char *text;
  size_t text_bytes;
  uint32_t ordinal;
  uint32_t start_char;
  uint32_t end_char;
} YAP_V2_CHUNK;

typedef struct {
  YAP_V2_CHUNK *chunks;
  size_t chunk_count;
} YAP_V2_CHUNK_SEQUENCE;

void YAP_V2_token_sequence_free(YAP_V2_TOKEN_SEQUENCE *sequence);
int YAP_V2_unicode_tokenize(const char *utf8, size_t utf8_bytes,
                            YAP_V2_TOKEN_SEQUENCE *sequence);
void YAP_V2_chunk_sequence_free(YAP_V2_CHUNK_SEQUENCE *sequence);
int YAP_V2_unicode_chunk(const char *document_id, const char *utf8, size_t utf8_bytes,
                         uint32_t max_chars, uint32_t overlap_chars,
                         YAP_V2_CHUNK_SEQUENCE *sequence);

#endif
