#include <ctype.h>
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

typedef enum {
  REQUEST_SEARCH = 1,
  REQUEST_UPDATE = 2
} request_kind_t;

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  size_t expected;
  size_t arrived;
  int released;
} start_gate_t;

typedef struct {
  int port;
  request_kind_t kind;
  const char *body;
  size_t body_bytes;
  const char *update_prefix;
  size_t first;
  size_t count;
  double *latencies;
  unsigned char *statuses;
  start_gate_t *gate;
  struct timespec ended_at;
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
  pthread_t *threads;
  worker_t *workers;
  double *latencies;
  unsigned char *statuses;
  size_t request_count;
  size_t concurrency;
  size_t started_threads;
  struct timespec started_at;
} mode_state_t;

typedef struct {
  int port;
  const char *search_path;
  size_t search_requests;
  size_t search_concurrency;
  size_t update_requests;
  size_t update_concurrency;
  const char *update_prefix;
  long core_pid;
  long front_pid;
  int require_all_success;
} options_t;

static void usage(FILE *output) {
  fputs(
    "usage: v2_mixed_load_probe --port N --core-pid PID --front-pid PID "
    "[--search-request FILE --search-requests N --search-concurrency N] "
    "[--update-requests N --update-concurrency N --update-prefix PREFIX] "
    "[--require-all-success]\n", output);
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static int compare_double(const void *left, const void *right) {
  double a = *(const double *)left;
  double b = *(const double *)right;
  return a < b ? -1 : a > b;
}

static double percentile(const double *sorted, size_t count, double fraction) {
  size_t rank;
  if (count == 0U) return 0.0;
  rank = (size_t)(fraction * (double)count + 0.999999);
  if (rank == 0U) rank = 1U;
  return sorted[rank - 1U];
}

static int gate_init(start_gate_t *gate, size_t expected) {
  if (gate == NULL || expected == 0U) return -1;
  memset(gate, 0, sizeof(*gate));
  gate->expected = expected;
  if (pthread_mutex_init(&gate->lock, NULL) != 0) return -1;
  if (pthread_cond_init(&gate->ready, NULL) != 0) {
    pthread_mutex_destroy(&gate->lock);
    return -1;
  }
  return 0;
}

static void gate_destroy(start_gate_t *gate) {
  if (gate == NULL) return;
  pthread_cond_destroy(&gate->ready);
  pthread_mutex_destroy(&gate->lock);
}

static int gate_wait_for_workers(start_gate_t *gate) {
  int failed = 0;
  pthread_mutex_lock(&gate->lock);
  while (gate->arrived < gate->expected && !failed)
    failed = pthread_cond_wait(&gate->ready, &gate->lock) != 0;
  if (!failed) {
    gate->released = 1;
    pthread_cond_broadcast(&gate->ready);
  }
  pthread_mutex_unlock(&gate->lock);
  return failed ? -1 : 0;
}

static void gate_release_after_failure(start_gate_t *gate) {
  pthread_mutex_lock(&gate->lock);
  gate->released = 1;
  pthread_cond_broadcast(&gate->ready);
  pthread_mutex_unlock(&gate->lock);
}

static int gate_arrive(start_gate_t *gate) {
  int failed = 0;
  pthread_mutex_lock(&gate->lock);
  gate->arrived++;
  pthread_cond_broadcast(&gate->ready);
  while (!gate->released && !failed)
    failed = pthread_cond_wait(&gate->ready, &gate->lock) != 0;
  pthread_mutex_unlock(&gate->lock);
  return failed ? -1 : 0;
}

static int classify_response(int request_status, const char *response) {
  if (request_status == 0 && response != NULL && strstr(response, "200 OK") != NULL)
    return 1;
  if (request_status == 0 && response != NULL &&
      strstr(response, "503 Service Unavailable") != NULL &&
      strstr(response, "\"code\":\"overloaded\"") != NULL)
    return 2;
  return 3;
}

static int build_search_request(const worker_t *worker, char **request) {
  size_t capacity;
  int written;
  if (worker->body_bytes > INT_MAX || worker->body_bytes > SIZE_MAX - 256U) return -1;
  capacity = worker->body_bytes + 256U;
  *request = malloc(capacity);
  if (*request == NULL) return -1;
  written = snprintf(
    *request, capacity,
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%.*s",
    worker->body_bytes, (int)worker->body_bytes, worker->body);
  if (written < 0 || (size_t)written >= capacity) {
    free(*request);
    *request = NULL;
    return -1;
  }
  return 0;
}

static int build_update_request(const worker_t *worker, size_t ordinal,
                                char *request, size_t capacity) {
  char body[512];
  int body_written = snprintf(
    body, sizeof(body),
    "{\"operations\":[{\"operation\":\"upsert\","
    "\"id\":\"%s-%012zu\",\"title\":\"million baseline\","
    "\"body\":\"common baseline update %zu\"}]}",
    worker->update_prefix, ordinal, ordinal);
  int request_written;
  if (body_written < 0 || (size_t)body_written >= sizeof(body)) return -1;
  request_written = snprintf(
    request, capacity,
    "POST /v2/documents:batch HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
    body_written, body);
  return request_written >= 0 && (size_t)request_written < capacity ? 0 : -1;
}

static void *run_worker(void *opaque) {
  worker_t *worker = opaque;
  char *search_request = NULL;
  size_t i;
  if (worker->kind == REQUEST_SEARCH &&
      build_search_request(worker, &search_request) != 0) {
    worker->failed = 1;
    (void)gate_arrive(worker->gate);
    return NULL;
  }
  if (gate_arrive(worker->gate) != 0) {
    free(search_request);
    worker->failed = 1;
    return NULL;
  }
  for (i = 0U; i < worker->count; i++) {
    struct timespec start, end;
    char update_request[1024];
    const char *request = search_request;
    char *response = NULL;
    int request_status;
    size_t result_index = worker->first + i;
    if (worker->kind == REQUEST_UPDATE) {
      if (build_update_request(worker, result_index,
                               update_request, sizeof(update_request)) != 0) {
        worker->failed = 1;
        break;
      }
      request = update_request;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
      worker->failed = 1;
      break;
    }
    request_status = ytest_http_send_text(worker->port, request, &response);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
      free(response);
      worker->failed = 1;
      break;
    }
    worker->latencies[result_index] = elapsed_ms(start, end);
    worker->statuses[result_index] =
      (unsigned char)classify_response(request_status, response);
    free(response);
  }
  if (clock_gettime(CLOCK_MONOTONIC, &worker->ended_at) != 0)
    worker->failed = 1;
  free(search_request);
  return NULL;
}

