#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "yappo_config_v2.h"
#include "yappo_http_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_update_v2.h"
#include "yappo_vector_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *text) {
  YAP_V2_BYTES_VIEW value = {(const unsigned char *)text, strlen(text)}; return value;
}

static void add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                          const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  assert_int_equal(YAP_V2_segment_descriptor_add_component(segment, component), YAP_V2_OK);
}

static void create_index(ytest_env_t *env) {
  YAP_V2_CONFIG config; YAP_V2_DOCUMENT_VIEW documents[2]; YAP_V2_PASSAGE_VIEW passages[2];
  YAP_V2_COMPONENT_DESCRIPTOR lexical[3], vectors, metadata; YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_MANIFEST manifest; YAP_EMBEDDING_RESULT embeddings; float values[] = {1,0,0,1};
  char segments[PATH_MAX], segment[PATH_MAX], path[PATH_MAX]; FILE *file;
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "config.toml"), 0);
  file = fopen(path, "wb"); assert_non_null(file);
  assert_true(fputs("format_version=2\n[tokenizer]\nid=\"unicode_nfkc_cf_v1\"\n"
                    "[chunking]\nmax_chars=5\noverlap_chars=0\n"
                    "[vector]\nenabled=true\nmodel_id=\"embed-v1\"\ndimensions=2\nmetric=\"cosine\"\n"
                    "[metadata]\nfilterable_fields=[\"category\"]\n", file) >= 0);
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_config_load(path, &config, NULL, 0U), YAP_V2_OK);
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-a"); documents[0].url = bytes("https://e.test/a");
  documents[0].title = bytes("Old"); documents[0].body = bytes("old");
  documents[0].metadata_json = bytes("{\"category\":\"old\"}");
  documents[1].id = bytes("doc-b"); documents[1].url = bytes("https://e.test/b");
  documents[1].title = bytes("Delete"); documents[1].body = bytes("gone");
  documents[1].metadata_json = bytes("{\"category\":\"old\"}");
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes("passage-a"); passages[0].parent_document_id = documents[0].id;
  passages[0].text = bytes("old"); passages[0].end_char = 3U;
  passages[1].id = bytes("passage-b"); passages[1].parent_document_id = documents[1].id;
  passages[1].text = bytes("gone"); passages[1].end_char = 4U;
  assert_int_equal(ytest_path_join(segments, sizeof(segments), env->tmp_root, "segments"), 0);
  assert_int_equal(ytest_path_join(segment, sizeof(segment), segments, "seg-1"), 0);
  assert_int_equal(ytest_mkdir_p(segment, 0700), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment, "documents.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "seg-1", 1U, documents, 2U, passages, 2U,
                                        &descriptor), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(segment, 1U, documents, 2U, passages, 2U, lexical), YAP_V2_OK);
  add_component(&descriptor, &lexical[0]); add_component(&descriptor, &lexical[1]);
  add_component(&descriptor, &lexical[2]);
  embeddings.values = values; embeddings.input_count = 2U; embeddings.dimensions = 2U;
  assert_int_equal(ytest_path_join(path, sizeof(path), segment, "vectors.yap2"), 0);
  assert_int_equal(YAP_V2_vectors_write(path, 1U, &config, passages, 2U, &embeddings, &vectors), YAP_V2_OK);
  add_component(&descriptor, &vectors);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment, "metadata.yap2"), 0);
  assert_int_equal(YAP_V2_metadata_write(path, 1U, &config, documents, 2U, &metadata), YAP_V2_OK);
  add_component(&descriptor, &metadata);
  YAP_V2_manifest_init(&manifest); manifest.generation = 1U;
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &descriptor), YAP_V2_OK);
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
}

static yyjson_doc *execute(ytest_env_t *env, YAP_V2_HTTP_OPERATION operation, const char *body,
                           int expected_status) {
  int http_status; char *response = NULL; size_t response_bytes = 0U; yyjson_doc *document;
  assert_int_equal(YAP_V2_http_execute(env->tmp_root, operation, (const unsigned char *)body,
                                      strlen(body), &http_status, &response, &response_bytes), 0);
  if (http_status != expected_status && response != NULL)
    print_message("unexpected response for %s: %s\n", body, response);
  assert_int_equal(http_status, expected_status); assert_non_null(response);
  document = yyjson_read(response, response_bytes, 0U); free(response); assert_non_null(document);
  return document;
}

