#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "components/yappo_ann_v2.h"

typedef struct {
  size_t segments;
  size_t vectors;
  size_t dimensions;
  size_t iterations;
  size_t top_k;
} OPTIONS;

typedef struct {
  uint64_t key;
  double score;
} HIT;

static int parse_size(const char *value, size_t minimum, size_t maximum,
                      size_t *output) {
  char *end = NULL;
  unsigned long long parsed;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return -1;
  *output = (size_t)parsed;
  return 0;
}

static int parse_options(int argc, char **argv, OPTIONS *options) {
  int i;
  options->segments = 1000U;
  options->vectors = 100000U;
  options->dimensions = 32U;
  options->iterations = 101U;
  options->top_k = 10U;
  for (i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) return -1;
    if (strcmp(argv[i], "--segments") == 0) {
      if (parse_size(argv[i + 1], 1U, 100000U, &options->segments) != 0) return -1;
    } else if (strcmp(argv[i], "--vectors") == 0) {
      if (parse_size(argv[i + 1], 1U, 100000000U, &options->vectors) != 0) return -1;
    } else if (strcmp(argv[i], "--dimensions") == 0) {
      if (parse_size(argv[i + 1], 2U, 4096U, &options->dimensions) != 0) return -1;
    } else if (strcmp(argv[i], "--iterations") == 0) {
      if (parse_size(argv[i + 1], 3U, 100000U, &options->iterations) != 0) return -1;
    } else if (strcmp(argv[i], "--top-k") == 0) {
      if (parse_size(argv[i + 1], 1U, 1000U, &options->top_k) != 0) return -1;
    } else {
      return -1;
    }
  }
  return options->segments <= options->vectors && options->top_k <= options->vectors ? 0 : -1;
}

static uint64_t next_random(uint64_t *state) {
  uint64_t value = *state;
  value ^= value >> 12U;
  value ^= value << 25U;
  value ^= value >> 27U;
  *state = value;
  return value * UINT64_C(2685821657736338717);
}

static void generate_vectors(float *vectors, size_t count, size_t dimensions) {
  uint64_t random_state = UINT64_C(0x9e3779b97f4a7c15);
  size_t i, d;
  for (i = 0U; i < count; i++) {
    double squared = 0.0;
    for (d = 0U; d < dimensions; d++) {
      float value = (float)((int)(next_random(&random_state) % 20001U) - 10000) / 10000.0f;
      vectors[i * dimensions + d] = value;
      squared += (double)value * (double)value;
    }
    squared = sqrt(squared);
    if (squared == 0.0) {
      vectors[i * dimensions] = 1.0f;
      squared = 1.0;
    }
    for (d = 0U; d < dimensions; d++)
      vectors[i * dimensions + d] = (float)(vectors[i * dimensions + d] / squared);
  }
}

static double dot(const float *left, const float *right, size_t dimensions) {
  double score = 0.0;
  size_t i;
  for (i = 0U; i < dimensions; i++) score += (double)left[i] * (double)right[i];
  return score;
}

static void add_hit(HIT *hits, size_t capacity, size_t *count, uint64_t key,
                    double score) {
  size_t position;
  if (*count < capacity) {
    position = (*count)++;
  } else {
    if (score <= hits[*count - 1U].score) return;
    position = *count - 1U;
  }
  while (position > 0U && score > hits[position - 1U].score) {
    hits[position] = hits[position - 1U];
    position--;
  }
  hits[position].key = key;
  hits[position].score = score;
}