static int mode_start(mode_state_t *state, const options_t *options,
                      request_kind_t kind, const char *body, size_t body_bytes,
                      size_t request_count, size_t concurrency,
                      start_gate_t *gate) {
  size_t i, assigned = 0U;
  size_t base = request_count / concurrency;
  size_t remainder = request_count % concurrency;
  memset(state, 0, sizeof(*state));
  state->request_count = request_count;
  state->concurrency = concurrency;
  state->threads = calloc(concurrency, sizeof(*state->threads));
  state->workers = calloc(concurrency, sizeof(*state->workers));
  state->latencies = calloc(request_count, sizeof(*state->latencies));
  state->statuses = calloc(request_count, sizeof(*state->statuses));
  if (state->threads == NULL || state->workers == NULL ||
      state->latencies == NULL || state->statuses == NULL) return -1;
  for (i = 0U; i < concurrency; i++) {
    worker_t *worker = &state->workers[i];
    worker->port = options->port;
    worker->kind = kind;
    worker->body = body;
    worker->body_bytes = body_bytes;
    worker->update_prefix = options->update_prefix;
    worker->first = assigned;
    worker->count = base + (i < remainder ? 1U : 0U);
    worker->latencies = state->latencies;
    worker->statuses = state->statuses;
    worker->gate = gate;
    assigned += worker->count;
    if (pthread_create(&state->threads[i], NULL, run_worker, worker) != 0)
      return -1;
    state->started_threads++;
  }
  return 0;
}

static int mode_finish(mode_state_t *state, mode_result_t *result) {
  struct timespec ended_at = state->started_at;
  size_t i, successful_index = 0U;
  int failed = 0;
  memset(result, 0, sizeof(*result));
  for (i = 0U; i < state->started_threads; i++) {
    if (pthread_join(state->threads[i], NULL) != 0 || state->workers[i].failed)
      failed = 1;
    if (elapsed_ms(ended_at, state->workers[i].ended_at) > 0.0)
      ended_at = state->workers[i].ended_at;
  }
  if (!failed) {
    for (i = 0U; i < state->request_count; i++) {
      if (state->statuses[i] == 1U)
        state->latencies[successful_index++] = state->latencies[i];
      else if (state->statuses[i] == 2U)
        result->overloaded++;
      else
        result->failed++;
    }
    result->successful = successful_index;
    qsort(state->latencies, successful_index, sizeof(*state->latencies),
          compare_double);
    result->p50_ms = percentile(state->latencies, successful_index, 0.50);
    result->p95_ms = percentile(state->latencies, successful_index, 0.95);
    result->p99_ms = percentile(state->latencies, successful_index, 0.99);
    result->elapsed_ms = elapsed_ms(state->started_at, ended_at);
    if (result->elapsed_ms > 0.0)
      result->requests_per_second =
        (double)state->request_count * 1000.0 / result->elapsed_ms;
  }
  return failed ? -1 : 0;
}

