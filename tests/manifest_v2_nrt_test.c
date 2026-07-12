#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_env.h"
#include "test_fs.h"
#include "yappo_index_v2.h"
#include "yappo_manifest_v2.h"

static void fill_segment(YAP_V2_SEGMENT_DESCRIPTOR *segment, const char *id, unsigned char seed) {
  size_t i;

  memset(segment, 0, sizeof(*segment));
  strcpy(segment->id, id);
  segment->document_count = 10U;
  segment->passage_count = 20U;
  segment->file_bytes = 1234U;
  for (i = 0; i < sizeof(segment->checksum); i++) {
    segment->checksum[i] = (unsigned char)(seed + i);
  }
}

static void test_manifest_roundtrip_and_atomic_publish(void **state) {
  ytest_env_t env;
  YAP_V2_MANIFEST manifest;
  YAP_V2_MANIFEST loaded;
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  char path[PATH_MAX];
  char *json = NULL;
  size_t json_size = 0U;

  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), env.tmp_root, "manifest.json"), 0);

  YAP_V2_manifest_init(&manifest);
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
  YAP_V2_manifest_free(&loaded);
  YAP_V2_manifest_free(&manifest);

  YAP_V2_manifest_init(&manifest);
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
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_OK);
  YAP_V2_manifest_free(&manifest);
  assert_int_equal(ytest_write_file(path, "{broken", 7U), 0);
  YAP_V2_manifest_init(&manifest);
  assert_int_equal(YAP_V2_manifest_load(path, &manifest), YAP_V2_INVALID_FORMAT);
  YAP_V2_manifest_free(&manifest);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_manifest_roundtrip_and_atomic_publish),
    cmocka_unit_test(test_manifest_rejects_malformed_json),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
