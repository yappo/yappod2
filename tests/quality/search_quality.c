#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "search_quality_metrics.h"
#include "test_env.h"
#include "test_fs.h"
#include "test_index.h"

typedef struct {
  char *document_id;
  int relevance;
} quality_judgment_t;

typedef struct {
  char *query_id;
  char *query_text;
  quality_judgment_t *judgments;
  size_t judgment_count;
  char **hits;
  size_t hit_count;
} quality_query_t;

typedef struct {
  char *name;
  int is_minimum;
  double value;
} quality_threshold_t;

typedef struct {
  const char *build_dir;
  const char *input_path;
  const char *queries_path;
  const char *qrels_path;
  const char *baseline_path;
  int repeat;
  size_t cutoff;
} quality_options_t;

static char *quality_strdup(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value);
  copy = (char *)malloc(len + 1U);
  if (copy != NULL) {
    memcpy(copy, value, len + 1U);
  }
  return copy;
}

static void trim_line_end(char *line) {
  size_t len;

  if (line == NULL) {
    return;
  }
  len = strlen(line);
  while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
    line[--len] = '\0';
  }
}

static char *next_tsv_field(char **cursor) {
  char *field;
  char *tab;

  if (cursor == NULL || *cursor == NULL) {
    return NULL;
  }
  field = *cursor;
  tab = strchr(field, '\t');
  if (tab == NULL) {
    *cursor = NULL;
  } else {
    *tab = '\0';
    *cursor = tab + 1;
  }
  return field;
}

static quality_query_t *find_query(quality_query_t *queries, size_t query_count,
                                   const char *query_id) {
  size_t i;

  for (i = 0; i < query_count; i++) {
    if (strcmp(queries[i].query_id, query_id) == 0) {
      return &queries[i];
    }
  }
  return NULL;
}

static void free_hits(quality_query_t *query) {
  size_t i;

  for (i = 0; i < query->hit_count; i++) {
    free(query->hits[i]);
  }
  free(query->hits);
  query->hits = NULL;
  query->hit_count = 0U;
}

static void free_string_list(char **values, size_t value_count) {
  size_t i;

  for (i = 0; i < value_count; i++) {
    free(values[i]);
  }
  free(values);
}

static void free_queries(quality_query_t *queries, size_t query_count) {
  size_t i;
  size_t j;

  for (i = 0; i < query_count; i++) {
    free(queries[i].query_id);
    free(queries[i].query_text);
    for (j = 0; j < queries[i].judgment_count; j++) {
      free(queries[i].judgments[j].document_id);
    }
    free(queries[i].judgments);
    free_hits(&queries[i]);
  }
  free(queries);
}

static int load_queries(const char *path, quality_query_t **queries_out, size_t *query_count_out) {
  FILE *fp;
  char *line = NULL;
  size_t line_cap = 0U;
  ssize_t line_len;
  size_t line_number = 0U;
  quality_query_t *queries = NULL;
  size_t query_count = 0U;
  int rc = -1;

  fp = fopen(path, "r");
  if (fp == NULL) {
    perror(path);
    return -1;
  }

  while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
    char *cursor;
    char *query_id;
    char *query_text;
    quality_query_t *next;
    (void)line_len;
    line_number++;
    trim_line_end(line);
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    cursor = line;
    query_id = next_tsv_field(&cursor);
    query_text = next_tsv_field(&cursor);
    if (query_id == NULL || query_text == NULL || cursor != NULL || query_id[0] == '\0' ||
        query_text[0] == '\0' || find_query(queries, query_count, query_id) != NULL) {
      fprintf(stderr, "%s:%zu: expected unique query_id<TAB>query_text\n", path, line_number);
      goto done;
    }

    next = (quality_query_t *)realloc(queries, sizeof(*queries) * (query_count + 1U));
    if (next == NULL) {
      goto done;
    }
    queries = next;
    memset(&queries[query_count], 0, sizeof(queries[query_count]));
    query_count++;
    queries[query_count - 1U].query_id = quality_strdup(query_id);
    queries[query_count - 1U].query_text = quality_strdup(query_text);
    if (queries[query_count - 1U].query_id == NULL ||
        queries[query_count - 1U].query_text == NULL) {
      goto done;
    }
  }

  if (ferror(fp) || query_count == 0U) {
    fprintf(stderr, "%s: no valid queries\n", path);
    goto done;
  }

  *queries_out = queries;
  *query_count_out = query_count;
  queries = NULL;
  query_count = 0U;
  rc = 0;

