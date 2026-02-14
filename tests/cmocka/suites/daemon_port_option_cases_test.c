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
#include "test_proc.h"
#include "test_yappod.h"

typedef struct {
  ytest_env_t env;
  char index_dir[PATH_MAX];
} ctx_t;

static void expect_tool_failure_contains(const ytest_env_t *env, const char *tool,
                                         const char *const args[], size_t args_len,
                                         const char *expected) {
  ytest_cmd_result_t result;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(env, tool, args, args_len, NULL, &result), 0);
  assert_true(result.exited);
  assert_int_not_equal(result.exit_code, 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, expected));
  ytest_cmd_result_free(&result);
}

static void expect_daemon_parent_success(const ytest_env_t *env, const char *tool,
                                         const char *const args[], size_t args_len,
                                         const char *cwd) {
  ytest_cmd_result_t result;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(env, tool, args, args_len, cwd, &result), 0);
  assert_true(result.exited);
  assert_int_equal(result.exit_code, 0);
  ytest_cmd_result_free(&result);
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

static void test_core_port_option_validation(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  const char *args_invalid_zero[] = {"-l", ctx->index_dir, "-p", "0"};
  const char *args_invalid_range[] = {"-l", ctx->index_dir, "-p", "70000"};
  const char *args_invalid_alpha[] = {"-l", ctx->index_dir, "-p", "abc"};
  const char *args_missing_value[] = {"-l", ctx->index_dir, "-p"};

  expect_tool_failure_contains(&ctx->env, "yappod_core", args_invalid_zero,
                               sizeof(args_invalid_zero) / sizeof(args_invalid_zero[0]),
                               "invalid port for -p");
  expect_tool_failure_contains(&ctx->env, "yappod_core", args_invalid_range,
                               sizeof(args_invalid_range) / sizeof(args_invalid_range[0]),
                               "invalid port for -p");
  expect_tool_failure_contains(&ctx->env, "yappod_core", args_invalid_alpha,
                               sizeof(args_invalid_alpha) / sizeof(args_invalid_alpha[0]),
                               "invalid port for -p");
  expect_tool_failure_contains(&ctx->env, "yappod_core", args_missing_value,
                               sizeof(args_missing_value) / sizeof(args_missing_value[0]),
                               "invalid port for -p");
}

