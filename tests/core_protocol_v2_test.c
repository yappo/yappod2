#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>
#include "yappo_core_protocol_v2.h"

static void test_all_message_types_roundtrip(void **state) {
  uint16_t type; (void)state;
  for (type = YAP_V2_CORE_SEARCH_REQUEST; type <= YAP_V2_CORE_ERROR_RESPONSE; type++) {
    unsigned char wire[64]; const unsigned char json[] = "{\"ok\":true}";
    YAP_V2_CORE_FRAME source, decoded; size_t wire_bytes, consumed;
    YAP_V2_core_frame_init(&source); YAP_V2_core_frame_init(&decoded);
    source.type = type; source.request_id = 42U + type;
    source.payload = (unsigned char *)json; source.payload_bytes = sizeof(json) - 1U;
    assert_int_equal(YAP_V2_core_frame_encode(&source, 1024U, wire, sizeof(wire), &wire_bytes),
                     YAP_V2_CORE_FRAME_OK);
    assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes, 1024U, &decoded, &consumed),
                     YAP_V2_CORE_FRAME_OK);
    assert_int_equal(consumed, wire_bytes); assert_int_equal(decoded.type, type);
    assert_int_equal(decoded.request_id, source.request_id);
    assert_memory_equal(decoded.payload, json, sizeof(json) - 1U);
    YAP_V2_core_frame_free(&decoded);
  }
}

static void test_network_byte_order_and_output_capacity(void **state) {
  static const unsigned char expected_header[] = {
    'Y','A','P','2', 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08, 0x00,0x00,0x00,0x02
  };
  unsigned char wire[26], payload[] = {0xaa, 0x55};
  YAP_V2_CORE_FRAME frame; size_t wire_bytes = 99U;
  (void)state; YAP_V2_core_frame_init(&frame);
  frame.type = YAP_V2_CORE_SEARCH_REQUEST;
  frame.request_id = UINT64_C(0x0102030405060708);
  frame.payload = payload; frame.payload_bytes = sizeof(payload);
  assert_int_equal(YAP_V2_core_frame_encode(&frame, 1024U, wire, sizeof(wire) - 1U,
                                            &wire_bytes),
                   YAP_V2_CORE_FRAME_NEED_MORE);
  assert_int_equal(wire_bytes, 0U);
  assert_int_equal(YAP_V2_core_frame_encode(&frame, 1024U, wire, sizeof(wire), &wire_bytes),
                   YAP_V2_CORE_FRAME_OK);
  assert_memory_equal(wire, expected_header, sizeof(expected_header));
  assert_memory_equal(wire + sizeof(expected_header), payload, sizeof(payload));
}

static void test_partial_invalid_and_oversize_frames(void **state) {
  unsigned char wire[64]; unsigned char json[] = "{}";
  YAP_V2_CORE_FRAME source, decoded; size_t wire_bytes, consumed;
  (void)state; YAP_V2_core_frame_init(&source); YAP_V2_core_frame_init(&decoded);
  source.type = YAP_V2_CORE_HEALTH_REQUEST; source.request_id = 9U;
  source.payload = json; source.payload_bytes = 2U;
  assert_int_equal(YAP_V2_core_frame_encode(&source, 8U, wire, sizeof(wire), &wire_bytes),
                   YAP_V2_CORE_FRAME_OK);
  assert_int_equal(YAP_V2_core_frame_decode(wire, 23U, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_NEED_MORE);
  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes - 1U, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_NEED_MORE);
  wire[6] = 0U; wire[7] = 99U;
  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_INVALID);
  wire[6] = 0U; wire[7] = YAP_V2_CORE_HEALTH_REQUEST; wire[20] = 0U; wire[21] = 0U;
  wire[22] = 0U; wire[23] = 9U;
  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_TOO_LARGE);

  wire[20] = 0U; wire[21] = 0U; wire[22] = 0U; wire[23] = 2U;
  wire[4] = 0U; wire[5] = 2U;
  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_INVALID);
  wire[4] = 0U; wire[5] = 1U; wire[10] = 1U;
  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes, 8U, &decoded, &consumed),
                   YAP_V2_CORE_FRAME_INVALID);

  assert_int_equal(YAP_V2_core_frame_decode(wire, wire_bytes,
                                            YAP_V2_CORE_MAX_PAYLOAD_BYTES + 1U,
                                            &decoded, &consumed),
                   YAP_V2_CORE_FRAME_INVALID_ARGUMENT);
}

