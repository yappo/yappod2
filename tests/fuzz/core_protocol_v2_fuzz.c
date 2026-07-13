#include <stddef.h>
#include <stdint.h>

#include "yappo_core_protocol_v2.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const uint32_t limits[] = {1U, 64U, 4096U, YAP_V2_CORE_MAX_PAYLOAD_BYTES};
  size_t i;
  for (i = 0U; i < sizeof(limits) / sizeof(limits[0]); i++) {
    YAP_V2_CORE_FRAME frame;
    size_t consumed = 0U;
    YAP_V2_core_frame_init(&frame);
    (void)YAP_V2_core_frame_decode(data, size, limits[i], &frame, &consumed);
    YAP_V2_core_frame_free(&frame);
  }
  return 0;
}
