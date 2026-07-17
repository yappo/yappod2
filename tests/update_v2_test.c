#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmocka.h>
#include <yyjson.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "yappo_config_v2.h"
#include "yappo_compact_v2.h"
#include "yappo_http_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_observability_v2.h"
#include "yappo_segment_planner_v2.h"
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

static size_t segment_directory_count(ytest_env_t *env) {
  char path[PATH_MAX];
  DIR *directory;
  struct dirent *entry;
  size_t count = 0U;
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "segments"), 0);
  directory = opendir(path); assert_non_null(directory);
  while ((entry = readdir(directory)) != NULL)
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
  assert_int_equal(closedir(directory), 0);
  return count;
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
  assert_null(yyjson_obj_get(yyjson_doc_get_root(document), "segment_id"));
  assert_int_equal(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(document), "segment_ids")), 1U);
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
  ytest_env_t env; char input[PATH_MAX], executable[PATH_MAX], application[PATH_MAX], source[8192]; ytest_cmd_result_t command;
  char *argv[7]; const char *line = "{\"operation\":\"upsert\",\"id\":\"doc-cli\","
    "\"body\":\"hello\",\"vectors\":[[0,1]]}\n"; yyjson_doc *document;
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  assert_int_equal(ytest_path_join(input, sizeof(input), env.tmp_root, "operations.ndjson"), 0);
  assert_int_equal(ytest_write_file(input, line, strlen(line)), 0);
  assert_int_equal(ytest_path_join(executable, sizeof(executable), env.build_dir, "yappo_makeindex"), 0);
  assert_int_equal(ytest_path_join(application, sizeof(application), env.tmp_root, "application.toml"), 0);
  assert_true(snprintf(source, sizeof(source),
    "schema_version=1\nformat_version=2\nindex.directory='%s'\n[tokenizer]\nid='unicode_nfkc_cf_v1'\n[chunking]\nmax_chars=5\noverlap_chars=0\n[vector]\nenabled=true\nmodel_id='test-2d'\ndimensions=2\nmetric='cosine'\n[metadata]\nfilterable_fields=['category']\n[daemon]\nrun_directory='%s/run'\ncore_host='127.0.0.1'\ncore_port=18401\nfront_host='127.0.0.1'\nfront_port=18400\n",
    env.tmp_root, env.tmp_root) > 0);
  assert_int_equal(ytest_write_file(application, source, strlen(source)), 0);
  argv[0] = executable; argv[1] = "update"; argv[2] = "--input"; argv[3] = input;
  argv[4] = "--config"; argv[5] = application; argv[6] = NULL;
  ytest_cmd_result_init(&command); assert_int_equal(ytest_cmd_run(argv, NULL, NULL, 0U, &command), 0);
  assert_true(command.exited); assert_int_equal(command.exit_code, 0); assert_non_null(strstr(command.output, "\"generation\":2"));
  assert_non_null(strstr(command.output, "\"segment_ids\":["));
  assert_null(strstr(command.output, "\"segment_id\":"));
  ytest_cmd_result_free(&command); assert_int_equal(search_count(&env, "hello", "doc-cli"), 1U);
  document = execute(&env, YAP_V2_HTTP_INGEST, "{\"operations\":[],\"extra\":true}", 400);
  yyjson_doc_free(document); ytest_env_destroy(&env);
}

static void test_update_split_is_atomic_and_cleans_failed_segments(void **state) {
  ytest_env_t env;
  yyjson_doc *document;
  yyjson_val *root;
  size_t segments;
  const char *batch = "{\"operations\":["
    "{\"operation\":\"upsert\",\"id\":\"doc-c\",\"title\":\"Alpha\","
    "\"body\":\"alpha\",\"metadata\":{\"category\":\"new\"},\"vectors\":[[1,0]]},"
    "{\"operation\":\"upsert\",\"id\":\"doc-d\",\"title\":\"Beta\","
    "\"body\":\"beta\",\"metadata\":{\"category\":\"new\"},\"vectors\":[[0,1]]}]}";
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  YAP_V2_segment_planner_set_payload_limit_for_testing(300U);
  YAP_V2_update_set_failpoint_for_testing("after_first_segment");
  document = execute(&env, YAP_V2_HTTP_INGEST, batch, 503); yyjson_doc_free(document);
  YAP_V2_update_set_failpoint_for_testing(NULL);
  assert_int_equal(manifest_generation(&env, &segments), 1U); assert_int_equal(segments, 1U);
  assert_int_equal(segment_directory_count(&env), 1U);
  document = execute(&env, YAP_V2_HTTP_INGEST, batch, 200);
  YAP_V2_segment_planner_set_payload_limit_for_testing(0U);
  root = yyjson_doc_get_root(document);
  assert_null(yyjson_obj_get(root, "segment_id"));
  assert_int_equal(yyjson_arr_size(yyjson_obj_get(root, "segment_ids")), 2U);
  yyjson_doc_free(document);
  assert_int_equal(manifest_generation(&env, &segments), 2U); assert_int_equal(segments, 3U);
  assert_int_equal(search_count(&env, "alpha", "doc-c"), 1U);
  assert_int_equal(search_count(&env, "beta", "doc-d"), 1U);
  ytest_env_destroy(&env);
}

