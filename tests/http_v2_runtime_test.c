#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_config_v2.h"
#include "yappo_http_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
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
                    "[chunking]\nmax_chars=100\noverlap_chars=0\n"
                    "[vector]\nenabled=true\nmodel_id=\"embed-v1\"\ndimensions=2\nmetric=\"cosine\"\n"
                    "[metadata]\nfilterable_fields=[\"category\"]\n", file) >= 0);
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_config_load(path, &config, NULL, 0U), YAP_V2_OK);
  memset(documents, 0, sizeof(documents));
  documents[0].id = bytes("doc-fruit"); documents[0].url = bytes("https://e.test/fruit");
  documents[0].title = bytes("Fruit guide"); documents[0].body = bytes("fresh apple");
  documents[0].metadata_json = bytes("{\"category\":\"fruit\"}");
  documents[1].id = bytes("doc-tech"); documents[1].url = bytes("https://e.test/tech");
  documents[1].title = bytes("Tech guide");
  documents[1].body = bytes("prefix <script>alert(1)</script> apple computer");
  documents[1].metadata_json = bytes("{\"category\":\"tech\"}");
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes("passage-fruit"); passages[0].parent_document_id = documents[0].id;
  passages[0].text = bytes("fresh apple"); passages[0].end_char = 11U;
  passages[1].id = bytes("passage-tech"); passages[1].parent_document_id = documents[1].id;
  passages[1].text = bytes("apple computer"); passages[1].end_char = 14U;
  assert_int_equal(ytest_path_join(segments, sizeof(segments), env->tmp_root, "segments"), 0);
  assert_int_equal(ytest_path_join(segment, sizeof(segment), segments, "seg-1"), 0);
  assert_int_equal(ytest_mkdir_p(segment, 0700), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), segment, "documents.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "seg-1", 1U, documents, 2U, passages, 2U, &descriptor), YAP_V2_OK);
  assert_int_equal(YAP_V2_lexical_write(segment, 1U, documents, 2U, passages, 2U, lexical), YAP_V2_OK);
  add_component(&descriptor, &lexical[0]); add_component(&descriptor, &lexical[1]); add_component(&descriptor, &lexical[2]);
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

static yyjson_doc *execute(ytest_env_t *env, YAP_V2_HTTP_OPERATION operation, const char *request,
                           int expected_status) {
  char *response = NULL; size_t response_bytes = 0U; int status = 0; yyjson_doc *document;
  assert_int_equal(YAP_V2_http_execute(env->tmp_root, operation, (const unsigned char *)request,
                                      strlen(request), &status, &response, &response_bytes), 0);
  assert_int_equal(status, expected_status); assert_non_null(response);
  document = yyjson_read(response, response_bytes, 0U); free(response); assert_non_null(document);
  return document;
}

static void copy_json_string(char *output, size_t capacity, yyjson_val *value) {
  const char *text;
  size_t length;
  assert_non_null(output);
  assert_true(capacity > 0U);
  assert_true(yyjson_is_str(value));
  text = yyjson_get_str(value);
  length = yyjson_get_len(value);
  assert_non_null(text);
  assert_true(length < capacity);
  memcpy(output, text, length);
  output[length] = '\0';
}

