#ifndef YAPPO_CONFIG_V2_H
#define YAPPO_CONFIG_V2_H

#include "common/yappo_types_v2.h"

#include <stddef.h>

#define YAP_V2_CONFIG_FINGERPRINT_BYTES 32U
#define YAP_V2_MAX_MODEL_ID_BYTES 255U
#define YAP_V2_MAX_VECTOR_DIMENSIONS 65536U
#define YAP_V2_MAX_FILTER_FIELDS 64U
#define YAP_V2_MAX_FILTER_FIELD_BYTES 127U

typedef enum {
  YAP_V2_VECTOR_DISABLED = 0,
  YAP_V2_VECTOR_COSINE = 1,
  YAP_V2_VECTOR_DOT = 2,
  YAP_V2_VECTOR_L2 = 3
} YAP_V2_VECTOR_METRIC;

typedef struct {
  uint32_t format_version;
  char tokenizer_id[YAP_V2_MAX_IDENTIFIER_BYTES + 1U];
  uint32_t chunk_max_chars;
  uint32_t chunk_overlap_chars;
  char vector_model_id[YAP_V2_MAX_MODEL_ID_BYTES + 1U];
  uint32_t vector_dimensions;
  YAP_V2_VECTOR_METRIC vector_metric;
  char filterable_fields[YAP_V2_MAX_FILTER_FIELDS][YAP_V2_MAX_FILTER_FIELD_BYTES + 1U];
  size_t filterable_field_count;
} YAP_V2_CONFIG;

void YAP_V2_config_init(YAP_V2_CONFIG *config);
int YAP_V2_config_validate(const YAP_V2_CONFIG *config);
int YAP_V2_config_load(const char *path, YAP_V2_CONFIG *config, char *error, size_t error_size);
int YAP_V2_config_save(const char *path, const YAP_V2_CONFIG *config,
                       char *error, size_t error_size);
int YAP_V2_config_fingerprint(const YAP_V2_CONFIG *config,
                              unsigned char output[YAP_V2_CONFIG_FINGERPRINT_BYTES]);
void YAP_V2_config_fingerprint_hex(
    const unsigned char fingerprint[YAP_V2_CONFIG_FINGERPRINT_BYTES],
    char output[YAP_V2_CONFIG_FINGERPRINT_BYTES * 2U + 1U]);
void YAP_V2_sha256_bytes(const unsigned char *data, size_t length,
                         unsigned char output[YAP_V2_CONFIG_FINGERPRINT_BYTES]);

#endif
