#include "v2_quality_fixture.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "test_fs.h"
#include "yappo_ann_v2.h"
#include "yappo_config_v2.h"
#include "yappo_lexical_v2.h"
#include "yappo_manifest_v2.h"
#include "yappo_metadata_v2.h"
#include "yappo_vector_v2.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FIXTURE_DOCUMENTS 9U
#define FIXTURE_DIMENSIONS 3U

static YAP_V2_BYTES_VIEW bytes(const char *value) {
  YAP_V2_BYTES_VIEW view = {(const unsigned char *)value, strlen(value)};
  return view;
}

static int add_component(YAP_V2_SEGMENT_DESCRIPTOR *segment,
                         const YAP_V2_COMPONENT_DESCRIPTOR *component) {
  return YAP_V2_segment_descriptor_add_component(segment, component);
}

int YAP_Test_v2_quality_index_create(const char *index_dir) {
  static const char *const topics[3] = {"red", "green", "blue"};
  YAP_V2_CONFIG config;
  YAP_V2_DOCUMENT_VIEW documents[FIXTURE_DOCUMENTS];
  YAP_V2_PASSAGE_VIEW passages[FIXTURE_DOCUMENTS];
  YAP_V2_COMPONENT_DESCRIPTOR lexical[3], vectors_component, ann_component;
  YAP_V2_COMPONENT_DESCRIPTOR metadata_component;
  YAP_V2_SEGMENT_DESCRIPTOR descriptor;
  YAP_V2_MANIFEST manifest;
  YAP_V2_VECTOR_SEGMENT vector_segment;
  YAP_EMBEDDING_RESULT embeddings;
  float values[FIXTURE_DOCUMENTS * FIXTURE_DIMENSIONS];
  char ids[FIXTURE_DOCUMENTS][32], passage_ids[FIXTURE_DOCUMENTS][40];
  char urls[FIXTURE_DOCUMENTS][64], titles[FIXTURE_DOCUMENTS][64];
  char bodies[FIXTURE_DOCUMENTS][96], metadata[FIXTURE_DOCUMENTS][48];
  char segments_dir[PATH_MAX], segment_dir[PATH_MAX], path[PATH_MAX];
  FILE *file = NULL;
  size_t i;
  int status = -1;

  if (index_dir == NULL ||
      ytest_path_join(path, sizeof(path), index_dir, "config.toml") != 0)
    return -1;
  file = fopen(path, "wb");
  if (file == NULL) return -1;
  if (fputs("format_version=2\n[tokenizer]\nid=\"unicode_nfkc_cf_v1\"\n"
            "[chunking]\nmax_chars=256\noverlap_chars=0\n"
            "[vector]\nenabled=true\nmodel_id=\"quality-3d-v1\"\n"
            "dimensions=3\nmetric=\"cosine\"\n"
            "[metadata]\nfilterable_fields=[\"topic\"]\n", file) < 0 ||
      fclose(file) != 0)
    return -1;
  file = NULL;
  if (YAP_V2_config_load(path, &config, NULL, 0U) != YAP_V2_OK) return -1;

  memset(documents, 0, sizeof(documents));
  memset(passages, 0, sizeof(passages));
  memset(values, 0, sizeof(values));
  for (i = 0U; i < FIXTURE_DOCUMENTS; i++) {
    size_t topic = i / 3U;
    if (snprintf(ids[i], sizeof(ids[i]), "doc-%s-%zu", topics[topic], i % 3U) < 0 ||
        snprintf(passage_ids[i], sizeof(passage_ids[i]), "passage-%s-%zu", topics[topic],
                 i % 3U) < 0 ||
        snprintf(urls[i], sizeof(urls[i]), "https://quality.test/%s/%zu", topics[topic],
                 i % 3U) < 0 ||
        snprintf(titles[i], sizeof(titles[i]), "%s reference %zu", topics[topic],
                 i % 3U) < 0 ||
        snprintf(bodies[i], sizeof(bodies[i]), "%s evidence corpus item %zu", topics[topic],
                 i % 3U) < 0 ||
        snprintf(metadata[i], sizeof(metadata[i]), "{\"topic\":\"%s\"}", topics[topic]) < 0)
      return -1;
    documents[i].id = bytes(ids[i]);
    documents[i].url = bytes(urls[i]);
    documents[i].title = bytes(titles[i]);
    documents[i].body = bytes(bodies[i]);
    documents[i].metadata_json = bytes(metadata[i]);
    passages[i].id = bytes(passage_ids[i]);
    passages[i].parent_document_id = documents[i].id;
    passages[i].text = documents[i].body;
    passages[i].end_char = (uint32_t)strlen(bodies[i]);
    values[i * FIXTURE_DIMENSIONS + topic] = 1.0f;
  }

  if (ytest_path_join(segments_dir, sizeof(segments_dir), index_dir, "segments") != 0 ||
      ytest_path_join(segment_dir, sizeof(segment_dir), segments_dir, "quality-seg-1") != 0 ||
      ytest_mkdir_p(segment_dir, 0700) != 0 ||
      ytest_path_join(path, sizeof(path), segment_dir, "documents.yap2") != 0 ||
      YAP_V2_segment_write(path, "quality-seg-1", 1U, documents, FIXTURE_DOCUMENTS,
                           passages, FIXTURE_DOCUMENTS, &descriptor) != YAP_V2_OK ||
      YAP_V2_lexical_write(segment_dir, 1U, documents, FIXTURE_DOCUMENTS, passages,
                           FIXTURE_DOCUMENTS, lexical) != YAP_V2_OK ||
      add_component(&descriptor, &lexical[0]) != YAP_V2_OK ||
      add_component(&descriptor, &lexical[1]) != YAP_V2_OK ||
      add_component(&descriptor, &lexical[2]) != YAP_V2_OK)
    return -1;

  embeddings.values = values;
  embeddings.input_count = FIXTURE_DOCUMENTS;
  embeddings.dimensions = FIXTURE_DIMENSIONS;
  if (ytest_path_join(path, sizeof(path), segment_dir, "vectors.yap2") != 0 ||
      YAP_V2_vectors_write(path, 1U, &config, passages, FIXTURE_DOCUMENTS, &embeddings,
                           &vectors_component) != YAP_V2_OK ||
      add_component(&descriptor, &vectors_component) != YAP_V2_OK)
    return -1;
  YAP_V2_vector_segment_init(&vector_segment);
  if (YAP_V2_vector_segment_open(path, 1U, &config, &vector_segment, NULL) != YAP_V2_OK)
    return -1;
  if (ytest_path_join(path, sizeof(path), segment_dir, "vectors.usearch") == 0 &&
      YAP_V2_ann_build_save(path, &vector_segment, 16U, 128U, 64U, &ann_component) ==
        YAP_ANN_OK &&
      add_component(&descriptor, &ann_component) == YAP_V2_OK)
    status = 0;
  YAP_V2_vector_segment_close(&vector_segment);
  if (status != 0) return -1;

  if (ytest_path_join(path, sizeof(path), segment_dir, "metadata.yap2") != 0 ||
      YAP_V2_metadata_write(path, 1U, &config, documents, FIXTURE_DOCUMENTS,
                            &metadata_component) != YAP_V2_OK ||
      add_component(&descriptor, &metadata_component) != YAP_V2_OK)
    return -1;

  YAP_V2_manifest_init(&manifest);
  manifest.generation = 1U;
  if (YAP_V2_config_fingerprint(&config, manifest.config_fingerprint) != YAP_V2_OK ||
      YAP_V2_manifest_add_segment(&manifest, &descriptor) != YAP_V2_OK ||
      ytest_path_join(path, sizeof(path), index_dir, "manifest.json") != 0 ||
      YAP_V2_manifest_save_atomic(path, &manifest) != YAP_V2_OK) {
    YAP_V2_manifest_free(&manifest);
    return -1;
  }
  YAP_V2_manifest_free(&manifest);
  return 0;
}
