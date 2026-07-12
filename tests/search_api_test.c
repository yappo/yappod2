#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_search_api.h"

static void test_cursor_and_pages(void **state) {
  char cursor[32];
  size_t start;
  size_t end;

  (void)state;
  assert_int_equal(YAP_Search_api_cursor_encode(20U, cursor, sizeof(cursor)), 0);
  assert_string_equal(cursor, "v1.20");
  assert_int_equal(YAP_Search_api_cursor_decode(cursor, &start), 0);
  assert_int_equal(start, 20U);
  assert_int_equal(YAP_Search_api_page(45U, 20U, NULL, &start, &end), 0);
  assert_int_equal(start, 0U);
  assert_int_equal(end, 20U);
  assert_int_equal(YAP_Search_api_page(45U, 20U, "v1.20", &start, &end), 0);
  assert_int_equal(start, 20U);
  assert_int_equal(end, 40U);
  assert_int_equal(YAP_Search_api_page(45U, 20U, "v1.40", &start, &end), 0);
  assert_int_equal(start, 40U);
  assert_int_equal(end, 45U);
  assert_int_not_equal(YAP_Search_api_cursor_decode("v2.1", &start), 0);
  assert_int_not_equal(YAP_Search_api_page(45U, 101U, NULL, &start, &end), 0);
  assert_int_not_equal(YAP_Search_api_page(45U, 20U, "v1.46", &start, &end), 0);
}

static void test_json_writer_escapes_and_cursor(void **state) {
  FILE *stream;
  char output[1024];
  size_t length;
  YAP_SEARCH_API_DOCUMENT documents[1] = {
    {"https://example.test/quote\"", "line\nnext", 12, 1700000000L, 1.25},
  };

  (void)state;
  stream = tmpfile();
  assert_non_null(stream);
  assert_int_equal(YAP_Search_api_write_json(stream, 2U, 0U, 1U, 1U, documents), 0);
  assert_int_equal(fseek(stream, 0L, SEEK_SET), 0);
  length = fread(output, 1U, sizeof(output) - 1U, stream);
  output[length] = '\0';
  assert_non_null(strstr(output, "\"api_version\":2"));
  assert_non_null(strstr(output, "\"next_cursor\":\"v1.1\""));
  assert_non_null(strstr(output, "quote\\\""));
  assert_non_null(strstr(output, "line\\nnext"));
  fclose(stream);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_cursor_and_pages),
    cmocka_unit_test(test_json_writer_escapes_and_cursor),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
