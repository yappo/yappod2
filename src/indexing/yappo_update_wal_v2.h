#ifndef YAPPO_UPDATE_WAL_V2_H
#define YAPPO_UPDATE_WAL_V2_H

#include "indexing/yappo_ingest.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t base_generation;
  uint64_t target_generation;
  YAP_V2_INGEST_OPERATION *operations;
  size_t operation_count;
} YAP_V2_UPDATE_WAL;

void YAP_V2_update_wal_init(YAP_V2_UPDATE_WAL *wal);
void YAP_V2_update_wal_free(YAP_V2_UPDATE_WAL *wal);
int YAP_V2_update_wal_exists(const char *index_dir);
int YAP_V2_update_wal_write(
  const char *index_dir, uint64_t base_generation,
  const YAP_V2_INGEST_OPERATION *operations, size_t operation_count);
int YAP_V2_update_wal_load(const char *index_dir, YAP_V2_UPDATE_WAL *wal);
int YAP_V2_update_wal_clear(const char *index_dir);

#endif
