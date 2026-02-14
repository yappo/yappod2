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
#include "test_input_builder.h"

typedef struct {
  ytest_env_t env;
} ctx_t;

typedef struct {
  const char *query;
  const char *needle;
} search_hit_case_t;

static int setup(void **state) {
  ctx_t *ctx = (ctx_t *)calloc(1U, sizeof(*ctx));
  if (ctx == NULL) {
    return -1;
  }
  if (ytest_env_init(&ctx->env) != 0) {
    free(ctx);
    return -1;
  }
  *state = ctx;
  return 0;
}

static int teardown(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);

  if (ctx != NULL) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
  }
  return 0;
}

static void assert_search_hits(ctx_t *ctx, const char *index_dir, const search_hit_case_t *cases,
                               size_t case_count) {
  size_t i;

  for (i = 0; i < case_count; i++) {
    assert_int_equal(
      ytest_search_expect_contains(&ctx->env, index_dir, cases[i].query, cases[i].needle), 0);
  }
}

static void assert_search_no_hits(ctx_t *ctx, const char *index_dir, const char *const queries[],
                                  size_t query_count) {
  size_t i;

  for (i = 0; i < query_count; i++) {
    assert_int_equal(ytest_search_expect_no_hit(&ctx->env, index_dir, queries[i]), 0);
  }
}

static void assert_baseline_docs_present(ctx_t *ctx, const char *index_dir) {
  static const search_hit_case_t cases[] = {
    {"OpenAI2025", "http://example.com/doc1"},
    {"abc123", "http://example.com/doc5"},
  };
  assert_search_hits(ctx, index_dir, cases, sizeof(cases) / sizeof(cases[0]));
}

static void expect_hit_with_size(ctx_t *ctx, const char *index_dir, const char *query,
                                 const char *url, const char *size) {
  ytest_cmd_result_t result;
  const char *args[] = {query};
  char token[64];

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_search_capture(&ctx->env, index_dir, args, 1U, &result), 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, url));
  assert_true(snprintf(token, sizeof(token), "(size:%s)", size) < (int)sizeof(token));
  assert_non_null(strstr(result.output, token));
  ytest_cmd_result_free(&result);
}

static void test_invalid_url_rows_are_skipped(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root, "invalid_url.txt"),
                   0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root, "index_invalid_url"),
                   0);

  assert_int_equal(ytest_fixture_copy_with_inject(
                     ctx->env.fixture_path, input_path, 2,
                     "not_a_url\tADD\tBadURL\t100\tこの行はURL形式不正ですが処理継続の確認用です。\n", 3),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, input_path, index_dir, NULL, 0U), 0);
  assert_baseline_docs_present(ctx, index_dir);
}

static void test_broken_rows_are_skipped(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root, "broken_rows.txt"),
                   0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root, "index_broken_rows"),
                   0);

  assert_int_equal(ytest_fixture_copy_with_inject(
                     ctx->env.fixture_path, input_path, 3,
                     "\nMALFORMED LINE WITHOUT TABS\n"
                     "http://example.com/missingbody\tADD\tOnlyTitle\t100\n"
                     "\tADD\tNoUrl\t100\tnourltoken222xxxyyyzzz\n"
                     "http://example.com/badcmd\tUPSERT\tBadCmd\t100\tbadcmdtoken333mmmnnnooo\n",
                     4),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, input_path, index_dir, NULL, 0U), 0);
  assert_baseline_docs_present(ctx, index_dir);

  {
    static const char *const queries[] = {
      "badcmdtoken333mmmnnnooo",
      "nourltoken222xxxyyyzzz",
    };
    assert_search_no_hits(ctx, index_dir, queries, sizeof(queries) / sizeof(queries[0]));
  }
}

static void test_oversized_lines_are_skipped(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root,
                                   "oversized_rows.txt"),
                   0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root,
                                   "index_oversized_rows"),
                   0);

  assert_int_equal(ytest_fixture_build_oversized_burst(ctx->env.fixture_path, input_path, 1100000U),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, input_path, index_dir, NULL, 0U), 0);
  assert_baseline_docs_present(ctx, index_dir);

  {
    static const char *const queries[] = {
      "削除対象",
      "oversizedtoken666lllmmmnnn",
      "oversizedtoken777ooopppqqq",
    };
    assert_search_no_hits(ctx, index_dir, queries, sizeof(queries) / sizeof(queries[0]));
  }
}

static void test_invalid_body_size_rows_are_skipped(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root,
                                   "invalid_size_rows.txt"),
                   0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root,
                                   "index_invalid_size_rows"),
                   0);

  assert_int_equal(ytest_fixture_copy_with_inject(
                     ctx->env.fixture_path, input_path, 2,
                     "http://example.com/badsize-neg\tADD\tBadSizeNeg\t-1\tbadsizeNEGtoken888aaabbb\n"
                     "http://example.com/badsize-over\tADD\tBadSizeOver\t2147483648\tbadsizeOVERtoken999cccddd\n"
                     "http://example.com/badsize-alpha\tADD\tBadSizeAlpha\tabc\tbadsizeALPHAtoken000eeefff\n",
                     3),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, input_path, index_dir, NULL, 0U), 0);
  assert_baseline_docs_present(ctx, index_dir);

  {
    static const char *const queries[] = {
      "badsizeNEGtoken888aaabbb",
      "badsizeOVERtoken999cccddd",
      "badsizeALPHAtoken000eeefff",
    };
    assert_search_no_hits(ctx, index_dir, queries, sizeof(queries) / sizeof(queries[0]));
  }
}

static void test_default_body_size_boundaries(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root,
                                   "size_boundary_rows.txt"),
                   0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root,
                                   "index_size_boundary_rows"),
                   0);

  assert_int_equal(ytest_fixture_rewrite_size_boundary(ctx->env.fixture_path, input_path), 0);
  assert_int_equal(ytest_build_index(&ctx->env, input_path, index_dir, NULL, 0U), 0);

  expect_hit_with_size(ctx, index_dir, "OpenAI2025", "http://example.com/doc1", "24");
  assert_int_equal(ytest_search_expect_contains(&ctx->env, index_dir, "サンプル本文",
                                                "https://example.com/doc3"),
                   0);
  {
    static const char *const queries[] = {
      "検索用のテスト本文です",
      "abc123",
    };
    assert_search_no_hits(ctx, index_dir, queries, sizeof(queries) / sizeof(queries[0]));
  }
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_invalid_url_rows_are_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(test_broken_rows_are_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(test_oversized_lines_are_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(test_invalid_body_size_rows_are_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(test_default_body_size_boundaries, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
