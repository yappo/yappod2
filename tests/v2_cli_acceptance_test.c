#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "test_cli.h"
#include "test_env.h"
#include "test_fs.h"

static const char config_text[] =
  "format_version=2\n"
  "[tokenizer]\n"
  "id=\"unicode_nfkc_cf_v1\"\n"
  "[chunking]\n"
  "max_chars=256\n"
  "overlap_chars=0\n"
  "[vector]\n"
  "enabled=true\n"
  "model_id=\"acceptance-2d-v1\"\n"
  "dimensions=2\n"
  "metric=\"cosine\"\n"
  "[metadata]\n"
  "filterable_fields=[\"category\"]\n";

static const char documents[] =
  "{\"operation\":\"upsert\",\"id\":\"doc-red\","
  "\"url\":\"https://example.test/red\",\"title\":\"Red guide\","
  "\"body\":\"scarlet search handbook\","
  "\"metadata\":{\"category\":\"guide\"},\"vectors\":[[1,0]]}\n"
  "{\"operation\":\"upsert\",\"id\":\"doc-blue\","
  "\"url\":\"https://example.test/blue\",\"title\":\"Blue guide\","
  "\"body\":\"azure retrieval manual\","
  "\"metadata\":{\"category\":\"manual\"},\"vectors\":[[0,1]]}\n";

static void run_command(char *const argv[], int expected_exit, const char *expected_output) {
  ytest_cmd_result_t result;
  ytest_cmd_result_init(&result);
  assert_int_equal(ytest_cmd_run(argv, NULL, NULL, 0U, &result), 0);
  assert_true(result.exited);
  if (result.exit_code != expected_exit)
    print_message("command output: %s\n", result.output == NULL ? "" : result.output);
  assert_int_equal(result.exit_code, expected_exit);
  if (expected_output != NULL) {
    assert_non_null(result.output);
    assert_non_null(strstr(result.output, expected_output));
  }
  ytest_cmd_result_free(&result);
}

static void create_inputs(const ytest_env_t *env, char *config, char *input, char *index) {
  assert_int_equal(ytest_path_join(config, PATH_MAX, env->tmp_root, "config.toml"), 0);
  assert_int_equal(ytest_path_join(input, PATH_MAX, env->tmp_root, "documents.ndjson"), 0);
  assert_int_equal(ytest_path_join(index, PATH_MAX, env->tmp_root, "index"), 0);
  assert_int_equal(ytest_write_file(config, config_text, sizeof(config_text) - 1U), 0);
  assert_int_equal(ytest_write_file(input, documents, sizeof(documents) - 1U), 0);
}

static void test_build_and_all_search_modes(void **state) {
  ytest_env_t env;
  char config[PATH_MAX], input[PATH_MAX], index[PATH_MAX];
  char makeindex[PATH_MAX], search[PATH_MAX];
  char *build_argv[9], *lexical_argv[9], *vector_argv[9], *hybrid_argv[11];
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  create_inputs(&env, config, input, index);
  assert_int_equal(ytest_path_join(makeindex, sizeof(makeindex), env.build_dir,
                                   "yappo_makeindex"), 0);
  assert_int_equal(ytest_path_join(search, sizeof(search), env.build_dir, "search"), 0);

  build_argv[0] = makeindex; build_argv[1] = "build"; build_argv[2] = "--config";
  build_argv[3] = config; build_argv[4] = "--input"; build_argv[5] = input;
  build_argv[6] = "--index"; build_argv[7] = index; build_argv[8] = NULL;
  run_command(build_argv, 0, "\"accepted\":2");

  lexical_argv[0] = search; lexical_argv[1] = "--index"; lexical_argv[2] = index;
  lexical_argv[3] = "--mode"; lexical_argv[4] = "lexical";
  lexical_argv[5] = "--query"; lexical_argv[6] = "scarlet";
  lexical_argv[7] = NULL;
  run_command(lexical_argv, 0, "doc-red");

  vector_argv[0] = search; vector_argv[1] = "--index"; vector_argv[2] = index;
  vector_argv[3] = "--mode"; vector_argv[4] = "vector";
  vector_argv[5] = "--vector"; vector_argv[6] = "0,1";
  vector_argv[7] = NULL;
  run_command(vector_argv, 0, "\"results\":[{\"id\":\"doc-blue\"");

  hybrid_argv[0] = search; hybrid_argv[1] = "--index"; hybrid_argv[2] = index;
  hybrid_argv[3] = "--mode"; hybrid_argv[4] = "hybrid";
  hybrid_argv[5] = "--query"; hybrid_argv[6] = "scarlet";
  hybrid_argv[7] = "--vector"; hybrid_argv[8] = "1,0";
  hybrid_argv[9] = NULL;
  run_command(hybrid_argv, 0, "\"results\":[{\"id\":\"doc-red\"");
  ytest_env_destroy(&env);
}

static void test_build_is_atomic_and_legacy_cli_is_rejected(void **state) {
  ytest_env_t env;
  char config[PATH_MAX], input[PATH_MAX], index[PATH_MAX], invalid[PATH_MAX];
  char makeindex[PATH_MAX], search[PATH_MAX], marker[PATH_MAX];
  char *invalid_argv[9], *legacy_build[6], *legacy_search[5];
  (void)state;
  assert_int_equal(ytest_env_init(&env), 0);
  create_inputs(&env, config, input, index);
  assert_int_equal(ytest_path_join(invalid, sizeof(invalid), env.tmp_root, "invalid.ndjson"), 0);
  assert_int_equal(ytest_write_file(invalid, "not-json\n", 9U), 0);
  assert_int_equal(ytest_path_join(makeindex, sizeof(makeindex), env.build_dir,
                                   "yappo_makeindex"), 0);
  assert_int_equal(ytest_path_join(search, sizeof(search), env.build_dir, "search"), 0);

  invalid_argv[0] = makeindex; invalid_argv[1] = "build"; invalid_argv[2] = "--config";
  invalid_argv[3] = config; invalid_argv[4] = "--input"; invalid_argv[5] = invalid;
  invalid_argv[6] = "--index"; invalid_argv[7] = index; invalid_argv[8] = NULL;
  run_command(invalid_argv, 1, "Build failed");
  assert_int_equal(access(index, F_OK), -1);

  assert_int_equal(ytest_mkdir_p(index, 0700), 0);
  assert_int_equal(ytest_path_join(marker, sizeof(marker), index, "marker"), 0);
  assert_int_equal(ytest_write_file(marker, "keep", 4U), 0);
  invalid_argv[5] = input;
  run_command(invalid_argv, 1, "already exists");
  assert_int_equal(access(marker, F_OK), 0);

  legacy_build[0] = makeindex; legacy_build[1] = "-f"; legacy_build[2] = input;
  legacy_build[3] = "-d"; legacy_build[4] = index; legacy_build[5] = NULL;
  run_command(legacy_build, 1, "Unknown command");
  legacy_search[0] = search; legacy_search[1] = "-l"; legacy_search[2] = index;
  legacy_search[3] = "scarlet"; legacy_search[4] = NULL;
  run_command(legacy_search, 1, "Usage:");
  ytest_env_destroy(&env);
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_build_and_all_search_modes),
    cmocka_unit_test(test_build_is_atomic_and_legacy_cli_is_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
