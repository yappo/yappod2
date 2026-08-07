#ifndef YAPPO_EXECUTOR_V2_H
#define YAPPO_EXECUTOR_V2_H

#include <stddef.h>
#include <stdint.h>

typedef void (*YAP_V2_EXECUTOR_FUNCTION)(void *context);
typedef void (*YAP_V2_EXECUTOR_BATCH_FUNCTION)(void *batch_context,
                                                void **items,
                                                size_t item_count);

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
int YAP_V2_executor_open_batch(YAP_V2_EXECUTOR *executor,
                               size_t queue_capacity, size_t max_batch,
                               uint32_t max_delay_microseconds,
                               YAP_V2_EXECUTOR_BATCH_FUNCTION function,
                               void *batch_context);
int YAP_V2_executor_try_submit(YAP_V2_EXECUTOR *executor,
                               YAP_V2_EXECUTOR_FUNCTION function,
                               void *context);
int YAP_V2_executor_try_submit_item(YAP_V2_EXECUTOR *executor, void *item);
int YAP_V2_executor_snapshot(YAP_V2_EXECUTOR *executor,
                             YAP_V2_EXECUTOR_STATE *snapshot);
void YAP_V2_executor_close(YAP_V2_EXECUTOR *executor);

#endif
