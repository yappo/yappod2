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

int main(void) {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  SEARCH_RESULT input;
  SEARCH_RESULT *filtered;
  unsigned int deletefile_size;

  memset(&db, 0, sizeof(db));
  memset(&input, 0, sizeof(input));
  YAP_Db_cache_init(&cache);

  cache.total_filenum = 8;
  deletefile_size = (cache.total_filenum / 8U) + 1U;
  cache.deletefile = (unsigned char *)calloc(deletefile_size, sizeof(unsigned char));
  if (cache.deletefile == NULL) {
    YAP_Db_cache_destroy(&cache);
    return fail("failed to allocate deletefile bitmap");
  }
  cache.deletefile_num = deletefile_size;
  db.cache = &cache;

  input.keyword_docs_num = 2;
  input.docs_list = (SEARCH_DOCUMENT *)calloc((size_t)input.keyword_docs_num, sizeof(SEARCH_DOCUMENT));
  if (input.docs_list == NULL) {
    YAP_Db_cache_destroy(&cache);
    return fail("failed to allocate docs list");
  }

  input.docs_list[0].fileindex = 1;
  input.docs_list[0].pos_len = 3;
  input.docs_list[0].score = 1.0;
  input.docs_list[1].fileindex = INT_MAX;
  input.docs_list[1].pos_len = 5;
  input.docs_list[1].score = 2.0;

  filtered = YAP_Search_result_delete(&db, &input);
  if (filtered == NULL) {
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected NULL result");
  }
  if (filtered->keyword_docs_num != 1) {
    YAP_Search_result_free(filtered);
    free(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("expected exactly one surviving document");
  }
  if (filtered->docs_list[0].fileindex != 1) {
    YAP_Search_result_free(filtered);
    free(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected surviving fileindex");
  }
  if (filtered->keyword_total_num != 3) {
    YAP_Search_result_free(filtered);
    free(filtered);
    free(input.docs_list);
    YAP_Db_cache_destroy(&cache);
    return fail("unexpected keyword_total_num");
  }

  YAP_Search_result_free(filtered);
  free(filtered);
  free(input.docs_list);
  YAP_Db_cache_destroy(&cache);
  return 0;
}