static void test_real_search_and_retrieve_runtime(void **state) {
  ytest_env_t env; yyjson_doc *document; yyjson_val *root, *results, *item;
  char cursor[256], request[1024], first_id[64], tampered[256];
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  document = execute(&env, YAP_V2_HTTP_SEARCH,
    "{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"scope\":\"documents\","
    "\"filter\":{\"eq\":{\"field\":\"category\",\"value\":\"fruit\"}},\"limit\":1}", 200);
  root = yyjson_doc_get_root(document); results = yyjson_obj_get(root, "results");
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(root, "generation")), 1U);
  assert_int_equal(yyjson_arr_size(results), 1U); item = yyjson_arr_get_first(results);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "id")), "doc-fruit");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "title")), "Fruit guide");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "url")), "https://e.test/fruit");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "snippet")), "fresh apple");
  assert_true(yyjson_get_real(yyjson_obj_get(item, "lexical_score")) > 0.0);
  assert_true(yyjson_get_real(yyjson_obj_get(item, "vector_score")) > 0.0);
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_SEARCH,
    "{\"query\":\"computer\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1}", 200);
  item = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(document), "results"));
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "id")), "doc-tech");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "title")), "Tech guide");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "url")), "https://e.test/tech");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "snippet")),
                      "prefix <script>alert(1)</script> apple computer");
  assert_null(strstr(yyjson_get_str(yyjson_obj_get(item, "snippet")), "<mark>"));
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_SEARCH,
    "{\"vector\":[0,1],\"mode\":\"vector\",\"scope\":\"passages\",\"limit\":1}", 200);
  item = yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(document), "results"));
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "id")), "passage-tech");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "title")), "Tech guide");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "url")), "https://e.test/tech");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "snippet")), "apple computer");
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_SEARCH,
    "{\"query\":\"apple\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1}", 200);
  root = yyjson_doc_get_root(document); item = yyjson_arr_get_first(yyjson_obj_get(root, "results"));
  copy_json_string(cursor, sizeof(cursor), yyjson_obj_get(root, "next_cursor"));
  copy_json_string(first_id, sizeof(first_id), yyjson_obj_get(item, "id"));
  yyjson_doc_free(document);
  assert_true(snprintf(request, sizeof(request),
    "{\"scope\":\"documents\",\"limit\":1,\"mode\":\"lexical\",\"query\":\"apple\",\"cursor\":\"%s\"}", cursor) > 0);
  document = execute(&env, YAP_V2_HTTP_SEARCH, request, 200); root = yyjson_doc_get_root(document);
  item = yyjson_arr_get_first(yyjson_obj_get(root, "results")); assert_non_null(item);
  assert_string_not_equal(yyjson_get_str(yyjson_obj_get(item, "id")), first_id);
  assert_true(yyjson_is_null(yyjson_obj_get(root, "next_cursor"))); yyjson_doc_free(document);
  assert_true(snprintf(tampered, sizeof(tampered), "%s", cursor) > 0);
  tampered[strlen(tampered) - 1U] = tampered[strlen(tampered) - 1U] == '0' ? '1' : '0';
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"apple\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1,\"cursor\":\"%s\"}", tampered) > 0);
  document = execute(&env, YAP_V2_HTTP_SEARCH, request, 400); yyjson_doc_free(document);
  assert_true(snprintf(tampered, sizeof(tampered), "%s", cursor) > 0); tampered[5] = '2';
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"apple\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1,\"cursor\":\"%s\"}", tampered) > 0);
  document = execute(&env, YAP_V2_HTTP_SEARCH, request, 400); yyjson_doc_free(document);
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"computer\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1,\"cursor\":\"%s\"}", cursor) > 0);
  document = execute(&env, YAP_V2_HTTP_SEARCH, request, 400); yyjson_doc_free(document);
  assert_true(snprintf(request, sizeof(request), "%s", cursor) > 0); request[3] = '2';
  assert_true(snprintf(tampered, sizeof(tampered),
    "{\"query\":\"apple\",\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":1,\"cursor\":\"%s\"}", request) > 0);
  document = execute(&env, YAP_V2_HTTP_SEARCH, tampered, 400); yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_RETRIEVE,
    "{\"query\":\"apple\",\"vector\":[1,0],\"mode\":\"hybrid\",\"limit\":1,"
    "\"max_passages_per_document\":1,\"max_context_bytes\":100}", 200);
  root = yyjson_doc_get_root(document); assert_string_equal(yyjson_get_str(yyjson_obj_get(root, "context")), "fresh apple");
  item = yyjson_arr_get_first(yyjson_obj_get(root, "citations"));
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "passage_id")), "passage-fruit");
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "url")), "https://e.test/fruit");
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(item, "start_char")), 0U);
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(item, "end_char")), 11U);
  assert_true(yyjson_get_real(yyjson_obj_get(item, "fused_score")) > 0.0);
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_SEARCH, "{\"query\":\"apple\",\"mode\":\"hybrid\"}", 400);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "error"), "code")), "invalid_request");
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_SEARCH, "{\"query\":\"apple\",\"mode\":\"lexical\",\"limit\":101}", 400);
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_PREPARE,
    "{\"id\":\"doc-new\",\"body\":\"First sentence. Second sentence.\"}", 200);
  root = yyjson_doc_get_root(document);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(root, "model_id")), "embed-v1");
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(root, "dimensions")), 2U);
  results = yyjson_obj_get(root, "passages"); assert_int_equal(yyjson_arr_size(results), 1U);
  item = yyjson_arr_get_first(results);
  assert_int_equal(yyjson_get_uint(yyjson_obj_get(item, "ordinal")), 0U);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(item, "text")),
                      "first sentence. second sentence.");
  yyjson_doc_free(document);
  document = execute(&env, YAP_V2_HTTP_PREPARE,
    "{\"id\":\"doc-new\",\"body\":\"text\",\"extra\":true}", 400);
  yyjson_doc_free(document); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {cmocka_unit_test(test_real_search_and_retrieve_runtime)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
