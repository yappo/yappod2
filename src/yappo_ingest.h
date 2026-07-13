#ifndef YAPPO_INGEST_H
#define YAPPO_INGEST_H

#include "yappo_index_v2.h"

#include <stddef.h>
#include <stdint.h>

typedef enum { YAP_V2_INGEST_UPSERT = 1, YAP_V2_INGEST_DELETE = 2 } YAP_V2_INGEST_KIND;

typedef struct {
  YAP_V2_INGEST_KIND kind;
  char *id;
  char *url;
  char *title;
  char *body;
  char *metadata_json;
  int64_t updated_at_unix_ms;
  float *vectors;
  size_t vector_count;
  size_t vector_dimensions;
} YAP_V2_INGEST_OPERATION;

void YAP_V2_ingest_operation_free(YAP_V2_INGEST_OPERATION *operation);
int YAP_V2_ingest_parse_ndjson(const char *line, size_t length,
                               YAP_V2_INGEST_OPERATION *operation,
                               char *error, size_t error_size);
int YAP_V2_ingest_parse_tsv(char *line, YAP_V2_INGEST_OPERATION *operation,
                            char *error, size_t error_size);
int YAP_V2_prepare_main(int argc, char **argv);

#endif