done:
  free(line);
  fclose(fp);
  free_queries(queries, query_count);
  return rc;
}

static int append_judgment(quality_query_t *query, const char *document_id, int relevance) {
  quality_judgment_t *next;
  size_t i;

  for (i = 0; i < query->judgment_count; i++) {
    if (strcmp(query->judgments[i].document_id, document_id) == 0) {
      return -1;
    }
  }

  next = (quality_judgment_t *)realloc(
    query->judgments, sizeof(*query->judgments) * (query->judgment_count + 1U));
  if (next == NULL) {
    return -1;
  }
  query->judgments = next;
  query->judgments[query->judgment_count].document_id = quality_strdup(document_id);
  query->judgments[query->judgment_count].relevance = relevance;
  if (query->judgments[query->judgment_count].document_id == NULL) {
    return -1;
  }
  query->judgment_count++;
  return 0;
}

static int load_qrels(const char *path, quality_query_t *queries, size_t query_count) {
  FILE *fp;
  char *line = NULL;
  size_t line_cap = 0U;
  ssize_t line_len;
  size_t line_number = 0U;
  int rc = -1;

  fp = fopen(path, "r");
  if (fp == NULL) {
    perror(path);
    return -1;
  }

  while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
    char *cursor;
    char *query_id;
    char *document_id;
    char *relevance_text;
    char *endptr;
    long relevance;
    quality_query_t *query;
    (void)line_len;
    line_number++;
    trim_line_end(line);
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    cursor = line;
    query_id = next_tsv_field(&cursor);
    document_id = next_tsv_field(&cursor);
    relevance_text = next_tsv_field(&cursor);
    errno = 0;
    relevance = (relevance_text != NULL) ? strtol(relevance_text, &endptr, 10) : -1;
    query = (query_id != NULL) ? find_query(queries, query_count, query_id) : NULL;
    if (query_id == NULL || document_id == NULL || relevance_text == NULL || cursor != NULL ||
        query == NULL || document_id[0] == '\0' || errno != 0 || endptr == relevance_text ||
        *endptr != '\0' || relevance < 0L || relevance > 3L ||
        append_judgment(query, document_id, (int)relevance) != 0) {
      fprintf(stderr,
              "%s:%zu: expected known_query_id<TAB>document_id<TAB>relevance(0..3)\n", path,
              line_number);
      goto done;
    }
  }

  if (ferror(fp)) {
    goto done;
  }
  {
    size_t i;
    for (i = 0; i < query_count; i++) {
      size_t j;
      int has_relevant = 0;
      for (j = 0; j < queries[i].judgment_count; j++) {
        if (queries[i].judgments[j].relevance > 0) {
          has_relevant = 1;
        }
      }
      if (!has_relevant) {
        fprintf(stderr, "%s: query %s has no relevant judgment\n", path, queries[i].query_id);
        goto done;
      }
    }
  }
  rc = 0;

done:
  free(line);
  fclose(fp);
  return rc;
}

static int parse_hits(const char *output, quality_query_t *query) {
  const char *cursor = output;

  free_hits(query);
  while (cursor != NULL && *cursor != '\0') {
    const char *line_end = strchr(cursor, '\n');
    size_t line_len = (line_end != NULL) ? (size_t)(line_end - cursor) : strlen(cursor);

    if (line_len > 4U && strncmp(cursor, "URL:", 4U) == 0) {
      char **next =
        (char **)realloc(query->hits, sizeof(*query->hits) * (query->hit_count + 1U));
      char *document_id;
      if (next == NULL) {
        return -1;
      }
      query->hits = next;
      document_id = (char *)malloc(line_len - 4U + 1U);
      if (document_id == NULL) {
        return -1;
      }
      memcpy(document_id, cursor + 4U, line_len - 4U);
      document_id[line_len - 4U] = '\0';
      query->hits[query->hit_count++] = document_id;
    }
    cursor = (line_end != NULL) ? line_end + 1 : NULL;
  }
  return 0;
}

