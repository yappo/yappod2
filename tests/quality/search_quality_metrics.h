#ifndef YAPPO_SEARCH_QUALITY_METRICS_H
#define YAPPO_SEARCH_QUALITY_METRICS_H

#include <stddef.h>

typedef struct {
  const char *document_id;
  int relevance;
} YAP_QUALITY_JUDGMENT;

typedef struct {
  const char *document_id;
} YAP_QUALITY_HIT;

typedef struct {
  double ndcg_at_k;
  double mrr_at_k;
  double recall_at_k;
} YAP_QUALITY_QUERY_METRICS;

int YAP_Quality_metrics_calculate(const YAP_QUALITY_JUDGMENT *judgments,
                                  size_t judgment_count, const YAP_QUALITY_HIT *hits,
                                  size_t hit_count, size_t cutoff,
                                  YAP_QUALITY_QUERY_METRICS *metrics);

double YAP_Quality_percentile_nearest_rank(const double *values, size_t value_count,
                                           double percentile);

#endif
