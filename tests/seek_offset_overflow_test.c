#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "yappo_io.h"

static void test_valid_offset(void **state) {
  long offset = -1;

  (void)state;

  assert_int_equal(YAP_seek_offset_index(sizeof(int), 10UL, &offset), 0);
  assert_int_equal(offset, (long)(sizeof(int) * 10UL));

  assert_int_equal(YAP_seek_offset_index(sizeof(double), 3UL, &offset), 0);
  assert_int_equal(offset, (long)(sizeof(double) * 3UL));
}

static void test_overflow_offset(void **state) {
  unsigned long too_large_int = ((unsigned long)LONG_MAX / (unsigned long)sizeof(int)) + 1UL;
  unsigned long too_large_double =
    ((unsigned long)LONG_MAX / (unsigned long)sizeof(double)) + 1UL;
  long offset = 0;

  (void)state;

  assert_int_not_equal(YAP_seek_offset_index(sizeof(int), too_large_int, &offset), 0);
  assert_int_not_equal(YAP_seek_offset_index(sizeof(double), too_large_double, &offset), 0);
}

static void test_null_output(void **state) {
  (void)state;
  assert_int_not_equal(YAP_seek_offset_index(sizeof(int), 1UL, NULL), 0);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_valid_offset),
    cmocka_unit_test(test_overflow_offset),
    cmocka_unit_test(test_null_output),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
