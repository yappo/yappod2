#include "server/yappo_executor_v2.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/yappo_types_v2.h"

typedef struct {
  YAP_V2_EXECUTOR_FUNCTION function;
  void *context;
} EXECUTOR_JOB;

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t available;
  pthread_t *threads;
  EXECUTOR_JOB *jobs;
  size_t worker_threads;
  size_t started_threads;
  size_t queue_capacity;
  size_t head;
  size_t queued;
  size_t active;
  size_t submitted;
  size_t completed;
  size_t rejected;
  int accepting;
  int initialized;
} EXECUTOR_STATE;

static void *run_worker(void *opaque) {
  EXECUTOR_STATE *state = opaque;
  for (;;) {
    EXECUTOR_JOB job;
    pthread_mutex_lock(&state->lock);
    while (state->queued == 0U && state->accepting)
      pthread_cond_wait(&state->available, &state->lock);
    if (state->queued == 0U && !state->accepting) {
      pthread_mutex_unlock(&state->lock);
      break;
    }
    job = state->jobs[state->head];
    state->head = (state->head + 1U) % state->queue_capacity;
    state->queued--;
    state->active++;
    pthread_mutex_unlock(&state->lock);

    job.function(job.context);

    pthread_mutex_lock(&state->lock);
    state->active--;
    state->completed++;
    pthread_mutex_unlock(&state->lock);
  }
  return NULL;
}

void YAP_V2_executor_init(YAP_V2_EXECUTOR *executor) {
  if (executor != NULL) executor->state = NULL;
}

int YAP_V2_executor_open(YAP_V2_EXECUTOR *executor, size_t worker_threads,
                         size_t queue_capacity) {
  EXECUTOR_STATE *state;
  size_t i;
  if (executor == NULL || executor->state != NULL || worker_threads == 0U ||
      queue_capacity == 0U ||
      worker_threads > SIZE_MAX / sizeof(pthread_t) ||
      queue_capacity > SIZE_MAX / sizeof(EXECUTOR_JOB))
    return YAP_V2_INVALID_ARGUMENT;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return YAP_V2_ALLOCATION_FAILED;
  state->threads = calloc(worker_threads, sizeof(*state->threads));
  state->jobs = calloc(queue_capacity, sizeof(*state->jobs));
  if (state->threads == NULL || state->jobs == NULL) {
    free(state->threads);
    free(state->jobs);
    free(state);
    return YAP_V2_ALLOCATION_FAILED;
  }
  if (pthread_mutex_init(&state->lock, NULL) != 0) {
    free(state->threads);
    free(state->jobs);
    free(state);
    return YAP_V2_IO_ERROR;
  }
  if (pthread_cond_init(&state->available, NULL) != 0) {
    pthread_mutex_destroy(&state->lock);
    free(state->threads);
    free(state->jobs);
    free(state);
    return YAP_V2_IO_ERROR;
  }
  state->worker_threads = worker_threads;
  state->queue_capacity = queue_capacity;
  state->accepting = 1;
  state->initialized = 1;
  for (i = 0U; i < worker_threads; i++) {
    if (pthread_create(&state->threads[i], NULL, run_worker, state) != 0) break;
    state->started_threads++;
  }
  if (state->started_threads != worker_threads) {
    pthread_mutex_lock(&state->lock);
    state->accepting = 0;
    pthread_cond_broadcast(&state->available);
    pthread_mutex_unlock(&state->lock);
    for (i = 0U; i < state->started_threads; i++)
      (void)pthread_join(state->threads[i], NULL);
    pthread_cond_destroy(&state->available);
    pthread_mutex_destroy(&state->lock);
    free(state->threads);
    free(state->jobs);
    free(state);
    return YAP_V2_IO_ERROR;
  }
  executor->state = state;
  return YAP_V2_OK;
}

int YAP_V2_executor_try_submit(YAP_V2_EXECUTOR *executor,
                               YAP_V2_EXECUTOR_FUNCTION function,
                               void *context) {
  EXECUTOR_STATE *state;
  size_t tail;
  if (executor == NULL || executor->state == NULL || function == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  state = executor->state;
  pthread_mutex_lock(&state->lock);
  if (!state->accepting || state->queued == state->queue_capacity) {
    state->rejected++;
    pthread_mutex_unlock(&state->lock);
    return YAP_V2_EXECUTOR_FULL;
  }
  tail = (state->head + state->queued) % state->queue_capacity;
  state->jobs[tail].function = function;
  state->jobs[tail].context = context;
  state->queued++;
  state->submitted++;
  pthread_cond_signal(&state->available);
  pthread_mutex_unlock(&state->lock);
  return YAP_V2_OK;
}

int YAP_V2_executor_snapshot(YAP_V2_EXECUTOR *executor,
                             YAP_V2_EXECUTOR_STATE *snapshot) {
  EXECUTOR_STATE *state;
  if (executor == NULL || executor->state == NULL || snapshot == NULL)
    return YAP_V2_INVALID_ARGUMENT;
  state = executor->state;
  pthread_mutex_lock(&state->lock);
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->worker_threads = state->worker_threads;
  snapshot->queue_capacity = state->queue_capacity;
  snapshot->queued = state->queued;
  snapshot->active = state->active;
  snapshot->submitted = state->submitted;
  snapshot->completed = state->completed;
  snapshot->rejected = state->rejected;
  snapshot->accepting = state->accepting;
  pthread_mutex_unlock(&state->lock);
  return YAP_V2_OK;
}

void YAP_V2_executor_close(YAP_V2_EXECUTOR *executor) {
  EXECUTOR_STATE *state;
  size_t i;
  if (executor == NULL || executor->state == NULL) return;
  state = executor->state;
  pthread_mutex_lock(&state->lock);
  state->accepting = 0;
  pthread_cond_broadcast(&state->available);
  pthread_mutex_unlock(&state->lock);
  for (i = 0U; i < state->started_threads; i++)
    (void)pthread_join(state->threads[i], NULL);
  pthread_cond_destroy(&state->available);
  pthread_mutex_destroy(&state->lock);
  free(state->threads);
  free(state->jobs);
  free(state);
  executor->state = NULL;
}