static void test_single_document_capacity_error_is_detailed(void **state) {
  ytest_env_t env;
  yyjson_doc *document;
  yyjson_val *message;
  const char *text;
  size_t segments;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  YAP_V2_segment_planner_set_payload_limit_for_testing(100U);
  document = execute(&env, YAP_V2_HTTP_INGEST,
    "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-too-large\","
    "\"title\":\"large\",\"body\":\"large\",\"metadata\":{\"category\":\"x\"},"
    "\"vectors\":[[1,0]]}]}", 400);
  YAP_V2_segment_planner_set_payload_limit_for_testing(0U);
  message = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "error"), "message");
  text = yyjson_get_str(message); assert_non_null(text);
  assert_non_null(strstr(text, "doc-too-large")); assert_non_null(strstr(text, ".yap2"));
  assert_non_null(strstr(text, "requires")); assert_non_null(strstr(text, "limit 100"));
  yyjson_doc_free(document);
  assert_int_equal(manifest_generation(&env, &segments), 1U); assert_int_equal(segments, 1U);
  ytest_env_destroy(&env);
}

static void test_build_batch_uses_the_same_split_planner(void **state) {
  ytest_env_t env;
  YAP_V2_INGEST_OPERATION operations[2];
  YAP_V2_UPDATE_RESULT result;
  char error[256] = {0};
  const char *lines[] = {
    "{\"operation\":\"upsert\",\"id\":\"build-a\",\"title\":\"Alpha\","
    "\"body\":\"alpha\",\"metadata\":{\"category\":\"new\"},\"vectors\":[[1,0]]}",
    "{\"operation\":\"upsert\",\"id\":\"build-b\",\"title\":\"Beta\","
    "\"body\":\"beta\",\"metadata\":{\"category\":\"new\"},\"vectors\":[[0,1]]}"
  };
  size_t i, segments;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  memset(operations, 0, sizeof(operations));
  for (i = 0U; i < 2U; i++)
    assert_int_equal(YAP_V2_ingest_parse_ndjson(lines[i], strlen(lines[i]), &operations[i],
                                                error, sizeof(error)), YAP_V2_OK);
  YAP_V2_update_result_init(&result);
  YAP_V2_segment_planner_set_payload_limit_for_testing(300U);
  assert_int_equal(YAP_V2_build_apply(env.tmp_root, operations, 2U, &result,
                                      error, sizeof(error)), YAP_V2_OK);
  YAP_V2_segment_planner_set_payload_limit_for_testing(0U);
  assert_int_equal(result.accepted, 2U); assert_int_equal(result.segment_ids.count, 2U);
  assert_int_equal(manifest_generation(&env, &segments), 2U); assert_int_equal(segments, 3U);
  YAP_V2_update_result_free(&result);
  for (i = 0U; i < 2U; i++) YAP_V2_ingest_operation_free(&operations[i]);
  ytest_env_destroy(&env);
}

static void assert_manifest_shape(ytest_env_t *env, uint64_t generation, size_t segment_count,
                                  size_t tombstone_count) {
  char path[PATH_MAX]; YAP_V2_MANIFEST manifest;
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_OK);
  assert_int_equal(manifest.generation, generation); assert_int_equal(manifest.segment_count, segment_count);
  if (segment_count != 0U) assert_int_equal(manifest.segments[0].tombstone_count, tombstone_count);
  assert_int_equal(YAP_V2_manifest_verify_components(env->tmp_root, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
}

static void assert_search_mode(ytest_env_t *env, const char *query, const char *vector,
                               const char *mode, const char *scope, const char *expected_id) {
  char request[512]; yyjson_doc *document; yyjson_val *results, *first;
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"%s\",\"vector\":[%s],\"mode\":\"%s\","
    "\"scope\":\"%s\",\"limit\":10}", query, vector, mode, scope) > 0);
  document = execute(env, YAP_V2_HTTP_SEARCH, request, 200);
  results = yyjson_obj_get(yyjson_doc_get_root(document), "results");
  assert_true(yyjson_arr_size(results) > 0U); first = yyjson_arr_get_first(results);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(first,
    strcmp(scope, "passages") == 0 ? "document_id" : "id")), expected_id);
  yyjson_doc_free(document);
}

