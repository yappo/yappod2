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
#include "yappo_config_v2.h"
#include "yappo_segment_planner_v2.h"

static YAP_V2_BYTES_VIEW bytes(const char *text) {
  YAP_V2_BYTES_VIEW value = {(const unsigned char *)text, strlen(text)};
  return value;
}

static void load_config(ytest_env_t *env, YAP_V2_CONFIG *config) {
  char path[PATH_MAX];
  FILE *file;
  assert_int_equal(ytest_path_join(path, sizeof(path), env->tmp_root, "config.toml"), 0);
  file = fopen(path, "wb");
  assert_non_null(file);
  assert_true(fputs("format_version=2\n[tokenizer]\nid=\"unicode_nfkc_cf_v1\"\n"
                    "[chunking]\nmax_chars=1024\noverlap_chars=0\n"
                    "[vector]\nenabled=true\nmodel_id=\"m\"\ndimensions=3\nmetric=\"cosine\"\n"
                    "[metadata]\nfilterable_fields=[\"tags\"]\n", file) >= 0);
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_config_load(path, config, NULL, 0U), YAP_V2_OK);
}

static size_t payload_bytes(const char *path) {
  unsigned char encoded[YAP_V2_FILE_HEADER_BYTES];
  YAP_V2_FILE_HEADER header;
  FILE *file = fopen(path, "rb");
  assert_non_null(file);
  assert_int_equal(fread(encoded, 1U, sizeof(encoded), file), sizeof(encoded));
  assert_int_equal(fclose(file), 0);
  assert_int_equal(YAP_V2_file_header_decode(encoded, &header), YAP_V2_OK);
  return (size_t)header.payload_bytes;
}

static size_t largest_payload(const YAP_V2_SEGMENT_SLICE *slice, size_t *component) {
  size_t largest = 0U;
  size_t i;
  for (i = 0U; i < YAP_V2_SEGMENT_COMPONENT_COUNT; i++) {
    if (slice->payload_bytes[i] > largest) {
      largest = slice->payload_bytes[i];
      if (component != NULL) *component = i;
    }
  }
  return largest;
}

static void assert_payloads_equal(const char *directory,
                                  const YAP_V2_SEGMENT_SLICE *slice) {
  static const char *const names[YAP_V2_SEGMENT_COMPONENT_COUNT] = {
    "documents.yap2", "terms.yap2", "postings.yap2", "positions.yap2",
    "metadata.yap2", "vectors.yap2", "tombstones.yap2"
  };
  char path[PATH_MAX];
  size_t i;
  for (i = 0U; i < YAP_V2_SEGMENT_COMPONENT_COUNT; i++) {
    if (slice->payload_bytes[i] == 0U) continue;
    assert_int_equal(ytest_path_join(path, sizeof(path), directory, names[i]), 0);
    assert_int_equal(payload_bytes(path), slice->payload_bytes[i]);
  }
}

static void test_exact_payload_sizes_include_block_metadata_vector_and_tombstones(void **state) {
  enum { DOCUMENT_COUNT = 129 };
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_DOCUMENT_VIEW *documents;
  YAP_V2_PASSAGE_VIEW *passages;
  YAP_V2_SEGMENT_UNIT *units;
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_SEGMENT_PLAN plan;
  YAP_V2_BYTES_VIEW tombstones[3];
  YAP_V2_SEGMENT_UNIT delete_units[3];
  float *vectors;
  char (*document_ids)[16];
  char (*passage_ids)[16];
  char directory[PATH_MAX];
  size_t i;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  load_config(&env, &config);
  documents = calloc(DOCUMENT_COUNT, sizeof(*documents));
  passages = calloc(DOCUMENT_COUNT, sizeof(*passages));
  units = calloc(DOCUMENT_COUNT, sizeof(*units));
  vectors = calloc(DOCUMENT_COUNT * config.vector_dimensions, sizeof(*vectors));
  document_ids = calloc(DOCUMENT_COUNT, sizeof(*document_ids));
  passage_ids = calloc(DOCUMENT_COUNT, sizeof(*passage_ids));
  assert_non_null(documents); assert_non_null(passages); assert_non_null(units);
  assert_non_null(vectors); assert_non_null(document_ids); assert_non_null(passage_ids);
  for (i = 0U; i < DOCUMENT_COUNT; i++) {
    assert_true(snprintf(document_ids[i], sizeof(document_ids[i]), "doc-%03zu", i) > 0);
    assert_true(snprintf(passage_ids[i], sizeof(passage_ids[i]), "p-%03zu", i) > 0);
    documents[i].id = bytes(document_ids[i]);
    documents[i].url = bytes("https://example.test");
    documents[i].title = bytes("shared");
    documents[i].body = bytes("shared");
    documents[i].metadata_json = bytes("{\"tags\":[\"a\",\"b\"]}");
    documents[i].updated_at_unix_ms = (int64_t)i;
    passages[i].id = bytes(passage_ids[i]);
    passages[i].parent_document_id = documents[i].id;
    passages[i].text = bytes("shared");
    passages[i].end_char = 6U;
    units[i].document = &documents[i];
    units[i].passages = &passages[i];
    units[i].passage_count = 1U;
    units[i].vectors = vectors + i * config.vector_dimensions;
    vectors[i * config.vector_dimensions] = 1.0F;
  }
  YAP_V2_segment_plan_init(&plan);
  assert_int_equal(YAP_V2_segment_plan(&config, units, DOCUMENT_COUNT, strlen("seg-test"),
                                       YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES, &plan, NULL),
                   YAP_V2_OK);
  assert_int_equal(plan.count, 1U);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "seg-test"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  assert_int_equal(YAP_V2_segment_slice_write(directory, "seg-test", 1U, &config, units,
                                               &plan, plan.slices[0], &descriptor), YAP_V2_OK);
  assert_payloads_equal(directory, &plan.slices[0]);
  YAP_V2_segment_plan_free(&plan);

  memset(delete_units, 0, sizeof(delete_units));
  tombstones[0] = bytes("delete-a"); tombstones[1] = bytes("delete-b");
  tombstones[2] = bytes("delete-c");
  for (i = 0U; i < 3U; i++) delete_units[i].tombstone = tombstones[i];
  YAP_V2_segment_plan_init(&plan);
  assert_int_equal(YAP_V2_segment_plan(&config, delete_units, 3U, strlen("seg-delete"),
                                       YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES, &plan, NULL),
                   YAP_V2_OK);
  assert_int_equal(ytest_path_join(directory, sizeof(directory), env.tmp_root, "seg-delete"), 0);
  assert_int_equal(ytest_mkdir_p(directory, 0700), 0);
  assert_int_equal(YAP_V2_segment_slice_write(directory, "seg-delete", 2U, &config,
                                               delete_units, &plan, plan.slices[0], &descriptor),
                   YAP_V2_OK);
  assert_payloads_equal(directory, &plan.slices[0]);
  YAP_V2_segment_plan_free(&plan);
  free(documents); free(passages); free(units); free(vectors);
  free(document_ids); free(passage_ids);
  ytest_env_destroy(&env);
}

