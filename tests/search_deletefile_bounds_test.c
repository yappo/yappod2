#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yappo_db.h"
#include "yappo_search.h"

static int fail(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  return 1;
}

static int setup_cache(YAPPO_DB_FILES *db, YAPPO_CACHE *cache, unsigned int total_filenum) {
  unsigned int deletefile_size;

  memset(db, 0, sizeof(*db));
  YAP_Db_cache_init(cache);

  cache->total_filenum = total_filenum;
  deletefile_size = (cache->total_filenum / 8U) + 1U;
  cache->deletefile = (unsigned char *)calloc(deletefile_size, sizeof(unsigned char));
  if (cache->deletefile == NULL) {
    return fail("failed to allocate deletefile bitmap");
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

static int run_mixed_case(void) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;
  int rc;

  memset(&input, 0, sizeof(input));
  rc = setup_cache(&db, &cache, 16U);
  if (rc != 0) {
    YAP_Db_cache_destroy(&cache);
    return rc;
  }

  mark_deleted(&cache, 3);
  mark_deleted(&cache, 9);

  input.keyword_docs_num = 8;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  if (input.docs_list == NULL) {
    YAP_Db_cache_destroy(&cache);
    return fail("failed to allocate docs list");
  }

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
  if (filtered == NULL) {
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected NULL result");
  }
  if (filtered->keyword_docs_num != 2) {
    cleanup_result(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("expected exactly two surviving documents");
  }
  if (filtered->docs_list[0].fileindex != 1) {
    cleanup_result(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected surviving fileindex");
  }
  if (filtered->docs_list[1].fileindex != 16) {
    cleanup_result(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected boundary surviving fileindex");
  }
  if (filtered->keyword_total_num != 8) {
    cleanup_result(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected keyword_total_num");
  }

  cleanup_result(filtered);
  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
  return 0;
}

static int run_all_filtered_case(void) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;
  int rc;

  memset(&input, 0, sizeof(input));
  rc = setup_cache(&db, &cache, 8U);
  if (rc != 0) {
    YAP_Db_cache_destroy(&cache);
    return rc;
  }

  mark_deleted(&cache, 1);

  input.keyword_docs_num = 3;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  if (input.docs_list == NULL) {
    YAP_Db_cache_destroy(&cache);
    return fail("failed to allocate docs list");
  }

  input.docs_list[0].fileindex = 1;
  input.docs_list[1].fileindex = INT_MAX;
  input.docs_list[2].fileindex = 0;

  filtered = YAP_Search_result_delete(&db, &input);
  if (filtered != NULL) {
    cleanup_result(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("expected NULL when all documents are filtered");
  }

  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
  return 0;
}

int main(void) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;

  memset(&db, 0, sizeof(db));
  YAP_Db_cache_init(&cache);
  if (YAP_Search_result_delete(&db, NULL) != NULL) {
    YAP_Db_cache_destroy(&cache);
    return fail("expected NULL for NULL input");
  }
  YAP_Db_cache_destroy(&cache);

  if (run_mixed_case() != 0) {
    return 1;
  }
  if (run_all_filtered_case() != 0) {
    return 1;
  }
  return 0;
}