static void assert_retrieval(ytest_env_t *env, const char *query, const char *vector,
                             const char *expected_id) {
  char request[512];
  yyjson_doc *document;
  yyjson_val *root;
  yyjson_val *citation;
  assert_true(snprintf(request, sizeof(request),
    "{\"query\":\"%s\",\"vector\":[%s],\"mode\":\"hybrid\","
    "\"limit\":10,\"max_passages_per_document\":2,\"max_context_bytes\":1024}",
    query, vector) > 0);
  document = execute(env, YAP_V2_HTTP_RETRIEVE, request, 200);
  root = yyjson_doc_get_root(document);
  citation = yyjson_arr_get_first(yyjson_obj_get(root, "citations"));
  assert_non_null(strstr(yyjson_get_str(yyjson_obj_get(root, "context")), query));
  assert_non_null(citation);
  assert_string_equal(yyjson_get_str(yyjson_obj_get(citation, "document_id")), expected_id);
  yyjson_doc_free(document);
}

static void apply_live_changes(ytest_env_t *env) {
  yyjson_doc *document = execute(env, YAP_V2_HTTP_INGEST,
    "{\"operations\":[{\"operation\":\"upsert\",\"id\":\"doc-a\","
    "\"url\":\"https://e.test/new\",\"title\":\"Fresh\",\"body\":\"fresh\","
    "\"metadata\":{\"category\":\"new\"},\"vectors\":[[1,0]]},"
    "{\"operation\":\"delete\",\"id\":\"doc-b\"}]}", 200);
  yyjson_doc_free(document);
}

static void test_compaction_live_only_preserves_all_search_modes(void **state) {
  ytest_env_t env;
  YAP_V2_OPERATIONAL_STATE operational;
  char executable[PATH_MAX]; char *argv[4]; ytest_cmd_result_t command;
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env); apply_live_changes(&env);
  assert_int_equal(ytest_path_join(executable, sizeof(executable), env.build_dir, "yappo_compact"), 0);
  argv[0] = executable; argv[1] = "--index"; argv[2] = env.tmp_root; argv[3] = NULL;
  ytest_cmd_result_init(&command); assert_int_equal(ytest_cmd_run(argv, NULL, NULL, 0U, &command), 0);
  assert_true(command.exited); assert_int_equal(command.exit_code, 0);
  assert_non_null(strstr(command.output, "\"generation\":3"));
  assert_non_null(strstr(command.output, "\"segment_ids\":["));
  assert_null(strstr(command.output, "\"segment_id\":"));
  ytest_cmd_result_free(&command);
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, NULL, 0U), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_SUCCEEDED);
  assert_int_equal(operational.compaction_generation, 3U);
  assert_manifest_shape(&env, 3U, 1U, 0U);
  assert_int_equal(search_count(&env, "old", NULL), 0U); assert_int_equal(search_count(&env, "gone", NULL), 0U);
  assert_int_equal(search_count(&env, "fresh", "doc-a"), 1U);
  assert_search_mode(&env, "fresh", "1,0", "vector", "documents", "doc-a");
  assert_search_mode(&env, "fresh", "1,0", "hybrid", "documents", "doc-a");
  assert_search_mode(&env, "fresh", "1,0", "hybrid", "passages", "doc-a");
  assert_retrieval(&env, "fresh", "1,0", "doc-a");
  ytest_env_destroy(&env);
}

