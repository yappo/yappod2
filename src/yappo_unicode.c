#include "yappo_unicode.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unicode/ubrk.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

typedef struct {
  UChar *text;
  int32_t length;
  int32_t *byte_offsets;
  uint32_t *char_offsets;
  char *utf8;
  size_t utf8_bytes;
} NORMALIZED_TEXT;

static void normalized_free(NORMALIZED_TEXT *text) {
  if (text == NULL) return;
  free(text->text); free(text->byte_offsets); free(text->char_offsets); free(text->utf8);
  memset(text, 0, sizeof(*text));
}

static int normalize_text(const char *utf8, size_t bytes, NORMALIZED_TEXT *out) {
  const UNormalizer2 *normalizer;
  UChar *source = NULL;
  int32_t source_length = 0, normalized_length, utf8_length = 0;
  int32_t i, byte_offset = 0;
  uint32_t char_offset = 0;
  UErrorCode error = U_ZERO_ERROR;
  if (utf8 == NULL || out == NULL || bytes > INT32_MAX) return YAP_V2_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  u_strFromUTF8(NULL, 0, &source_length, utf8, (int32_t)bytes, &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) return YAP_V2_INVALID_FORMAT;
  error = U_ZERO_ERROR;
  source = (UChar *)malloc(((size_t)source_length + 1U) * sizeof(*source));
  if (source == NULL) return YAP_V2_ALLOCATION_FAILED;
  u_strFromUTF8(source, source_length + 1, NULL, utf8, (int32_t)bytes, &error);
  if (U_FAILURE(error)) { free(source); return YAP_V2_INVALID_FORMAT; }
  normalizer = unorm2_getNFKCCasefoldInstance(&error);
  if (U_FAILURE(error)) { free(source); return YAP_V2_INVALID_FORMAT; }
  normalized_length = unorm2_normalize(normalizer, source, source_length, NULL, 0, &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) { free(source); return YAP_V2_INVALID_FORMAT; }
  error = U_ZERO_ERROR;
  out->text = (UChar *)malloc(((size_t)normalized_length + 1U) * sizeof(*out->text));
  if (out->text == NULL) { free(source); return YAP_V2_ALLOCATION_FAILED; }
  out->length = unorm2_normalize(normalizer, source, source_length, out->text,
                                 normalized_length + 1, &error);
  free(source);
  if (U_FAILURE(error)) { normalized_free(out); return YAP_V2_INVALID_FORMAT; }
  u_strToUTF8(NULL, 0, &utf8_length, out->text, out->length, &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) { normalized_free(out); return YAP_V2_INVALID_FORMAT; }
  error = U_ZERO_ERROR;
  out->utf8 = (char *)malloc((size_t)utf8_length + 1U);
  out->byte_offsets = (int32_t *)calloc((size_t)out->length + 1U, sizeof(*out->byte_offsets));
  out->char_offsets = (uint32_t *)calloc((size_t)out->length + 1U, sizeof(*out->char_offsets));
  if (out->utf8 == NULL || out->byte_offsets == NULL || out->char_offsets == NULL) {
    normalized_free(out); return YAP_V2_ALLOCATION_FAILED;
  }
  u_strToUTF8(out->utf8, utf8_length + 1, NULL, out->text, out->length, &error);
  if (U_FAILURE(error)) { normalized_free(out); return YAP_V2_INVALID_FORMAT; }
  out->utf8_bytes = (size_t)utf8_length;
  i = 0;
  while (i < out->length) {
    int32_t start = i;
    UChar32 cp;
    U16_NEXT(out->text, i, out->length, cp);
    out->byte_offsets[start] = byte_offset;
    out->char_offsets[start] = char_offset;
    if (i - start == 2) { out->byte_offsets[start + 1] = byte_offset; out->char_offsets[start + 1] = char_offset; }
    byte_offset += U8_LENGTH(cp); char_offset++;
    out->byte_offsets[i] = byte_offset; out->char_offsets[i] = char_offset;
  }
  return YAP_V2_OK;
}

void YAP_V2_token_sequence_free(YAP_V2_TOKEN_SEQUENCE *sequence) {
  if (sequence == NULL) return;
  free(sequence->normalized_utf8); free(sequence->tokens); memset(sequence, 0, sizeof(*sequence));
}

int YAP_V2_unicode_tokenize(const char *utf8, size_t utf8_bytes, YAP_V2_TOKEN_SEQUENCE *sequence) {
  NORMALIZED_TEXT text;
  UBreakIterator *iterator;
  UErrorCode error = U_ZERO_ERROR;
  int32_t start, end;
  size_t count = 0U;
  int status;
  if (sequence == NULL) return YAP_V2_INVALID_ARGUMENT;
  memset(sequence, 0, sizeof(*sequence));
  status = normalize_text(utf8, utf8_bytes, &text);
  if (status != YAP_V2_OK) return status;
  iterator = ubrk_open(UBRK_WORD, "root", text.text, text.length, &error);
  if (U_FAILURE(error)) { normalized_free(&text); return YAP_V2_INVALID_FORMAT; }
  for (start = ubrk_first(iterator), end = ubrk_next(iterator); end != UBRK_DONE;
       start = end, end = ubrk_next(iterator)) {
    if (ubrk_getRuleStatus(iterator) != UBRK_WORD_NONE) count++;
  }
  sequence->tokens = count == 0U ? NULL : (YAP_V2_TOKEN *)calloc(count, sizeof(*sequence->tokens));
  if (count > 0U && sequence->tokens == NULL) { ubrk_close(iterator); normalized_free(&text); return YAP_V2_ALLOCATION_FAILED; }
  count = 0U;
  for (start = ubrk_first(iterator), end = ubrk_next(iterator); end != UBRK_DONE;
       start = end, end = ubrk_next(iterator)) {
    if (ubrk_getRuleStatus(iterator) == UBRK_WORD_NONE) continue;
    sequence->tokens[count].byte_start = (size_t)text.byte_offsets[start];
    sequence->tokens[count].byte_end = (size_t)text.byte_offsets[end];
    sequence->tokens[count].char_start = text.char_offsets[start];
    sequence->tokens[count].char_end = text.char_offsets[end];
    count++;
  }
  ubrk_close(iterator);
  sequence->normalized_utf8 = text.utf8; text.utf8 = NULL;
  sequence->normalized_bytes = text.utf8_bytes; sequence->token_count = count;
  normalized_free(&text);
  return YAP_V2_OK;
}