static int timespec_diff_ms(const struct timespec *start, const struct timespec *end,
                            double *milliseconds) {
  time_t seconds;
  long nanoseconds;

  if (start == NULL || end == NULL || milliseconds == NULL) {
    return -1;
  }
  seconds = end->tv_sec - start->tv_sec;
  nanoseconds = end->tv_nsec - start->tv_nsec;
  if (nanoseconds < 0L) {
    seconds--;
    nanoseconds += 1000000000L;
  }
  if (seconds < 0) {
    return -1;
  }
  *milliseconds = ((double)seconds * 1000.0) + ((double)nanoseconds / 1000000.0);
  return 0;
}

static int hits_match(const quality_query_t *query, char *const *expected, size_t expected_count) {
  size_t i;

  if (query->hit_count != expected_count) {
    return 0;
  }
  for (i = 0; i < expected_count; i++) {
    if (strcmp(query->hits[i], expected[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int run_queries(const ytest_env_t *env, const char *index_dir, quality_query_t *queries,
                       size_t query_count, int repeat, double **latencies_out,
                       size_t *latency_count_out) {
  double *latencies;
  size_t latency_count = 0U;
  size_t i;

  latencies = (double *)calloc(query_count * (size_t)repeat, sizeof(double));
  if (latencies == NULL) {
    return -1;
  }

  for (i = 0; i < query_count; i++) {
    int iteration;
    char **expected_hits = NULL;
    size_t expected_hit_count = 0U;

    for (iteration = 0; iteration < repeat; iteration++) {
      const char *args[1] = {queries[i].query_text};
      ytest_cmd_result_t result;
      struct timespec start;
      struct timespec end;

      ytest_cmd_result_init(&result);
      if (clock_gettime(CLOCK_MONOTONIC, &start) != 0 ||
          ytest_search_capture(env, index_dir, args, 1U, &result) != 0 || !result.exited ||
          result.exit_code != 0 || clock_gettime(CLOCK_MONOTONIC, &end) != 0 ||
          timespec_diff_ms(&start, &end, &latencies[latency_count]) != 0 ||
          parse_hits(result.output, &queries[i]) != 0) {
        fprintf(stderr, "query %s failed\n", queries[i].query_id);
        ytest_cmd_result_free(&result);
        free_string_list(expected_hits, expected_hit_count);
        free(latencies);
        return -1;
      }
      ytest_cmd_result_free(&result);
      latency_count++;

      if (iteration == 0) {
        size_t j;
        expected_hit_count = queries[i].hit_count;
        expected_hits = (char **)calloc(expected_hit_count, sizeof(*expected_hits));
        if (expected_hit_count > 0U && expected_hits == NULL) {
          free(latencies);
          return -1;
        }
        for (j = 0; j < expected_hit_count; j++) {
          expected_hits[j] = quality_strdup(queries[i].hits[j]);
          if (expected_hits[j] == NULL) {
            free_string_list(expected_hits, j);
            free(latencies);
            return -1;
          }
        }
      } else if (!hits_match(&queries[i], expected_hits, expected_hit_count)) {
        fprintf(stderr, "query %s returned non-deterministic ordering\n", queries[i].query_id);
        free_string_list(expected_hits, expected_hit_count);
        free(latencies);
        return -1;
      }
    }
    free_string_list(expected_hits, expected_hit_count);
  }

  *latencies_out = latencies;
  *latency_count_out = latency_count;
  return 0;
}

static int aggregate_metrics(quality_query_t *queries, size_t query_count, size_t cutoff,
                             double *ndcg, double *mrr, double *recall) {
  size_t i;

  *ndcg = 0.0;
  *mrr = 0.0;
  *recall = 0.0;
  for (i = 0; i < query_count; i++) {
    YAP_QUALITY_JUDGMENT *judgments;
    YAP_QUALITY_HIT *hits;
    YAP_QUALITY_QUERY_METRICS metrics;
    size_t j;

    judgments =
      (YAP_QUALITY_JUDGMENT *)calloc(queries[i].judgment_count, sizeof(*judgments));
    hits = (YAP_QUALITY_HIT *)calloc(queries[i].hit_count, sizeof(*hits));
    if (judgments == NULL || (queries[i].hit_count > 0U && hits == NULL)) {
      free(judgments);
      free(hits);
      return -1;
    }
    for (j = 0; j < queries[i].judgment_count; j++) {
      judgments[j].document_id = queries[i].judgments[j].document_id;
      judgments[j].relevance = queries[i].judgments[j].relevance;
    }
    for (j = 0; j < queries[i].hit_count; j++) {
      hits[j].document_id = queries[i].hits[j];
    }
    if (YAP_Quality_metrics_calculate(judgments, queries[i].judgment_count, hits,
                                      queries[i].hit_count, cutoff, &metrics) != 0) {
      free(judgments);
      free(hits);
      return -1;
    }
    *ndcg += metrics.ndcg_at_k;
    *mrr += metrics.mrr_at_k;
    *recall += metrics.recall_at_k;
    free(judgments);
    free(hits);
  }

  *ndcg /= (double)query_count;
  *mrr /= (double)query_count;
  *recall /= (double)query_count;
  return 0;
}

static int load_thresholds(const char *path, quality_threshold_t **thresholds_out,
                           size_t *threshold_count_out) {
  FILE *fp;
  char *line = NULL;
  size_t line_cap = 0U;
  ssize_t line_len;
  size_t line_number = 0U;
  quality_threshold_t *thresholds = NULL;
  size_t threshold_count = 0U;
  int rc = -1;

  fp = fopen(path, "r");
  if (fp == NULL) {
    perror(path);
    return -1;
  }
  while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
    char *cursor;
    char *name;
    char *comparison;
    char *value_text;
    char *endptr;
    double value;
    quality_threshold_t *next;
    (void)line_len;
    line_number++;
    trim_line_end(line);
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    cursor = line;
    name = next_tsv_field(&cursor);
    comparison = next_tsv_field(&cursor);
    value_text = next_tsv_field(&cursor);
    errno = 0;
    value = (value_text != NULL) ? strtod(value_text, &endptr) : -1.0;
    if (name == NULL || comparison == NULL || value_text == NULL || cursor != NULL ||
        (strcmp(comparison, "min") != 0 && strcmp(comparison, "max") != 0) || errno != 0 ||
        endptr == value_text || *endptr != '\0' || !isfinite(value) || value < 0.0) {
      fprintf(stderr, "%s:%zu: expected metric<TAB>min|max<TAB>value\n", path, line_number);
      goto done;
    }
    next = (quality_threshold_t *)realloc(
      thresholds, sizeof(*thresholds) * (threshold_count + 1U));
    if (next == NULL) {
      goto done;
    }
    thresholds = next;
    thresholds[threshold_count].name = quality_strdup(name);
    thresholds[threshold_count].is_minimum = strcmp(comparison, "min") == 0;
    thresholds[threshold_count].value = value;
    if (thresholds[threshold_count].name == NULL) {
      goto done;
    }
    threshold_count++;
  }
  if (ferror(fp) || threshold_count == 0U) {
    goto done;
  }

  *thresholds_out = thresholds;
  *threshold_count_out = threshold_count;
  thresholds = NULL;
  threshold_count = 0U;
  rc = 0;

done:
  {
    size_t i;
    for (i = 0; i < threshold_count; i++) {
      free(thresholds[i].name);
    }
  }
  free(thresholds);
  free(line);
  fclose(fp);
  return rc;
}

static int metric_value(const char *name, double ndcg, double mrr, double recall, double p95,
                        double *value) {
  if (strcmp(name, "ndcg_at_10") == 0) {
    *value = ndcg;
  } else if (strcmp(name, "mrr_at_10") == 0) {
    *value = mrr;
  } else if (strcmp(name, "recall_at_10") == 0) {
    *value = recall;
  } else if (strcmp(name, "p95_latency_ms") == 0) {
    *value = p95;
  } else {
    return -1;
  }
  return 0;
}

static int check_thresholds(const quality_threshold_t *thresholds, size_t threshold_count,
                            double ndcg, double mrr, double recall, double p95) {
  size_t i;
  int failed = 0;

  for (i = 0; i < threshold_count; i++) {
    double actual;
    int passes;
    if (metric_value(thresholds[i].name, ndcg, mrr, recall, p95, &actual) != 0) {
      fprintf(stderr, "unknown baseline metric: %s\n", thresholds[i].name);
      return -1;
    }
    passes = thresholds[i].is_minimum ? actual + 0.0000005 >= thresholds[i].value
                                      : actual <= thresholds[i].value;
    if (!passes) {
      fprintf(stderr, "baseline failed: %s actual=%.6f expected_%s=%.6f\n",
              thresholds[i].name, actual, thresholds[i].is_minimum ? "min" : "max",
              thresholds[i].value);
      failed = 1;
    }
  }
  return failed ? -1 : 0;
}

static void free_thresholds(quality_threshold_t *thresholds, size_t threshold_count) {
  size_t i;
  for (i = 0; i < threshold_count; i++) {
    free(thresholds[i].name);
  }
  free(thresholds);
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --build-dir DIR --input FILE --queries FILE --qrels FILE "
          "--baseline FILE [--repeat N]\n",
          program);
}

static int parse_positive_int(const char *text, int *value) {
  char *endptr;
  long parsed;

  errno = 0;
  parsed = strtol(text, &endptr, 10);
  if (errno != 0 || endptr == text || *endptr != '\0' || parsed <= 0L || parsed > 10000L) {
    return -1;
  }
  *value = (int)parsed;
  return 0;
}

static int parse_options(int argc, char **argv, quality_options_t *options) {
  int i;

  memset(options, 0, sizeof(*options));
  options->repeat = 5;
  options->cutoff = 10U;
  for (i = 1; i < argc; i++) {
    const char *name = argv[i];
    const char *value;
    if (i + 1 >= argc) {
      return -1;
    }
    value = argv[++i];
    if (strcmp(name, "--build-dir") == 0) {
      options->build_dir = value;
    } else if (strcmp(name, "--input") == 0) {
      options->input_path = value;
    } else if (strcmp(name, "--queries") == 0) {
      options->queries_path = value;
    } else if (strcmp(name, "--qrels") == 0) {
      options->qrels_path = value;
    } else if (strcmp(name, "--baseline") == 0) {
      options->baseline_path = value;
    } else if (strcmp(name, "--repeat") == 0) {
      if (parse_positive_int(value, &options->repeat) != 0) {
        return -1;
      }
    } else {
      return -1;
    }
  }

  if (options->build_dir == NULL || options->input_path == NULL || options->queries_path == NULL ||
      options->qrels_path == NULL || options->baseline_path == NULL) {
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  quality_options_t options;
  quality_query_t *queries = NULL;
  size_t query_count = 0U;
  quality_threshold_t *thresholds = NULL;
  size_t threshold_count = 0U;
  ytest_env_t env;
  char index_dir[PATH_MAX];
  double *latencies = NULL;
  size_t latency_count = 0U;
  double ndcg;
  double mrr;
  double recall;
  double p95;
  int rc = EXIT_FAILURE;

  memset(&env, 0, sizeof(env));
  if (parse_options(argc, argv, &options) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (load_queries(options.queries_path, &queries, &query_count) != 0 ||
      load_qrels(options.qrels_path, queries, query_count) != 0 ||
      load_thresholds(options.baseline_path, &thresholds, &threshold_count) != 0 ||
      ytest_env_init(&env) != 0 ||
      ytest_path_join(index_dir, sizeof(index_dir), env.tmp_root, "quality-index") != 0 ||
      ytest_build_index(&env, options.input_path, index_dir, NULL, 0U) != 0) {
    goto done;
  }

  if (snprintf(env.build_dir, sizeof(env.build_dir), "%s", options.build_dir) >=
      (int)sizeof(env.build_dir)) {
    goto done;
  }
  if (run_queries(&env, index_dir, queries, query_count, options.repeat, &latencies,
                  &latency_count) != 0 ||
      aggregate_metrics(queries, query_count, options.cutoff, &ndcg, &mrr, &recall) != 0) {
    goto done;
  }
  p95 = YAP_Quality_percentile_nearest_rank(latencies, latency_count, 0.95);
  if (p95 < 0.0) {
    goto done;
  }

  printf("queries\t%zu\n", query_count);
  printf("cutoff\t%zu\n", options.cutoff);
  printf("ndcg_at_10\t%.6f\n", ndcg);
  printf("mrr_at_10\t%.6f\n", mrr);
  printf("recall_at_10\t%.6f\n", recall);
  printf("p95_latency_ms\t%.3f\n", p95);

  if (check_thresholds(thresholds, threshold_count, ndcg, mrr, recall, p95) != 0) {
    goto done;
  }
  rc = EXIT_SUCCESS;

done:
  free(latencies);
  ytest_env_destroy(&env);
  free_thresholds(thresholds, threshold_count);
  free_queries(queries, query_count);
  return rc;
}