static void test_stream_roundtrip_and_truncation(void **state) {
  FILE *stream = tmpfile(); FILE *truncated = tmpfile(); FILE *truncated_payload = tmpfile();
  FILE *invalid = tmpfile(); unsigned char payload[] = "query";
  YAP_V2_CORE_FRAME source, decoded; unsigned char short_header[8] = {'Y','A','P','2',0,1,0,1};
  unsigned char wire[64]; size_t wire_bytes;
  (void)state; assert_non_null(stream); assert_non_null(truncated);
  assert_non_null(truncated_payload); assert_non_null(invalid);
  YAP_V2_core_frame_init(&source); YAP_V2_core_frame_init(&decoded);
  source.type = YAP_V2_CORE_SEARCH_REQUEST; source.request_id = UINT64_C(0x0102030405060708);
  source.payload = payload; source.payload_bytes = 5U;
  assert_int_equal(YAP_V2_core_frame_write(stream, &source, 1024U), YAP_V2_CORE_FRAME_OK);
  rewind(stream); assert_int_equal(YAP_V2_core_frame_read(stream, 1024U, &decoded),
                                   YAP_V2_CORE_FRAME_OK);
  assert_int_equal(decoded.request_id, source.request_id); assert_memory_equal(decoded.payload, payload, 5U);
  assert_int_equal(fwrite(short_header, 1U, sizeof(short_header), truncated), sizeof(short_header));
  rewind(truncated); assert_int_equal(YAP_V2_core_frame_read(truncated, 1024U, &decoded),
                                      YAP_V2_CORE_FRAME_IO_ERROR);
  assert_int_equal(YAP_V2_core_frame_encode(&source, 1024U, wire, sizeof(wire), &wire_bytes),
                   YAP_V2_CORE_FRAME_OK);
  assert_int_equal(fwrite(wire, 1U, wire_bytes - 1U, truncated_payload), wire_bytes - 1U);
  rewind(truncated_payload);
  assert_int_equal(YAP_V2_core_frame_read(truncated_payload, 1024U, &decoded),
                   YAP_V2_CORE_FRAME_IO_ERROR);
  /* A malformed header is rejected before trusting its advertised length. */
  wire[4] = 0U; wire[5] = 2U;
  wire[20] = 0xffU; wire[21] = 0xffU; wire[22] = 0xffU; wire[23] = 0xffU;
  assert_int_equal(fwrite(wire, 1U, YAP_V2_CORE_FRAME_HEADER_BYTES, invalid),
                   YAP_V2_CORE_FRAME_HEADER_BYTES);
  rewind(invalid);
  assert_int_equal(YAP_V2_core_frame_read(invalid, 1024U, &decoded),
                   YAP_V2_CORE_FRAME_INVALID);
  /* Failed reads leave the previously decoded frame and its allocation intact. */
  assert_int_equal(decoded.type, source.type);
  assert_int_equal(decoded.payload_bytes, source.payload_bytes);
  assert_memory_equal(decoded.payload, payload, sizeof(payload) - 1U);
  YAP_V2_core_frame_free(&decoded); fclose(stream); fclose(truncated);
  fclose(truncated_payload); fclose(invalid);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_all_message_types_roundtrip),
    cmocka_unit_test(test_network_byte_order_and_output_capacity),
    cmocka_unit_test(test_partial_invalid_and_oversize_frames),
    cmocka_unit_test(test_stream_roundtrip_and_truncation)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
