#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_filter_v2.h"
#include "yappo_config_v2.h"
#include "yappo_snippet_v2.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static YAP_V2_BYTES_VIEW view(const char *text) {
  YAP_V2_BYTES_VIEW result = {(const unsigned char *)text, strlen(text)};
  return result;
}

static YAP_V2_DOCUMENT_VIEW document(const char *id, const char *metadata) {
  YAP_V2_DOCUMENT_VIEW result;
  memset(&result, 0, sizeof(result)); result.id = view(id); result.url = view("https://example.test");
  result.title = view("title"); result.body = view("body"); result.metadata_json = view(metadata); return result;
}

static void test_metadata_filter_roundtrip(void **state) {
  char path[] = "/tmp/yappod-metadata-XXXXXX"; int fd; YAP_V2_CONFIG config;
  YAP_V2_DOCUMENT_VIEW documents[3]; YAP_V2_COMPONENT_DESCRIPTOR written, read;
  YAP_V2_METADATA_INDEX metadata; YAP_V2_FILTER filter; int matches;
  (void)state; fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  YAP_V2_config_init(&config); config.filterable_field_count = 3U;
  strcpy(config.filterable_fields[0], "author.name"); strcpy(config.filterable_fields[1], "lang"); strcpy(config.filterable_fields[2], "year");
  documents[0] = document("a", "{\"author\":{\"name\":\"Ada\"},\"lang\":\"ja\",\"year\":2024}");
  documents[1] = document("b", "{\"author\":{\"name\":\"Bob\"},\"lang\":[\"en\",\"ja\"],\"year\":2019}");
  documents[2] = document("c", "{}");
  assert_int_equal(YAP_V2_metadata_write(path, 7U, &config, documents, 3U, &written), YAP_V2_OK);
  assert_string_equal(written.name, "metadata.yap2"); assert_int_equal(written.file_type, YAP_V2_FILE_METADATA);
  YAP_V2_metadata_index_init(&metadata); assert_int_equal(YAP_V2_metadata_read(path, 7U, &config, &metadata, &read), YAP_V2_OK);
  assert_int_equal(metadata.document_count, 3U); assert_int_equal(metadata.entry_count, 7U);
  YAP_V2_filter_init(&filter);
  assert_int_equal(YAP_V2_filter_compile(view("{\"and\":[{\"eq\":{\"field\":\"lang\",\"value\":\"ja\"}},{\"range\":{\"field\":\"year\",\"gte\":2020}}]}"), &metadata, &filter), YAP_V2_OK);
  assert_int_equal(YAP_V2_filter_matches(&filter, 0U, &matches), YAP_V2_OK); assert_true(matches);
  assert_int_equal(YAP_V2_filter_matches(&filter, 1U, &matches), YAP_V2_OK); assert_false(matches);
  YAP_V2_filter_free(&filter);
  assert_int_equal(YAP_V2_filter_compile(view("{\"exists\":{\"field\":\"missing\"}}"), &metadata, &filter), YAP_V2_INVALID_FORMAT);
  YAP_V2_metadata_index_free(&metadata); assert_int_equal(unlink(path), 0);
}

static void test_snippet_grapheme_boundaries(void **state) {
  const char *text = "前半🙂これは検索結果です。後半"; YAP_V2_BYTES_VIEW terms[] = {view("検索")};
  char output[128]; size_t bytes;
  (void)state;
  assert_int_equal(YAP_V2_snippet(view(text), terms, 1U, 8U, "<mark>", "</mark>", output, sizeof(output), &bytes), YAP_V2_OK);
  assert_non_null(strstr(output, "<mark>検索</mark>")); assert_int_equal(strlen(output), bytes);
}

int main(void) {
  const struct CMUnitTest tests[] = {cmocka_unit_test(test_metadata_filter_roundtrip), cmocka_unit_test(test_snippet_grapheme_boundaries)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
