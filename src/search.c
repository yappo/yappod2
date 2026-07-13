#include "yappo_http_v2.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

typedef struct {
  const char *index_dir;
  const char *mode;
  const char *query;
  const char *scope;
  const char *vector_text;
  size_t limit;
} options_t;

static void usage(FILE *output) {
  fputs("Usage: search --index INDEX_DIR --mode lexical|vector|hybrid "
        "[--query TEXT] [--vector N,N,...] [--scope documents|passages] "
        "[--limit 1..100]\n",
        output);
}

static int parse_size(const char *text, size_t *value) {
  char *end = NULL;
  unsigned long long parsed;
  errno = 0;
  parsed = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed == 0U || parsed > 100U)
    return -1;
  *value = (size_t)parsed;
  return 0;
}

static int parse_options(int argc, char **argv, options_t *options) {
  int i;
  memset(options, 0, sizeof(*options));
  options->scope = "documents";
  options->limit = 10U;
  for (i = 1; i < argc; i++) {
    const char *name = argv[i];
    const char **target = NULL;
    if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) return 1;
    if (strcmp(name, "--index") == 0) target = &options->index_dir;
    else if (strcmp(name, "--mode") == 0) target = &options->mode;
    else if (strcmp(name, "--query") == 0) target = &options->query;
    else if (strcmp(name, "--scope") == 0) target = &options->scope;
    else if (strcmp(name, "--vector") == 0) target = &options->vector_text;
    else if (strcmp(name, "--limit") == 0) {
      if (++i >= argc || parse_size(argv[i], &options->limit) != 0) return -1;
      continue;
    } else return -1;
    if (++i >= argc) return -1;
    *target = argv[i];
  }
  if (options->index_dir == NULL || options->mode == NULL ||
      (strcmp(options->mode, "lexical") != 0 && strcmp(options->mode, "vector") != 0 &&
       strcmp(options->mode, "hybrid") != 0) ||
      (strcmp(options->scope, "documents") != 0 && strcmp(options->scope, "passages") != 0) ||
      (strcmp(options->mode, "vector") != 0 &&
       (options->query == NULL || options->query[0] == '\0')) ||
      (strcmp(options->mode, "lexical") != 0 && options->vector_text == NULL)) return -1;
  return 0;
}

static int add_vector(yyjson_mut_doc *document, yyjson_mut_val *root, const char *text) {
  yyjson_mut_val *array = yyjson_mut_arr(document);
  const char *cursor = text;
  size_t count = 0U;
  if (array == NULL || cursor == NULL || *cursor == '\0') return -1;
  for (;;) {
    char *end = NULL;
    double value;
    errno = 0;
    value = strtod(cursor, &end);
    if (errno != 0 || end == cursor || !isfinite(value) || value > FLT_MAX || value < -FLT_MAX ||
        !yyjson_mut_arr_add_real(document, array, value)) return -1;
    count++;
    if (*end == '\0') break;
    if (*end != ',') return -1;
    cursor = end + 1;
    if (*cursor == '\0') return -1;
  }
  return count == 0U || !yyjson_mut_obj_add_val(document, root, "vector", array) ? -1 : 0;
}

static char *make_request(const options_t *options, size_t *request_bytes) {
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root;
  char *request;
  if (document == NULL) return NULL;
  root = yyjson_mut_obj(document);
  if (root == NULL) { yyjson_mut_doc_free(document); return NULL; }
  yyjson_mut_doc_set_root(document, root);
  if (!yyjson_mut_obj_add_str(document, root, "mode", options->mode) ||
      !yyjson_mut_obj_add_str(document, root, "scope", options->scope) ||
      !yyjson_mut_obj_add_uint(document, root, "limit", options->limit) ||
      (options->query != NULL &&
       !yyjson_mut_obj_add_str(document, root, "query", options->query)) ||
      (options->vector_text != NULL && add_vector(document, root, options->vector_text) != 0)) {
    yyjson_mut_doc_free(document);
    return NULL;
  }
  request = yyjson_mut_write_opts(document, YYJSON_WRITE_NOFLAG, NULL, request_bytes, NULL);
  yyjson_mut_doc_free(document);
  return request;
}

int main(int argc, char **argv) {
  options_t options;
  char *request, *response = NULL;
  size_t request_bytes = 0U, response_bytes = 0U;
  int parsed, http_status = 0;
  parsed = parse_options(argc, argv, &options);
  if (parsed == 1) { usage(stdout); return EXIT_SUCCESS; }
  if (parsed != 0) { usage(stderr); return EXIT_FAILURE; }
  request = make_request(&options, &request_bytes);
  if (request == NULL) {
    fputs("Invalid query vector\n", stderr);
    return EXIT_FAILURE;
  }
  if (YAP_V2_http_execute(options.index_dir, YAP_V2_HTTP_SEARCH,
                          (const unsigned char *)request, request_bytes,
                          &http_status, &response, &response_bytes) != 0 || response == NULL) {
    free(request);
    fputs("Search failed: index is not a valid v2 snapshot\n", stderr);
    return EXIT_FAILURE;
  }
  free(request);
  if (http_status != 200) {
    (void)fwrite(response, 1U, response_bytes, stderr);
    (void)fputc('\n', stderr);
    free(response);
    return EXIT_FAILURE;
  }
  if (fwrite(response, 1U, response_bytes, stdout) != response_bytes || fputc('\n', stdout) == EOF) {
    free(response);
    return EXIT_FAILURE;
  }
  free(response);
  return EXIT_SUCCESS;
}