static void test_front_port_option_validation(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  const char *args_invalid_p[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-p", "0"};
  const char *args_invalid_P[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-P", "70000"};
  const char *args_missing_p[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-p"};
  const char *args_missing_P[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-P"};
  const char *args_missing_s[] = {"-l", ctx->index_dir};

  expect_tool_failure_contains(&ctx->env, "yappod_front", args_invalid_p,
                               sizeof(args_invalid_p) / sizeof(args_invalid_p[0]),
                               "invalid port for -p");
  expect_tool_failure_contains(&ctx->env, "yappod_front", args_invalid_P,
                               sizeof(args_invalid_P) / sizeof(args_invalid_P[0]),
                               "invalid port for -P");
  expect_tool_failure_contains(&ctx->env, "yappod_front", args_missing_p,
                               sizeof(args_missing_p) / sizeof(args_missing_p[0]),
                               "invalid port for -p");
  expect_tool_failure_contains(&ctx->env, "yappod_front", args_missing_P,
                               sizeof(args_missing_P) / sizeof(args_missing_P[0]),
                               "invalid port for -P");
  expect_tool_failure_contains(&ctx->env, "yappod_front", args_missing_s,
                               sizeof(args_missing_s) / sizeof(args_missing_s[0]),
                               "missing required option -s");
}

static void test_front_core_port_mismatch_should_exit(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char core_run_dir[PATH_MAX];
  char front_run_dir[PATH_MAX];
  char front_error_path[PATH_MAX];
  char *front_error = NULL;
  size_t front_error_len = 0U;
  char core_port_str[16];
  char front_port_str[16];
  char wrong_port_str[16];
  int core_port = 0;
  int front_port = 0;
  int wrong_port = 0;
  pid_t core_pid = 0;
  pid_t front_pid = 0;

  assert_int_equal(ytest_path_join(core_run_dir, sizeof(core_run_dir), ctx->env.tmp_root, "core_run"),
                   0);
  assert_int_equal(ytest_path_join(front_run_dir, sizeof(front_run_dir), ctx->env.tmp_root,
                                   "front_run"),
                   0);
  assert_int_equal(ytest_mkdir_p(core_run_dir, 0700), 0);
  assert_int_equal(ytest_mkdir_p(front_run_dir, 0700), 0);

  assert_int_equal(ytest_pick_unused_port(&core_port), 0);
  assert_int_equal(ytest_pick_unused_port(&front_port), 0);
  do {
    assert_int_equal(ytest_pick_unused_port(&wrong_port), 0);
  } while (wrong_port == core_port || wrong_port == front_port);

  assert_true(snprintf(core_port_str, sizeof(core_port_str), "%d", core_port) <
              (int)sizeof(core_port_str));
  assert_true(snprintf(front_port_str, sizeof(front_port_str), "%d", front_port) <
              (int)sizeof(front_port_str));
  assert_true(snprintf(wrong_port_str, sizeof(wrong_port_str), "%d", wrong_port) <
              (int)sizeof(wrong_port_str));

  {
    const char *core_args[] = {"-l", ctx->index_dir, "-p", core_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_core", core_args,
                                 sizeof(core_args) / sizeof(core_args[0]), core_run_dir);
  }
  assert_int_equal(ytest_read_pid_file(core_run_dir, "core.pid", 100, 20, &core_pid), 0);
  assert_int_equal(ytest_wait_for_port(core_port, 50, 100), 0);

  {
    const char *front_args[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-p", front_port_str,
                                "-P", wrong_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_front", front_args,
                                 sizeof(front_args) / sizeof(front_args[0]), front_run_dir);
  }

  assert_int_equal(ytest_path_join(front_error_path, sizeof(front_error_path), front_run_dir,
                                   "front.error"),
                   0);
  assert_int_equal(ytest_wait_file_contains(front_error_path, "connect 127.0.0.1", 80, 50), 0);
  assert_int_equal(ytest_wait_file_contains(front_error_path, wrong_port_str, 80, 50), 0);
  assert_int_equal(ytest_read_file(front_error_path, &front_error, &front_error_len), 0);
  assert_non_null(front_error);
  assert_non_null(strstr(front_error, "connect 127.0.0.1"));
  assert_non_null(strstr(front_error, wrong_port_str));

  free(front_error);
  if (ytest_read_pid_file(front_run_dir, "front.pid", 100, 20, &front_pid) == 0) {
    ytest_stop_pid_if_alive(front_pid, 30, 100);
  }
  ytest_stop_pid_if_alive(core_pid, 30, 100);
}

static void test_bind_port_collision_should_exit(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char core_run_dir_a[PATH_MAX];
  char core_run_dir_b[PATH_MAX];
  char front_run_dir_a[PATH_MAX];
  char front_run_dir_b[PATH_MAX];
  char core_error_path[PATH_MAX];
  char front_error_path[PATH_MAX];
  char *core_error = NULL;
  char *front_error = NULL;
  size_t error_len = 0U;
  int core_port = 0;
  int front_port = 0;
  pid_t core_pid = 0;
  pid_t core_fail_pid = 0;
  pid_t front_pid = 0;
  pid_t front_fail_pid = 0;
  char core_port_str[16];
  char front_port_str[16];

  assert_int_equal(ytest_path_join(core_run_dir_a, sizeof(core_run_dir_a), ctx->env.tmp_root,
                                   "collision_core_a"),
                   0);
  assert_int_equal(ytest_path_join(core_run_dir_b, sizeof(core_run_dir_b), ctx->env.tmp_root,
                                   "collision_core_b"),
                   0);
  assert_int_equal(ytest_path_join(front_run_dir_a, sizeof(front_run_dir_a), ctx->env.tmp_root,
                                   "collision_front_a"),
                   0);
  assert_int_equal(ytest_path_join(front_run_dir_b, sizeof(front_run_dir_b), ctx->env.tmp_root,
                                   "collision_front_b"),
                   0);
  assert_int_equal(ytest_mkdir_p(core_run_dir_a, 0700), 0);
  assert_int_equal(ytest_mkdir_p(core_run_dir_b, 0700), 0);
  assert_int_equal(ytest_mkdir_p(front_run_dir_a, 0700), 0);
  assert_int_equal(ytest_mkdir_p(front_run_dir_b, 0700), 0);

  assert_int_equal(ytest_pick_unused_port(&core_port), 0);
  do {
    assert_int_equal(ytest_pick_unused_port(&front_port), 0);
  } while (front_port == core_port);
  assert_true(snprintf(core_port_str, sizeof(core_port_str), "%d", core_port) <
              (int)sizeof(core_port_str));
  assert_true(snprintf(front_port_str, sizeof(front_port_str), "%d", front_port) <
              (int)sizeof(front_port_str));

  {
    const char *core_args[] = {"-l", ctx->index_dir, "-p", core_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_core", core_args,
                                 sizeof(core_args) / sizeof(core_args[0]), core_run_dir_a);
  }
  assert_int_equal(ytest_read_pid_file(core_run_dir_a, "core.pid", 100, 20, &core_pid), 0);
  assert_int_equal(ytest_wait_for_port(core_port, 50, 100), 0);

  {
    const char *core_fail_args[] = {"-l", ctx->index_dir, "-p", core_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_core", core_fail_args,
                                 sizeof(core_fail_args) / sizeof(core_fail_args[0]),
                                 core_run_dir_b);
  }
  assert_int_equal(ytest_path_join(core_error_path, sizeof(core_error_path), core_run_dir_b,
                                   "core.error"),
                   0);
  assert_int_equal(ytest_wait_file_contains(core_error_path, "bind failed on port", 80, 50), 0);
  assert_int_equal(ytest_wait_file_contains(core_error_path, core_port_str, 80, 50), 0);
  assert_int_equal(ytest_read_file(core_error_path, &core_error, &error_len), 0);
  assert_non_null(core_error);
  assert_non_null(strstr(core_error, "bind failed on port"));
  assert_non_null(strstr(core_error, core_port_str));
  free(core_error);
  if (ytest_read_pid_file(core_run_dir_b, "core.pid", 100, 20, &core_fail_pid) == 0) {
    ytest_stop_pid_if_alive(core_fail_pid, 30, 100);
  }

  {
    const char *front_args[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-p", front_port_str,
                                "-P", core_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_front", front_args,
                                 sizeof(front_args) / sizeof(front_args[0]), front_run_dir_a);
  }
  assert_int_equal(ytest_read_pid_file(front_run_dir_a, "front.pid", 100, 20, &front_pid), 0);
  assert_int_equal(ytest_wait_for_port(front_port, 50, 100), 0);

  {
    const char *front_fail_args[] = {"-l", ctx->index_dir, "-s", "127.0.0.1", "-p", front_port_str,
                                     "-P", core_port_str};
    expect_daemon_parent_success(&ctx->env, "yappod_front", front_fail_args,
                                 sizeof(front_fail_args) / sizeof(front_fail_args[0]),
                                 front_run_dir_b);
  }
  assert_int_equal(ytest_path_join(front_error_path, sizeof(front_error_path), front_run_dir_b,
                                   "front.error"),
                   0);
  assert_int_equal(ytest_wait_file_contains(front_error_path, "bind failed on port", 80, 50), 0);
  assert_int_equal(ytest_wait_file_contains(front_error_path, front_port_str, 80, 50), 0);
  assert_int_equal(ytest_read_file(front_error_path, &front_error, &error_len), 0);
  assert_non_null(front_error);
  assert_non_null(strstr(front_error, "bind failed on port"));
  assert_non_null(strstr(front_error, front_port_str));
  free(front_error);

  if (ytest_read_pid_file(front_run_dir_b, "front.pid", 100, 20, &front_fail_pid) == 0) {
    ytest_stop_pid_if_alive(front_fail_pid, 30, 100);
  }
  ytest_stop_pid_if_alive(front_pid, 30, 100);
  ytest_stop_pid_if_alive(core_pid, 30, 100);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_core_port_option_validation, setup, teardown),
    cmocka_unit_test_setup_teardown(test_front_port_option_validation, setup, teardown),
    cmocka_unit_test_setup_teardown(test_front_core_port_mismatch_should_exit, setup, teardown),
    cmocka_unit_test_setup_teardown(test_bind_port_collision_should_exit, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
