#ifndef YAPPO_CONFIG_V2_H
#define YAPPO_CONFIG_V2_H

#include "yappo_index_v2.h"

#include <stddef.h>

#define YAP_V2_CONFIG_FINGERPRINT_BYTES 32U

void YAP_V2_config_init(YAP_V2_CONFIG *config);
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
