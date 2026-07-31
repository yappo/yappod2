#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <yyjson.h>

#include "test_fs.h"
#include "test_http.h"

typedef struct {
  int port;
  const char *body;
  size_t body_bytes;
  size_t first;
  size_t count;
  double *latencies;
  unsigned char *statuses;
  int failed;
} worker_t;

typedef struct {
  size_t successful;
  size_t overloaded;
  size_t failed;
  double p50_ms;
  double p95_ms;
  double p99_ms;
  double elapsed_ms;
  double requests_per_second;
} mode_result_t;

typedef struct {
  int port;
  const char *lexical_path;
  const char *hybrid_path;
  size_t requests;
  size_t concurrency;
  long core_pid;
  long front_pid;
  uint64_t documents;
  uint64_t passages;
  size_t dimensions;
  int assert_reference;
  int require_all_success;
} options_t;

static void usage(FILE *output) {
  fputs("usage: v2_load_probe --port N [--lexical-request FILE] "
        "[--hybrid-request FILE] --core-pid PID --front-pid PID "
        "[--requests N] [--concurrency N] "
        "[--documents N --passages N --dimensions N --assert-reference] "
        "[--require-all-success]\n", output);
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static int compare_double(const void *left, const void *right) {
  double a = *(const double *)left, b = *(const double *)right;
  return a < b ? -1 : a > b;
}

static double percentile(const double *sorted, size_t count, double fraction) {
  size_t rank;
  if (count == 0U) return 0.0;
  rank = (size_t)(fraction * (double)count + 0.999999);
  if (rank == 0U) rank = 1U;
  return sorted[rank - 1U];
}

static void *run_worker(void *opaque) {
  worker_t *worker = (worker_t *)opaque;
  char *request;
  size_t request_capacity = worker->body_bytes + 256U, i;
  int written;
  request = malloc(request_capacity);
  if (request == NULL) { worker->failed = 1; return NULL; }
  written = snprintf(request, request_capacity,
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
    "Content-Length: %zu\r\n\r\n%.*s", worker->body_bytes, (int)worker->body_bytes,
    worker->body);
  if (written < 0 || (size_t)written >= request_capacity) {
    free(request); worker->failed = 1; return NULL;
  }
  for (i = 0U; i < worker->count; i++) {
    struct timespec start, end;
    char *response = NULL;
    int request_status;
    size_t result_index = worker->first + i;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
      free(request); worker->failed = 1; return NULL;
    }
    request_status = ytest_http_send_text(worker->port, request, &response);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
      free(response); free(request); worker->failed = 1; return NULL;
    }
    worker->latencies[worker->first + i] = elapsed_ms(start, end);
    if (request_status == 0 && response != NULL && strstr(response, "200 OK") != NULL)
      worker->statuses[result_index] = 1U;
    else if (request_status == 0 && response != NULL &&
             strstr(response, "503 Service Unavailable") != NULL &&
             strstr(response, "\"code\":\"overloaded\"") != NULL)
      worker->statuses[result_index] = 2U;
    else
      worker->statuses[result_index] = 3U;
    free(response);
  }
  free(request);
  return NULL;
}

static int run_mode(const options_t *options, const char *body, size_t body_bytes,
                    mode_result_t *result) {
  pthread_t *threads;
  worker_t *workers;
  double *latencies;
  unsigned char *statuses;
  size_t i, assigned = 0U, base = options->requests / options->concurrency;
  size_t remainder = options->requests % options->concurrency;
  struct timespec start, end;
  int failed = 0;
  memset(result, 0, sizeof(*result));
  if (body_bytes > INT_MAX || body_bytes > SIZE_MAX - 256U) return -1;
  threads = calloc(options->concurrency, sizeof(*threads));
  workers = calloc(options->concurrency, sizeof(*workers));
  latencies = calloc(options->requests, sizeof(*latencies));
  statuses = calloc(options->requests, sizeof(*statuses));
  if (threads == NULL || workers == NULL || latencies == NULL || statuses == NULL ||
      clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    free(threads); free(workers); free(latencies); free(statuses); return -1;
  }
  for (i = 0U; i < options->concurrency; i++) {
    workers[i].port = options->port; workers[i].body = body; workers[i].body_bytes = body_bytes;
    workers[i].first = assigned; workers[i].count = base + (i < remainder ? 1U : 0U);
    workers[i].latencies = latencies; workers[i].statuses = statuses;
    assigned += workers[i].count;
    if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0) { failed = 1; break; }
  }
  while (i > 0U) { i--; (void)pthread_join(threads[i], NULL); if (workers[i].failed) failed = 1; }
  if (!failed && clock_gettime(CLOCK_MONOTONIC, &end) == 0) {
    size_t successful_index = 0U;
    for (i = 0U; i < options->requests; i++) {
      if (statuses[i] == 1U) latencies[successful_index++] = latencies[i];
      else if (statuses[i] == 2U) result->overloaded++;
      else result->failed++;
    }
    result->successful = successful_index;
    qsort(latencies, successful_index, sizeof(*latencies), compare_double);
    result->p50_ms = percentile(latencies, successful_index, 0.50);
    result->p95_ms = percentile(latencies, successful_index, 0.95);
    result->p99_ms = percentile(latencies, successful_index, 0.99);
    result->elapsed_ms = elapsed_ms(start, end);
    if (result->elapsed_ms > 0.0)
      result->requests_per_second = (double)options->requests * 1000.0 / result->elapsed_ms;
  } else {
    failed = 1;
  }
  free(threads); free(workers); free(latencies); free(statuses);
  return failed ? -1 : 0;
}

