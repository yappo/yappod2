#include <stddef.h>
#include <stdint.h>

#include "yappo_core_http_v2.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  YAP_V2_CORE_HTTP_REQUEST request;
  YAP_V2_core_http_request_init(&request);
  (void)YAP_V2_core_http_parse_head(data, size, &request);
  YAP_V2_core_http_request_free(&request);
  return 0;
}