static void mode_free(mode_state_t *state) {
  if (state == NULL) return;
  free(state->threads);
  free(state->workers);
  free(state->latencies);
  free(state->statuses);
  memset(state, 0, sizeof(*state));
}

static int parse_size(const char *value, size_t *output) {
  char *end = NULL;
  unsigned long long parsed;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' ||
      parsed == 0U || parsed > SIZE_MAX) return -1;
  *output = (size_t)parsed;
  return 0;
}

static int valid_prefix(const char *value) {
  size_t i, bytes;
  if (value == NULL) return 0;
  bytes = strlen(value);
  if (bytes == 0U || bytes > 64U) return 0;
  for (i = 0U; i < bytes; i++)
    if (!isalnum((unsigned char)value[i]) && value[i] != '-' && value[i] != '_')
      return 0;
  return 1;
}

static int parse_options(int argc, char **argv, options_t *options) {
  int i;
  memset(options, 0, sizeof(*options));
  for (i = 1; i < argc; i++) {
    const char *key = argv[i];
    const char *value;
    if (strcmp(key, "--require-all-success") == 0) {
      options->require_all_success = 1;
      continue;
    }
    if (++i >= argc) return -1;
    value = argv[i];
    if (strcmp(key, "--port") == 0) {
      size_t parsed;
      if (parse_size(value, &parsed) != 0 || parsed > 65535U) return -1;
      options->port = (int)parsed;
    } else if (strcmp(key, "--core-pid") == 0) {
      options->core_pid = strtol(value, NULL, 10);
    } else if (strcmp(key, "--front-pid") == 0) {
      options->front_pid = strtol(value, NULL, 10);
    } else if (strcmp(key, "--search-request") == 0) {
      options->search_path = value;
    } else if (strcmp(key, "--search-requests") == 0) {
      if (parse_size(value, &options->search_requests) != 0) return -1;
    } else if (strcmp(key, "--search-concurrency") == 0) {
      if (parse_size(value, &options->search_concurrency) != 0) return -1;
    } else if (strcmp(key, "--update-requests") == 0) {
      if (parse_size(value, &options->update_requests) != 0) return -1;
    } else if (strcmp(key, "--update-concurrency") == 0) {
      if (parse_size(value, &options->update_concurrency) != 0) return -1;
    } else if (strcmp(key, "--update-prefix") == 0) {
      options->update_prefix = value;
    } else {
      return -1;
    }
  }
  if (options->port <= 0 || options->core_pid <= 0 || options->front_pid <= 0)
    return -1;
  if (options->search_requests > 0U) {
    if (options->search_path == NULL || options->search_concurrency == 0U ||
        options->search_requests < options->search_concurrency) return -1;
  } else if (options->search_path != NULL || options->search_concurrency != 0U) {
    return -1;
  }
  if (options->update_requests > 0U) {
    if (options->update_concurrency == 0U ||
        options->update_requests < options->update_concurrency ||
        !valid_prefix(options->update_prefix)) return -1;
  } else if (options->update_concurrency != 0U || options->update_prefix != NULL) {
    return -1;
  }
  return options->search_requests > 0U || options->update_requests > 0U ? 0 : -1;
}

static int validate_search_request(const char *json, size_t bytes) {
  yyjson_doc *document = yyjson_read(json, bytes, 0U);
  yyjson_val *root, *mode;
  int valid;
  if (document == NULL) return -1;
  root = yyjson_doc_get_root(document);
  mode = yyjson_is_obj(root) ? yyjson_obj_get(root, "mode") : NULL;
  valid = yyjson_is_str(mode) && strcmp(yyjson_get_str(mode), "lexical") == 0;
  yyjson_doc_free(document);
  return valid ? 0 : -1;
}

