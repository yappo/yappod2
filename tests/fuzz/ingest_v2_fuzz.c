#include <stddef.h>
#include <stdint.h>

#include "yappo_ingest.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  YAP_V2_INGEST_OPERATION operation;
  char error[64];
  if (size > YAP_V2_MAX_METADATA_BYTES * 4U) return 0;
  if (YAP_V2_ingest_parse_ndjson((const char *)data, size, &operation, error,
                                 sizeof(error)) == YAP_V2_OK)
    YAP_V2_ingest_operation_free(&operation);
  return 0;
}
