#include "search_quality_metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int compare_int_desc(const void *left, const void *right) {
  const int a = *((const int *)left);
  const int b = *((const int *)right);

  if (a < b) {
    return 1;
  }
  if (a > b) {
    return -1;
  }
  return 0;
}

static int compare_double_asc(const void *left, const void *right) {
  const double a = *((const double *)left);
  const double b = *((const double *)right);

  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

static int judgment_for_document(const YAP_QUALITY_JUDGMENT *judgments,
                                 size_t judgment_count, const char *document_id) {
  size_t i;

  for (i = 0; i < judgment_count; i++) {
    if (strcmp(judgments[i].document_id, document_id) == 0) {
      return judgments[i].relevance;
    }
  }
  return 0;
}

static int hit_seen_before(const YAP_QUALITY_HIT *hits, size_t hit_index) {
  size_t i;

  for (i = 0; i < hit_index; i++) {
    if (hits[i].document_id != NULL &&
        strcmp(hits[i].document_id, hits[hit_index].document_id) == 0) {
      return 1;
    }
  }
  return 0;
}

int YAP_Quality_metrics_calculate(const YAP_QUALITY_JUDGMENT *judgments,
                                  size_t judgment_count, const YAP_QUALITY_HIT *hits,
                                  size_t hit_count, size_t cutoff,
                                  YAP_QUALITY_QUERY_METRICS *metrics) {
  double dcg = 0.0;
  double ideal_dcg = 0.0;
  size_t relevant_total = 0U;
  size_t relevant_retrieved = 0U;
  size_t considered_hits;
  int *ideal_relevances;
  size_t i;

  if (judgments == NULL || judgment_count == 0U || metrics == NULL || cutoff == 0U ||
      (hits == NULL && hit_count != 0U)) {
    return -1;
  }

  memset(metrics, 0, sizeof(*metrics));
  ideal_relevances = (int *)calloc(judgment_count, sizeof(int));
  if (ideal_relevances == NULL) {
    return -1;
  }

  for (i = 0; i < judgment_count; i++) {
    if (judgments[i].document_id == NULL || judgments[i].relevance < 0) {
      free(ideal_relevances);
      return -1;
    }
    ideal_relevances[i] = judgments[i].relevance;
    if (judgments[i].relevance > 0) {
      relevant_total++;
    }
  }
  if (relevant_total == 0U) {
    free(ideal_relevances);
    return -1;
  }

  qsort(ideal_relevances, judgment_count, sizeof(int), compare_int_desc);
  for (i = 0; i < judgment_count && i < cutoff; i++) {
    ideal_dcg += (pow(2.0, (double)ideal_relevances[i]) - 1.0) / log2((double)i + 2.0);
  }

  considered_hits = (hit_count < cutoff) ? hit_count : cutoff;
  for (i = 0; i < considered_hits; i++) {
    int relevance;

    if (hits[i].document_id == NULL || hit_seen_before(hits, i)) {
      continue;
    }
    relevance = judgment_for_document(judgments, judgment_count, hits[i].document_id);
    dcg += (pow(2.0, (double)relevance) - 1.0) / log2((double)i + 2.0);
    if (relevance > 0) {
      relevant_retrieved++;
      if (metrics->mrr_at_k == 0.0) {
        metrics->mrr_at_k = 1.0 / ((double)i + 1.0);
      }
    }
  }

  if (ideal_dcg > 0.0) {
    metrics->ndcg_at_k = dcg / ideal_dcg;
  }
  metrics->recall_at_k = (double)relevant_retrieved / (double)relevant_total;

  free(ideal_relevances);
  return 0;
}

double YAP_Quality_percentile_nearest_rank(const double *values, size_t value_count,
                                           double percentile) {
  double *sorted;
  size_t rank;
  double result;

  if (values == NULL || value_count == 0U || percentile <= 0.0 || percentile > 1.0) {
    return -1.0;
  }

  sorted = (double *)malloc(sizeof(double) * value_count);
  if (sorted == NULL) {
    return -1.0;
  }
  memcpy(sorted, values, sizeof(double) * value_count);
  qsort(sorted, value_count, sizeof(double), compare_double_asc);

  rank = (size_t)ceil(percentile * (double)value_count);
  if (rank == 0U) {
    rank = 1U;
  }
  result = sorted[rank - 1U];
  free(sorted);
  return result;
}
