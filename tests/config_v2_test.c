#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_config_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_config(const char *path, const char *content) {
  FILE *file = fopen(path, "w");
  assert_non_null(file);
  assert_true(fputs(content, file) >= 0);
  assert_int_equal(fclose(file), 0);
}

static const char *valid_config =
    "format_version = 2\n[tokenizer]\nid = \"unicode_nfkc_casefold_v2\"\n"
    "[chunking]\nmax_chars = 800\noverlap_chars = 100\n"
    "[vector]\nenabled = true\nmodel_id = \"embed-v1\"\ndimensions = 768\nmetric = \"cosine\"\n";

static void test_load_and_fingerprint(void **state) {
  char path[] = "/tmp/yappod-config-XXXXXX";
  char error[256];
  char hex1[65], hex2[65];
  unsigned char first[32], second[32];
  YAP_V2_CONFIG config;
  int fd;
  (void)state;
  fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  write_config(path, valid_config);
  assert_int_equal(YAP_V2_config_load(path, &config, error, sizeof(error)), YAP_V2_OK);
  assert_string_equal(config.vector_model_id, "embed-v1");
  assert_int_equal(config.vector_dimensions, 768);
  assert_int_equal(YAP_V2_config_fingerprint(&config, first), YAP_V2_OK);
  assert_int_equal(YAP_V2_config_fingerprint(&config, second), YAP_V2_OK);
  assert_memory_equal(first, second, sizeof(first));
  YAP_V2_config_fingerprint_hex(first, hex1); YAP_V2_config_fingerprint_hex(second, hex2);
  assert_int_equal(strlen(hex1), 64); assert_string_equal(hex1, hex2);
  assert_string_equal(hex1, "ebe3216b1958354a6823506648fe91ee25cc6abfd082301886b92251322bfc02");
  assert_int_equal(unlink(path), 0);
}

static void test_defaults(void **state) {
  char path[] = "/tmp/yappod-config-XXXXXX";
  char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state;
  fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path, "format_version=2\n[tokenizer]\n[chunking]\n[vector]\nenabled=false\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_OK);
  assert_string_equal(config.tokenizer_id,"unicode_nfkc_casefold_v2");
  assert_int_equal(config.chunk_max_chars,1200); assert_int_equal(config.chunk_overlap_chars,200);
  assert_int_equal(config.vector_metric,YAP_V2_VECTOR_DISABLED);
  assert_int_equal(config.filterable_field_count,0);
  assert_int_equal(unlink(path),0);
}

static void test_filterable_fields_are_canonical(void **state) {
  char path[]="/tmp/yappod-config-XXXXXX"; char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state; fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path,"format_version=2\n[tokenizer]\n[chunking]\n[vector]\nenabled=false\n"
                    "[metadata]\nfilterable_fields=[\"year\",\"author.name\",\"lang\"]\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_OK);
  assert_int_equal(config.filterable_field_count,3);
  assert_string_equal(config.filterable_fields[0],"author.name");
  assert_string_equal(config.filterable_fields[1],"lang");
  assert_string_equal(config.filterable_fields[2],"year");
  assert_int_equal(unlink(path),0);
}

static void test_rejects_duplicate_filterable_field(void **state) {
  char path[]="/tmp/yappod-config-XXXXXX"; char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state; fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path,"format_version=2\n[tokenizer]\n[chunking]\n[vector]\nenabled=false\n"
                    "[metadata]\nfilterable_fields=[\"lang\",\"lang\"]\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_DUPLICATE);
  assert_int_equal(unlink(path),0);
}

static void test_rejects_unknown_key(void **state) {
  char path[]="/tmp/yappod-config-XXXXXX"; char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state; fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path,"format_version=2\nsurprise=true\n[tokenizer]\n[chunking]\n[vector]\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
  assert_non_null(strstr(error,"unknown")); assert_int_equal(unlink(path),0);
}

static void test_rejects_unknown_nested_key(void **state) {
  char path[]="/tmp/yappod-config-XXXXXX"; char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state; fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path,"format_version=2\n[tokenizer]\nmisspelled=1\n[chunking]\n[vector]\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
  assert_non_null(strstr(error,"unknown")); assert_int_equal(unlink(path),0);
}

static void test_rejects_invalid_disabled_vector(void **state) {
  char path[]="/tmp/yappod-config-XXXXXX"; char error[256]; YAP_V2_CONFIG config; int fd;
  (void)state; fd=mkstemp(path); assert_true(fd>=0); assert_int_equal(close(fd),0);
  write_config(path,"format_version=2\n[tokenizer]\n[chunking]\n[vector]\nenabled=false\nmodel_id=\"bad\"\n");
  assert_int_equal(YAP_V2_config_load(path,&config,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
  assert_int_equal(unlink(path),0);
}

static void test_save_round_trips_escaped_strings(void **state) {
  char path[] = "/tmp/yappod-config-save-XXXXXX";
  char error[256] = {0};
  YAP_V2_CONFIG source, loaded;
  int fd;
  (void)state;
  fd = mkstemp(path); assert_true(fd >= 0); assert_int_equal(close(fd), 0);
  assert_int_equal(unlink(path), 0);
  YAP_V2_config_init(&source);
  assert_true(snprintf(source.tokenizer_id, sizeof(source.tokenizer_id), "token\\\"id") > 0);
  source.filterable_field_count = 1U;
  assert_true(snprintf(source.filterable_fields[0], sizeof(source.filterable_fields[0]),
                       "field\"name") > 0);
  assert_int_equal(YAP_V2_config_save(path, &source, error, sizeof(error)), YAP_V2_OK);
  assert_int_equal(YAP_V2_config_load(path, &loaded, error, sizeof(error)), YAP_V2_OK);
  assert_string_equal(loaded.tokenizer_id, source.tokenizer_id);
  assert_string_equal(loaded.filterable_fields[0], source.filterable_fields[0]);
  assert_int_equal(unlink(path), 0);
}

int main(void) {
  const struct CMUnitTest tests[]={cmocka_unit_test(test_load_and_fingerprint),cmocka_unit_test(test_defaults),cmocka_unit_test(test_filterable_fields_are_canonical),cmocka_unit_test(test_rejects_duplicate_filterable_field),cmocka_unit_test(test_rejects_unknown_key),cmocka_unit_test(test_rejects_unknown_nested_key),cmocka_unit_test(test_rejects_invalid_disabled_vector),cmocka_unit_test(test_save_round_trips_escaped_strings)};
  return cmocka_run_group_tests(tests,NULL,NULL);
}
