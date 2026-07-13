#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_embedding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  size_t calls;
  size_t sleeps;
  uint32_t last_delay;
} MOCK_STATE;

static YAP_V2_BYTES_VIEW view(const char *text) {
  YAP_V2_BYTES_VIEW result = {(const unsigned char *)text, strlen(text)};
  return result;
}

static int mock_transport(void *context, const char *endpoint, const char *authorization,
                          const char *request_body, size_t request_bytes, uint32_t timeout_ms,
                          char **response_body, size_t *response_bytes, long *http_status) {
  MOCK_STATE *state = context;
  yyjson_doc *document;
  yyjson_val *input;
  const char *response;
  assert_string_equal(endpoint, "https://embed.test/v1/embeddings");
  assert_string_equal(authorization, "secret");
  assert_int_equal(timeout_ms, 1234U);
  document = yyjson_read(request_body, request_bytes, 0U);
  assert_non_null(document);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(document), "model")),
                      "embed-v1");
  input = yyjson_obj_get(yyjson_doc_get_root(document), "input");
  assert_true(yyjson_is_arr(input));
  state->calls++;
  if (state->calls == 1U) {
    response = "{\"error\":\"rate limited\"}";
    *http_status = 429L;
  } else if (yyjson_arr_size(input) == 2U) {
    response = "{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,2]}]}";
    *http_status = 200L;
  } else {
    response = "{\"data\":[{\"index\":0,\"embedding\":[5,6]}]}";
    *http_status = 200L;
  }
  *response_bytes = strlen(response);
  *response_body = malloc(*response_bytes + 1U);
  assert_non_null(*response_body);
  memcpy(*response_body, response, *response_bytes + 1U);
  yyjson_doc_free(document);
  return YAP_EMBEDDING_OK;
}

static void mock_sleep(void *context, uint32_t milliseconds) {
  MOCK_STATE *state = context;
  state->sleeps++;
  state->last_delay = milliseconds;
}

static void test_http_batches_retries_and_orders(void **state_ptr) {
  YAP_EMBEDDING_INPUT inputs[] = {{view("a"), view("first")}, {view("b"), view("second")},
                                  {view("c"), view("third")}};
  YAP_EMBEDDING_HTTP_CONFIG config;
  YAP_EMBEDDING_RESULT result;
  MOCK_STATE state = {0};
  (void)state_ptr;
  YAP_Embedding_http_config_init(&config);
  config.endpoint = "https://embed.test/v1/embeddings";
  config.api_key = "secret";
  config.model_id = "embed-v1";
  config.dimensions = 2U;
  config.batch_size = 2U;
  config.timeout_ms = 1234U;
  config.max_retries = 2U;
  config.retry_backoff_ms = 7U;
  config.transport = mock_transport;
  config.transport_context = &state;
  config.sleep = mock_sleep;
  config.sleep_context = &state;
  YAP_Embedding_result_init(&result);
  assert_int_equal(YAP_Embedding_http(&config, inputs, 3U, &result), YAP_EMBEDDING_OK);
  assert_int_equal(state.calls, 3U);
  assert_int_equal(state.sleeps, 1U);
  assert_int_equal(state.last_delay, 7U);
  assert_int_equal(result.input_count, 3U);
  assert_int_equal(result.dimensions, 2U);
  assert_float_equal(result.values[0], 1.0f, 0.0f);
  assert_float_equal(result.values[3], 4.0f, 0.0f);
  assert_float_equal(result.values[5], 6.0f, 0.0f);
  YAP_Embedding_result_free(&result);
}

static void write_text(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  assert_non_null(file);
  assert_true(fputs(content, file) >= 0);
  assert_int_equal(fclose(file), 0);
}

static void test_precomputed_matches_ids_not_file_order(void **state_ptr) {
  char path[] = "/tmp/yappod-embeddings-XXXXXX";
  YAP_EMBEDDING_INPUT inputs[] = {{view("a"), {NULL, 0U}}, {view("b"), {NULL, 0U}}};
  YAP_EMBEDDING_RESULT result;
  int fd;
  (void)state_ptr;
  fd = mkstemp(path);
  assert_true(fd >= 0);
  assert_int_equal(close(fd), 0);
  write_text(path, "{\"id\":\"b\",\"embedding\":[3,4]}\n"
                   "{\"id\":\"a\",\"embedding\":[1,2]}\n");
  YAP_Embedding_result_init(&result);
  assert_int_equal(YAP_Embedding_precomputed_read(path, inputs, 2U, 2U, &result),
                   YAP_EMBEDDING_OK);
  assert_float_equal(result.values[0], 1.0f, 0.0f);
  assert_float_equal(result.values[3], 4.0f, 0.0f);
  YAP_Embedding_result_free(&result);
  assert_int_equal(unlink(path), 0);
}

static void test_precomputed_rejects_duplicate_and_missing(void **state_ptr) {
  char path[] = "/tmp/yappod-embeddings-XXXXXX";
  YAP_EMBEDDING_INPUT inputs[] = {{view("a"), {NULL, 0U}}, {view("b"), {NULL, 0U}}};
  YAP_EMBEDDING_RESULT result;
  int fd;
  (void)state_ptr;
  fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  write_text(path, "{\"id\":\"a\",\"embedding\":[1,2]}\n"
                   "{\"id\":\"a\",\"embedding\":[3,4]}\n");
  YAP_Embedding_result_init(&result);
  assert_int_equal(YAP_Embedding_precomputed_read(path, inputs, 2U, 2U, &result),
                   YAP_EMBEDDING_DUPLICATE_ID);
  write_text(path, "{\"id\":\"a\",\"embedding\":[1,2]}\n");
  assert_int_equal(YAP_Embedding_precomputed_read(path, inputs, 2U, 2U, &result),
                   YAP_EMBEDDING_MISSING_ID);
  write_text(path, "{\"id\":\"a\",\"embedding\":[1,2,3]}\n");
  assert_int_equal(YAP_Embedding_precomputed_read(path, inputs, 2U, 2U, &result),
                   YAP_EMBEDDING_DIMENSION_MISMATCH);
  assert_int_equal(unlink(path), 0);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_http_batches_retries_and_orders),
      cmocka_unit_test(test_precomputed_matches_ids_not_file_order),
      cmocka_unit_test(test_precomputed_rejects_duplicate_and_missing)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
