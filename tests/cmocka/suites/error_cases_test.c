#include <setjmp.h>
#include <signal.h>
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
#include "test_http.h"
#include "test_index.h"
#include "test_input_builder.h"
#include "test_mutation.h"
#include "test_proc.h"
#include "test_yappod.h"

typedef struct {
  ytest_env_t env;
  ytest_daemon_stack_t stack;
} ctx_t;

typedef struct {
  const char *query;
  const char *needle;
} search_hit_case_t;

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

static int setup(void **state) {
  ctx_t *ctx = (ctx_t *)calloc(1U, sizeof(*ctx));

  if (ctx == NULL) {
    return -1;
  }
  ytest_daemon_stack_init(&ctx->stack);

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
    ytest_daemon_stack_stop(&ctx->stack);
    ytest_env_destroy(&ctx->env);
    free(ctx);
  }
  return 0;
}

static void make_index_named(ctx_t *ctx, const char *index_name, const char *input_file,
                             char *index_dir, size_t index_dir_size) {
  assert_int_equal(ytest_path_join(index_dir, index_dir_size, ctx->env.tmp_root, index_name), 0);
  assert_int_equal(ytest_build_index(&ctx->env, input_file, index_dir, NULL, 0U), 0);
}

static void start_daemons(ctx_t *ctx, const char *index_dir, const char *run_tag,
                          char *run_dir, size_t run_dir_size) {
  int rc;

  assert_int_equal(ytest_path_join(run_dir, run_dir_size, ctx->env.tmp_root, run_tag), 0);
  rc = ytest_daemon_stack_start(&ctx->stack, ctx->env.build_dir, index_dir, run_dir);
  if (rc != 0) {
    ytest_daemon_stack_dump_logs(&ctx->stack, stderr);
  }
  assert_int_equal(rc, 0);
}

static void test_case_01_missing_pos_directory(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  const char *args[4];
  ytest_cmd_result_t result;

  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), ctx->env.tmp_root, "no_pos"), 0);
  assert_int_equal(ytest_mkdir_p(index_dir, 0700), 0);

  args[0] = "-f";
  args[1] = ctx->env.fixture_path;
  args[2] = "-d";
  args[3] = index_dir;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(&ctx->env, "yappo_makeindex", args, 4U, NULL, &result), 0);
  assert_true(result.exited);
  assert_int_not_equal(result.exit_code, 0);
  ytest_cmd_result_free(&result);
}

static void test_case_02_corrupted_metadata_should_not_segv(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char path[PATH_MAX];
  size_t i;
  static const char *write_empty_files[] = {
    "keyword_docsnum",
    "keyword_totalnum",
  };
  struct {
    const char *path;
    off_t size;
  } truncate_cases[] = {
    {"pos/0", 4},
    {"size", 1},
    {"domainid", 1},
    {"score", 1},
  };

  for (i = 0; i < sizeof(write_empty_files) / sizeof(write_empty_files[0]); i++) {
    char name[32];

    assert_true(snprintf(name, sizeof(name), "meta_empty_%zu", i) < (int)sizeof(name));
    make_index_named(ctx, name, ctx->env.fixture_path, index_dir, sizeof(index_dir));
    assert_int_equal(ytest_path_join(path, sizeof(path), index_dir, write_empty_files[i]), 0);
    assert_int_equal(ytest_write_file(path, "", 0U), 0);
    assert_int_equal(ytest_search_expect_not_signaled(&ctx->env, index_dir, "テスト"), 0);
  }

  for (i = 0; i < sizeof(truncate_cases) / sizeof(truncate_cases[0]); i++) {
    char name[32];

    assert_true(snprintf(name, sizeof(name), "meta_trunc_%zu", i) < (int)sizeof(name));
    make_index_named(ctx, name, ctx->env.fixture_path, index_dir, sizeof(index_dir));
    assert_int_equal(ytest_path_join(path, sizeof(path), index_dir, truncate_cases[i].path), 0);
    assert_int_equal(ytest_truncate_file(path, truncate_cases[i].size), 0);
    assert_int_equal(ytest_search_expect_not_signaled(&ctx->env, index_dir, "テスト"), 0);
  }
}

static void test_case_03_malformed_postings_should_not_segv(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];

  make_index_named(ctx, "malformed_postings", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  assert_int_equal(ytest_mutate_malformed_postings(index_dir), 0);
  assert_int_equal(ytest_search_expect_not_signaled(&ctx->env, index_dir, "テスト"), 0);
}

static void test_case_04_invalid_percent_escapes_search(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  size_t i;
  static const char *queries[] = {
    "%",
    "%A",
    "%ZZ",
    "A%2",
  };

  make_index_named(ctx, "invalid_percent_search", ctx->env.fixture_path, index_dir,
                   sizeof(index_dir));

  for (i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
    assert_int_equal(ytest_search_expect_not_signaled(&ctx->env, index_dir, queries[i]), 0);
  }
}

