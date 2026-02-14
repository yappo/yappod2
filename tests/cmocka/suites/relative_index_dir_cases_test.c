#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "test_yappod.h"

static void test_relative_index_dir(void **state) {
  ytest_env_t env;
  ytest_cmd_result_t result;
  char pos_dir[PATH_MAX];
  const char *makeindex_args[] = {"-f", NULL, "-d", "."};
  const char *search_args[] = {"-l", NULL, "OpenAI2025"};

  (void)state;

  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(pos_dir, sizeof(pos_dir), env.tmp_root, "pos"), 0);
  assert_int_equal(ytest_mkdir_p(pos_dir, 0700), 0);

  makeindex_args[1] = env.fixture_path;
  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(&env, "yappo_makeindex", makeindex_args, 4U, env.tmp_root, &result),
                   0);
  assert_true(result.exited);
  assert_int_equal(result.exit_code, 0);
  ytest_cmd_result_free(&result);

  search_args[1] = env.tmp_root;
  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(&env, "search", search_args, 3U, NULL, &result), 0);
  assert_true(result.exited);
  assert_int_equal(result.exit_code, 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, "http://example.com/doc1"));
  ytest_cmd_result_free(&result);

  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_relative_index_dir),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
