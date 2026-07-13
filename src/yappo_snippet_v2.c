#include "yappo_snippet_v2.h"

#include <string.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>

static int valid_utf8(const unsigned char *data, size_t len) {
  int32_t i = 0; UChar32 codepoint;
  if (len > INT32_MAX) return 0;
  while (i < (int32_t)len) { U8_NEXT(data, i, (int32_t)len, codepoint); if (codepoint < 0) return 0; }
  return 1;
}

static int boundary(const int32_t *points, size_t count, size_t offset) {
  size_t i; for (i = 0U; i < count; i++) if ((size_t)points[i] == offset) return 1; return 0;
}

int YAP_V2_snippet(YAP_V2_BYTES_VIEW text, const YAP_V2_BYTES_VIEW *terms, size_t term_count,
                   size_t max_graphemes, const char *open_mark, const char *close_mark,
                   char *output, size_t output_capacity, size_t *output_bytes) {
  UErrorCode error = U_ZERO_ERROR; UText *utext = NULL; UBreakIterator *iterator = NULL;
  int32_t points[4098]; size_t point_count = 0U, i, start_index = 0U, end_index, focus = 0U, out = 0U;
  if ((text.len > 0U && text.data == NULL) || (term_count > 0U && terms == NULL) || max_graphemes == 0U ||
      max_graphemes > 4096U || open_mark == NULL || close_mark == NULL || output == NULL || output_capacity == 0U || output_bytes == NULL || !valid_utf8(text.data, text.len)) return YAP_V2_INVALID_ARGUMENT;
  for (i = 0U; i < term_count; i++) if (terms[i].len == 0U || terms[i].data == NULL || !valid_utf8(terms[i].data, terms[i].len)) return YAP_V2_INVALID_ARGUMENT;
  utext = utext_openUTF8(NULL, (const char *)text.data, (int64_t)text.len, &error);
  iterator = ubrk_open(UBRK_CHARACTER, "und", NULL, 0, &error);
  if (U_FAILURE(error) || utext == NULL || iterator == NULL) goto allocation_error;
  ubrk_setUText(iterator, utext, &error); if (U_FAILURE(error)) goto allocation_error;
  for (points[point_count++] = ubrk_first(iterator); point_count < sizeof(points) / sizeof(points[0]); ) {
    int32_t next = ubrk_next(iterator); if (next == UBRK_DONE) break; points[point_count++] = next;
  }
  if (point_count == 0U || (size_t)points[point_count - 1U] != text.len) goto invalid;
  for (i = 0U; i < term_count; i++) {
    size_t at; for (at = 0U; at + terms[i].len <= text.len; at++) if (boundary(points, point_count, at) && boundary(points, point_count, at + terms[i].len) && memcmp(text.data + at, terms[i].data, terms[i].len) == 0) { focus = at; i = term_count; break; }
  }
  for (i = 0U; i + 1U < point_count && (size_t)points[i] < focus; i++) start_index = i;
  if (point_count - 1U > max_graphemes) {
    size_t half = max_graphemes / 2U; start_index = start_index > half ? start_index - half : 0U;
    if (start_index + max_graphemes >= point_count) start_index = point_count - 1U - max_graphemes;
  } else start_index = 0U;
  end_index = start_index + max_graphemes; if (end_index >= point_count) end_index = point_count - 1U;
  for (i = (size_t)points[start_index]; i < (size_t)points[end_index]; ) {
    size_t term_index, matched = SIZE_MAX;
    for (term_index = 0U; term_index < term_count; term_index++) if (i + terms[term_index].len <= (size_t)points[end_index] && memcmp(text.data + i, terms[term_index].data, terms[term_index].len) == 0 && boundary(points, point_count, i + terms[term_index].len)) { matched = term_index; break; }
    if (matched != SIZE_MAX) {
      size_t open_len = strlen(open_mark), close_len = strlen(close_mark);
      if (open_len + terms[matched].len + close_len > output_capacity - 1U - out) goto range_error;
      memcpy(output + out, open_mark, open_len); out += open_len; memcpy(output + out, text.data + i, terms[matched].len); out += terms[matched].len; memcpy(output + out, close_mark, close_len); out += close_len; i += terms[matched].len;
    } else {
      size_t next = i + 1U; while (next < (size_t)points[end_index] && !boundary(points, point_count, next)) next++;
      if (next - i > output_capacity - 1U - out) goto range_error; memcpy(output + out, text.data + i, next - i); out += next - i; i = next;
    }
  }
  output[out] = '\0'; *output_bytes = out; ubrk_close(iterator); utext_close(utext); return YAP_V2_OK;
range_error:
  ubrk_close(iterator); utext_close(utext); return YAP_V2_OUT_OF_RANGE;
allocation_error:
  if (iterator != NULL) ubrk_close(iterator); if (utext != NULL) utext_close(utext); return YAP_V2_ALLOCATION_FAILED;
invalid:
  ubrk_close(iterator); utext_close(utext); return YAP_V2_INVALID_FORMAT;
}
