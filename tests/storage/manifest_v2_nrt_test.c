#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "storage/yappo_storage_v2.h"
#include "config/yappo_config_v2.h"
#include "storage/yappo_manifest_v2.h"

static void fill_segment(YAP_V2_SEGMENT_DESCRIPTOR *segment, const char *id, unsigned char seed) {
  YAP_V2_COMPONENT_DESCRIPTOR component;
  size_t i;

  memset(segment, 0, sizeof(*segment));
  strcpy(segment->id, id);
  segment->document_count = 10U;
  segment->passage_count = 20U;
  memset(&component, 0, sizeof(component));
  strcpy(component.name, "documents.yap2");
  component.file_type = YAP_V2_FILE_DOCUMENTS;
  component.record_count = 30U;
  component.file_bytes = 1234U;
  for (i = 0; i < sizeof(component.checksum); i++)
    component.checksum[i] = (unsigned char)(seed + i);
  assert_int_equal(YAP_V2_segment_descriptor_add_component(segment, &component), YAP_V2_OK);
}

static void fill_fingerprint(YAP_V2_MANIFEST *manifest, unsigned char seed) {
  size_t i;
  for (i = 0U; i < sizeof(manifest->config_fingerprint); i++) {
    manifest->config_fingerprint[i] = (unsigned char)(seed + i);
  }
}

static void fill_maximum_segment(YAP_V2_SEGMENT_DESCRIPTOR *segment, size_t ordinal) {
  YAP_V2_COMPONENT_DESCRIPTOR component;
  size_t prefix_bytes;
  size_t i;

  memset(segment, 0, sizeof(*segment));
  assert_true(snprintf(segment->id, sizeof(segment->id), "seg-%06zu-", ordinal) > 0);
  prefix_bytes = strlen(segment->id);
  memset(segment->id + prefix_bytes, 's', YAP_V2_MAX_IDENTIFIER_BYTES - prefix_bytes);
  segment->id[YAP_V2_MAX_IDENTIFIER_BYTES] = '\0';
  segment->document_count = 1U;
  segment->passage_count = 1U;
  for (i = 0U; i < YAP_V2_MAX_COMPONENTS; i++) {
    memset(&component, 0, sizeof(component));
    assert_true(snprintf(component.name, sizeof(component.name), "component-%zu-", i) > 0);
    prefix_bytes = strlen(component.name);
    memset(component.name + prefix_bytes, (int)('a' + i),
           YAP_V2_MAX_COMPONENT_NAME_BYTES - prefix_bytes);
    component.name[YAP_V2_MAX_COMPONENT_NAME_BYTES] = '\0';
    component.file_type = (uint32_t)i + 1U;
    component.record_count = 1U;
    component.file_bytes = component.file_type == YAP_V2_FILE_ANN
      ? 1U : YAP_V2_FILE_HEADER_BYTES;
    memset(component.checksum, (int)i + 1, sizeof(component.checksum));
    assert_int_equal(YAP_V2_segment_descriptor_add_component(segment, &component), YAP_V2_OK);
  }
}

