#ifndef YAPPO_UPDATE_V2_H
#define YAPPO_UPDATE_V2_H

#include "yappo_ingest.h"

#define YAP_V2_UPDATE_MAX_OPERATIONS 100U
#define YAP_V2_BUILD_BATCH_OPERATIONS 10000U

typedef struct {
  uint64_t generation;
  size_t accepted;
  size_t upserts;
  size_t deletes;
  YAP_V2_SEGMENT_ID_LIST segment_ids;
} YAP_V2_UPDATE_RESULT;

void YAP_V2_update_result_init(YAP_V2_UPDATE_RESULT *result);
void YAP_V2_update_result_free(YAP_V2_UPDATE_RESULT *result);
void YAP_V2_update_set_failpoint_for_testing(const char *name);

int YAP_V2_update_apply(const char *index_dir, const YAP_V2_INGEST_OPERATION *operations,
                        size_t operation_count, YAP_V2_UPDATE_RESULT *result,
                        char *error, size_t error_size);
int YAP_V2_build_apply(const char *index_dir, const YAP_V2_INGEST_OPERATION *operations,
                       size_t operation_count, YAP_V2_UPDATE_RESULT *result,
                       char *error, size_t error_size);
int YAP_V2_update_ndjson(const char *index_dir, const unsigned char *input, size_t input_bytes,
                         YAP_V2_UPDATE_RESULT *result, char *error, size_t error_size);
int YAP_V2_update_json_batch(const char *index_dir, const unsigned char *input,
                             size_t input_bytes, YAP_V2_UPDATE_RESULT *result,
                             char *error, size_t error_size);
int YAP_V2_update_main(int argc, char **argv);

#endif
