/* Stable JSON search response and cursor helpers for the v2 HTTP API. */
#ifndef YAPPO_SEARCH_API_H
#define YAPPO_SEARCH_API_H

#include <stddef.h>
#include <stdio.h>

#define YAP_SEARCH_API_DEFAULT_LIMIT 20
#define YAP_SEARCH_API_MAX_LIMIT 100

typedef struct {
  const char *url;
  const char *title;
  int size;
  long lastmod;
  double score;
} YAP_SEARCH_API_DOCUMENT;

/* Cursors are intentionally opaque to clients but deterministic within a result set. */
int YAP_Search_api_cursor_encode(size_t offset, char *out, size_t out_size);
int YAP_Search_api_cursor_decode(const char *cursor, size_t *offset_out);
int YAP_Search_api_page(size_t total, size_t limit, const char *cursor, size_t *start_out,
                        size_t *end_out);

/* Writes one complete JSON object. Returns -1 on invalid arguments or I/O failure. */
int YAP_Search_api_write_json(FILE *stream, size_t total, size_t start, size_t end,
                              size_t limit, const YAP_SEARCH_API_DOCUMENT *documents);

#endif
