#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_manifest_v2.h"
#include "yappo_snapshot_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)};
  return view;
}

static YAP_V2_DOCUMENT_VIEW document(const char *id, const char *title) {
  YAP_V2_DOCUMENT_VIEW value;
  memset(&value, 0, sizeof(value));
  value.id = bytes(id); value.url = bytes("https://example.test/doc");
  value.title = bytes(title); value.body = bytes("body"); value.metadata_json = bytes("{}");
  return value;
}

static void write_segment(const char *root, const char *id, uint64_t generation,
                          const YAP_V2_DOCUMENT_VIEW *documents, size_t document_count,
                          const char *deleted_id, YAP_V2_SEGMENT_DESCRIPTOR *descriptor) {
  char dir[PATH_MAX], path[PATH_MAX];
  assert_int_equal(ytest_path_join(dir, sizeof(dir), root, "segments"), 0);
  assert_int_equal(ytest_path_join(dir, sizeof(dir), dir, id), 0);
  assert_int_equal(ytest_mkdir_p(dir, 0700), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), dir, "documents.yap2"), 0);
  assert_int_equal(YAP_V2_segment_write(path, id, generation, documents, document_count,
                                        NULL, 0U, descriptor), YAP_V2_OK);
  if (deleted_id != NULL) {
    YAP_V2_COMPONENT_DESCRIPTOR tombstone;
    YAP_V2_BYTES_VIEW deleted = bytes(deleted_id);
    assert_int_equal(ytest_path_join(path, sizeof(path), dir, "tombstones.yap2"), 0);
    assert_int_equal(YAP_V2_tombstones_write(path, generation, &deleted, 1U, &tombstone),
                     YAP_V2_OK);
    assert_int_equal(YAP_V2_segment_descriptor_add_component(descriptor, &tombstone), YAP_V2_OK);
    descriptor->tombstone_count = 1U;
  }
}

static void publish(const char *path, const YAP_V2_CONFIG *config, uint64_t generation,
                    const YAP_V2_SEGMENT_DESCRIPTOR *segments, size_t segment_count) {
  YAP_V2_MANIFEST manifest;
  size_t i;
  YAP_V2_manifest_init(&manifest); manifest.generation = generation;
  assert_int_equal(YAP_V2_config_fingerprint(config, manifest.config_fingerprint), YAP_V2_OK);
  for (i = 0U; i < segment_count; i++)
    assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segments[i]), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
}

static void test_reload_latest_wins_and_snapshot_lifetime(void **state) {
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *old_snapshot, *new_snapshot;
  YAP_V2_SEGMENT_DESCRIPTOR first[2], second[2];
  YAP_V2_DOCUMENT_VIEW old_documents[2] = {document("same", "old"),
                                           document("deleted", "visible")};
  YAP_V2_DOCUMENT_VIEW new_document = document("same", "new");
  YAP_V2_DOCUMENT_HIT hit;
  char manifest_path[PATH_MAX];
  int changed;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root,
                                   "manifest.json"), 0);
  YAP_V2_config_init(&config);
  write_segment(env.tmp_root, "seg-old", 1U, old_documents, 2U, NULL, &first[0]);
  publish(manifest_path, &config, 1U, first, 1U);
  YAP_V2_snapshot_manager_init(&manager);
  assert_int_equal(YAP_V2_snapshot_manager_open(&manager, env.tmp_root, manifest_path, &config),
                   YAP_V2_OK);
  old_snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(old_snapshot);
  assert_int_equal(YAP_V2_snapshot_generation(old_snapshot), 1U);
  assert_int_equal(YAP_V2_snapshot_lookup_document(old_snapshot, bytes("same"), &hit), YAP_V2_OK);
  assert_memory_equal(hit.document->title.data, "old", 3U);

  write_segment(env.tmp_root, "seg-old", 2U, old_documents, 2U, NULL, &second[0]);
  write_segment(env.tmp_root, "seg-new", 2U, &new_document, 1U, "deleted", &second[1]);
  publish(manifest_path, &config, 2U, second, 2U);
  assert_int_equal(YAP_V2_snapshot_manager_reload(&manager, &changed), YAP_V2_OK);
  assert_int_equal(changed, 1);
  new_snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(new_snapshot);
  assert_int_equal(YAP_V2_snapshot_generation(new_snapshot), 2U);
  assert_int_equal(YAP_V2_snapshot_segment_count(new_snapshot), 2U);
  assert_int_equal(YAP_V2_snapshot_lookup_document(new_snapshot, bytes("same"), &hit), YAP_V2_OK);
  assert_memory_equal(hit.document->title.data, "new", 3U);
  assert_int_equal(hit.segment_ordinal, 1U);
  assert_false(YAP_V2_snapshot_document_visible(new_snapshot, 0U, bytes("same")));
  assert_true(YAP_V2_snapshot_document_visible(new_snapshot, 1U, bytes("same")));
  assert_int_equal(YAP_V2_snapshot_lookup_document(new_snapshot, bytes("deleted"), &hit),
                   YAP_V2_NOT_FOUND);
  assert_false(YAP_V2_snapshot_document_visible(new_snapshot, 0U, bytes("deleted")));
  assert_int_equal(YAP_V2_snapshot_lookup_document(old_snapshot, bytes("same"), &hit), YAP_V2_OK);
  assert_memory_equal(hit.document->title.data, "old", 3U);
  YAP_V2_snapshot_release(new_snapshot); YAP_V2_snapshot_release(old_snapshot);
  assert_int_equal(YAP_V2_snapshot_manager_reload(&manager, &changed), YAP_V2_OK);
  assert_int_equal(changed, 0);
  YAP_V2_snapshot_manager_close(&manager); ytest_env_destroy(&env);
}

static void test_failed_reload_keeps_current(void **state) {
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_SNAPSHOT_MANAGER manager;
  YAP_V2_SEARCH_SNAPSHOT *snapshot;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_DOCUMENT_VIEW doc = document("doc", "stable");
  char manifest_path[PATH_MAX];
  int changed;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(manifest_path, sizeof(manifest_path), env.tmp_root,
                                   "manifest.json"), 0);
  YAP_V2_config_init(&config);
  write_segment(env.tmp_root, "seg", 1U, &doc, 1U, NULL, &segment);
  publish(manifest_path, &config, 1U, &segment, 1U);
  YAP_V2_snapshot_manager_init(&manager);
  assert_int_equal(YAP_V2_snapshot_manager_open(&manager, env.tmp_root, manifest_path, &config),
                   YAP_V2_OK);
  assert_int_equal(ytest_write_file(manifest_path, "{broken", 7U), 0);
  assert_int_equal(YAP_V2_snapshot_manager_reload(&manager, &changed), YAP_V2_INVALID_FORMAT);
  assert_int_equal(changed, 0);
  snapshot = YAP_V2_snapshot_acquire(&manager); assert_non_null(snapshot);
  assert_int_equal(YAP_V2_snapshot_generation(snapshot), 1U);
  YAP_V2_snapshot_release(snapshot); YAP_V2_snapshot_manager_close(&manager);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_reload_latest_wins_and_snapshot_lifetime),
    cmocka_unit_test(test_failed_reload_keeps_current)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
