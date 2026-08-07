#ifndef YAPPO_CORE_REACTOR_V2_H
#define YAPPO_CORE_REACTOR_V2_H

#include <stddef.h>

#include "config/yappo_runtime_policy_v2.h"
#include "indexing/yappo_compact_v2.h"
#include "server/yappo_executor_v2.h"
#include "server/yappo_http_v2.h"

typedef struct {
  void *state;
} YAP_V2_CORE_REACTOR_SERVER;

void YAP_V2_core_reactor_server_init(YAP_V2_CORE_REACTOR_SERVER *server);
int YAP_V2_core_reactor_server_open(
  YAP_V2_CORE_REACTOR_SERVER *server, int listen_socket, const char *index_dir,
  YAP_V2_HTTP_RUNTIME *runtime, YAP_V2_EXECUTOR *search_executor,
  YAP_V2_EXECUTOR *writer_executor, YAP_V2_RUNTIME_LIMITER *search_limiter,
  YAP_V2_RUNTIME_LIMITER *writer_limiter,
  const YAP_V2_RUNTIME_POLICY *runtime_policy,
  const YAP_V2_COMPACTION_POLICY *compaction_policy, size_t reactor_threads);
void YAP_V2_core_reactor_server_stop_accepting(
  YAP_V2_CORE_REACTOR_SERVER *server);
void YAP_V2_core_reactor_server_close(YAP_V2_CORE_REACTOR_SERVER *server);
void YAP_V2_core_reactor_execute_ingest_batch(void *context, void **items,
                                              size_t item_count);

#endif
