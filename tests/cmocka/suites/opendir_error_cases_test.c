#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cmocka.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"
#include "test_yappod.h"

static void test_opendir_failure(void **state) {
  ytest_env_t env;
  ytest_cmd_result_t result;
  char input_dir[PATH_MAX];
  char index_dir[PATH_MAX];
  char pos_dir[PATH_MAX];
  const char *args[5];

  (void)state;

  assert_int_equal(ytest_env_init(&env), 0);
  assert_int_equal(ytest_path_join(input_dir, sizeof(input_dir), env.tmp_root, "input"), 0);
  assert_int_equal(ytest_path_join(index_dir, sizeof(index_dir), env.tmp_root, "index"), 0);
  assert_int_equal(ytest_path_join(pos_dir, sizeof(pos_dir), index_dir, "pos"), 0);

  assert_int_equal(ytest_mkdir_p(input_dir, 0700), 0);
  assert_int_equal(ytest_mkdir_p(pos_dir, 0700), 0);
  assert_int_equal(chmod(input_dir, 0000), 0);

  args[0] = "-l";
  args[1] = input_dir;
  args[2] = "-d";
  args[3] = index_dir;
  args[4] = NULL;

  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_tool_run(&env, "yappo_makeindex", args, 4U, NULL, &result), 0);
  assert_true(result.exited);
  assert_int_not_equal(result.exit_code, 0);
  assert_non_null(result.output);
  assert_non_null(strstr(result.output, "ERROR: opendir failed"));
  ytest_cmd_result_free(&result);

  assert_int_equal(chmod(input_dir, 0700), 0);
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_opendir_failure),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