static uint64_t manifest_generation(ytest_env_t *env, size_t *segments) {
  char path[PATH_MAX]; YAP_V2_MANIFEST manifest; uint64_t generation;
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_OK);
  generation = manifest.generation; if (segments != NULL) *segments = manifest.segment_count;
  YAP_V2_manifest_free(&manifest); return generation;
}

static size_t search_count(ytest_env_t *env, const char *query, const char *expected_id) {
  char request[256]; yyjson_doc *document; yyjson_val *results, *item; size_t count;
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"%s\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":10}", query) > 0);
  document = execute(env, YAP_V2_HTTP_SEARCH, request, 200);
  results = yyjson_obj_get(yyjson_doc_get_root(document), "results"); count = yyjson_arr_size(results);
  if (expected_id != NULL) { item = yyjson_arr_get_first(results); assert_non_null(item);
    assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "id")), expected_id); }
  yyjson_doc_free(document); return count;
}

static void test_http_atomic_update_latest_wins_and_delete(void **state) {
  ytest_env_t env; yyjson_doc *document; size_t segments; uint64_t generation;
  const char *upsert = "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-a\","
    "\"url\":\"https://e.test/a2\",\"title\":\"New\",\"body\":\"fresh\","
    "\"metadata\":{\"category\":\"new\"},\"vectors\":[[1,0]]}]}";
  const char *invalid = "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-c\","
    "\"body\":\"abcdefghij\",\"vectors\":[[1,0]]},{\"operation\":\"delete\",\"id\":\"doc-b\"}]}";
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  document = execute(&env, YAP_V2_HTTP_INGEST, upsert, 200);
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(document), "generation")), 2U);
  yyjson_doc_free(document);
  assert_int_equal(manifest_generation(&env, &segments), 2U); assert_int_equal(segments, 2U);
  assert_int_equal(search_count(&env, "fresh", "doc-a"), 1U);
  assert_int_equal(search_count(&env, "old", NULL), 0U);
  document = execute(&env, YAP_V2_HTTP_INGEST, invalid, 400); yyjson_doc_free(document);
  generation = manifest_generation(&env, &segments); assert_int_equal(generation, 2U); assert_int_equal(segments, 2U);
  document = execute(&env, YAP_V2_HTTP_INGEST,
    "{\"operations\":[{\"operation\":\"delete\",\"id\":\"doc-b\"}]}", 200);
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(document), "generation")), 3U);
  yyjson_doc_free(document); assert_int_equal(search_count(&env, "gone", NULL), 0U);
  ytest_env_destroy(&env);
}

static void test_cli_update_and_strict_batch_schema(void **state) {
  ytest_env_t env; char input[PATH_MAX], executable[PATH_MAX]; ytest_cmd_result_t command;
  char *argv[7]; const char *line = "{\"operation\":\"upsert\",\"id\":\"doc-cli\","
    "\"body\":\"hello\",\"vectors\":[[0,1]]}\n"; yyjson_doc *document;
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  assert_int_equal(ytest_path_join(input, sizeof(input), env.tmp_root, "operations.ndjson"), 0);
  assert_int_equal(ytest_write_file(input, line, strlen(line)), 0);
  assert_int_equal(ytest_path_join(executable, sizeof(executable), env.build_dir, "yappo_makeindex"), 0);
  argv[0] = executable; argv[1] = "update"; argv[2] = "--input"; argv[3] = input;
  argv[4] = "--index"; argv[5] = env.tmp_root; argv[6] = NULL;
  ytest_cmd_result_init(&command); assert_int_equal(ytest_cmd_run(argv, NULL, NULL, 0U, &command), 0);
  assert_true(command.exited); assert_int_equal(command.exit_code, 0); assert_non_null(strstr(command.output, "\"generation\":2"));
  ytest_cmd_result_free(&command); assert_int_equal(search_count(&env, "hello", "doc-cli"), 1U);
  document = execute(&env, YAP_V2_HTTP_INGEST, "{\"operations\":[],\"extra\":true}", 400);
  yyjson_doc_free(document); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_http_atomic_update_latest_wins_and_delete),
    cmocka_unit_test(test_cli_update_and_strict_batch_schema)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
