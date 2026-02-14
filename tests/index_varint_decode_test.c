#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "yappo_index.h"

static int fail(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  return 1;
}

static int test_roundtrip(void) {
  int input[] = {0, 1, 127, 128, 4096, INT_MAX};
  int input_len = (int)(sizeof(input) / sizeof(input[0]));
  int encoded_len = 0;
  int decoded_len = 0;
  int i;
  unsigned char *encoded = YAP_Index_8bit_encode(input, input_len, &encoded_len);
  int *decoded = YAP_Index_8bit_decode(encoded, &decoded_len, encoded_len);

  if (decoded_len != input_len) {
    free(encoded);
    free(decoded);
    return fail("roundtrip length mismatch");
  }
  for (i = 0; i < input_len; i++) {
    if (decoded[i] != input[i]) {
      free(encoded);
      free(decoded);
      return fail("roundtrip value mismatch");
    }
  }

  free(encoded);
  free(decoded);
  return 0;
}

static int test_overflow_payload(void) {
  unsigned char malformed[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
  int decoded_len = -1;
  int *decoded = YAP_Index_8bit_decode(malformed, &decoded_len, (int)sizeof(malformed));

  if (decoded_len != 0) {
    free(decoded);
    return fail("overflow payload should be rejected");
  }
  free(decoded);
  return 0;
}

static int test_truncated_payload(void) {
  unsigned char malformed[] = {0x80};
  int decoded_len = -1;
  int *decoded = YAP_Index_8bit_decode(malformed, &decoded_len, (int)sizeof(malformed));

  if (decoded_len != 0) {
    free(decoded);
    return fail("truncated payload should be rejected");
  }
  free(decoded);
  return 0;
}

int main(void) {
  if (test_roundtrip() != 0) {
    return 1;
  }
  if (test_overflow_payload() != 0) {
    return 1;
  }
  if (test_truncated_payload() != 0) {
    return 1;
  }
  return 0;
}
