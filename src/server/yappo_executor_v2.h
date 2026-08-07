#ifndef YAPPO_EXECUTOR_V2_H
#define YAPPO_EXECUTOR_V2_H

#include <stddef.h>

typedef void (*YAP_V2_EXECUTOR_FUNCTION)(void *context);

#define YAP_V2_EXECUTOR_FULL 1

typedef struct {
  void *state;
} YAP_V2_EXECUTOR;

typedef struct {
  size_t worker_threads;
  size_t queue_capacity;
  size_t queued;
  size_t active;
  size_t submitted;
  size_t completed;
  size_t rejected;
  int accepting;
} YAP_V2_EXECUTOR_STATE;

void YAP_V2_executor_init(YAP_V2_EXECUTOR *executor);
int YAP_V2_executor_open(YAP_V2_EXECUTOR *executor, size_t worker_threads,
                         size_t queue_capacity);
int YAP_V2_executor_try_submit(YAP_V2_EXECUTOR *executor,
                               YAP_V2_EXECUTOR_FUNCTION function,
                               void *context);
int YAP_V2_executor_snapshot(YAP_V2_EXECUTOR *executor,
                             YAP_V2_EXECUTOR_STATE *snapshot);
void YAP_V2_executor_close(YAP_V2_EXECUTOR *executor);

#endif