static void test_manifest_roundtrip_and_atomic_publish(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  YAP_V2_MANIFEST loaded;
  YAP_V2_CONFIG config;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_FILE_HEADER header;
  char path[PATH_MAX];
  char *data = NULL;
  size_t data_size = 0U;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.yap2"), 0);

  YAP_V2_manifest_init(&manifest);
  YAP_V2_config_init(&config);
  assert_int_equal(YAP_V2_config_fingerprint(&config, manifest.config_fingerprint), YAP_V2_OK);
  fill_segment(&segment, "seg-000001", 1U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  assert_int_equal(ytest_read_file(path, &data, &data_size), 0);
  assert_true(data_size > YAP_V2_FILE_HEADER_BYTES);
  assert_int_equal(YAP_V2_file_header_decode((const unsigned char *)data, &header), YAP_V2_OK);
  assert_int_equal(header.file_type, YAP_V2_FILE_MANIFEST);
  assert_int_equal(header.payload_bytes, data_size - YAP_V2_FILE_HEADER_BYTES);
  free(data);
  data = NULL;

  YAP_V2_manifest_init(&loaded);
  assert_int_equal(YAP_V2_manifest_load(path, &loaded), YAP_V2_OK);
  assert_int_equal(loaded.generation, 1U);
  assert_int_equal(loaded.segment_count, 1U);
  assert_string_equal(loaded.segments[0].id, "seg-000001");
  assert_memory_equal(loaded.segments[0].components[0].checksum,
                      segment.components[0].checksum,
                      sizeof(segment.components[0].checksum));
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
  assert_int_equal(YAP_V2_manifest_publish_if_generation(path, 1U, &manifest), YAP_V2_CONFLICT);
  assert_int_equal(YAP_V2_manifest_load(path, &loaded), YAP_V2_OK);
  assert_int_equal(loaded.generation, 2U);
  assert_int_equal(loaded.segment_count, 2U);
  YAP_V2_manifest_free(&loaded);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

static void test_manifest_rejects_corrupt_binary(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  char path[PATH_MAX];
  char *data = NULL;
  size_t data_size = 0U;
  static const char malformed[] = "{broken";

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.yap2"), 0);
  assert_int_equal(ytest_write_file(path, malformed, sizeof(malformed) - 1U), 0);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_INVALID_FORMAT);
  fill_fingerprint(&manifest, 4U);
  fill_segment(&segment, "seg-corrupt", 7U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  assert_int_equal(ytest_read_file(path, &data, &data_size), 0);
  assert_true(data_size > YAP_V2_FILE_HEADER_BYTES);
  data[data_size - 1U] ^= 1;
  assert_int_equal(ytest_write_file(path, data, data_size), 0);
  free(data);
  data = NULL;
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_CHECKSUM_MISMATCH);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

static void test_manifest_roundtrips_maximum_segment_count(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  YAP_V2_MANIFEST loaded;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  struct stat info;
  char path[PATH_MAX];
  uint64_t expected_file_bytes;
  size_t i;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.yap2"), 0);
  YAP_V2_manifest_init(&manifest);
  fill_fingerprint(&manifest, 80U);
  for (i = 0U; i < YAP_V2_MAX_SEGMENTS; i++) {
    fill_maximum_segment(&segment, i);
    assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  }
  fill_segment(&segment, "seg-over-limit", 1U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OUT_OF_RANGE);
  assert_int_equal(YAP_V2_manifest_save_atomic(path, &manifest), YAP_V2_OK);
  assert_int_equal(stat(path, &info), 0);
  assert_true(info.st_size > 0);
  assert_true((uint64_t)info.st_size <= YAP_V2_MAX_MANIFEST_BYTES);
  expected_file_bytes = YAP_V2_FILE_HEADER_BYTES + 40U +
    (uint64_t)YAP_V2_MAX_SEGMENTS *
      (32U + YAP_V2_MAX_IDENTIFIER_BYTES +
       YAP_V2_MAX_COMPONENTS * (56U + YAP_V2_MAX_COMPONENT_NAME_BYTES));
  assert_int_equal((uint64_t)info.st_size, expected_file_bytes);
  YAP_V2_manifest_free(&manifest);

  YAP_V2_manifest_init(&loaded);
  assert_int_equal(YAP_V2_manifest_load(path, &loaded), YAP_V2_OK);
  assert_int_equal(loaded.segment_count, YAP_V2_MAX_SEGMENTS);
  assert_memory_equal(loaded.segments[0].id, "seg-000000-", 11U);
  assert_memory_equal(loaded.segments[YAP_V2_MAX_SEGMENTS - 1U].id,
                      "seg-099999-", 11U);
  assert_int_equal(strlen(loaded.segments[0].id), YAP_V2_MAX_IDENTIFIER_BYTES);
  assert_int_equal(loaded.segments[0].component_count, YAP_V2_MAX_COMPONENTS);
  assert_int_equal(strlen(loaded.segments[0].components[0].name),
                   YAP_V2_MAX_COMPONENT_NAME_BYTES);
  assert_int_equal(YAP_V2_manifest_validate(&loaded), YAP_V2_OK);
  YAP_V2_manifest_free(&loaded);
  ytest_env_destroy(&env);
}

static void test_tombstone_component_verification(void **state) {
  static const unsigned char deleted_id[] = "deleted-doc";
  ytest_env_t env;
  YAP_V2_BYTES_VIEW id;
  YAP_V2_COMPONENT_DESCRIPTOR component;
  YAP_V2_COMPONENT_DESCRIPTOR ann_component;
  YAP_V2_TOMBSTONES tombstones;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_MANIFEST manifest;
  char segments_dir[PATH_MAX], segment_dir[PATH_MAX], tombstone_path[PATH_MAX], ann_path[PATH_MAX];
  uint64_t ann_bytes;
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
  assert_int_equal(ytest_path_join(ann_path, sizeof(ann_path), segment_dir, "vectors.usearch"), 0);
  assert_int_equal(ytest_write_file(ann_path, "USearch", 7U), 0);
  memset(&ann_component, 0, sizeof(ann_component));
  strcpy(ann_component.name, "vectors.usearch"); ann_component.file_type = YAP_V2_FILE_ANN;
  ann_component.record_count = 1U;
  assert_int_equal(YAP_V2_file_sha256(ann_path, ann_component.checksum, &ann_bytes), YAP_V2_OK);
  ann_component.file_bytes = ann_bytes;
  assert_int_equal(YAP_V2_segment_descriptor_add_component(&segment, &ann_component), YAP_V2_OK);
  YAP_V2_manifest_init(&manifest);
  fill_fingerprint(&manifest, 70U);
  assert_int_equal(YAP_V2_manifest_add_segment(&manifest, &segment), YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_verify_components(env.tmp_root, &manifest), YAP_V2_OK);
  manifest.generation = 2U;
  assert_int_equal(YAP_V2_manifest_verify_components(env.tmp_root, &manifest), YAP_V2_OK);
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
    cmocka_unit_test(test_manifest_rejects_corrupt_binary),
    cmocka_unit_test(test_manifest_roundtrips_maximum_segment_count),
    cmocka_unit_test(test_tombstone_component_verification),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
