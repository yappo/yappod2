#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_core_http_v2.h"

static void test_parse_query_head(void **state) {
  static const char head[] =
    "QUERY /v2/search HTTP/1.1\r\n"
    "Host: core.test:18401\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Content-Length: 2\r\n"
    "Authorization: Bearer test-token\r\n"
    "Connection: close\r\n\r\n";
  YAP_V2_CORE_HTTP_REQUEST request;
  (void)state;
  YAP_V2_core_http_request_init(&request);
  assert_int_equal(YAP_V2_core_http_parse_head(
    (const unsigned char *)head, sizeof(head) - 1U, &request), YAP_V2_CORE_HTTP_OK);
  assert_string_equal(request.method, "QUERY");
  assert_string_equal(request.target, "/v2/search");
  assert_true(request.have_content_length);
  assert_int_equal(request.content_length, 2U);
  assert_true(request.json_content_type);
  assert_string_equal(request.authorization, "Bearer test-token");
  YAP_V2_core_http_request_free(&request);
}

static void test_parse_rejects_invalid_heads(void **state) {
  static const char missing_host[] =
    "QUERY /v2/search HTTP/1.1\r\nContent-Length: 2\r\n\r\n";
  static const char old_version[] =
    "QUERY /v2/search HTTP/1.0\r\nHost: localhost\r\n\r\n";
  static const char transfer_encoding[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\n"
    "Transfer-Encoding: chunked\r\n\r\n";
  static const char duplicate_length[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Length: 2\r\nContent-Length: 2\r\n\r\n";
  static const char signed_length[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\nContent-Length: +2\r\n\r\n";
  static const char invalid_header_name[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\nBad Header: value\r\n\r\n";
  const char *cases[] = {missing_host, old_version, transfer_encoding, duplicate_length,
                         signed_length, invalid_header_name};
  size_t i;
  YAP_V2_CORE_HTTP_REQUEST request;
  (void)state;
  YAP_V2_core_http_request_init(&request);
  for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
    assert_int_equal(YAP_V2_core_http_parse_head(
      (const unsigned char *)cases[i], strlen(cases[i]), &request),
      YAP_V2_CORE_HTTP_INVALID);
  }
  YAP_V2_core_http_request_free(&request);
}

static void test_parse_preserves_unsupported_media_type(void **state) {
  static const char head[] =
    "QUERY /v2/search HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Type: text/plain\r\nContent-Length: 2\r\n\r\n";
  YAP_V2_CORE_HTTP_REQUEST request;
  (void)state;
  YAP_V2_core_http_request_init(&request);
  assert_int_equal(YAP_V2_core_http_parse_head(
    (const unsigned char *)head, sizeof(head) - 1U, &request), YAP_V2_CORE_HTTP_OK);
  assert_false(request.json_content_type);
  YAP_V2_core_http_request_free(&request);
}

static void test_stream_read_and_body_limit(void **state) {
  static const char request_text[] =
    "POST /v2/documents:batch HTTP/1.1\r\n"
    "Host: localhost\r\nContent-Type: application/json\r\n"
    "Content-Length: 2\r\n\r\n{}";
  FILE *stream = tmpfile();
  YAP_V2_CORE_HTTP_REQUEST request;
  (void)state;
  assert_non_null(stream);
  assert_int_equal(fwrite(request_text, 1U, sizeof(request_text) - 1U, stream),
                   sizeof(request_text) - 1U);
  rewind(stream);
  YAP_V2_core_http_request_init(&request);
  assert_int_equal(YAP_V2_core_http_read_request(stream, 2U, &request),
                   YAP_V2_CORE_HTTP_OK);
  assert_int_equal(request.body_bytes, 2U);
  assert_memory_equal(request.body, "{}", 2U);
  YAP_V2_core_http_request_free(&request);
  rewind(stream);
  assert_int_equal(YAP_V2_core_http_read_request(stream, 1U, &request),
                   YAP_V2_CORE_HTTP_TOO_LARGE);
  YAP_V2_core_http_request_free(&request);
  fclose(stream);
}

static void test_stream_rejects_truncated_body(void **state) {
  static const char request_text[] =
    "QUERY /v2/search HTTP/1.1\r\n"
    "Host: localhost\r\nContent-Type: application/json\r\n"
    "Content-Length: 3\r\n\r\n{}";
  FILE *stream = tmpfile();
  YAP_V2_CORE_HTTP_REQUEST request;
  (void)state;
  assert_non_null(stream);
  assert_int_equal(fwrite(request_text, 1U, sizeof(request_text) - 1U, stream),
                   sizeof(request_text) - 1U);
  rewind(stream);
  YAP_V2_core_http_request_init(&request);
  assert_int_equal(YAP_V2_core_http_read_request(stream, 3U, &request),
                   YAP_V2_CORE_HTTP_IO_ERROR);
  assert_null(request.body);
  YAP_V2_core_http_request_free(&request);
  fclose(stream);
}

static void test_response_headers_and_limit(void **state) {
  FILE *stream = tmpfile();
  char output[1024];
  size_t bytes;
  (void)state;
  assert_non_null(stream);
  assert_int_equal(YAP_V2_core_http_write_response(
    stream, 405, "application/json; charset=utf-8", "QUERY", 1, "{}", 2U),
    YAP_V2_CORE_HTTP_OK);
  rewind(stream);
  bytes = fread(output, 1U, sizeof(output) - 1U, stream);
  output[bytes] = '\0';
  assert_non_null(strstr(output, "HTTP/1.1 405 Method Not Allowed\r\n"));
  assert_non_null(strstr(output, "Allow: QUERY\r\n"));
  assert_non_null(strstr(output, "Accept-Query: application/json\r\n"));
  assert_non_null(strstr(output, "Cache-Control: no-store\r\n"));
  assert_int_equal(YAP_V2_core_http_write_response(
    stream, 200, "application/json", NULL, 0, "x",
    YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES + 1U), YAP_V2_CORE_HTTP_INVALID_ARGUMENT);
  fclose(stream);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_parse_query_head),
    cmocka_unit_test(test_parse_rejects_invalid_heads),
    cmocka_unit_test(test_parse_preserves_unsupported_media_type),
    cmocka_unit_test(test_stream_read_and_body_limit),
    cmocka_unit_test(test_stream_rejects_truncated_body),
    cmocka_unit_test(test_response_headers_and_limit)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