static void test_case_05_broken_index_over_daemon_path(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  char score_path[PATH_MAX];

  make_index_named(ctx, "broken_daemon", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  assert_int_equal(ytest_path_join(score_path, sizeof(score_path), index_dir, "score"), 0);
  assert_int_equal(ytest_truncate_file(score_path, 1), 0);

  start_daemons(ctx, index_dir, "daemon_broken", run_dir, sizeof(run_dir));
  assert_int_equal(
    ytest_http_text_contains(ctx->stack.front_port, "GET / bad\r\n\r\n", "400 Bad Request"), 0);
  assert_int_equal(
    ytest_http_text_contains(
      ctx->stack.front_port,
      "GET /d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n", "HTTP/1.0"),
    0);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_case_06_front_survives_core_disconnect(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  int i;
  int got_response = 0;

  make_index_named(ctx, "core_disconnect", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  start_daemons(ctx, index_dir, "daemon_disconnect", run_dir, sizeof(run_dir));

  assert_int_equal(kill(ctx->stack.core_pid, SIGTERM), 0);
  usleep(300000);

  for (i = 0; i < 10; i++) {
    char *response_text = NULL;
    if (ytest_http_send_text(ctx->stack.front_port,
                             "GET /d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n",
                             &response_text) == 0) {
      assert_non_null(response_text);
      assert_non_null(strstr(response_text, "HTTP/1.0"));
      free(response_text);
      got_response = 1;
      break;
    }
    free(response_text);
    assert_int_equal(kill(ctx->stack.front_pid, 0), 0);
    usleep(200000);
  }

  (void)got_response;
  assert_int_equal(kill(ctx->stack.front_pid, 0), 0);
}

static void test_case_07_daemon_oversized_query(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  char *request;

  make_index_named(ctx, "oversized_query", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  start_daemons(ctx, index_dir, "daemon_oversized_query", run_dir, sizeof(run_dir));

  request = ytest_http_build_request_with_repeat("GET /d/100/OR/0-10?", 'A', 200000U,
                                                 " HTTP/1.0\r\nHost: localhost\r\n\r\n");
  assert_non_null(request);
  assert_int_equal(ytest_http_text_contains(ctx->stack.front_port, request, "400 Bad Request"), 0);
  free(request);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_case_08_daemon_invalid_utf8_bytes(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  size_t i;
  static const char *hex_queries[] = {
    "474554202f642f3130302f4f522f302d31303ffffe20485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a",
    "474554202f642f3130302f4f522f302d31303fe38120485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a",
  };

  make_index_named(ctx, "invalid_utf8_http", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  start_daemons(ctx, index_dir, "daemon_invalid_utf8", run_dir, sizeof(run_dir));

  for (i = 0; i < sizeof(hex_queries) / sizeof(hex_queries[0]); i++) {
    unsigned char *resp = NULL;
    size_t resp_len = 0U;
    assert_int_equal(ytest_http_send_hex(ctx->stack.front_port, hex_queries[i], &resp, &resp_len),
                     0);
    free(resp);
  }
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_case_09_standard_http_path(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];

  make_index_named(ctx, "standard_http_path", ctx->env.fixture_path, index_dir,
                   sizeof(index_dir));
  start_daemons(ctx, index_dir, "daemon_standard_path", run_dir, sizeof(run_dir));

  assert_int_equal(
    ytest_http_text_contains(
      ctx->stack.front_port,
      "GET /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n",
      "http://example.com/doc1"),
    0);
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_case_10_malformed_input_fixture_should_continue(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];

  make_index_named(ctx, "malformed_fixture", ctx->env.fixture_malformed_path, index_dir,
                   sizeof(index_dir));
  {
    static const search_hit_case_t hits[] = {
      {"OpenAI2025", "http://example.com/doc1"},
      {"検索用のテスト本文です", "http://example.com/doc2"},
    };
    static const char *const no_hits[] = {
      "badcmdtoken999",
    };
    assert_search_hits(ctx, index_dir, hits, sizeof(hits) / sizeof(hits[0]));
    assert_search_no_hits(ctx, index_dir, no_hits, sizeof(no_hits) / sizeof(no_hits[0]));
  }
}

static void test_case_11_edge_rows_should_continue(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char input_path[PATH_MAX];
  char index_dir[PATH_MAX];

  assert_int_equal(ytest_path_join(input_path, sizeof(input_path), ctx->env.tmp_root,
                                   "index_edge_cases.txt"),
                   0);
  assert_int_equal(ytest_fixture_build_edge_cases(input_path, 1100000U), 0);
  make_index_named(ctx, "edge_rows", input_path, index_dir, sizeof(index_dir));
  {
    static const search_hit_case_t hits[] = {
      {"aaaaaaaaaaaaaaaaaaaaaaaa", "http://example.com/ok1"},
      {"cccccccccccccccccccccccc", "http://example.com/ok2"},
      {"dddddddddddddddddddddddd", "http://example.com/ok3"},
    };
    static const char *const no_hits[] = {
      "badsizepayload",
      "Dup2",
    };
    assert_search_hits(ctx, index_dir, hits, sizeof(hits) / sizeof(hits[0]));
    assert_search_no_hits(ctx, index_dir, no_hits, sizeof(no_hits) / sizeof(no_hits[0]));
  }
}

static void test_case_12_invalid_percent_http_requests(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  size_t i;
  static const char *reqs[] = {
    "GET /yappo/100000/AND/0-10?% HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10?%A HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10?%ZZ HTTP/1.1\r\nHost: localhost\r\n\r\n",
    "GET /yappo/100000/AND/0-10?A%2 HTTP/1.1\r\nHost: localhost\r\n\r\n",
  };

  make_index_named(ctx, "invalid_percent_http", ctx->env.fixture_path, index_dir,
                   sizeof(index_dir));
  start_daemons(ctx, index_dir, "daemon_invalid_percent", run_dir, sizeof(run_dir));

  for (i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    assert_int_equal(ytest_http_text_contains(ctx->stack.front_port, reqs[i], "HTTP/1.0 200 OK"),
                     0);
  }
  assert_true(ytest_daemon_stack_alive(&ctx->stack));
}

static void test_case_13_finite_score_under_extreme_values(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  ytest_cmd_result_t result;
  const char *args[] = {"OpenAI2025"};

  make_index_named(ctx, "finite_score", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  assert_int_equal(ytest_mutate_extreme_score_record(index_dir, 1), 0);

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_search_capture(&ctx->env, index_dir, args, 1U, &result), 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, "http://example.com/doc1"));
  assert_null(strstr(result.output, "SCORE:nan"));
  assert_null(strstr(result.output, "SCORE:inf"));
  assert_null(strstr(result.output, "SCORE:-inf"));
  ytest_cmd_result_free(&result);
}

static void test_case_14_missing_filedata_strings_should_not_print_null(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char run_dir[PATH_MAX];
  ytest_cmd_result_t result;
  const char *args[] = {"OpenAI2025"};

  make_index_named(ctx, "missing_filedata_strings", ctx->env.fixture_path, index_dir,
                   sizeof(index_dir));
  assert_int_equal(ytest_mutate_filedata_missing_strings(index_dir, 1), 0);

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_search_capture(&ctx->env, index_dir, args, 1U, &result), 0);
  assert_non_null(result.output);
  assert_null(strstr(result.output, "(null)"));
  assert_non_null(strstr(result.output, "URL:"));
  ytest_cmd_result_free(&result);

  start_daemons(ctx, index_dir, "daemon_missing_filedata", run_dir, sizeof(run_dir));
  assert_int_equal(
    ytest_http_text_contains(
      ctx->stack.front_port,
      "GET /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n", "\n\t"),
    0);
}

static void test_case_15_postings_query_bytes_cap(void **state) {
  ctx_t *ctx = (ctx_t *)(*state);
  char index_dir[PATH_MAX];
  char search_bin[PATH_MAX];
  char *argv[7];
  ytest_cmd_result_t result;

  make_index_named(ctx, "postings_cap", ctx->env.fixture_path, index_dir, sizeof(index_dir));
  assert_int_equal(ytest_bin_path(&ctx->env, "search", search_bin, sizeof(search_bin)), 0);

  argv[0] = "/usr/bin/env";
  argv[1] = "YAPPOD_MAX_POSTINGS_QUERY_BYTES=1";
  argv[2] = search_bin;
  argv[3] = "-l";
  argv[4] = index_dir;
  argv[5] = "OpenAI2025";
  argv[6] = NULL;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_cmd_run(argv, NULL, NULL, 0U, &result), 0);
  assert_false(result.signaled);
  assert_non_null(result.output);
  assert_true(strstr(result.output, "not found") != NULL ||
              strstr(result.output, "Hit num: 0") != NULL);
  ytest_cmd_result_free(&result);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_case_01_missing_pos_directory, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_02_corrupted_metadata_should_not_segv, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_case_03_malformed_postings_should_not_segv, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_case_04_invalid_percent_escapes_search, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_05_broken_index_over_daemon_path, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_06_front_survives_core_disconnect, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_07_daemon_oversized_query, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_08_daemon_invalid_utf8_bytes, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_09_standard_http_path, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_10_malformed_input_fixture_should_continue, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_case_11_edge_rows_should_continue, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_12_invalid_percent_http_requests, setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_13_finite_score_under_extreme_values, setup,
                                    teardown),
    cmocka_unit_test_setup_teardown(test_case_14_missing_filedata_strings_should_not_print_null,
                                    setup, teardown),
    cmocka_unit_test_setup_teardown(test_case_15_postings_query_bytes_cap, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