static void test_compaction_splits_output_and_builds_segment_local_bm25_stats(void **state) {
  ytest_env_t env;
  YAP_V2_COMPACTION_RESULT result;
  YAP_V2_MANIFEST manifest;
  char manifest_path[PATH_MAX], segment_path[PATH_MAX];
  char error[256] = {0};
  size_t i;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  YAP_V2_compaction_result_init(&result);
  YAP_V2_segment_planner_set_payload_limit_for_testing(300U);
  assert_int_equal(YAP_V2_compact(env.tmp_root, &result, error, sizeof(error)), YAP_V2_OK);
  YAP_V2_segment_planner_set_payload_limit_for_testing(0U);
  assert_int_equal(result.segment_ids.count, 2U);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root,
                                   "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_load(manifest_path, &manifest), YAP_V2_OK);
  assert_int_equal(manifest.segment_count, 2U);
  for (i = 0U; i < manifest.segment_count; i++) {
    YAP_V2_LEXICAL_SEGMENT lexical;
    assert_int_equal(manifest.segments[i].document_count, 1U);
    assert_true(snprintf(segment_path, sizeof(segment_path), "%s/segments/%s",
                         env.tmp_root, manifest.segments[i].id) > 0);
    YAP_V2_lexical_segment_init(&lexical);
    assert_int_equal(YAP_V2_lexical_segment_open(segment_path, 2U, &lexical), YAP_V2_OK);
    assert_int_equal(lexical.document_count, 1U);
    YAP_V2_lexical_segment_close(&lexical);
  }
  assert_int_equal(search_count(&env, "old", "doc-a"), 1U);
  assert_int_equal(search_count(&env, "gone", "doc-b"), 1U);
  assert_search_mode(&env, "old", "1,0", "vector", "documents", "doc-a");
  assert_search_mode(&env, "old", "1,0", "hybrid", "documents", "doc-a");
  assert_search_mode(&env, "old", "1,0", "hybrid", "passages", "doc-a");
  assert_retrieval(&env, "old", "1,0", "doc-a");
  YAP_V2_manifest_free(&manifest); YAP_V2_compaction_result_free(&result);
  ytest_env_destroy(&env);
}

static void run_crashing_compaction(ytest_env_t *env, const char *point) {
  pid_t child = fork(); int child_status;
  assert_true(child >= 0);
  if (child == 0) {
    YAP_V2_COMPACTION_RESULT result; char error[256] = {0};
    YAP_V2_compaction_set_failpoint_for_testing(point);
    (void)YAP_V2_compact(env->tmp_root, &result, error, sizeof(error)); _exit(99);
  }
  assert_int_equal(waitpid(child, &child_status, 0), child);
  assert_true(WIFEXITED(child_status)); assert_int_equal(WEXITSTATUS(child_status), 86);
}

static void test_compaction_crash_recovery_and_orphan_gc(void **state) {
  ytest_env_t env; YAP_V2_COMPACTION_RESULT result; YAP_V2_OPERATIONAL_STATE operational; char error[256] = {0};
  (void)state; assert_int_equal(ytest_env_init(&env), 0); create_index(&env);
  YAP_V2_segment_planner_set_payload_limit_for_testing(300U);
  run_crashing_compaction(&env, "before_publish");
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, NULL, 0U), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_INTERRUPTED);
  assert_manifest_shape(&env, 1U, 1U, 0U); assert_int_equal(search_count(&env, "old", "doc-a"), 1U);
  YAP_V2_compaction_result_init(&result);
  assert_int_equal(YAP_V2_compact(env.tmp_root, &result, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(result.generation, 2U); assert_true(result.removed_segments >= 2U);
  assert_int_equal(result.segment_ids.count, 2U);
  YAP_V2_compaction_result_free(&result);
  assert_int_equal(YAP_V2_operational_probe_index(env.tmp_root, &operational, NULL, 0U), YAP_V2_OK);
  assert_int_equal(operational.compaction_state, YAP_V2_COMPACTION_SUCCEEDED);
  run_crashing_compaction(&env, "after_publish");
  assert_manifest_shape(&env, 3U, 2U, 0U); assert_int_equal(search_count(&env, "old", "doc-a"), 1U);
  YAP_V2_compaction_result_init(&result); memset(error, 0, sizeof(error));
  assert_int_equal(YAP_V2_compact(env.tmp_root, &result, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(result.generation, 4U); assert_true(result.removed_segments >= 2U);
  assert_int_equal(result.segment_ids.count, 2U);
  assert_manifest_shape(&env, 4U, 2U, 0U); YAP_V2_compaction_result_free(&result);
  YAP_V2_segment_planner_set_payload_limit_for_testing(0U);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_http_atomic_update_latest_wins_and_delete),
    cmocka_unit_test(test_cli_update_and_strict_batch_schema),
    cmocka_unit_test(test_update_split_is_atomic_and_cleans_failed_segments),
    cmocka_unit_test(test_single_document_capacity_error_is_detailed),
    cmocka_unit_test(test_build_batch_uses_the_same_split_planner),
    cmocka_unit_test(test_compaction_live_only_preserves_all_search_modes),
    cmocka_unit_test(test_compaction_splits_output_and_builds_segment_local_bm25_stats),
    cmocka_unit_test(test_compaction_crash_recovery_and_orphan_gc)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