static void test_greedy_prefix_and_single_document_capacity_error(void **state) {
  ytest_env_t env;
  YAP_V2_CONFIG config;
  YAP_V2_DOCUMENT_VIEW documents[3];
  YAP_V2_PASSAGE_VIEW passages[3];
  YAP_V2_SEGMENT_UNIT units[3];
  YAP_V2_SEGMENT_PLAN one, two, split;
  YAP_V2_SEGMENT_CAPACITY_ERROR error;
  float vectors[9] = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  const char *ids[] = {"doc-a", "doc-b", "doc-c"};
  const char *passage_ids[] = {"passage-a", "passage-b", "passage-c"};
  static const char *const component_names[YAP_V2_SEGMENT_COMPONENT_COUNT] = {
    "documents.yap2", "terms.yap2", "postings.yap2", "positions.yap2",
    "metadata.yap2", "vectors.yap2", "tombstones.yap2"
  };
  size_t i, component = 0U, limit;
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  load_config(&env, &config);
  memset(documents, 0, sizeof(documents)); memset(passages, 0, sizeof(passages));
  memset(units, 0, sizeof(units));
  for (i = 0U; i < 3U; i++) {
    documents[i].id = bytes(ids[i]); documents[i].url = bytes("");
    documents[i].title = bytes("term"); documents[i].body = bytes("term");
    documents[i].metadata_json = bytes("{\"tags\":[\"a\",\"b\"]}");
    passages[i].id = bytes(passage_ids[i]); passages[i].parent_document_id = documents[i].id;
    passages[i].text = bytes("term"); passages[i].end_char = 4U;
    units[i].document = &documents[i]; units[i].passages = &passages[i];
    units[i].passage_count = 1U; units[i].vectors = vectors + i * 3U;
  }
  YAP_V2_segment_plan_init(&one); YAP_V2_segment_plan_init(&two);
  YAP_V2_segment_plan_init(&split);
  assert_int_equal(YAP_V2_segment_plan(&config, units, 1U, 31U,
                                       YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES, &one, NULL),
                   YAP_V2_OK);
  assert_int_equal(YAP_V2_segment_plan(&config, units, 2U, 31U,
                                       YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES, &two, NULL),
                   YAP_V2_OK);
  limit = largest_payload(&two.slices[0], NULL);
  assert_int_equal(YAP_V2_segment_plan(&config, units, 3U, 31U, limit, &split, NULL),
                   YAP_V2_OK);
  assert_int_equal(split.count, 2U);
  assert_int_equal(split.slices[0].first, 0U); assert_int_equal(split.slices[0].count, 2U);
  assert_int_equal(split.slices[1].first, 2U); assert_int_equal(split.slices[1].count, 1U);
  assert_int_equal(YAP_V2_segment_plan_bisect(&split, 0U), YAP_V2_OK);
  assert_int_equal(split.count, 3U);
  for (i = 0U; i < 3U; i++) {
    assert_int_equal(split.slices[i].first, i);
    assert_int_equal(split.slices[i].count, 1U);
  }
  YAP_V2_segment_plan_free(&split);
  limit = largest_payload(&one.slices[0], &component) - 1U;
  assert_int_equal(YAP_V2_segment_plan(&config, units, 1U, 31U, limit, &split, &error),
                   YAP_V2_SEGMENT_CAPACITY_EXCEEDED);
  assert_memory_equal(error.document_id.data, "doc-a", error.document_id.len);
  assert_string_equal(error.component, component_names[component]);
  assert_true(error.required_bytes > error.limit_bytes);
  YAP_V2_segment_plan_free(&one); YAP_V2_segment_plan_free(&two);
  assert_int_equal(YAP_V2_segment_count_validate(YAP_V2_MAX_SEGMENTS - 1U, 1U), YAP_V2_OK);
  assert_int_equal(YAP_V2_segment_count_validate(YAP_V2_MAX_SEGMENTS - 1U, 2U),
                   YAP_V2_OUT_OF_RANGE);
  assert_int_equal(YAP_V2_segment_count_validate(YAP_V2_MAX_SEGMENTS + 1U, 0U),
                   YAP_V2_OUT_OF_RANGE);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_exact_payload_sizes_include_block_metadata_vector_and_tombstones),
    cmocka_unit_test(test_greedy_prefix_and_single_document_capacity_error)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
