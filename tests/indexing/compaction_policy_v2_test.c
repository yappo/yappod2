#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "indexing/yappo_compact_v2.h"

static void add_segment(YAP_V2_MANIFEST *manifest, const char *id,
                        uint64_t file_bytes) {
  YAP_V2_SEGMENT_DESCRIPTOR segment;
  YAP_V2_COMPONENT_DESCRIPTOR component;
  memset(&segment, 0, sizeof(segment));
  memset(&component, 0, sizeof(component));
  assert_true(snprintf(segment.id, sizeof(segment.id), "%s", id) > 0);
  assert_true(snprintf(component.name, sizeof(component.name),
                       "documents.bin") > 0);
  component.file_type = YAP_V2_FILE_DOCUMENTS;
  component.file_bytes = file_bytes;
  assert_int_equal(
    YAP_V2_segment_descriptor_add_component(&segment, &component),
    YAP_V2_OK);
  assert_int_equal(YAP_V2_manifest_add_segment(manifest, &segment),
                   YAP_V2_OK);
}

static void test_triggers_only_at_small_segment_threshold(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_COMPACTION_POLICY policy;
  int needed = -1;
  size_t small = 0U;
  (void)state;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_compaction_policy_init(&policy);
  add_segment(&manifest, "seg-1", 1024U);
  add_segment(&manifest, "seg-2", 2048U);
  add_segment(&manifest, "seg-3", 4096U);
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_false(needed);
  assert_int_equal(small, 3U);
  add_segment(&manifest, "seg-4", 8192U);
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_true(needed);
  assert_int_equal(small, 4U);
  {
    YAP_V2_COMPONENT_DESCRIPTOR component;
    memset(&component, 0, sizeof(component));
    assert_true(snprintf(component.name, sizeof(component.name),
                         "vectors.yap2") > 0);
    component.file_type = YAP_V2_FILE_VECTORS;
    component.file_bytes = policy.small_segment_bytes;
    assert_int_equal(YAP_V2_segment_descriptor_add_component(
                       &manifest.segments[1], &component), YAP_V2_OK);
  }
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_false(needed);
  assert_int_equal(small, 2U);
  YAP_V2_manifest_free(&manifest);
}

static void test_ignores_healthy_and_disabled_segments(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_COMPACTION_POLICY policy;
  int needed = -1;
  size_t small = 0U;
  (void)state;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_compaction_policy_init(&policy);
  add_segment(&manifest, "seg-1", policy.small_segment_bytes);
  add_segment(&manifest, "seg-2", policy.small_segment_bytes + 1U);
  add_segment(&manifest, "seg-3", policy.small_segment_bytes * 2U);
  add_segment(&manifest, "seg-4", policy.small_segment_bytes * 8U);
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_false(needed);
  assert_int_equal(small, 0U);
  policy.small_segment_bytes *= 4U;
  policy.enabled = 0;
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_false(needed);
  assert_int_equal(small, 3U);
  YAP_V2_manifest_free(&manifest);
}

static void test_triggers_for_a_full_non_small_size_tier(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_COMPACTION_POLICY policy;
  int needed = -1;
  size_t small = 99U;
  (void)state;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_compaction_policy_init(&policy);
  add_segment(&manifest, "seg-1", policy.small_segment_bytes);
  add_segment(&manifest, "seg-2", policy.small_segment_bytes + 1024U);
  add_segment(&manifest, "seg-3", policy.small_segment_bytes * 2U);
  add_segment(&manifest, "seg-4", policy.small_segment_bytes * 3U);
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_true(needed);
  assert_int_equal(small, 0U);
  YAP_V2_manifest_free(&manifest);
}

static void test_does_not_merge_across_size_tiers(void **state) {
  YAP_V2_MANIFEST manifest;
  YAP_V2_COMPACTION_POLICY policy;
  int needed = -1;
  size_t small = 0U;
  (void)state;
  YAP_V2_manifest_init(&manifest);
  YAP_V2_compaction_policy_init(&policy);
  add_segment(&manifest, "tiny-1", 1024U);
  add_segment(&manifest, "large-1", policy.small_segment_bytes);
  add_segment(&manifest, "tiny-2", 1024U);
  add_segment(&manifest, "large-2", policy.small_segment_bytes);
  add_segment(&manifest, "tiny-3", 1024U);
  add_segment(&manifest, "large-3", policy.small_segment_bytes);
  add_segment(&manifest, "tiny-4", 1024U);
  add_segment(&manifest, "large-4", policy.small_segment_bytes);
  assert_int_equal(YAP_V2_manifest_needs_compaction(
                     &manifest, &policy, &needed, &small), YAP_V2_OK);
  assert_false(needed);
  assert_int_equal(small, 1U);
  YAP_V2_manifest_free(&manifest);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_triggers_only_at_small_segment_threshold),
    cmocka_unit_test(test_ignores_healthy_and_disabled_segments),
    cmocka_unit_test(test_triggers_for_a_full_non_small_size_tier),
    cmocka_unit_test(test_does_not_merge_across_size_tiers),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