static unsigned long process_rss_kib(long pid) {
  char command[64];
  FILE *stream;
  unsigned long rss = 0UL;
  int written = snprintf(command, sizeof(command), "ps -o rss= -p %ld", pid);
  if (written < 0 || (size_t)written >= sizeof(command)) return 0UL;
  stream = popen(command, "r");
  if (stream == NULL) return 0UL;
  if (fscanf(stream, "%lu", &rss) != 1) rss = 0UL;
  (void)pclose(stream);
  return rss;
}

int main(int argc, char **argv) {
  options_t options;
  mode_state_t search_state, update_state;
  mode_result_t search_result, update_result;
  start_gate_t gate;
  char *search_body = NULL;
  size_t search_body_bytes = 0U;
  size_t total_concurrency;
  struct timespec started_at;
  unsigned long rss_kib;
  int search_enabled, update_enabled, failed = 0, passed;
  memset(&search_state, 0, sizeof(search_state));
  memset(&update_state, 0, sizeof(update_state));
  memset(&search_result, 0, sizeof(search_result));
  memset(&update_result, 0, sizeof(update_result));
  if (argc == 2 &&
      (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    usage(stdout);
    return EXIT_SUCCESS;
  }
  if (parse_options(argc, argv, &options) != 0) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  search_enabled = options.search_requests > 0U;
  update_enabled = options.update_requests > 0U;
  if (search_enabled &&
      (ytest_read_file(options.search_path, &search_body,
                       &search_body_bytes) != 0 ||
       validate_search_request(search_body, search_body_bytes) != 0)) {
    free(search_body);
    return EXIT_FAILURE;
  }
  total_concurrency = options.search_concurrency + options.update_concurrency;
  if (gate_init(&gate, total_concurrency) != 0) {
    free(search_body);
    return EXIT_FAILURE;
  }
  if ((search_enabled &&
       mode_start(&search_state, &options, REQUEST_SEARCH,
                  search_body, search_body_bytes, options.search_requests,
                  options.search_concurrency, &gate) != 0) ||
      (update_enabled &&
       mode_start(&update_state, &options, REQUEST_UPDATE,
                  NULL, 0U, options.update_requests,
                  options.update_concurrency, &gate) != 0) ||
      clock_gettime(CLOCK_MONOTONIC, &started_at) != 0) {
    gate_release_after_failure(&gate);
    failed = 1;
  } else {
    search_state.started_at = started_at;
    update_state.started_at = started_at;
    if (gate_wait_for_workers(&gate) != 0) failed = 1;
  }
  if (search_enabled && mode_finish(&search_state, &search_result) != 0)
    failed = 1;
  if (update_enabled && mode_finish(&update_state, &update_result) != 0)
    failed = 1;
  rss_kib = process_rss_kib(options.core_pid) +
            process_rss_kib(options.front_pid);
  passed = !failed && rss_kib > 0UL &&
           search_result.failed == 0U && update_result.failed == 0U;
  if (options.require_all_success)
    passed = passed && search_result.overloaded == 0U &&
             update_result.overloaded == 0U;
  printf(
    "{\"search_requests\":%zu,\"search_concurrency\":%zu,"
    "\"search_successful\":%zu,\"search_overloaded\":%zu,"
    "\"search_failed\":%zu,\"search_p50_ms\":%.3f,"
    "\"search_p95_ms\":%.3f,\"search_p99_ms\":%.3f,"
    "\"search_elapsed_ms\":%.3f,\"search_requests_per_second\":%.3f,"
    "\"update_requests\":%zu,\"update_concurrency\":%zu,"
    "\"update_successful\":%zu,\"update_overloaded\":%zu,"
    "\"update_failed\":%zu,\"update_p50_ms\":%.3f,"
    "\"update_p95_ms\":%.3f,\"update_p99_ms\":%.3f,"
    "\"update_elapsed_ms\":%.3f,\"update_requests_per_second\":%.3f,"
    "\"daemon_rss_kib\":%lu,\"passed\":%s}\n",
    options.search_requests, options.search_concurrency,
    search_result.successful, search_result.overloaded, search_result.failed,
    search_result.p50_ms, search_result.p95_ms, search_result.p99_ms,
    search_result.elapsed_ms, search_result.requests_per_second,
    options.update_requests, options.update_concurrency,
    update_result.successful, update_result.overloaded, update_result.failed,
    update_result.p50_ms, update_result.p95_ms, update_result.p99_ms,
    update_result.elapsed_ms, update_result.requests_per_second,
    rss_kib, passed ? "true" : "false");
  mode_free(&search_state);
  mode_free(&update_state);
  gate_destroy(&gate);
  free(search_body);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
