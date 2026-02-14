#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "test_index.h"
#include "test_yappod.h"

typedef struct {
  ytest_env_t env;
  char index_dir[PATH_MAX];
  char index_loose_dir[PATH_MAX];
} e2e_ctx_t;

typedef struct {
  const char *query;
  const char *needle;
} search_hit_case_t;

static void assert_search_hits(e2e_ctx_t *ctx, const char *index_dir, const search_hit_case_t *cases,
                               size_t case_count) {
  size_t i;

  for (i = 0; i < case_count; i++) {
    assert_int_equal(
      ytest_search_expect_contains(&ctx->env, index_dir, cases[i].query, cases[i].needle), 0);
  }
}

static void assert_search_no_hits(e2e_ctx_t *ctx, const char *index_dir, const char *const queries[],
                                  size_t query_count) {
  size_t i;

  for (i = 0; i < query_count; i++) {
    assert_int_equal(ytest_search_expect_no_hit(&ctx->env, index_dir, queries[i]), 0);
  }
}

static int setup(void **state) {
  e2e_ctx_t *ctx;
  const char *min_body_size[] = {"--min-body-size", "1"};

  ctx = (e2e_ctx_t *)calloc(1U, sizeof(*ctx));
  if (ctx == NULL) {
    return -1;
  }

  if (ytest_env_init(&ctx->env) != 0 ||
      ytest_path_join(ctx->index_dir, sizeof(ctx->index_dir), ctx->env.tmp_root, "index") != 0 ||
      ytest_path_join(ctx->index_loose_dir, sizeof(ctx->index_loose_dir), ctx->env.tmp_root,
                      "index_loose") != 0) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
    return -1;
  }

  if (ytest_build_index(&ctx->env, ctx->env.fixture_path, ctx->index_dir, NULL, 0U) != 0 ||
      ytest_build_index(&ctx->env, ctx->env.fixture_path, ctx->index_loose_dir, min_body_size,
                        2U) != 0) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
    return -1;
  }

  *state = ctx;
  return 0;
}

static int teardown(void **state) {
  e2e_ctx_t *ctx = (e2e_ctx_t *)(*state);

  if (ctx != NULL) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
  }
  return 0;
}

static void test_e2e_queries(void **state) {
  e2e_ctx_t *ctx = (e2e_ctx_t *)(*state);
  const char *and_args[] = {"-a", "テスト", "本文", "検索"};
  static const search_hit_case_t hits[] = {
    {"テスト", "http://example.com/doc1"},
    {"テスト", "http://example.com/doc2"},
    {"OpenAI2025", "http://example.com/doc1"},
    {"openai2025", "http://example.com/doc1"},
    {"サンプル本文", "https://example.com/doc3"},
    {"abc123", "http://example.com/doc5"},
    {"foo+bar@example.com", "http://example.com/doc5"},
    {"日本語", "http://example.com/doc5"},
    {"本文です。", "http://example.com/doc1"},
    {"本文です。OpenAI2025", "http://example.com/doc1"},
    {"検索用のテスト本文です", "http://example.com/doc2"},
    {"日本語とabc123", "http://example.com/doc5"},
  };
  static const char *const no_hits[] = {
    "削除対象",
    "短い",
    "abc124",
    "OpenAI",
    "2025",
    "abc",
    "123",
    "テスト。",
    "本文です。OpenAI",
    "日本語とabc",
  };

  assert_search_hits(ctx, ctx->index_dir, hits, sizeof(hits) / sizeof(hits[0]));

  {
    ytest_cmd_result_t result;
    const char *args[] = {"サンプル本文"};
    ytest_cmd_result_init(&result);
    assert_int_equal(ytest_search_capture(&ctx->env, ctx->index_dir, args, 1U, &result), 0);
    assert_non_null(result.output);
    assert_non_null(strstr(result.output, "https://example.com/doc3"));
    assert_non_null(strstr(result.output, "(domainid:1)"));
    ytest_cmd_result_free(&result);
  }

  assert_search_no_hits(ctx, ctx->index_dir, no_hits, sizeof(no_hits) / sizeof(no_hits[0]));
  assert_int_equal(
    ytest_search_expect_contains(&ctx->env, ctx->index_loose_dir, "短い", "http://example.com/skip"),
    0);

  assert_int_equal(ytest_search_expect_contains_args(&ctx->env, ctx->index_dir, and_args,
                                                     sizeof(and_args) / sizeof(and_args[0]),
                                                     "http://example.com/doc2"),
                   0);
  assert_int_equal(ytest_search_expect_not_contains_args(&ctx->env, ctx->index_dir, and_args,
                                                         sizeof(and_args) / sizeof(and_args[0]),
                                                         "http://example.com/doc1"),
                   0);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_e2e_queries, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
