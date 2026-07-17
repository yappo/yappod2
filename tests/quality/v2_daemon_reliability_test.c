#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>
#include <pthread.h>

#include "test_env.h"
#include "test_fs.h"
#include "test_http.h"
#include "test_proc.h"
#include "v2_quality_fixture.h"

#define SEARCH_WORKERS 16U
#define REQUESTS_PER_WORKER 12U
#define UPDATE_COUNT 8U
#define SMOKE_P95_LIMIT_MS 2000.0
#define SMOKE_RSS_LIMIT_KIB (1024UL * 1024UL)

typedef struct {
  int port;
  double latency_ms[REQUESTS_PER_WORKER];
  int failed;
} search_worker_t;

typedef struct {
  int port;
  int failed;
} update_worker_t;

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static int send_post(int port, const char *endpoint, const char *body, char **response) {
  char request[2048];
  int written = snprintf(request, sizeof(request),
    "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
    "Connection: close\r\nContent-Length: %zu\r\n\r\n%s",
    endpoint, strlen(body), body);
  if (written < 0 || (size_t)written >= sizeof(request)) return -1;
  return ytest_http_send_text(port, request, response);
}

static void *search_worker(void *opaque) {
  static const char body[] =
    "{\"query\":\"red\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1}";
  search_worker_t *worker = (search_worker_t *)opaque;
  size_t i;
  for (i = 0U; i < REQUESTS_PER_WORKER; i++) {
    struct timespec start, end;
    char *response = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0 ||
        send_post(worker->port, "/v2/search", body, &response) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0 || response == NULL ||
        strstr(response, "200 OK") == NULL || strstr(response, "\"id\":\"doc-red-") == NULL) {
      if (response != NULL) fprintf(stderr, "search load response: %s\n", response);
      worker->failed = 1;
      free(response);
      return NULL;
    }
    worker->latency_ms[i] = elapsed_ms(start, end);
    free(response);
  }
  return NULL;
}

static void *update_worker(void *opaque) {
  update_worker_t *worker = (update_worker_t *)opaque;
  size_t i;
  for (i = 0U; i < UPDATE_COUNT; i++) {
    char body[512], *response = NULL;
    int written = snprintf(body, sizeof(body),
      "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-live\","
      "\"url\":\"https://quality.test/live\",\"title\":\"live %zu\","
      "\"body\":\"live generation %zu\",\"metadata\":{\"topic\":\"live\"},"
      "\"vectors\":[[1,0,0]]}]}", i, i);
    if (written < 0 || (size_t)written >= sizeof(body) ||
        send_post(worker->port, "/v2/documents:batch", body, &response) != 0 ||
        response == NULL || strstr(response, "200 OK") == NULL) {
      if (response != NULL) fprintf(stderr, "update load response: %s\n", response);
      worker->failed = 1;
      free(response);
      return NULL;
    }
    free(response);
  }
  return NULL;
}

static int compare_double(const void *left, const void *right) {
  double a = *(const double *)left, b = *(const double *)right;
  return a < b ? -1 : a > b;
}

static unsigned long process_rss_kib(pid_t pid) {
  char command[64];
  FILE *stream;
  unsigned long rss = 0UL;
  int written = snprintf(command, sizeof(command), "ps -o rss= -p %ld", (long)pid);
  if (written < 0 || (size_t)written >= sizeof(command)) return 0UL;
  stream = popen(command, "r");
  if (stream == NULL) return 0UL;
  if (fscanf(stream, "%lu", &rss) != 1) rss = 0UL;
  (void)pclose(stream);
  return rss;
}

