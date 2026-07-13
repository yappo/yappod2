#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>
#include "test_env.h"
#include "test_fs.h"
#include "yappo_manifest_v2.h"
#include "yappo_retrieve_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *s) {
  YAP_V2_BYTES_VIEW v = {(const unsigned char *)s, strlen(s)}; return v;
}

static void test_context_limits_and_citations(void **state) {
  ytest_env_t env; YAP_V2_CONFIG config; YAP_V2_DOCUMENT_VIEW docs[2];
  YAP_V2_PASSAGE_VIEW passages[3]; YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_MANIFEST manifest; YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot; YAP_V2_QUERY_HIT hits[4];
  YAP_V2_RETRIEVE_OPTIONS options; YAP_V2_CITATION citations[3];
  unsigned char context[64]; size_t context_bytes, citation_count;
  char segments[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], manifest_path[PATH_MAX];
  (void)state; assert_int_equal(ytest_env_init(&env), 0); YAP_V2_config_init(&config);
  memset(docs, 0, sizeof(docs));
  docs[0].id = bytes("doc-a"); docs[0].url = bytes("https://e.test/a");
  docs[0].title = bytes("Alpha"); docs[0].body = bytes("first second"); docs[0].metadata_json = bytes("{}");
  docs[1].id = bytes("doc-b"); docs[1].url = bytes("https://e.test/b");
  docs[1].title = bytes("Beta"); docs[1].body = bytes("third"); docs[1].metadata_json = bytes("{}");
  memset(passages, 0, sizeof(passages));
  passages[0].id = bytes("p-first"); passages[0].parent_document_id = docs[0].id;
  passages[0].text = bytes("first"); passages[0].end_char = 5U;
  passages[1].id = bytes("p-second"); passages[1].parent_document_id = docs[0].id;
  passages[1].text = bytes("second"); passages[1].ordinal = 1U;
  passages[1].start_char = 6U; passages[1].end_char = 12U;
  passages[2].id = bytes("p-third"); passages[2].parent_document_id = docs[1].id;
  passages[2].text = bytes("third"); passages[2].end_char = 5U;
  assert_int_equal(ytest_path_join(segments, sizeof(segments), env.tmp_root, "segments"), 0);
  assert_int_equal(ytest_path_join(dir, sizeof(dir), segments, "seg-1"), 0);
  assert_int_equal(ytest_mkdir_p(dir, 0700), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), dir, "documents.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, "seg-1", 1U, docs, 2U, passages, 3U, &descriptor), YAP_V2_OK);
  YAP_V2_manifest_init(&manifest); manifest.generation = 1U;
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &descriptor), YAP_V2_OK);
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root, "manifest.json"), 0);
  assert_int_equal(YAP_V2_manifest_save_atomic(manifest_path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest); YAP_V2_snapshot_manager_init(&manager);
  assert_int_equal(YAP_V2_snapshot_manager_open(&manager, env.tmp_root, manifest_path, &config), YAP_V2_OK);
  snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(snapshot); memset(hits, 0, sizeof(hits));
  hits[0].id = passages[0].id; hits[0].parent_document_id = docs[0].id; hits[0].lexical_score = 4.0;
  hits[1].id = passages[1].id; hits[1].parent_document_id = docs[0].id; hits[1].object_ordinal = 1U;
  hits[2].id = passages[2].id; hits[2].parent_document_id = docs[1].id;
  hits[2].object_ordinal = 2U; hits[2].vector_score = 0.8; hits[2].fused_score = 0.7;
  hits[3] = hits[2]; YAP_V2_retrieve_options_init(&options);
  options.max_passages = 3U; options.max_passages_per_document = 1U; options.max_context_bytes = 64U;
  assert_int_equal(YAP_V2_retrieve_context(snapshot, hits, 4U, &options, context, sizeof(context),
                                            &context_bytes, citations, 3U, &citation_count), YAP_V2_OK);
  assert_int_equal(citation_count, 2U); assert_int_equal(context_bytes, 12U);
  assert_memory_equal(context, "first\n\nthird", 12U);
  assert_memory_equal(citations[0].url.data, "https://e.test/a", 16U);
  assert_int_equal(citations[0].context_start, 0U); assert_int_equal(citations[1].context_start, 7U);
  assert_float_equal(citations[0].lexical_score, 4.0, 0.0);
  options.max_passages_per_document = 2U; options.max_context_bytes = 6U;
  assert_int_equal(YAP_V2_retrieve_context(snapshot, hits + 1, 2U, &options, context, sizeof(context),
                                            &context_bytes, citations, 3U, &citation_count), YAP_V2_OK);
  assert_int_equal(citation_count, 1U); assert_memory_equal(context, "second", 6U);
  assert_int_equal(YAP_V2_retrieve_context(snapshot, NULL, 0U, &options, context, sizeof(context),
                                            &context_bytes, citations, 3U, &citation_count), YAP_V2_OK);
  assert_int_equal(context_bytes, 0U); assert_int_equal(citation_count, 0U);
  hits[0].id = bytes("wrong");
  assert_int_equal(YAP_V2_retrieve_context(snapshot, hits, 1U, &options, context, sizeof(context),
                                            &context_bytes, citations, 3U, &citation_count), YAP_V2_CONFLICT);
  YAP_V2_snapshot_release(snapshot); YAP_V2_snapshot_manager_close(&manager); ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {cmocka_unit_test(test_context_limits_and_citations)};
  return cmocka_run_group_tests(tests, NULL, NULL);
}
