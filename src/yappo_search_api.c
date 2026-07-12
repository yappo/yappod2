/* Stable JSON search response and cursor helpers for the v2 HTTP API. */
#include "yappo_search_api.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static int write_json_string(FILE *stream, const char *value) {
  const unsigned char *p;

  if (fputc('"', stream) == EOF) {
    return -1;
  }
  if (value == NULL) {
    value = "";
  }
  for (p = (const unsigned char *)value; *p != '\0'; p++) {
    switch (*p) {
    case '"':
      if (fputs("\\\"", stream) == EOF) return -1;
      break;
    case '\\':
      if (fputs("\\\\", stream) == EOF) return -1;
      break;
    case '\b':
      if (fputs("\\b", stream) == EOF) return -1;
      break;
    case '\f':
      if (fputs("\\f", stream) == EOF) return -1;
      break;
    case '\n':
      if (fputs("\\n", stream) == EOF) return -1;
      break;
    case '\r':
      if (fputs("\\r", stream) == EOF) return -1;
      break;
    case '\t':
      if (fputs("\\t", stream) == EOF) return -1;
      break;
    default:
      if (*p < 0x20U && fprintf(stream, "\\u%04x", (unsigned int)*p) < 0) {
        return -1;
      }
      if (*p >= 0x20U && fputc((int)*p, stream) == EOF) {
        return -1;
      }
      break;
    }
  }
  return fputc('"', stream) == EOF ? -1 : 0;
}

int YAP_Search_api_cursor_encode(size_t offset, char *out, size_t out_size) {
  int written;

  if (out == NULL || out_size == 0U || offset > (size_t)INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  written = snprintf(out, out_size, "v1.%zu", offset);
  if (written < 0 || (size_t)written >= out_size) {
    errno = ERANGE;
    return -1;
  }
  return 0;
}

int YAP_Search_api_cursor_decode(const char *cursor, size_t *offset_out) {
  const char *p;
  unsigned long long value = 0;

  if (cursor == NULL || offset_out == NULL || strncmp(cursor, "v1.", 3) != 0 ||
      cursor[3] == '\0') {
    errno = EINVAL;
    return -1;
  }
  p = cursor + 3;
  for (; *p != '\0'; p++) {
    unsigned int digit;
    if (*p < '0' || *p > '9') {
      errno = EINVAL;
      return -1;
    }
    digit = (unsigned int)(*p - '0');
    if (value > (ULLONG_MAX - digit) / 10ULL) {
      errno = ERANGE;
      return -1;
    }
    value = value * 10ULL + digit;
  }
  if (value > (unsigned long long)INT_MAX) {
    errno = ERANGE;
    return -1;
  }
  *offset_out = (size_t)value;
  return 0;
}

int YAP_Search_api_page(size_t total, size_t limit, const char *cursor, size_t *start_out,
                        size_t *end_out) {
  size_t start = 0U;

  if (start_out == NULL || end_out == NULL || limit == 0U ||
      limit > YAP_SEARCH_API_MAX_LIMIT) {
    errno = EINVAL;
    return -1;
  }
  if (cursor != NULL && cursor[0] != '\0' && YAP_Search_api_cursor_decode(cursor, &start) != 0) {
    return -1;
  }
  if (start > total) {
    errno = EINVAL;
    return -1;
  }
  *start_out = start;
  *end_out = start;
  if (start < total) {
    *end_out = start + (limit < total - start ? limit : total - start);
  }
  return 0;
}

int YAP_Search_api_write_json(FILE *stream, size_t total, size_t start, size_t end,
                              size_t limit, const YAP_SEARCH_API_DOCUMENT *documents) {
  size_t i;
  char cursor[64];

  if (stream == NULL || start > end || end > total || limit == 0U ||
      limit > YAP_SEARCH_API_MAX_LIMIT || (end > start && documents == NULL)) {
    errno = EINVAL;
    return -1;
  }
  if (fprintf(stream, "{\"api_version\":2,\"total\":%zu,\"limit\":%zu,\"next_cursor\":",
              total, limit) < 0) {
    return -1;
  }
  if (end < total) {
    if (YAP_Search_api_cursor_encode(end, cursor, sizeof(cursor)) != 0 ||
        write_json_string(stream, cursor) != 0) {
      return -1;
    }
  } else if (fputs("null", stream) == EOF) {
    return -1;
  }
  if (fputs(",\"results\":[", stream) == EOF) {
    return -1;
  }
  for (i = start; i < end; i++) {
    const YAP_SEARCH_API_DOCUMENT *document = &documents[i - start];
    if (i > start && fputc(',', stream) == EOF) {
      return -1;
    }
    if (fputs("{\"url\":", stream) == EOF || write_json_string(stream, document->url) != 0 ||
        fputs(",\"title\":", stream) == EOF || write_json_string(stream, document->title) != 0 ||
        fprintf(stream, ",\"size\":%d,\"lastmod\":%ld,\"score\":%.17g}", document->size,
                document->lastmod, document->score) < 0) {
      return -1;
    }
  }
  return fputs("]}\n", stream) == EOF ? -1 : 0;
}