static int compare_double(const void *left, const void *right) {
  const double a = *(const double *)left;
  const double b = *(const double *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static size_t overlap(const HIT *left, size_t left_count,
                      const HIT *right, size_t right_count) {
  size_t i, j, found = 0U;
  for (i = 0U; i < left_count; i++)
    for (j = 0U; j < right_count; j++)
      if (left[i].key == right[j].key) { found++; break; }
  return found;
}

static uint64_t peak_rss_bytes(void) {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0U;
#if defined(__APPLE__)
  return (uint64_t)usage.ru_maxrss;
#else
  return (uint64_t)usage.ru_maxrss * 1024U;
#endif
}

int main(int argc, char **argv) {
  OPTIONS options;
  YAP_V2_ANN_INDEX global;
  YAP_V2_ANN_INDEX *local = NULL;
  float *vectors = NULL;
  uint64_t *keys = NULL;
  double *fanout_ms = NULL, *base_ms = NULL;
  size_t *starts = NULL, *counts = NULL;
  size_t request_count, segment, i, iteration;
  double fanout_recall = 0.0, base_recall = 0.0;
  int status = EXIT_FAILURE;
  if (parse_options(argc, argv, &options) != 0) {
    fprintf(stderr, "usage: %s [--segments N] [--vectors N] [--dimensions N] "
                    "[--iterations N] [--top-k N]\n", argv[0]);
    return EXIT_FAILURE;
  }
  request_count = options.top_k > SIZE_MAX / 4U ? options.vectors : options.top_k * 4U;
  if (request_count > options.vectors) request_count = options.vectors;
  vectors = malloc(options.vectors * options.dimensions * sizeof(*vectors));
  local = calloc(options.segments, sizeof(*local));
  starts = calloc(options.segments, sizeof(*starts));
  counts = calloc(options.segments, sizeof(*counts));
  keys = malloc(request_count * sizeof(*keys));
  fanout_ms = malloc(options.iterations * sizeof(*fanout_ms));
  base_ms = malloc(options.iterations * sizeof(*base_ms));
  if (vectors == NULL || local == NULL || starts == NULL || counts == NULL || keys == NULL ||
      fanout_ms == NULL || base_ms == NULL) goto done;
  generate_vectors(vectors, options.vectors, options.dimensions);
  YAP_V2_ann_index_init(&global);
  if (YAP_V2_ann_index_create(YAP_V2_VECTOR_COSINE, options.dimensions, options.vectors,
                              16U, 128U, 128U, &global) != YAP_ANN_OK) goto done;
  for (segment = 0U; segment < options.segments; segment++) {
    size_t end = (options.vectors * (segment + 1U)) / options.segments;
    starts[segment] = (options.vectors * segment) / options.segments;
    counts[segment] = end - starts[segment];
    YAP_V2_ann_index_init(&local[segment]);
    if (YAP_V2_ann_index_create(YAP_V2_VECTOR_COSINE, options.dimensions,
                                counts[segment], 16U, 128U, 64U,
                                &local[segment]) != YAP_ANN_OK) goto close_indexes;
    for (i = starts[segment]; i < end; i++) {
      if (YAP_V2_ann_index_add(&local[segment], i,
                               vectors + i * options.dimensions) != YAP_ANN_OK ||
          YAP_V2_ann_index_add(&global, i,
                               vectors + i * options.dimensions) != YAP_ANN_OK)
        goto close_indexes;
    }
  }
  for (iteration = 0U; iteration < options.iterations + 5U; iteration++) {
    const float *query = vectors + ((iteration * UINT64_C(104729)) % options.vectors) *
                                  options.dimensions;
    HIT exact[1000], fanout[1000], base[1000];
    size_t exact_count = 0U, fanout_count = 0U, base_count = 0U;
    struct timespec start, end;
    if (options.top_k > 1000U) goto close_indexes;
    for (i = 0U; i < options.vectors; i++)
      add_hit(exact, options.top_k, &exact_count, i,
              dot(query, vectors + i * options.dimensions, options.dimensions));
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) goto close_indexes;
    for (segment = 0U; segment < options.segments; segment++) {
      size_t local_request = request_count < counts[segment] ? request_count : counts[segment];
      size_t found = 0U;
      if (YAP_V2_ann_index_search(&local[segment], query, options.dimensions,
                                  local_request, keys, request_count, &found) != YAP_ANN_OK)
        goto close_indexes;
      for (i = 0U; i < found; i++)
        add_hit(fanout, options.top_k, &fanout_count, keys[i],
                dot(query, vectors + keys[i] * options.dimensions, options.dimensions));
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) goto close_indexes;
    if (iteration >= 5U) fanout_ms[iteration - 5U] = elapsed_ms(start, end);
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0 ||
        YAP_V2_ann_index_search(&global, query, options.dimensions, request_count,
                                keys, request_count, &i) != YAP_ANN_OK)
      goto close_indexes;
    {
      size_t found = i;
      for (i = 0U; i < found; i++)
        add_hit(base, options.top_k, &base_count, keys[i],
                dot(query, vectors + keys[i] * options.dimensions, options.dimensions));
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) goto close_indexes;
    if (iteration >= 5U) {
      base_ms[iteration - 5U] = elapsed_ms(start, end);
      fanout_recall += (double)overlap(exact, exact_count, fanout, fanout_count) /
                       options.top_k;
      base_recall += (double)overlap(exact, exact_count, base, base_count) /
                     options.top_k;
    }
  }
  qsort(fanout_ms, options.iterations, sizeof(*fanout_ms), compare_double);
  qsort(base_ms, options.iterations, sizeof(*base_ms), compare_double);
  printf("mode\tsegments\tvectors\tdimensions\titerations\tann_calls_per_query\t"
         "median_ms\tp95_ms\tqps\trecall_at_%zu\tpeak_rss_bytes\n", options.top_k);
  printf("fanout\t%zu\t%zu\t%zu\t%zu\t%zu\t%.6f\t%.6f\t%.2f\t%.4f\t%llu\n",
         options.segments, options.vectors, options.dimensions, options.iterations,
         options.segments, fanout_ms[options.iterations / 2U],
         fanout_ms[(options.iterations * 95U) / 100U],
         1000.0 / fanout_ms[options.iterations / 2U],
         fanout_recall / options.iterations, (unsigned long long)peak_rss_bytes());
  printf("base\t%zu\t%zu\t%zu\t%zu\t1\t%.6f\t%.6f\t%.2f\t%.4f\t%llu\n",
         options.segments, options.vectors, options.dimensions, options.iterations,
         base_ms[options.iterations / 2U], base_ms[(options.iterations * 95U) / 100U],
         1000.0 / base_ms[options.iterations / 2U],
         base_recall / options.iterations, (unsigned long long)peak_rss_bytes());
  status = EXIT_SUCCESS;
close_indexes:
  for (segment = 0U; segment < options.segments; segment++)
    YAP_V2_ann_index_close(&local[segment]);
  YAP_V2_ann_index_close(&global);
done:
  free(base_ms); free(fanout_ms); free(keys); free(counts); free(starts);
  free(local); free(vectors);
  return status;
}
