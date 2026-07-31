#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "search_quality_metrics.h"
#include "test_env.h"
#include "test_fs.h"
#include "components/yappo_lexical_v2.h"
#include "config/yappo_config_v2.h"
#include "server/yappo_http_v2.h"
#include "storage/yappo_manifest_v2.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  size_t segments;
  size_t documents_per_segment;
  size_t iterations;
} BENCHMARK_OPTIONS;

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)};
  return view;
}

static int parse_size(const char *value, size_t minimum, size_t maximum, size_t *parsed) {
  char *end = NULL;
  unsigned long long number;
  errno = 0;
  number = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || number < minimum || number > maximum)
    return -1;
  *parsed = (size_t)number;
  return 0;
}

static int parse_options(int argc, char **argv, BENCHMARK_OPTIONS *options) {
  int i;
  options->segments = 100U;
  options->documents_per_segment = 8U;
  options->iterations = 31U;
  for (i = 1; i < argc; i += 2) {
    if (i + 1 >= argc)
      return -1;
    if (strcmp(argv[i], "--segments") == 0) {
      if (parse_size(argv[i + 1], 1U, 100000U, &options->segments) != 0)
        return -1;
    } else if (strcmp(argv[i], "--documents-per-segment") == 0) {
      if (parse_size(argv[i + 1], 1U, 10000U, &options->documents_per_segment) != 0)
        return -1;
    } else if (strcmp(argv[i], "--iterations") == 0) {
      if (parse_size(argv[i + 1], 3U, 10000U, &options->iterations) != 0)
        return -1;
    } else {
      return -1;
    }
  }
  return 0;
}

static int add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                         const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  return YAP_V2_segment_descriptor_add_component(segment, component);
}

static int write_config(const char *index_dir, YAP_V2_CONFIG *config) {
  char path[PATH_MAX];
  FILE *file;
  if (ytest_path_join(path, sizeof(path), index_dir, "config.toml") != 0)
    return -1;
  file = fopen(path, "wb");
  if (file == NULL)
    return -1;
  if (fputs("format_version=2\n[tokenizer]\nid=\"unicode_nfkc_casefold_v2\"\n"
            "[chunking]\nmax_chars=256\noverlap_chars=0\n"
            "[vector]\nenabled=false\n[metadata]\nfilterable_fields=[]\n", file) < 0 ||
      fclose(file) != 0)
    return -1;
  return YAP_V2_config_load(path, config, NULL, 0U) == YAP_V2_OK ? 0 : -1;
}

