#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "yappo_ingest.h"

#include <stdio.h>
#include <string.h>

static void test_upsert_and_metadata_are_canonical(void **state) {
  const char line[]="{\"metadata\":{\"z\":1,\"a\":{\"y\":2,\"b\":1}},\"body\":\"Text\",\"id\":\"doc-1\",\"operation\":\"upsert\"}";
  YAP_V2_INGEST_OPERATION operation; char error[128];
  (void)state;
  assert_int_equal(YAP_V2_ingest_parse_ndjson(line,strlen(line),&operation,error,sizeof(error)),YAP_V2_OK);
  assert_int_equal(operation.kind,YAP_V2_INGEST_UPSERT); assert_string_equal(operation.id,"doc-1");
  assert_string_equal(operation.body,"Text"); assert_string_equal(operation.metadata_json,"{\"a\":{\"b\":1,\"y\":2},\"z\":1}");
  assert_string_equal(operation.url,""); assert_string_equal(operation.title,"");
  YAP_V2_ingest_operation_free(&operation);
}

static void test_delete_is_strict(void **state) {
  const char good[]="{\"operation\":\"delete\",\"id\":\"doc-1\"}";
  const char bad[]="{\"operation\":\"delete\",\"id\":\"doc-1\",\"body\":\"x\"}";
  YAP_V2_INGEST_OPERATION operation; char error[128]; (void)state;
  assert_int_equal(YAP_V2_ingest_parse_ndjson(good,strlen(good),&operation,error,sizeof(error)),YAP_V2_OK);
  YAP_V2_ingest_operation_free(&operation);
  assert_int_equal(YAP_V2_ingest_parse_ndjson(bad,strlen(bad),&operation,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
}

static void test_unknown_key_is_rejected(void **state) {
  const char line[]="{\"operation\":\"upsert\",\"id\":\"x\",\"body\":\"x\",\"typo\":1}";
  YAP_V2_INGEST_OPERATION operation; char error[128]; (void)state;
  assert_int_equal(YAP_V2_ingest_parse_ndjson(line,strlen(line),&operation,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
}

static void test_duplicate_metadata_key_is_rejected(void **state) {
  const char line[]="{\"operation\":\"upsert\",\"id\":\"x\",\"body\":\"x\",\"metadata\":{\"a\":1,\"a\":2}}";
  YAP_V2_INGEST_OPERATION operation; char error[128]; (void)state;
  assert_int_equal(YAP_V2_ingest_parse_ndjson(line,strlen(line),&operation,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
}

static void test_precomputed_passage_vectors(void **state) {
  const char good[]="{\"operation\":\"upsert\",\"id\":\"doc\",\"body\":\"body\",\"vectors\":[[1,0],[0.5,-0.25]]}";
  const char ragged[]="{\"operation\":\"upsert\",\"id\":\"doc\",\"body\":\"body\",\"vectors\":[[1,0],[1]]}";
  YAP_V2_INGEST_OPERATION operation; char error[128]; (void)state;
  assert_int_equal(YAP_V2_ingest_parse_ndjson(good,strlen(good),&operation,error,sizeof(error)),YAP_V2_OK);
  assert_int_equal(operation.vector_count,2U);assert_int_equal(operation.vector_dimensions,2U);
  assert_float_equal(operation.vectors[0],1.0f,0.0001f);
  assert_float_equal(operation.vectors[1],0.0f,0.0001f);
  assert_float_equal(operation.vectors[2],0.5f,0.0001f);
  assert_float_equal(operation.vectors[3],-0.25f,0.0001f);YAP_V2_ingest_operation_free(&operation);
  assert_int_equal(YAP_V2_ingest_parse_ndjson(ragged,strlen(ragged),&operation,error,sizeof(error)),YAP_V2_INVALID_FORMAT);
}

static void test_tsv_adapter(void **state) {
  char line[]="https://example.test/\tADD\tTitle\t4\tBody\n";
  YAP_V2_INGEST_OPERATION operation; char error[128]; (void)state;
  assert_int_equal(YAP_V2_ingest_parse_tsv(line,&operation,error,sizeof(error)),YAP_V2_OK);
  assert_string_equal(operation.id,"https://example.test/"); assert_string_equal(operation.title,"Title");
  assert_string_equal(operation.body,"Body"); YAP_V2_ingest_operation_free(&operation);
}

static void test_url_has_a_separate_limit_and_reports_oversize(void **state) {
  char url[YAP_V2_MAX_URL_BYTES + 2U];
  char line[YAP_V2_MAX_URL_BYTES + 80U];
  char error[128];
  YAP_V2_INGEST_OPERATION operation;
  int length;
  (void)state;

  memset(url, 'u', sizeof(url) - 1U);
  url[316] = '\0';
  length = snprintf(line, sizeof(line),
                    "{\"operation\":\"upsert\",\"id\":\"doc\",\"url\":\"%s\",\"body\":\"body\"}", url);
  assert_true(length > 0);
  assert_int_equal(YAP_V2_ingest_parse_ndjson(line, (size_t)length, &operation,
                                               error, sizeof(error)), YAP_V2_OK);
  YAP_V2_ingest_operation_free(&operation);

  memset(url, 'u', YAP_V2_MAX_URL_BYTES + 1U);
  url[YAP_V2_MAX_URL_BYTES + 1U] = '\0';
  length = snprintf(line, sizeof(line),
                    "{\"operation\":\"upsert\",\"id\":\"doc\",\"url\":\"%s\",\"body\":\"body\"}", url);
  assert_true(length > 0);
  assert_int_equal(YAP_V2_ingest_parse_ndjson(line, (size_t)length, &operation,
                                               error, sizeof(error)), YAP_V2_OUT_OF_RANGE);
  assert_string_equal(error, "url exceeds maximum of 8192 bytes (got 8193)");
}

int main(void) {
  const struct CMUnitTest tests[]={cmocka_unit_test(test_upsert_and_metadata_are_canonical),cmocka_unit_test(test_delete_is_strict),cmocka_unit_test(test_unknown_key_is_rejected),cmocka_unit_test(test_duplicate_metadata_key_is_rejected),cmocka_unit_test(test_precomputed_passage_vectors),cmocka_unit_test(test_tsv_adapter),cmocka_unit_test(test_url_has_a_separate_limit_and_reports_oversize)};
  return cmocka_run_group_tests(tests,NULL,NULL);
}
