#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <pthread.h>

#include "common/yappo_types_v2.h"
#include "server/yappo_executor_v2.h"

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t release;
  size_t entered;
  size_t finished;
  int blocked;
} JOB_STATE;

typedef struct {
  pthread_mutex_t lock;
  size_t calls;
  size_t items;
  size_t sum;
} BATCH_STATE;

static void run_job(void *opaque) {
  JOB_STATE *state = opaque;
  pthread_mutex_lock(&state->lock);
  state->entered++;
  while (state->blocked) pthread_cond_wait(&state->release, &state->lock);
  state->finished++;
  pthread_mutex_unlock(&state->lock);
}

static void run_batch(void *opaque, void **items, size_t item_count) {
  BATCH_STATE *state = opaque;
  size_t i;
  pthread_mutex_lock(&state->lock);
  state->calls++;
  state->items += item_count;
  for (i = 0U; i < item_count; i++)
    state->sum += (size_t)(uintptr_t)items[i];
  pthread_mutex_unlock(&state->lock);
}

static void test_bounded_executor_drains_and_rejects_overflow(void **unused) {
  YAP_V2_EXECUTOR executor;
  YAP_V2_EXECUTOR_STATE snapshot;
  JOB_STATE state;
  (void)unused;
  YAP_V2_executor_init(&executor);
  state.entered = 0U;
  state.finished = 0U;
  state.blocked = 1;
  assert_int_equal(pthread_mutex_init(&state.lock, NULL), 0);
  assert_int_equal(pthread_cond_init(&state.release, NULL), 0);
  assert_int_equal(YAP_V2_executor_open(&executor, 1U, 1U), YAP_V2_OK);
  assert_int_equal(YAP_V2_executor_try_submit(&executor, run_job, &state), YAP_V2_OK);
  for (;;) {
    pthread_mutex_lock(&state.lock);
    if (state.entered == 1U) {
      pthread_mutex_unlock(&state.lock);
      break;
    }
    pthread_mutex_unlock(&state.lock);
  }
  assert_int_equal(YAP_V2_executor_try_submit(&executor, run_job, &state), YAP_V2_OK);
  assert_int_equal(YAP_V2_executor_try_submit(&executor, run_job, &state),
                   YAP_V2_EXECUTOR_FULL);
  assert_int_equal(YAP_V2_executor_snapshot(&executor, &snapshot), YAP_V2_OK);
  assert_int_equal(snapshot.active, 1U);
  assert_int_equal(snapshot.queued, 1U);
  assert_int_equal(snapshot.rejected, 1U);
  pthread_mutex_lock(&state.lock);
  state.blocked = 0;
  pthread_cond_broadcast(&state.release);
  pthread_mutex_unlock(&state.lock);
  YAP_V2_executor_close(&executor);
  assert_int_equal(state.finished, 2U);
  assert_int_equal(pthread_cond_destroy(&state.release), 0);
  assert_int_equal(pthread_mutex_destroy(&state.lock), 0);
}

static void test_batch_executor_groups_and_drains(void **unused) {
  YAP_V2_EXECUTOR executor;
  BATCH_STATE state;
  (void)unused;
  memset(&state, 0, sizeof(state));
  assert_int_equal(pthread_mutex_init(&state.lock, NULL), 0);
  YAP_V2_executor_init(&executor);
  assert_int_equal(YAP_V2_executor_open_batch(
    &executor, 3U, 3U, 50000U, run_batch, &state), YAP_V2_OK);
  assert_int_equal(YAP_V2_executor_try_submit_item(
    &executor, (void *)(uintptr_t)1U), YAP_V2_OK);
  assert_int_equal(YAP_V2_executor_try_submit_item(
    &executor, (void *)(uintptr_t)2U), YAP_V2_OK);
  assert_int_equal(YAP_V2_executor_try_submit_item(
    &executor, (void *)(uintptr_t)3U), YAP_V2_OK);
  YAP_V2_executor_close(&executor);
  assert_int_equal(state.calls, 1U);
  assert_int_equal(state.items, 3U);
  assert_int_equal(state.sum, 6U);
  assert_int_equal(pthread_mutex_destroy(&state.lock), 0);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_bounded_executor_drains_and_rejects_overflow),
    cmocka_unit_test(test_batch_executor_groups_and_drains),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
