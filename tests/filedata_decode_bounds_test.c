#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "test_db_tmpfiles.h"
#include "yappo_index_filedata.h"
#include "yappo_limits.h"

static void expect_get_failed(const unsigned char *payload, int payload_len) {
  YAPPO_DB_FILES db;
  FILEDATA filedata;
  int rc;

  assert_int_equal(ytest_setup_filedata_db(&db, payload, payload_len), 0);
  memset(&filedata, 0, sizeof(filedata));
  rc = YAP_Index_Filedata_get(&db, 0, &filedata);
  YAP_Index_Filedata_free(&filedata);
  ytest_cleanup_filedata_db(&db);
  assert_int_not_equal(rc, 0);
}

static void test_truncated_payload(void **state) {
  unsigned char payload[1] = {0};

  (void)state;
  expect_get_failed(payload, 1);
}

static void test_invalid_string_length(void **state) {
  unsigned char payload[sizeof(size_t)];
  size_t bad_len = 1024;

  (void)state;

  memcpy(payload, &bad_len, sizeof(bad_len));
  expect_get_failed(payload, (int)sizeof(payload));
}

static void test_negative_other_length(void **state) {
  unsigned char payload[256];
  unsigned char *p = payload;
  size_t zero = 0;
  int size = 10;
  int keyword_num = 3;
  time_t lastmod = 123;
  int domainid = 7;
  int other_len = -1;
  int payload_len;

  (void)state;

  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &zero, sizeof(zero));
  p += sizeof(zero);
  memcpy(p, &size, sizeof(size));
  p += sizeof(size);
  memcpy(p, &keyword_num, sizeof(keyword_num));
  p += sizeof(keyword_num);
  memcpy(p, &lastmod, sizeof(lastmod));
  p += sizeof(lastmod);
  memcpy(p, &domainid, sizeof(domainid));
  p += sizeof(domainid);
  memcpy(p, &other_len, sizeof(other_len));
  p += sizeof(other_len);
  payload_len = (int)(p - payload);

  expect_get_failed(payload, payload_len);
}

static void test_oversized_payload_size(void **state) {
  (void)state;
  expect_get_failed(NULL, YAP_MAX_FILEDATA_RECORD_SIZE + 1);
}

static void test_valid_payload(void **state) {
  YAPPO_DB_FILES db;
  FILEDATA filedata;
  unsigned char payload[256];
  unsigned char *p = payload;
  size_t url_len = 1;
  size_t title_len = 1;
  size_t comment_len = 1;
  int size = 20;
  int keyword_num = 5;
  time_t lastmod = 456;
  int domainid = 8;
  int other_len = 2;
  const unsigned char other[2] = {'x', 'y'};
  int payload_len;
  int rc;

  (void)state;

  memcpy(p, &url_len, sizeof(url_len));
  p += sizeof(url_len);
  *p++ = 'u';
  memcpy(p, &title_len, sizeof(title_len));
  p += sizeof(title_len);
  *p++ = 't';
  memcpy(p, &comment_len, sizeof(comment_len));
  p += sizeof(comment_len);
  *p++ = 'c';
  memcpy(p, &size, sizeof(size));
  p += sizeof(size);
  memcpy(p, &keyword_num, sizeof(keyword_num));
  p += sizeof(keyword_num);
  memcpy(p, &lastmod, sizeof(lastmod));
  p += sizeof(lastmod);
  memcpy(p, &domainid, sizeof(domainid));
  p += sizeof(domainid);
  memcpy(p, &other_len, sizeof(other_len));
  p += sizeof(other_len);
  memcpy(p, other, sizeof(other));
  p += sizeof(other);
  payload_len = (int)(p - payload);

  assert_int_equal(ytest_setup_filedata_db(&db, payload, payload_len), 0);

  memset(&filedata, 0, sizeof(filedata));
  rc = YAP_Index_Filedata_get(&db, 0, &filedata);
  assert_int_equal(rc, 0);
  assert_string_equal(filedata.url, "u");
  assert_string_equal(filedata.title, "t");
  assert_string_equal(filedata.comment, "c");
  assert_int_equal(filedata.size, size);
  assert_int_equal(filedata.keyword_num, keyword_num);
  assert_int_equal(filedata.lastmod, lastmod);
  assert_int_equal(filedata.domainid, domainid);
  assert_int_equal(filedata.other_len, other_len);
  assert_int_equal(filedata.other[0], 'x');
  assert_int_equal(filedata.other[1], 'y');

  YAP_Index_Filedata_free(&filedata);
  ytest_cleanup_filedata_db(&db);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_truncated_payload),
    cmocka_unit_test(test_invalid_string_length),
    cmocka_unit_test(test_negative_other_length),
    cmocka_unit_test(test_oversized_payload_size),
    cmocka_unit_test(test_valid_payload),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