static unsigned long process_rss_kib(long pid) {
  char command[64]; FILE *stream; unsigned long rss = 0UL;
  int written = snprintf(command, sizeof(command), "ps -o rss= -p %ld", pid);
  if (written < 0 || (size_t)written >= sizeof(command)) return 0UL;
  stream = popen(command, "r");
  if (stream == NULL) return 0UL;
  if (fscanf(stream, "%lu", &rss) != 1) rss = 0UL;
  (void)pclose(stream); return rss;
}

static int parse_size(const char *value, size_t *output) {
  char *end = NULL; unsigned long long parsed;
  errno = 0; parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0U || parsed > SIZE_MAX) return -1;
  *output = (size_t)parsed; return 0;
}

static int parse_u64(const char *value, uint64_t *output) {
  char *end = NULL; unsigned long long parsed;
  errno = 0; parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0U) return -1;
  *output = (uint64_t)parsed; return 0;
}

static int parse_options(int argc, char **argv, options_t *options) {
  int i;
  memset(options, 0, sizeof(*options)); options->requests = 1000U; options->concurrency = 16U;
  for (i = 1; i < argc; i++) {
    const char *value;
    if (strcmp(argv[i], "--assert-reference") == 0) { options->assert_reference = 1; continue; }
    if (strcmp(argv[i], "--require-all-success") == 0) {
      options->require_all_success = 1; continue;
    }
    if (++i >= argc) return -1;
    value = argv[i];
    if (strcmp(argv[i - 1], "--port") == 0) { size_t parsed; if (parse_size(value, &parsed) != 0 || parsed > 65535U) return -1; options->port = (int)parsed; }
    else if (strcmp(argv[i - 1], "--lexical-request") == 0) options->lexical_path = value;
    else if (strcmp(argv[i - 1], "--hybrid-request") == 0) options->hybrid_path = value;
    else if (strcmp(argv[i - 1], "--requests") == 0) { if (parse_size(value, &options->requests) != 0) return -1; }
    else if (strcmp(argv[i - 1], "--concurrency") == 0) { if (parse_size(value, &options->concurrency) != 0) return -1; }
    else if (strcmp(argv[i - 1], "--core-pid") == 0) options->core_pid = strtol(value, NULL, 10);
    else if (strcmp(argv[i - 1], "--front-pid") == 0) options->front_pid = strtol(value, NULL, 10);
    else if (strcmp(argv[i - 1], "--documents") == 0) { if (parse_u64(value, &options->documents) != 0) return -1; }
    else if (strcmp(argv[i - 1], "--passages") == 0) { if (parse_u64(value, &options->passages) != 0) return -1; }
    else if (strcmp(argv[i - 1], "--dimensions") == 0) { if (parse_size(value, &options->dimensions) != 0) return -1; }
    else return -1;
  }
  return options->port > 0 &&
         (options->lexical_path != NULL || options->hybrid_path != NULL) &&
         options->requests >= options->concurrency && options->core_pid > 0 &&
         options->front_pid > 0 &&
         (!options->assert_reference ||
          (options->lexical_path != NULL && options->hybrid_path != NULL)) ? 0 : -1;
}

