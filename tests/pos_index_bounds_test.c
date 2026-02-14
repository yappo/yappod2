#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_db_tmpfiles.h"
#include "yappo_index_pos.h"
#include "yappo_limits.h"

static void expect_get_failed(int pos_size, int pos_index, const unsigned char *payload,
                              int payload_len) {
  YAPPO_DB_FILES db;
  unsigned char *postings = NULL;
  int postings_len = 0;
  int rc;

  assert_int_equal(ytest_setup_pos_db(&db, pos_size, pos_index, payload, payload_len), 0);

  rc = YAP_Index_Pos_get(&db, 1, &postings, &postings_len);
  free(postings);
  ytest_cleanup_pos_db(&db);
  assert_int_not_equal(rc, 0);
}

static void test_negative_size(void **state) {
  unsigned char payload[1] = {0x01};

  (void)state;
  expect_get_failed(-1, 0, payload, (int)sizeof(payload));
}

static void test_truncated_postings_data(void **state) {
  unsigned char payload[1] = {0x01};

  (void)state;
  expect_get_failed(4, 0, payload, (int)sizeof(payload));
}

static void test_negative_index(void **state) {
  unsigned char payload[4] = {0x01, 0x02, 0x03, 0x04};

  (void)state;
  expect_get_failed(4, -1, payload, (int)sizeof(payload));
}

static void test_oversized_postings_size(void **state) {
  unsigned char payload[1] = {0x00};

  (void)state;
  expect_get_failed(YAP_MAX_POSTINGS_BLOB_SIZE + 1, 0, payload, 0);
}

static void test_valid_postings(void **state) {
  YAPPO_DB_FILES db;
  unsigned char payload[4] = {0x10, 0x20, 0x30, 0x40};
  unsigned char *postings = NULL;
  int postings_len = 0;
  int rc;

  (void)state;

  assert_int_equal(ytest_setup_pos_db(&db, (int)sizeof(payload), 0, payload, (int)sizeof(payload)),
                   0);

  rc = YAP_Index_Pos_get(&db, 1, &postings, &postings_len);
  assert_int_equal(rc, 0);
  assert_int_equal(postings_len, (int)sizeof(payload));
  assert_memory_equal(postings, payload, (size_t)postings_len);

  free(postings);
  ytest_cleanup_pos_db(&db);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_negative_size),
    cmocka_unit_test(test_truncated_postings_data),
    cmocka_unit_test(test_negative_index),
    cmocka_unit_test(test_oversized_postings_size),
    cmocka_unit_test(test_valid_postings),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
