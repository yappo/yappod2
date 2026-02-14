#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yappo_db.h"
#include "yappo_search.h"

typedef struct {
  YAPPO_DB_FILES db;
  YAPPO_CACHE cache;
  pthread_mutex_t failure_mutex;
  int failed;
  char message[128];
} test_ctx_t;

static void set_failure(test_ctx_t *ctx, const char *msg) {
  pthread_mutex_lock(&ctx->failure_mutex);
  if (ctx->failed == 0) {
    ctx->failed = 1;
    snprintf(ctx->message, sizeof(ctx->message), "%s", msg);
  }
  pthread_mutex_unlock(&ctx->failure_mutex);
}

static int has_failure(test_ctx_t *ctx) {
  int failed;
  pthread_mutex_lock(&ctx->failure_mutex);
  failed = ctx->failed;
  pthread_mutex_unlock(&ctx->failure_mutex);
  return failed;
}

static void *writer_thread(void *arg) {
  test_ctx_t *ctx = (test_ctx_t *)arg;
  int i;

  for (i = 0; i < 20000; i++) {
    unsigned int total_filenum;
    unsigned int cap;
    unsigned char *new_bitmap;

    if (has_failure(ctx)) {
      return NULL;
    }

    total_filenum = (i % 2 == 0) ? 8U : 65535U;
    cap = (total_filenum / 8U) + 1U;

    pthread_mutex_lock(&ctx->cache.deletefile_mutex);
    new_bitmap = (unsigned char *)realloc(ctx->cache.deletefile, cap);
    if (new_bitmap == NULL) {
      pthread_mutex_unlock(&ctx->cache.deletefile_mutex);
      set_failure(ctx, "failed to reallocate deletefile bitmap");
      return NULL;
    }
    ctx->cache.deletefile = new_bitmap;
    memset(ctx->cache.deletefile, 0, cap);
    ctx->cache.total_filenum = total_filenum;
    ctx->cache.deletefile_num = cap;

    if ((i % 3) == 0) {
      ctx->cache.deletefile[0] |= (unsigned char)(1U << 2); /* fileindex=2 */
    }
    if (total_filenum > 4096U && (i % 5) == 0) {
      ctx->cache.deletefile[512] |= 1U; /* fileindex=4096 */
    }
    pthread_mutex_unlock(&ctx->cache.deletefile_mutex);
  }

  return NULL;
}

static void *reader_thread(void *arg) {
  test_ctx_t *ctx = (test_ctx_t *)arg;
  SEARCH_RESULT input;
  SEARCH_DOCUMENT docs[6];
  int i;

  memset(&input, 0, sizeof(input));
  memset(docs, 0, sizeof(docs));
  input.keyword_docs_num = 6;
  input.docs_list = docs;

  docs[0].fileindex = 1;
  docs[0].pos_len = 1;
  docs[1].fileindex = 2;
  docs[1].pos_len = 1;
  docs[2].fileindex = 4096;
  docs[2].pos_len = 1;
  docs[3].fileindex = INT_MAX;
  docs[3].pos_len = 1;
  docs[4].fileindex = -1;
  docs[4].pos_len = 1;
  docs[5].fileindex = 0;
  docs[5].pos_len = 1;

  for (i = 0; i < 20000; i++) {
    SEARCH_RESULT *filtered;
    int di;

    if (has_failure(ctx)) {
      return NULL;
    }

    filtered = YAP_Search_result_delete(&ctx->db, &input);
    if (filtered == NULL) {
      continue;
    }

    for (di = 0; di < filtered->keyword_docs_num; di++) {
      if (filtered->docs_list[di].fileindex <= 0) {
        YAP_Search_result_free(filtered);
        free(filtered);
        set_failure(ctx, "invalid non-positive fileindex survived filtering");
        return NULL;
      }
      if (filtered->docs_list[di].fileindex == INT_MAX) {
        YAP_Search_result_free(filtered);
        free(filtered);
        set_failure(ctx, "out-of-range fileindex survived filtering");
        return NULL;
      }
    }

    YAP_Search_result_free(filtered);
    free(filtered);
  }

  return NULL;
}

int main(void) {
  test_ctx_t ctx;
  pthread_t writer;
  pthread_t reader;

  memset(&ctx, 0, sizeof(ctx));
  YAP_Db_cache_init(&ctx.cache);
  pthread_mutex_init(&ctx.failure_mutex, NULL);

  ctx.cache.total_filenum = 8U;
  ctx.cache.deletefile_num = 2U;
  ctx.cache.deletefile = (unsigned char *)calloc(ctx.cache.deletefile_num, sizeof(unsigned char));
  if (ctx.cache.deletefile == NULL) {
    fprintf(stderr, "failed to allocate initial deletefile bitmap\n");
    pthread_mutex_destroy(&ctx.failure_mutex);
    YAP_Db_cache_destroy(&ctx.cache);
    return 1;
  }
  ctx.db.cache = &ctx.cache;

  if (pthread_create(&writer, NULL, writer_thread, &ctx) != 0) {
    fprintf(stderr, "failed to create writer thread\n");
    pthread_mutex_destroy(&ctx.failure_mutex);
    YAP_Db_cache_destroy(&ctx.cache);
    return 1;
  }
  if (pthread_create(&reader, NULL, reader_thread, &ctx) != 0) {
    fprintf(stderr, "failed to create reader thread\n");
    pthread_join(writer, NULL);
    pthread_mutex_destroy(&ctx.failure_mutex);
    YAP_Db_cache_destroy(&ctx.cache);
    return 1;
  }

  pthread_join(writer, NULL);
  pthread_join(reader, NULL);

  if (ctx.failed) {
    fprintf(stderr, "%s\n", ctx.message);
    pthread_mutex_destroy(&ctx.failure_mutex);
    YAP_Db_cache_destroy(&ctx.cache);
    return 1;
  }

  pthread_mutex_destroy(&ctx.failure_mutex);
  YAP_Db_cache_destroy(&ctx.cache);
  return 0;
}