static int create_index(const char *index_dir, const BENCHMARK_OPTIONS *options) {
  YAP_V2_CONFIG config;
  YAP_V2_MANIFEST manifest;
  YAP_V2_DOCUMENT_VIEW *documents = NULL;
  YAP_V2_COMPONENT_DESCRIPTOR lexical[3];
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  char *ids = NULL;
  char segments_dir[PATH_MAX], segment_dir[PATH_MAX], path[PATH_MAX];
  char segment_id[64];
  size_t segment_index, document_index;
  int status = -1;
  if (write_config(index_dir, &config) != 0 ||
      ytest_path_join(segments_dir, sizeof(segments_dir), index_dir, "segments") != 0 ||
      ytest_mkdir_p(segments_dir, 0700) != 0)
    return -1;
  documents = (YAP_V2_DOCUMENT_VIEW *)calloc(options->documents_per_segment,
                                              sizeof(*documents));
  ids = (char *)calloc(options->documents_per_segment, 64U);
  if (documents == NULL || ids == NULL)
    goto done;
  YAP_V2_manifest_init(&manifest);
  manifest.generation = 1U;
  if (YAP_V2_config_fingerprint(&config, manifest.config_fingerprint) != YAP_V2_OK)
    goto manifest_done;
  for (segment_index = 0U; segment_index < options->segments; segment_index++) {
    int rare_segment = segment_index % 100U == 0U;
    memset(documents, 0, sizeof(*documents) * options->documents_per_segment);
    for (document_index = 0U; document_index < options->documents_per_segment;
         document_index++) {
      char *id = ids + document_index * 64U;
      if (snprintf(id, 64U, "doc-%zu-%zu", segment_index, document_index) < 0)
        goto manifest_done;
      documents[document_index].id = bytes(id);
      documents[document_index].body =
        bytes(rare_segment && document_index == 0U ? "common rare filler" : "common filler");
    }
    if (snprintf(segment_id, sizeof(segment_id), "bench-%020zu", segment_index) < 0 ||
        ytest_path_join(segment_dir, sizeof(segment_dir), segments_dir, segment_id) != 0 ||
        ytest_mkdir_p(segment_dir, 0700) != 0 ||
        ytest_path_join(path, sizeof(path), segment_dir, "documents.yap2") != 0 ||
        YAP_V2_segment_write(path, segment_id, 1U, documents,
                             options->documents_per_segment, NULL, 0U,
                             &descriptor) != YAP_V2_OK ||
        YAP_V2_lexical_write(segment_dir, 1U, documents,
                             options->documents_per_segment, NULL, 0U,
                             lexical) != YAP_V2_OK ||
        add_component(&descriptor, &lexical[0]) != YAP_V2_OK ||
        add_component(&descriptor, &lexical[1]) != YAP_V2_OK ||
        add_component(&descriptor, &lexical[2]) != YAP_V2_OK ||
        YAP_V2_manifest_add_segment(&manifest, &descriptor) != YAP_V2_OK)
      goto manifest_done;
  }
  if (ytest_path_join(path, sizeof(path), index_dir, "manifest.json") != 0 ||
      YAP_V2_manifest_save_atomic(path, &manifest) != YAP_V2_OK)
    goto manifest_done;
  status = 0;
manifest_done:
  YAP_V2_manifest_free(&manifest);
done:
  free(ids);
  free(documents);
  return status;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static int execute_query(YAP_V2_HTTP_RUNTIME *runtime, const char *term,
                         double *latency_ms) {
  char request[256], *response = NULL;
  size_t response_bytes = 0U;
  int http_status = 0, result;
  struct timespec start, end;
  if (snprintf(request, sizeof(request),
               "{\"query\":\"%s\",\"mode\":\"lexical\","
               "\"scope\":\"documents\",\"limit\":10}", term) < 0 ||
      clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    return -1;
  result = YAP_V2_http_runtime_execute(runtime, YAP_V2_HTTP_SEARCH,
    (const unsigned char *)request, strlen(request), &http_status, &response, &response_bytes);
  if (clock_gettime(CLOCK_MONOTONIC, &end) != 0 || result != 0 || http_status != 200 ||
      response == NULL) {
    free(response);
    return -1;
  }
  *latency_ms = elapsed_ms(start, end);
  free(response);
  return 0;
}

static uint64_t peak_rss_bytes(void) {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0U;
#ifdef __APPLE__
  return (uint64_t)usage.ru_maxrss;
#else
  return (uint64_t)usage.ru_maxrss * 1024U;
#endif
}

static int benchmark_scenario(YAP_V2_HTTP_RUNTIME *runtime,
                              const BENCHMARK_OPTIONS *options,
                              const char *scenario, const char *term) {
  double *latencies;
  double ignored;
  size_t i;
  latencies = (double *)calloc(options->iterations, sizeof(*latencies));
  if (latencies == NULL)
    return -1;
  for (i = 0U; i < 3U; i++)
    if (execute_query(runtime, term, &ignored) != 0) {
      free(latencies);
      return -1;
    }
  for (i = 0U; i < options->iterations; i++)
    if (execute_query(runtime, term, &latencies[i]) != 0) {
      free(latencies);
      return -1;
    }
  printf("%zu\t%zu\t%zu\t%s\t%zu\t%.6f\t%.6f\t%llu\n",
         options->segments, options->documents_per_segment,
         options->segments * options->documents_per_segment, scenario,
         options->iterations,
         YAP_Quality_percentile_nearest_rank(latencies, options->iterations, 0.50),
         YAP_Quality_percentile_nearest_rank(latencies, options->iterations, 0.95),
         (unsigned long long)peak_rss_bytes());
  free(latencies);
  return 0;
}

int main(int argc, char **argv) {
  BENCHMARK_OPTIONS options;
  ytest_env_t env;
  YAP_V2_HTTP_RUNTIME runtime;
  int status = 1;
  if (parse_options(argc, argv, &options) != 0) {
    fprintf(stderr, "usage: %s [--segments N] [--documents-per-segment N] "
                    "[--iterations N]\n", argv[0]);
    return 2;
  }
  if (options.segments > SIZE_MAX / options.documents_per_segment ||
      ytest_env_init(&env) != 0)
    return 1;
  if (create_index(env.tmp_root, &options) != 0)
    goto done;
  YAP_V2_http_runtime_init(&runtime);
  if (YAP_V2_http_runtime_open(&runtime, env.tmp_root) != YAP_V2_OK)
    goto done;
  printf("segments\tdocuments_per_segment\ttotal_documents\tscenario\titerations\t"
         "p50_ms\tp95_ms\tpeak_rss_bytes\n");
  if (benchmark_scenario(&runtime, &options, "missing", "absenttoken") == 0 &&
      benchmark_scenario(&runtime, &options, "rare", "rare") == 0 &&
      benchmark_scenario(&runtime, &options, "common", "common") == 0)
    status = 0;
  YAP_V2_http_runtime_close(&runtime);
done:
  ytest_env_destroy(&env);
  return status;
}