static int validate_request(const char *json, size_t bytes, const char *mode,
                            int reference, size_t dimensions) {
  yyjson_doc *document = yyjson_read(json, bytes, 0U);
  yyjson_val *root, *value, *vector;
  int valid = 0;
  if (document == NULL) return -1;
  root = yyjson_doc_get_root(document); value = yyjson_obj_get(root, "mode");
  vector = yyjson_obj_get(root, "vector");
  if (yyjson_is_obj(root) && yyjson_is_str(value) && strcmp(yyjson_get_str(value), mode) == 0 &&
      (!reference || (yyjson_is_uint(yyjson_obj_get(root, "limit")) &&
                      yyjson_get_uint(yyjson_obj_get(root, "limit")) == 20U &&
                      (strcmp(mode, "hybrid") != 0 ||
                       (yyjson_is_arr(vector) && yyjson_arr_size(vector) == dimensions)))))
    valid = 1;
  yyjson_doc_free(document); return valid ? 0 : -1;
}

int main(int argc, char **argv) {
  options_t options; char *lexical = NULL, *hybrid = NULL;
  size_t lexical_bytes = 0U, hybrid_bytes = 0U;
  mode_result_t lexical_result, hybrid_result;
  unsigned long rss_kib; int passed;
  memset(&lexical_result, 0, sizeof(lexical_result));
  memset(&hybrid_result, 0, sizeof(hybrid_result));
  if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    usage(stdout);
    return EXIT_SUCCESS;
  }
  if (parse_options(argc, argv, &options) != 0 ||
      (options.lexical_path != NULL &&
       (ytest_read_file(options.lexical_path, &lexical, &lexical_bytes) != 0 ||
        validate_request(lexical, lexical_bytes, "lexical", options.assert_reference,
                         options.dimensions) != 0)) ||
      (options.hybrid_path != NULL &&
       (ytest_read_file(options.hybrid_path, &hybrid, &hybrid_bytes) != 0 ||
        validate_request(hybrid, hybrid_bytes, "hybrid", options.assert_reference,
                         options.dimensions) != 0))) {
    usage(stderr);
    free(lexical); free(hybrid); return EXIT_FAILURE;
  }
  if ((lexical != NULL &&
       run_mode(&options, lexical, lexical_bytes, &lexical_result) != 0) ||
      (hybrid != NULL &&
       run_mode(&options, hybrid, hybrid_bytes, &hybrid_result) != 0)) {
    free(lexical); free(hybrid); return EXIT_FAILURE;
  }
  rss_kib = process_rss_kib(options.core_pid) + process_rss_kib(options.front_pid);
  passed = rss_kib > 0UL && lexical_result.failed == 0U && hybrid_result.failed == 0U;
  if (options.require_all_success)
    passed = passed && lexical_result.overloaded == 0U && hybrid_result.overloaded == 0U;
  if (options.assert_reference)
    passed = passed && options.concurrency == 16U && options.documents >= 1000000U &&
      options.passages >= 3000000U && options.dimensions == 768U &&
      lexical_result.p95_ms <= 100.0 && hybrid_result.p95_ms <= 200.0 &&
      lexical_result.overloaded == 0U && hybrid_result.overloaded == 0U &&
      rss_kib <= 24UL * 1024UL * 1024UL;
  printf("{\"requests_per_mode\":%zu,\"concurrency\":%zu,\"documents\":%llu,"
    "\"passages\":%llu,\"dimensions\":%zu,"
    "\"lexical_successful\":%zu,\"lexical_overloaded\":%zu,\"lexical_failed\":%zu,"
    "\"lexical_p50_ms\":%.3f,\"lexical_p95_ms\":%.3f,\"lexical_p99_ms\":%.3f,"
    "\"lexical_elapsed_ms\":%.3f,\"lexical_requests_per_second\":%.3f,"
    "\"hybrid_successful\":%zu,\"hybrid_overloaded\":%zu,\"hybrid_failed\":%zu,"
    "\"hybrid_p50_ms\":%.3f,\"hybrid_p95_ms\":%.3f,\"hybrid_p99_ms\":%.3f,"
    "\"hybrid_elapsed_ms\":%.3f,\"hybrid_requests_per_second\":%.3f,"
    "\"daemon_rss_kib\":%lu,\"reference_asserted\":%s,\"passed\":%s}\n",
    options.requests, options.concurrency,
    (unsigned long long)options.documents, (unsigned long long)options.passages,
    options.dimensions,
    lexical_result.successful, lexical_result.overloaded, lexical_result.failed,
    lexical_result.p50_ms, lexical_result.p95_ms, lexical_result.p99_ms,
    lexical_result.elapsed_ms, lexical_result.requests_per_second,
    hybrid_result.successful, hybrid_result.overloaded, hybrid_result.failed,
    hybrid_result.p50_ms, hybrid_result.p95_ms, hybrid_result.p99_ms,
    hybrid_result.elapsed_ms, hybrid_result.requests_per_second, rss_kib,
    options.assert_reference ? "true" : "false", passed ? "true" : "false");
  free(lexical); free(hybrid); return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
