#ifndef YAPPO_OBSERVABILITY_V2_H
#define YAPPO_OBSERVABILITY_V2_H

#include "storage/yappo_compaction_status_v2.h"
#include "config/yappo_config_v2.h"

#include <pthread.h>

#define YAP_V2_LATENCY_BUCKET_COUNT 9U

typedef enum {
  YAP_V2_OBSERVE_SEARCH = 0,
  YAP_V2_OBSERVE_RETRIEVE = 1,
  YAP_V2_OBSERVE_INGEST = 2,
  YAP_V2_OBSERVE_OPERATION_COUNT = 3
} YAP_V2_OBSERVE_OPERATION;

typedef struct {
  int ready;
  uint64_t generation;
  size_t segment_count;
  int embedding_configured;
  char embedding_model_id[YAP_V2_MAX_MODEL_ID_BYTES + 1U];
  uint32_t embedding_dimensions;
  YAP_V2_COMPACTION_STATE compaction_state;
  uint64_t compaction_generation;
  int64_t compaction_updated_at_unix;
} YAP_V2_OPERATIONAL_STATE;

typedef struct {
  pthread_mutex_t lock;
  uint64_t requests[YAP_V2_OBSERVE_OPERATION_COUNT][3];
  uint64_t latency_count[YAP_V2_OBSERVE_OPERATION_COUNT];
  uint64_t latency_microseconds[YAP_V2_OBSERVE_OPERATION_COUNT];
  uint64_t latency_buckets[YAP_V2_OBSERVE_OPERATION_COUNT][YAP_V2_LATENCY_BUCKET_COUNT];
  int initialized;
} YAP_V2_METRICS;

void YAP_V2_operational_state_init(YAP_V2_OPERATIONAL_STATE *state);
int YAP_V2_operational_probe_index(const char *index_dir, YAP_V2_OPERATIONAL_STATE *state,
                                   char *error, size_t error_size);
int YAP_V2_operational_state_json(const YAP_V2_OPERATIONAL_STATE *state, const char *service,
                                  char **json, size_t *json_bytes);
int YAP_V2_metrics_init(YAP_V2_METRICS *metrics);
void YAP_V2_metrics_close(YAP_V2_METRICS *metrics);
void YAP_V2_metrics_record(YAP_V2_METRICS *metrics, YAP_V2_OBSERVE_OPERATION operation,
                           int http_status, uint64_t elapsed_microseconds);
int YAP_V2_metrics_render(YAP_V2_METRICS *metrics, const YAP_V2_OPERATIONAL_STATE *state,
                          size_t inflight, size_t inflight_bytes, size_t max_inflight,
                          size_t max_inflight_bytes, char **output, size_t *output_bytes);
uint64_t YAP_V2_monotonic_microseconds(void);

#endif