static uint64_t passage_hash(const char *document_id, uint32_t ordinal, const char *text, size_t bytes) {
  const unsigned char *p;
  uint64_t hash = UINT64_C(1469598103934665603);
  for (p=(const unsigned char *)document_id;*p;p++){hash^=*p;hash*=UINT64_C(1099511628211);}
  hash ^= ordinal; hash *= UINT64_C(1099511628211);
  for (p=(const unsigned char *)text;p<(const unsigned char *)text+bytes;p++){hash^=*p;hash*=UINT64_C(1099511628211);}
  return hash;
}

void YAP_V2_chunk_sequence_free(YAP_V2_CHUNK_SEQUENCE *sequence) {
  size_t i;
  if (sequence == NULL) return;
  for (i=0;i<sequence->chunk_count;i++) free(sequence->chunks[i].text);
  free(sequence->chunks); memset(sequence,0,sizeof(*sequence));
}

int YAP_V2_unicode_chunk(const char *document_id, const char *utf8, size_t utf8_bytes,
                         uint32_t max_chars, uint32_t overlap_chars,
                         YAP_V2_CHUNK_SEQUENCE *sequence) {
  NORMALIZED_TEXT text;
  UBreakIterator *graphemes = NULL, *sentences = NULL;
  UErrorCode error = U_ZERO_ERROR;
  int32_t start = 0;
  size_t capacity = 0U;
  int status;
  if (document_id == NULL || document_id[0] == '\0' || sequence == NULL || max_chars == 0U || overlap_chars >= max_chars) return YAP_V2_INVALID_ARGUMENT;
  memset(sequence,0,sizeof(*sequence));
  status=normalize_text(utf8,utf8_bytes,&text); if(status!=YAP_V2_OK)return status;
  graphemes=ubrk_open(UBRK_CHARACTER,"root",text.text,text.length,&error);
  sentences=ubrk_open(UBRK_SENTENCE,"root",text.text,text.length,&error);
  if(U_FAILURE(error)){if(graphemes)ubrk_close(graphemes);if(sentences)ubrk_close(sentences);normalized_free(&text);return YAP_V2_INVALID_FORMAT;}
  while(start<text.length){
    int32_t limit=start, candidate, end, next_start;
    uint32_t start_char=text.char_offsets[start];
    candidate=ubrk_following(graphemes,start);
    while(candidate!=UBRK_DONE && text.char_offsets[candidate]-start_char<=max_chars){limit=candidate;candidate=ubrk_next(graphemes);}
    if(limit<=start)limit=ubrk_following(graphemes,start);
    end=limit;
    candidate=ubrk_following(sentences,start);
    while(candidate!=UBRK_DONE && candidate<=limit){end=candidate;candidate=ubrk_next(sentences);}
    if(end<=start)end=limit;
    if(sequence->chunk_count==capacity){size_t new_capacity=capacity==0U?4U:capacity*2U;YAP_V2_CHUNK *grown=(YAP_V2_CHUNK *)realloc(sequence->chunks,new_capacity*sizeof(*grown));if(grown==NULL){status=YAP_V2_ALLOCATION_FAILED;goto done;}sequence->chunks=grown;capacity=new_capacity;}
    {
      YAP_V2_CHUNK *chunk=&sequence->chunks[sequence->chunk_count];
      size_t begin_byte=(size_t)text.byte_offsets[start], end_byte=(size_t)text.byte_offsets[end];
      uint64_t hash;
      memset(chunk,0,sizeof(*chunk));chunk->text_bytes=end_byte-begin_byte;chunk->text=(char *)malloc(chunk->text_bytes+1U);
      if(chunk->text==NULL){status=YAP_V2_ALLOCATION_FAILED;goto done;}
      memcpy(chunk->text,text.utf8+begin_byte,chunk->text_bytes);chunk->text[chunk->text_bytes]='\0';
      chunk->ordinal=(uint32_t)sequence->chunk_count;chunk->start_char=start_char;chunk->end_char=text.char_offsets[end];
      hash=passage_hash(document_id,chunk->ordinal,chunk->text,chunk->text_bytes);
      (void)snprintf(chunk->id,sizeof(chunk->id),"p-%016llx",(unsigned long long)hash);
      sequence->chunk_count++;
    }
    if(end==text.length)break;
    next_start=end;
    while(next_start>start && text.char_offsets[end]-text.char_offsets[next_start]<overlap_chars){int32_t previous=ubrk_preceding(graphemes,next_start);if(previous==UBRK_DONE||previous<=start)break;next_start=previous;}
    if(next_start<=start)next_start=end;
    start=next_start;
  }
  status=YAP_V2_OK;
done:
  ubrk_close(graphemes);ubrk_close(sentences);normalized_free(&text);
  if(status!=YAP_V2_OK)YAP_V2_chunk_sequence_free(sequence);
  return status;
}
