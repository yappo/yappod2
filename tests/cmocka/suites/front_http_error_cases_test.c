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
#include "test_http.h"
#include "test_index.h"
#include "test_proc.h"
#include "test_yappod.h"

typedef struct {
  ytest_env_t env;
  ytest_daemon_stack_t stack;
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
} ctx_t;

static int setup(void **state) {
  ctx_t *ctx = (ctx_t *)calloc(1U, sizeof(*ctx));

  if (ctx == NULL) {
    return -1;
  }

  ytest_daemon_stack_init(&ctx->stack);
  if (ytest_env_init(&ctx->env) != 0 ||
      ytest_path_join(ctx->index_dir, sizeof(ctx->index_dir), ctx->env.tmp_root, "index") != 0 ||
      ytest_path_join(ctx->run_dir, sizeof(ctx->run_dir), ctx->env.tmp_root, "daemon") != 0) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
    return -1;
  }

  if (ytest_build_index(&ctx->env, ctx->env.fixture_path, ctx->index_dir, NULL, 0U) != 0) {
    ytest_env_destroy(&ctx->env);
    free(ctx);
    return -1;
  }

  if (ytest_daemon_stack_start(&ctx->stack, ctx->env.build_dir, ctx->index_dir, ctx->run_dir) != 0) {
    ytest_daemon_stack_dump_logs(&ctx->stack, stderr);
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
    ytest_daemon_stack_stop(&ctx->stack);
    ytest_env_destroy(&ctx->env);
    free(ctx);
  }
  return 0;
}

static void expect_bad_request(ctx_t *ctx, const char *request) {
  assert_int_equal(ytest_http_text_contains(ctx->stack.front_port, request, "400 Bad Request"), 0);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_front_http_errors(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  size_t i;
  static const char *const bad_requests[] = {
    "POST /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10?OpenAI2025\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10? HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/notrange?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/-1-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/10-1?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/0/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10/extra?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
  };
  struct {
    const char *prefix;
    char repeat_char;
    size_t repeat_len;
    const char *suffix;
  } repeated_cases[] = {
    {"GET /", 'd', 2000U, "/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n"},
    {"GET /yappo/100000/AND/0-10?", 'A', 20000U, " HTTP/1.1\r\nHost: localhost\r\n\r\n"},
    {"GET /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\nX-Long: ", 'A',
     20000U, "\r\n\r\n"},
  };

  for (i = 0; i < sizeof(bad_requests) / sizeof(bad_requests[0]); i++) {
    expect_bad_request(ctx, bad_requests[i]);
  }

  for (i = 0; i < sizeof(repeated_cases) / sizeof(repeated_cases[0]); i++) {
    char *req = ytest_http_build_request_with_repeat(repeated_cases[i].prefix,
                                                     repeated_cases[i].repeat_char,
                                                     repeated_cases[i].repeat_len,
                                                     repeated_cases[i].suffix);
    assert_non_null(req);
    expect_bad_request(ctx, req);
    free(req);
  }
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_front_http_errors, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