static void test_concurrent_search_update_load_and_rss(void **state) {
  ytest_env_t env;
  ytest_daemon_stack_t stack;
  search_worker_t workers[SEARCH_WORKERS];
  update_worker_t updater;
  pthread_t search_threads[SEARCH_WORKERS], update_thread, concurrent_search_thread;
  double latencies[SEARCH_WORKERS * REQUESTS_PER_WORKER];
  char run_dir[PATH_MAX], policy_path[PATH_MAX], *response = NULL;
  FILE *policy_file;
  unsigned long rss_kib;
  size_t i, j, latency_count = 0U, p95_rank;
  (void)state;

  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(YAP_Test_v2_quality_index_create(env.tmp_root), 0);
  assert_int_equal(ytest_path_join(run_dir, sizeof(run_dir), env.tmp_root, "run"), 0);
  assert_int_equal(ytest_path_join(policy_path, sizeof(policy_path), env.tmp_root,
                                   "runtime.toml"), 0);
  policy_file = fopen(policy_path, "wb"); assert_non_null(policy_file);
  assert_true(fputs("[daemon]\nmax_inflight=16\n", policy_file) >= 0);
  assert_int_equal(fclose(policy_file), 0);
  ytest_daemon_stack_init(&stack);
  if (ytest_daemon_stack_start_with_config(&stack, env.build_dir, env.tmp_root, run_dir,
                                           policy_path) != 0) {
    ytest_daemon_stack_dump_logs(&stack, stderr);
    fail_msg("failed to start daemon stack");
  }
  memset(workers, 0, sizeof(workers));
  memset(&updater, 0, sizeof(updater));
  for (i = 0U; i < SEARCH_WORKERS; i++) {
    workers[i].port = stack.front_port;
    assert_int_equal(pthread_create(&search_threads[i], NULL, search_worker, &workers[i]), 0);
  }
  for (i = 0U; i < SEARCH_WORKERS; i++) assert_int_equal(pthread_join(search_threads[i], NULL), 0);
  for (i = 0U; i < SEARCH_WORKERS; i++) {
    assert_int_equal(workers[i].failed, 0);
    for (j = 0U; j < REQUESTS_PER_WORKER; j++) latencies[latency_count++] = workers[i].latency_ms[j];
  }
  memset(&workers[0], 0, sizeof(workers[0])); workers[0].port = stack.front_port;
  updater.port = stack.front_port;
  assert_int_equal(pthread_create(&concurrent_search_thread, NULL, search_worker, &workers[0]), 0);
  assert_int_equal(pthread_create(&update_thread, NULL, update_worker, &updater), 0);
  assert_int_equal(pthread_join(concurrent_search_thread, NULL), 0);
  assert_int_equal(pthread_join(update_thread, NULL), 0);
  assert_int_equal(workers[0].failed, 0); assert_int_equal(updater.failed, 0);
  qsort(latencies, latency_count, sizeof(latencies[0]), compare_double);
  p95_rank = (size_t)(0.95 * (double)latency_count + 0.999999);
  if (p95_rank == 0U) p95_rank = 1U;
  print_message("v2_daemon_concurrency\t%u\n", SEARCH_WORKERS);
  print_message("v2_daemon_requests\t%zu\n", latency_count);
  print_message("v2_daemon_lexical_p95_ms\t%.3f\n", latencies[p95_rank - 1U]);
  assert_true(latencies[p95_rank - 1U] <= SMOKE_P95_LIMIT_MS);

  rss_kib = process_rss_kib(stack.front_pid) + process_rss_kib(stack.core_pid);
  print_message("v2_daemon_rss_kib\t%lu\n", rss_kib);
  assert_true(rss_kib > 0UL);
  assert_true(rss_kib <= SMOKE_RSS_LIMIT_KIB);
  assert_int_equal(send_post(stack.front_port, "/v2/search",
    "{\"query\":\"generation\",\"mode\":\"lexical\","
    "\"scope\":\"documents\",\"limit\":1}", &response), 0);
  assert_non_null(strstr(response, "200 OK"));
  assert_non_null(strstr(response, "\"id\":\"doc-live\""));
  assert_non_null(strstr(response, "\"generation\":9"));
  free(response);
  assert_true(ytest_daemon_stack_alive(&stack));
  ytest_daemon_stack_stop(&stack);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_concurrent_search_update_load_and_rss)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
