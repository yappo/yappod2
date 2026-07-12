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
#include "yappo_index_v2.h"
#include "yappo_config_v2.h"
#include "yappo_manifest_v2.h"

static void fill_segment(YAP_V2_SEGMENT_DESCRIPTOR *segment, const char *id, unsigned char seed) {
  YAP_V2_COMPONENT_DESCRIPTOR component;
  size_t i;

  memset(segment, 0, sizeof(*segment));
  strcpy(segment->id, id);
  segment->document_count = 10U;
  segment->passage_count = 20U;
  segment->file_bytes = 1234U;
  for (i = 0; i < sizeof(segment->checksum); i++) {
    segment->checksum[i] = (unsigned char)(seed + i);
  }
  memset(&component, 0, sizeof(component));
  strcpy(component.name, "documents.yap2");
  component.file_type = YAP_V2_FILE_DOCUMENTS;
  component.record_count = 30U;
  component.file_bytes = segment->file_bytes;
  memcpy(component.checksum, segment->checksum, sizeof(component.checksum));
  assert_int_equal(YAP_V2_segment_descriptor_add_component(segment, &component), YAP_V2_OK);
}

static void fill_fingerprint(YAP_V2_MANIFEST *manifest, unsigned char seed) {
  size_t i;
  for (i = 0U; i < sizeof(manifest->config_fingerprint); i++) {
    manifest->config_fingerprint[i] = (unsigned char)(seed + i);
  }
}

static void test_manifest_roundtrip_and_atomic_publish(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  YAP_V2_MANIFEST loaded;
  YAP_V2_CONFIG config;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  char path[PATH_MAX];
  char *json = NULL;
  size_t json_size = 0U;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.json"), 0);

  YAP_V2_manifest_init(&manifest);
  YAP_V2_config_init(&config);
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  fill_segment(&segment, "seg-000001", 1U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  assert_int_equal(ytest_read_file(path, &json, &json_size), 0);
  assert_true(json_size > 0U);
  assert_null(strstr(json, ".tmp"));
  free(json);
  json = NULL;

  YAP_V2_manifest_init(&loaded);
  assert_int_equal(YAP_V2_manifest_load(path, &loaded), YAP_V2_OK);
  assert_int_equal(loaded.generation, 1U);
  assert_int_equal(loaded.segment_count, 1U);
  assert_string_equal(loaded.segments[0].id, "seg-000001");
  assert_memory_equal(loaded.segments[0].checksum, segment.checksum, sizeof(segment.checksum));
  assert_int_equal(loaded.segments[0].component_count, 1U);
  assert_string_equal(loaded.segments[0].components[0].name, "documents.yap2");
  assert_memory_equal(loaded.config_fingerprint, manifest.config_fingerprint,
                      sizeof(manifest.config_fingerprint));
  YAP_V2_manifest_free(&loaded);
  YAP_V2_manifest_init(&loaded);
  assert_int_equal(YAP_V2_manifest_load_for_config(path, &config, &loaded), YAP_V2_OK);
  YAP_V2_manifest_free(&loaded);
  config.chunk_max_chars++;
  YAP_V2_manifest_init(&loaded);
  assert_int_equal(YAP_V2_manifest_load_for_config(path, &config, &loaded), YAP_V2_CONFLICT);
  YAP_V2_manifest_free(&manifest);

  YAP_V2_manifest_init(&manifest);
  fill_fingerprint(&manifest, 50U);
  fill_segment(&segment, "seg-000001", 9U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(ytest_rm_rf(path), 0);
  assert_int_equal(YAP_V2_manifest_publish_next(path, &manifest), YAP_V2_OK);
  assert_int_equal(manifest.generation, 1U);
  fill_segment(&segment, "seg-000002", 18U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_publish_next(path, &manifest), YAP_V2_OK);
  assert_int_equal(manifest.generation, 2U);
  assert_int_equal(YAP_V2_manifest_load(path, &loaded), YAP_V2_OK);
  assert_int_equal(loaded.generation, 2U);
  assert_int_equal(loaded.segment_count, 2U);
  YAP_V2_manifest_free(&loaded);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

static void test_manifest_rejects_malformed_json(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  char path[PATH_MAX];
  static const char malformed[] = "{\"format_version\":2,\"generation\":1,\"segments\":[]}";

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.json"), 0);
  assert_int_equal(ytest_write_file(path, malformed, sizeof(malformed) - 1U), 0);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_INVALID_FORMAT);
  YAP_V2_manifest_free(&manifest);
  assert_int_equal(ytest_write_file(path, "{broken", 7U), 0);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_INVALID_FORMAT);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

static void test_tombstone_component_verification(void **state) {
  static const unsigned char deleted_id[] = "deleted-doc";
  ytest_env_t env;
  YAP_V2_BYTES_VIEW id;
  YAP_V2_COMPONENT_DESCRIPTOR component;
  YAP_V2_TOMBSTONES tombstones;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_MANIFEST manifest;
  char segments_dir[PATH_MAX], segment_dir[PATH_MAX], tombstone_path[PATH_MAX];
  FILE *file;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(segments_dir, sizeof(segments_dir), env.tmp_root, "segments"),
                   0);
  assert_int_equal(ytest_path_join(segment_dir, sizeof(segment_dir), segments_dir, "seg-000001"),
                   0);
  assert_int_equal(ytest_mkdir_p(segment_dir, 0700), 0);
  assert_int_equal(
    ytest_path_join(tombstone_path, sizeof(tombstone_path), segment_dir, "tombstones.yap2"), 0);
  id.data = deleted_id;
  id.len = sizeof(deleted_id) - 1U;
  assert_int_equal(YAP_V2_tombstones_write(tombstone_path, 1U, &id, 1U, &component), YAP_V2_OK);
  YAP_V2_tombstones_init(&tombstones);
  assert_int_equal(YAP_V2_tombstones_read(tombstone_path, 1U, &tombstones), YAP_V2_OK);
  assert_int_equal(tombstones.count, 1U);
  assert_memory_equal(tombstones.document_ids[0].data, deleted_id, sizeof(deleted_id) - 1U);
  assert_int_equal(tombstones.document_ids[0].len, sizeof(deleted_id) - 1U);
  YAP_V2_tombstones_free(&tombstones);
  assert_int_equal(YAP_V2_tombstones_read(tombstone_path, 2U, &tombstones), YAP_V2_INVALID_FORMAT);

  memset(&segment, 0, sizeof(segment));
  strcpy(segment.id, "seg-000001");
  segment.tombstone_count = 1U;
  assert_int_equal(YAP_V2_segment_descriptor_add_component(&segment, &component), YAP_V2_OK);
  YAP_V2_manifest_init(&manifest);
  fill_fingerprint(&manifest, 70U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_verify_components(env.tmp_root, &manifest), YAP_V2_OK);
  manifest.generation = 2U;
  assert_int_equal(YAP_V2_manifest_verify_components(env.tmp_root, &manifest),
                   YAP_V2_INVALID_FORMAT);
  manifest.generation = 1U;

  file = fopen(tombstone_path, "ab");
  assert_non_null(file);
  assert_int_equal(fputc('x', file), 'x');
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_manifest_verify_components(env.tmp_root, &manifest),
                   YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_manifest_roundtrip_and_atomic_publish),
    cmocka_unit_test(test_manifest_rejects_malformed_json),
    cmocka_unit_test(test_tombstone_component_verification),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
