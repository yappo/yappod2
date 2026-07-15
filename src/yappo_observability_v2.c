#include "yappo_observability_v2.h"

#include "yappo_config_v2.h"
#include "yappo_manifest_v2.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <yyjson.h>

#define YAP_V2_COMPACTION_STATUS_FILE "compaction.state"
#define YAP_V2_METRICS_CAPACITY 32768U

static const uint64_t latency_bucket_us[YAP_V2_LATENCY_BUCKET_COUNT] = {
  5000U, 10000U, 25000U, 50000U, 100000U, 200000U, 500000U, 1000000U, UINT64_MAX
};
static const char *const latency_bucket_labels[YAP_V2_LATENCY_BUCKET_COUNT] = {
  "0.005", "0.010", "0.025", "0.050", "0.100", "0.200", "0.500", "1.000", "+Inf"
};
static const char *const operation_names[YAP_V2_OBSERVE_OPERATION_COUNT] = {
  "search", "retrieve", "ingest"
};

static void set_error(char *error, size_t capacity, const char *message) {
  if (error != NULL && capacity > 0U) (void)snprintf(error, capacity, "%s", message);
}

static int join_path(char *output, size_t capacity, const char *left, const char *right) {
  int written = snprintf(output, capacity, "%s/%s", left, right);
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

const char *YAP_V2_compaction_state_name(YAP_V2_COMPACTION_STATE state) {
  switch (state) {
    case YAP_V2_COMPACTION_IDLE: return "idle";
    case YAP_V2_COMPACTION_RUNNING: return "running";
    case YAP_V2_COMPACTION_SUCCEEDED: return "succeeded";
    case YAP_V2_COMPACTION_FAILED: return "failed";
    case YAP_V2_COMPACTION_INTERRUPTED: return "interrupted";
    default: return "unknown";
  }
}

void YAP_V2_operational_state_init(YAP_V2_OPERATIONAL_STATE *state) {
  if (state == NULL) return;
  memset(state, 0, sizeof(*state)); state->compaction_state = YAP_V2_COMPACTION_IDLE;
}

static YAP_V2_COMPACTION_STATE parse_compaction_state(const char *value) {
  if (strcmp(value, "running") == 0) return YAP_V2_COMPACTION_RUNNING;
  if (strcmp(value, "succeeded") == 0) return YAP_V2_COMPACTION_SUCCEEDED;
  if (strcmp(value, "failed") == 0) return YAP_V2_COMPACTION_FAILED;
  return YAP_V2_COMPACTION_UNKNOWN;
}

static void read_compaction_status(const char *index_dir, YAP_V2_OPERATIONAL_STATE *state) {
  char path[4096], line[256], version[32], value[32], trailing;
  long process_id; unsigned long long generation; long long updated; FILE *file;
  if (join_path(path, sizeof(path), index_dir, YAP_V2_COMPACTION_STATUS_FILE) != 0) {
    state->compaction_state = YAP_V2_COMPACTION_UNKNOWN; return;
  }
  file = fopen(path, "rb");
  if (file == NULL) {
    state->compaction_state = errno == ENOENT ? YAP_V2_COMPACTION_IDLE : YAP_V2_COMPACTION_UNKNOWN;
    return;
  }
  if (fgets(line, sizeof(line), file) == NULL) {
    (void)fclose(file); state->compaction_state = YAP_V2_COMPACTION_UNKNOWN; return;
  }
  {
    int extra = fgetc(file), read_error = ferror(file), close_error = fclose(file);
    if (extra != EOF || read_error || close_error != 0) {
      state->compaction_state = YAP_V2_COMPACTION_UNKNOWN; return;
    }
  }
  if (
      sscanf(line, "%31s\t%31s\t%ld\t%llu\t%lld%c", version, value, &process_id,
             &generation, &updated, &trailing) != 6 || strcmp(version, "YAP2-COMPACTION") != 0 ||
      trailing != '\n' || process_id < 0 || updated < 0) {
    state->compaction_state = YAP_V2_COMPACTION_UNKNOWN; return;
  }
  state->compaction_state = parse_compaction_state(value);
  state->compaction_generation = (uint64_t)generation;
  state->compaction_updated_at_unix = (int64_t)updated;
  if (state->compaction_state == YAP_V2_COMPACTION_RUNNING &&
      (process_id == 0 || (kill((pid_t)process_id, 0) != 0 && errno != EPERM)))
    state->compaction_state = YAP_V2_COMPACTION_INTERRUPTED;
}

int YAP_V2_compaction_status_write(const char *index_dir, YAP_V2_COMPACTION_STATE state,
                                   uint64_t generation) {
  char path[4096], temporary[4096]; FILE *file = NULL; int fd = -1, status = YAP_V2_IO_ERROR;
  long process_id; time_t now; int written;
  if (index_dir == NULL || (state != YAP_V2_COMPACTION_RUNNING &&
      state != YAP_V2_COMPACTION_SUCCEEDED && state != YAP_V2_COMPACTION_FAILED))
    return YAP_V2_INVALID_ARGUMENT;
  if (join_path(path, sizeof(path), index_dir, YAP_V2_COMPACTION_STATUS_FILE) != 0)
    return YAP_V2_OUT_OF_RANGE;
  written = snprintf(temporary, sizeof(temporary), "%s/.compaction-state-XXXXXX", index_dir);
  if (written < 0 || (size_t)written >= sizeof(temporary)) return YAP_V2_OUT_OF_RANGE;
  fd = mkstemp(temporary); if (fd < 0) return YAP_V2_IO_ERROR;
  if (fchmod(fd, 0600) != 0 || (file = fdopen(fd, "wb")) == NULL) goto done;
  fd = -1; process_id = state == YAP_V2_COMPACTION_RUNNING ? (long)getpid() : 0L;
  now = time(NULL); if (now < 0 ||
      fprintf(file, "YAP2-COMPACTION\t%s\t%ld\t%llu\t%lld\n",
              YAP_V2_compaction_state_name(state), process_id,
              (unsigned long long)generation, (long long)now) < 0 ||
      fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
    file = NULL; goto done;
  }
  file = NULL;
  if (rename(temporary, path) != 0) goto done;
  {
    int directory = open(index_dir, O_RDONLY);
    if (directory < 0) goto done;
    if (fsync(directory) != 0) { (void)close(directory); goto done; }
    if (close(directory) != 0) goto done;
  }
  status = YAP_V2_OK;
done:
  if (file != NULL) (void)fclose(file); else if (fd >= 0) (void)close(fd);
  if (status != YAP_V2_OK) (void)unlink(temporary);
  return status;
}

int YAP_V2_operational_probe_index(const char *index_dir, YAP_V2_OPERATIONAL_STATE *state,
                                   char *error, size_t error_size) {
  YAP_V2_CONFIG config; YAP_V2_MANIFEST manifest; char config_path[4096], manifest_path[4096];
  char config_error[256] = {0}; int status;
  if (index_dir == NULL || state == NULL) return YAP_V2_INVALID_ARGUMENT;
  YAP_V2_operational_state_init(state); read_compaction_status(index_dir, state);
  YAP_V2_manifest_init(&manifest);
  if (join_path(config_path, sizeof(config_path), index_dir, "config.toml") != 0 ||
      join_path(manifest_path, sizeof(manifest_path), index_dir, "manifest.json") != 0) {
    set_error(error, error_size, "index path is too long"); return YAP_V2_OUT_OF_RANGE;
  }
  status = YAP_V2_config_load(config_path, &config, config_error, sizeof(config_error));
  if (status != YAP_V2_OK) { set_error(error, error_size, config_error); goto done; }
  status = YAP_V2_manifest_load_for_config(manifest_path, &config, &manifest);
  if (status != YAP_V2_OK) { set_error(error, error_size, "index snapshot is invalid"); goto done; }
  state->ready = 1; state->generation = manifest.generation; state->segment_count = manifest.segment_count;
  state->embedding_configured = config.vector_metric != YAP_V2_VECTOR_DISABLED;
  state->embedding_dimensions = config.vector_dimensions;
  memcpy(state->embedding_model_id, config.vector_model_id, strlen(config.vector_model_id) + 1U);
done:
  YAP_V2_manifest_free(&manifest); return status;
}

int YAP_V2_operational_state_json(const YAP_V2_OPERATIONAL_STATE *state, const char *service,
                                  char **json, size_t *json_bytes) {
  yyjson_mut_doc *document; yyjson_mut_val *root, *embedding, *compaction; char *rendered;
  if (state == NULL || service == NULL || json == NULL || json_bytes == NULL) return YAP_V2_INVALID_ARGUMENT;
  *json = NULL; *json_bytes = 0U; document = yyjson_mut_doc_new(NULL);
  if (document == NULL) return YAP_V2_ALLOCATION_FAILED;
  root = yyjson_mut_obj(document); embedding = yyjson_mut_obj(document); compaction = yyjson_mut_obj(document);
  if (root == NULL || embedding == NULL || compaction == NULL ||
      !yyjson_mut_obj_add_str(document, root, "status", state->ready ? "ready" : "not_ready") ||
      !yyjson_mut_obj_add_str(document, root, "service", service) ||
      !yyjson_mut_obj_add_bool(document, root, "ready", state->ready != 0) ||
      !yyjson_mut_obj_add_uint(document, root, "generation", state->generation) ||
      !yyjson_mut_obj_add_uint(document, root, "segments", state->segment_count) ||
      !yyjson_mut_obj_add_str(document, embedding, "state",
        state->embedding_configured ? "precomputed_ready" : "disabled") ||
      !yyjson_mut_obj_add_str(document, embedding, "model_id", state->embedding_model_id) ||
      !yyjson_mut_obj_add_uint(document, embedding, "dimensions", state->embedding_dimensions) ||
      !yyjson_mut_obj_add_val(document, root, "embedding", embedding) ||
      !yyjson_mut_obj_add_str(document, compaction, "state",
        YAP_V2_compaction_state_name(state->compaction_state)) ||
      !yyjson_mut_obj_add_uint(document, compaction, "generation", state->compaction_generation) ||
      !yyjson_mut_obj_add_sint(document, compaction, "updated_at_unix", state->compaction_updated_at_unix) ||
      !yyjson_mut_obj_add_val(document, root, "compaction", compaction)) {
    yyjson_mut_doc_free(document); return YAP_V2_ALLOCATION_FAILED;
  }
  yyjson_mut_doc_set_root(document, root);
  rendered = yyjson_mut_write_opts(document, YYJSON_WRITE_NOFLAG, NULL, json_bytes, NULL);
  yyjson_mut_doc_free(document);
  if (rendered == NULL) return YAP_V2_ALLOCATION_FAILED;
  *json = rendered; return YAP_V2_OK;
}

int YAP_V2_metrics_init(YAP_V2_METRICS *metrics) {
  if (metrics == NULL) return YAP_V2_INVALID_ARGUMENT;
  memset(metrics, 0, sizeof(*metrics));
  if (pthread_mutex_init(&metrics->lock, NULL) != 0) return YAP_V2_IO_ERROR;
  metrics->initialized = 1; return YAP_V2_OK;
}

void YAP_V2_metrics_close(YAP_V2_METRICS *metrics) {
  if (metrics == NULL || !metrics->initialized) return;
  (void)pthread_mutex_destroy(&metrics->lock); memset(metrics, 0, sizeof(*metrics));
}

static uint64_t saturated_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

void YAP_V2_metrics_record(YAP_V2_METRICS *metrics, YAP_V2_OBSERVE_OPERATION operation,
                           int http_status, uint64_t elapsed_microseconds) {
  size_t status_class, i;
  if (metrics == NULL || !metrics->initialized || operation < 0 ||
      operation >= YAP_V2_OBSERVE_OPERATION_COUNT) return;
  status_class = http_status >= 200 && http_status < 300 ? 0U :
                 http_status >= 400 && http_status < 500 ? 1U : 2U;
  pthread_mutex_lock(&metrics->lock);
  metrics->requests[operation][status_class] = saturated_add(metrics->requests[operation][status_class], 1U);
  metrics->latency_count[operation] = saturated_add(metrics->latency_count[operation], 1U);
  metrics->latency_microseconds[operation] = saturated_add(metrics->latency_microseconds[operation], elapsed_microseconds);
  for (i = 0U; i < YAP_V2_LATENCY_BUCKET_COUNT; i++)
    if (elapsed_microseconds <= latency_bucket_us[i])
      metrics->latency_buckets[operation][i] = saturated_add(metrics->latency_buckets[operation][i], 1U);
  pthread_mutex_unlock(&metrics->lock);
}

static int append(char *output, size_t capacity, size_t *used, const char *format, ...) {
  va_list args; int written;
  if (*used >= capacity) return -1;
  va_start(args, format); written = vsnprintf(output + *used, capacity - *used, format, args); va_end(args);
  if (written < 0 || (size_t)written >= capacity - *used) return -1;
  *used += (size_t)written; return 0;
}

int YAP_V2_metrics_render(YAP_V2_METRICS *metrics, const YAP_V2_OPERATIONAL_STATE *state,
                          size_t inflight, size_t inflight_bytes, size_t max_inflight,
                          size_t max_inflight_bytes, char **output, size_t *output_bytes) {
  uint64_t requests[YAP_V2_OBSERVE_OPERATION_COUNT][3];
  uint64_t counts[YAP_V2_OBSERVE_OPERATION_COUNT], sums[YAP_V2_OBSERVE_OPERATION_COUNT];
  uint64_t buckets[YAP_V2_OBSERVE_OPERATION_COUNT][YAP_V2_LATENCY_BUCKET_COUNT];
  char *rendered; size_t used = 0U, operation, bucket;
  if (metrics == NULL || !metrics->initialized || state == NULL || output == NULL || output_bytes == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  *output = NULL; *output_bytes = 0U;
  pthread_mutex_lock(&metrics->lock);
  memcpy(requests, metrics->requests, sizeof(requests)); memcpy(counts, metrics->latency_count, sizeof(counts));
  memcpy(sums, metrics->latency_microseconds, sizeof(sums)); memcpy(buckets, metrics->latency_buckets, sizeof(buckets));
  pthread_mutex_unlock(&metrics->lock);
  rendered = malloc(YAP_V2_METRICS_CAPACITY); if (rendered == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,"# HELP yappod_v2_requests_total Completed v2 HTTP requests.\n# TYPE yappod_v2_requests_total counter\n") != 0) goto range;
  for (operation = 0U; operation < YAP_V2_OBSERVE_OPERATION_COUNT; operation++) {
    static const char *const classes[] = {"2xx","4xx","5xx"}; size_t status_class;
    for (status_class = 0U; status_class < 3U; status_class++)
      if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,
          "yappod_v2_requests_total{operation=\"%s\",status_class=\"%s\"} %llu\n",
          operation_names[operation], classes[status_class],
          (unsigned long long)requests[operation][status_class]) != 0) goto range;
  }
  if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,"# HELP yappod_v2_request_duration_seconds End-to-end v2 HTTP request latency.\n# TYPE yappod_v2_request_duration_seconds histogram\n") != 0) goto range;
  for (operation = 0U; operation < YAP_V2_OBSERVE_OPERATION_COUNT; operation++) {
    for (bucket = 0U; bucket < YAP_V2_LATENCY_BUCKET_COUNT; bucket++)
      if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,
          "yappod_v2_request_duration_seconds_bucket{operation=\"%s\",le=\"%s\"} %llu\n",
          operation_names[operation], latency_bucket_labels[bucket],
          (unsigned long long)buckets[operation][bucket]) != 0) goto range;
    if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,
        "yappod_v2_request_duration_seconds_sum{operation=\"%s\"} %.6f\n"
        "yappod_v2_request_duration_seconds_count{operation=\"%s\"} %llu\n",
        operation_names[operation], (double)sums[operation] / 1000000.0,
        operation_names[operation], (unsigned long long)counts[operation]) != 0) goto range;
  }
  if (append(rendered,YAP_V2_METRICS_CAPACITY,&used,
      "# TYPE yappod_v2_ready gauge\nyappod_v2_ready %d\n"
      "# TYPE yappod_v2_manifest_generation gauge\nyappod_v2_manifest_generation %llu\n"
      "# TYPE yappod_v2_inflight_requests gauge\nyappod_v2_inflight_requests %zu\n"
      "# TYPE yappod_v2_inflight_request_bytes gauge\nyappod_v2_inflight_request_bytes %zu\n"
      "# TYPE yappod_v2_inflight_request_limit gauge\nyappod_v2_inflight_request_limit %zu\n"
      "# TYPE yappod_v2_inflight_byte_limit gauge\nyappod_v2_inflight_byte_limit %zu\n"
      "# TYPE yappod_v2_embedding_configured gauge\nyappod_v2_embedding_configured %d\n"
      "# TYPE yappod_v2_compaction_state gauge\nyappod_v2_compaction_state{state=\"%s\"} 1\n"
      "# TYPE yappod_v2_compaction_generation gauge\nyappod_v2_compaction_generation %llu\n",
      state->ready != 0, (unsigned long long)state->generation, inflight, inflight_bytes,
      max_inflight, max_inflight_bytes, state->embedding_configured != 0,
      YAP_V2_compaction_state_name(state->compaction_state),
      (unsigned long long)state->compaction_generation) != 0) goto range;
  *output = rendered; *output_bytes = used; return YAP_V2_OK;
range:
  free(rendered); return YAP_V2_OUT_OF_RANGE;
}

uint64_t YAP_V2_monotonic_microseconds(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0U;
  return (uint64_t)value.tv_sec * UINT64_C(1000000) + (uint64_t)value.tv_nsec / 1000U;
}
