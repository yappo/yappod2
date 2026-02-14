#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "test_index.h"
#include "test_mutation.h"
#include "test_yappod.h"

typedef struct {
  ytest_env_t env;
  char index_dir[PATH_MAX];
} ctx_t;

static void run_expect_fail(ctx_t *ctx, const char *expect, const char *const args[],
                            size_t args_len) {
  ytest_cmd_result_t result;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_mergepos_run(&ctx->env, args, args_len, &result), 0);
  assert_true(result.exited);
  assert_int_not_equal(result.exit_code, 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, expect));
  ytest_cmd_result_free(&result);
}

typedef struct {
  const char *expect;
  const char *const *args;
  size_t args_len;
} mergepos_fail_case_t;

static void run_fail_cases(ctx_t *ctx, const mergepos_fail_case_t *cases, size_t case_count) {
  size_t i;

  for (i = 0; i < case_count; i++) {
    run_expect_fail(ctx, cases[i].expect, cases[i].args, cases[i].args_len);
  }
}

static int setup(void **state) {
  ctx_t *ctx = (ctx_t *)calloc(1U, sizeof(*ctx));

  if (ctx == NULL) {
    return -1;
  }

  if (ytest_env_init(&ctx->env) != 0 ||
      ytest_path_join(ctx->index_dir, sizeof(ctx->index_dir), ctx->env.tmp_root, "index") != 0 ||
      ytest_build_index(&ctx->env, ctx->env.fixture_path, ctx->index_dir, NULL, 0U) != 0) {
    ytest_env_destroy(&ctx->env);
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

static void test_mergepos_errors(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char out_path[PATH_MAX];
  char no_such_dir[PATH_MAX];
  char index_no_delete[PATH_MAX];
  char index_no_keynum[PATH_MAX];
  char index_malformed[PATH_MAX];
  char path[PATH_MAX];
  const char *args_missing_d[] = {"-l", ctx->index_dir, "-s", "0", "-e", "0"};
  const char *args_missing_l[] = {"-d", out_path, "-s", "0", "-e", "0"};
  const char *args_invalid_s[] = {"-l", ctx->index_dir, "-d", out_path, "-s", "-1", "-e", "0"};
  const char *args_invalid_e[] = {"-l", ctx->index_dir, "-d", out_path, "-s", "0", "-e", "abc"};
  const char *args_invalid_range[] = {"-l", ctx->index_dir, "-d", out_path, "-s", "1", "-e", "0"};
  const char *args_missing_shard[] = {"-l", ctx->index_dir, "-d", out_path, "-s", "0", "-e", "1"};
  const mergepos_fail_case_t base_cases[] = {
    {"Missing required option: -d output_file", args_missing_d,
     sizeof(args_missing_d) / sizeof(args_missing_d[0])},
    {"Missing required option: -l input_index", args_missing_l,
     sizeof(args_missing_l) / sizeof(args_missing_l[0])},
    {"Invalid start value: -1", args_invalid_s, sizeof(args_invalid_s) / sizeof(args_invalid_s[0])},
    {"Invalid end value: abc", args_invalid_e, sizeof(args_invalid_e) / sizeof(args_invalid_e[0])},
    {"pos num error: 0 - 1 = 0", args_invalid_range,
     sizeof(args_invalid_range) / sizeof(args_invalid_range[0])},
    {"Missing required pos shard file:", args_missing_shard,
     sizeof(args_missing_shard) / sizeof(args_missing_shard[0])},
  };

  assert_int_equal(ytest_path_join(out_path, sizeof(out_path), ctx->env.tmp_root, "outpos"), 0);
  run_fail_cases(ctx, base_cases, sizeof(base_cases) / sizeof(base_cases[0]));

  assert_int_equal(ytest_path_join(no_such_dir, sizeof(no_such_dir), ctx->env.tmp_root, "no_such_dir"),
                   0);
  {
    const char *args[] = {"-l", no_such_dir, "-d", out_path, "-s", "0", "-e", "0"};
    run_expect_fail(ctx, "Please specify an existing index directory.", args,
                    sizeof(args) / sizeof(args[0]));
  }

  assert_int_equal(ytest_path_join(index_no_delete, sizeof(index_no_delete), ctx->env.tmp_root,
                                   "index_no_delete"),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, ctx->env.fixture_path, index_no_delete, NULL, 0U), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), index_no_delete, "deletefile"), 0);
  assert_int_equal(unlink(path), 0);
  {
    const char *args[] = {"-l", index_no_delete, "-d", out_path, "-s", "0", "-e", "0"};
    run_expect_fail(ctx, "fopen error:", args, sizeof(args) / sizeof(args[0]));
  }

  assert_int_equal(ytest_path_join(index_no_keynum, sizeof(index_no_keynum), ctx->env.tmp_root,
                                   "index_no_keynum"),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, ctx->env.fixture_path, index_no_keynum, NULL, 0U), 0);
  assert_int_equal(ytest_path_join(path, sizeof(path), index_no_keynum, "keywordnum"), 0);
  assert_int_equal(unlink(path), 0);
  {
    const char *args[] = {"-l", index_no_keynum, "-d", out_path, "-s", "0", "-e", "0"};
    run_expect_fail(ctx, "fopen error:", args, sizeof(args) / sizeof(args[0]));
  }

  assert_int_equal(ytest_path_join(index_malformed, sizeof(index_malformed), ctx->env.tmp_root,
                                   "index_malformed_postings"),
                   0);
  assert_int_equal(ytest_build_index(&ctx->env, ctx->env.fixture_path, index_malformed, NULL, 0U),
                   0);
  assert_int_equal(ytest_mutate_malformed_postings(index_malformed), 0);
  {
    const char *args[] = {"-l", index_malformed, "-d", out_path, "-s", "0", "-e", "0"};
    run_expect_fail(ctx, "malformed postings payload:", args, sizeof(args) / sizeof(args[0]));
  }
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_mergepos_errors, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
