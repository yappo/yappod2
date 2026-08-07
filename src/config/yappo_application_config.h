#ifndef YAPPO_APPLICATION_CONFIG_H
#define YAPPO_APPLICATION_CONFIG_H

#include "config/yappo_config_v2.h"
#include "config/yappo_compaction_policy_v2.h"
#include "config/yappo_runtime_policy_v2.h"

#include <stddef.h>
#include <stdint.h>

#define YAP_APPLICATION_PATH_BYTES 4096U
#define YAP_APPLICATION_HOST_BYTES 256U
#define YAP_APPLICATION_DEFAULT_IO_THREADS 16U
#define YAP_APPLICATION_DEFAULT_SEARCH_THREADS 16U
#define YAP_APPLICATION_MAX_EXECUTION_THREADS 1024U

typedef struct {
  YAP_V2_CONFIG index_config;
  char index_directory[YAP_APPLICATION_PATH_BYTES];
  char run_directory[YAP_APPLICATION_PATH_BYTES];
  char core_host[YAP_APPLICATION_HOST_BYTES];
  uint16_t core_port;
  char front_host[YAP_APPLICATION_HOST_BYTES];
  uint16_t front_port;
  YAP_V2_RUNTIME_POLICY runtime_policy;
  size_t front_io_threads;
  size_t core_io_threads;
  size_t core_search_threads;
  size_t core_writer_queue_capacity;
  YAP_V2_COMPACTION_POLICY compaction_policy;
} YAP_APPLICATION_CONFIG;

void YAP_application_config_init(YAP_APPLICATION_CONFIG *config);
int YAP_application_config_load(const char *path, YAP_APPLICATION_CONFIG *config,
                                char *error, size_t error_size);

#endif
