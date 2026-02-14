#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "yappo_db.h"
#include "yappo_search.h"

static int setup_cache(YAPPO_DB_FILES *db, YAPPO_CACHE *cache, unsigned int total_filenum) {
  unsigned int deletefile_size;

  memset(db, 0, sizeof(*db));
  YAP_Db_cache_init(cache);

  cache->total_filenum = total_filenum;
  deletefile_size = (cache->total_filenum / 8U) + 1U;
  cache->deletefile = (unsigned char *)calloc(deletefile_size, sizeof(unsigned char));
  if (cache->deletefile == NULL) {
    return -1;
  }
  cache->deletefile_num = deletefile_size;
  db->cache = cache;
  return 0;
}

static void mark_deleted(YAPPO_CACHE *cache, int fileindex) {
  int seek = fileindex / 8;
  int bit = fileindex % 8;
  cache->deletefile[seek] |= (unsigned char)(1U << bit);
}

static void cleanup_result(SEARCH_RESULT *result) {
  if (result == NULL) {
    return;
  }
  YAP_Search_result_free(result);
  free(result);
}

static void test_null_input(void **state) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;

  (void)state;

  memset(&db, 0, sizeof(db));
  YAP_Db_cache_init(&cache);
  assert_null(YAP_Search_result_delete(&db, NULL));
  YAP_Db_cache_destroy(&cache);
}

static void test_null_db_or_cache(void **state) {
  SEARCH_RESULT input;
  SEARCH_DOCUMENT doc;
  YAPPO_DB_FILES db;

  (void)state;

  memset(&input, 0, sizeof(input));
  memset(&doc, 0, sizeof(doc));
  memset(&db, 0, sizeof(db));

  input.keyword_docs_num = 1;
  input.docs_list = &doc;
  input.docs_list[0].fileindex = 1;
  input.docs_list[0].pos_len = 1;

  assert_null(YAP_Search_result_delete(NULL, &input));
  assert_null(YAP_Search_result_delete(&db, &input));
}

static void test_null_bitmap_is_safe(void **state) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;

  (void)state;

  memset(&input, 0, sizeof(input));
  assert_int_equal(setup_cache(&db, &cache, 16U), 0);

  free(cache.deletefile);
  cache.deletefile = NULL;
  cache.deletefile_num = 0;

  input.keyword_docs_num = 2;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  assert_non_null(input.docs_list);

  input.docs_list[0].fileindex = 1;
  input.docs_list[0].pos_len = 2;
  input.docs_list[1].fileindex = 24;
  input.docs_list[1].pos_len = 3;

  filtered = YAP_Search_result_delete(&db, &input);
  assert_non_null(filtered);
  assert_int_equal(filtered->keyword_docs_num, 1);
  assert_int_equal(filtered->docs_list[0].fileindex, 1);
  assert_int_equal(filtered->keyword_total_num, 2);

  cleanup_result(filtered);
  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
}

static void test_mixed_case(void **state) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;

  (void)state;

  memset(&input, 0, sizeof(input));
  assert_int_equal(setup_cache(&db, &cache, 16U), 0);

  mark_deleted(&cache, 3);
  mark_deleted(&cache, 9);

  input.keyword_docs_num = 8;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  assert_non_null(input.docs_list);

  input.docs_list[0].fileindex = 1;
  input.docs_list[0].pos_len = 2;
  input.docs_list[0].score = 1.0;
  input.docs_list[1].fileindex = 3;
  input.docs_list[1].pos_len = 4;
  input.docs_list[1].score = 2.0;
  input.docs_list[2].fileindex = 9;
  input.docs_list[2].pos_len = 7;
  input.docs_list[2].score = 3.0;
  input.docs_list[3].fileindex = 0;
  input.docs_list[3].pos_len = 5;
  input.docs_list[3].score = 4.0;
  input.docs_list[4].fileindex = -1;
  input.docs_list[4].pos_len = 8;
  input.docs_list[4].score = 5.0;
  input.docs_list[5].fileindex = 16;
  input.docs_list[5].pos_len = 6;
  input.docs_list[5].score = 6.0;
  input.docs_list[6].fileindex = 24;
  input.docs_list[6].pos_len = 9;
  input.docs_list[6].score = 7.0;
  input.docs_list[7].fileindex = INT_MAX;
  input.docs_list[7].pos_len = 10;
  input.docs_list[7].score = 8.0;

  filtered = YAP_Search_result_delete(&db, &input);
  assert_non_null(filtered);
  assert_int_equal(filtered->keyword_docs_num, 2);
  assert_int_equal(filtered->docs_list[0].fileindex, 1);
  assert_int_equal(filtered->docs_list[1].fileindex, 16);
  assert_int_equal(filtered->keyword_total_num, 8);

  cleanup_result(filtered);
  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
}

static void test_all_filtered_case(void **state) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;

  (void)state;

  memset(&input, 0, sizeof(input));
  assert_int_equal(setup_cache(&db, &cache, 8U), 0);

  mark_deleted(&cache, 1);

  input.keyword_docs_num = 3;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  assert_non_null(input.docs_list);

  input.docs_list[0].fileindex = 1;
  input.docs_list[1].fileindex = INT_MAX;
  input.docs_list[2].fileindex = 0;

  filtered = YAP_Search_result_delete(&db, &input);
  assert_null(filtered);

  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_null_input),
    cmocka_unit_test(test_null_db_or_cache),
    cmocka_unit_test(test_null_bitmap_is_safe),
    cmocka_unit_test(test_mixed_case),
    cmocka_unit_test(test_all_filtered_case),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
