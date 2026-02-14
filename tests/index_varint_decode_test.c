#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include <cmocka.h>

#include "yappo_index.h"

static void test_roundtrip(void **state) {
  int input[] = {0, 1, 127, 128, 4096, INT_MAX};
  int input_len = (int)(sizeof(input) / sizeof(input[0]));
  int encoded_len = 0;
  int decoded_len = 0;
  int i;
  unsigned char *encoded;
  int *decoded;

  (void)state;

  encoded = YAP_Index_8bit_encode(input, input_len, &encoded_len);
  assert_non_null(encoded);
  decoded = YAP_Index_8bit_decode(encoded, &decoded_len, encoded_len);

  assert_int_equal(decoded_len, input_len);
  for (i = 0; i < input_len; i++) {
    assert_int_equal(decoded[i], input[i]);
  }

  free(encoded);
  free(decoded);
}

static void test_overflow_payload(void **state) {
  unsigned char malformed[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
  int decoded_len = -1;
  int *decoded;

  (void)state;

  decoded = YAP_Index_8bit_decode(malformed, &decoded_len, (int)sizeof(malformed));
  assert_int_equal(decoded_len, 0);
  free(decoded);
}

static void test_truncated_payload(void **state) {
  unsigned char malformed[] = {0x80};
  int decoded_len = -1;
  int *decoded;

  (void)state;

  decoded = YAP_Index_8bit_decode(malformed, &decoded_len, (int)sizeof(malformed));
  assert_int_equal(decoded_len, 0);
  free(decoded);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_roundtrip),
    cmocka_unit_test(test_overflow_payload),
    cmocka_unit_test(test_truncated_payload),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
