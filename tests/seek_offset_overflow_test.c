#include <limits.h>
#include <stdio.h>

#include "yappo_io.h"

static int fail(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  return 1;
}

static int test_valid_offset(void) {
  long offset = -1;

  if (YAP_seek_offset_index(sizeof(int), 10UL, &offset) != 0) {
    return fail("valid int offset should succeed");
  }
  if (offset != (long)(sizeof(int) * 10UL)) {
    return fail("valid int offset mismatch");
  }

  if (YAP_seek_offset_index(sizeof(double), 3UL, &offset) != 0) {
    return fail("valid double offset should succeed");
  }
  if (offset != (long)(sizeof(double) * 3UL)) {
    return fail("valid double offset mismatch");
  }

  return 0;
}

static int test_overflow_offset(void) {
  unsigned long too_large_int = ((unsigned long)LONG_MAX / (unsigned long)sizeof(int)) + 1UL;
  unsigned long too_large_double =
    ((unsigned long)LONG_MAX / (unsigned long)sizeof(double)) + 1UL;
  long offset = 0;

  if (YAP_seek_offset_index(sizeof(int), too_large_int, &offset) == 0) {
    return fail("int offset overflow should fail");
  }
  if (YAP_seek_offset_index(sizeof(double), too_large_double, &offset) == 0) {
    return fail("double offset overflow should fail");
  }

  return 0;
}

static int test_null_output(void) {
  if (YAP_seek_offset_index(sizeof(int), 1UL, NULL) == 0) {
    return fail("null output pointer should fail");
  }
  return 0;
}

int main(void) {
  if (test_valid_offset() != 0) {
    return 1;
  }
  if (test_overflow_offset() != 0) {
    return 1;
  }
  if (test_null_output() != 0) {
    return 1;
  }
  return 0;
}
