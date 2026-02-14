#include "test_yappod.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "test_fs.h"

int ytest_bin_path(const ytest_env_t *env, const char *bin_name, char *out, size_t out_size) {
  if (env == NULL || bin_name == NULL || out == NULL || out_size == 0U) {
    errno = EINVAL;
    return -1;
  }
  return ytest_path_join(out, out_size, env->build_dir, bin_name);
}

static int run_tool(const ytest_env_t *env, const char *bin_name, const char *const args[],
                    size_t args_len, const char *cwd, ytest_cmd_result_t *result) {
  char bin_path[PATH_MAX];
  char **argv = NULL;
  size_t i;
  int rc;

  if (ytest_bin_path(env, bin_name, bin_path, sizeof(bin_path)) != 0) {
    return -1;
  }

  argv = (char **)calloc(args_len + 2U, sizeof(char *));
  if (argv == NULL) {
    return -1;
  }

  argv[0] = bin_path;
  for (i = 0; i < args_len; i++) {
    argv[i + 1U] = (char *)args[i];
  }
  argv[args_len + 1U] = NULL;

  rc = ytest_cmd_run(argv, cwd, NULL, 0U, result);
  free(argv);
  return rc;
}

int ytest_tool_run(const ytest_env_t *env, const char *bin_name, const char *const args[],
                   size_t args_len, const char *cwd, ytest_cmd_result_t *result) {
  return run_tool(env, bin_name, args, args_len, cwd, result);
}

int ytest_makeindex_run(const ytest_env_t *env, const char *input_file, const char *index_dir,
                        const char *const extra_args[], size_t extra_args_len,
                        ytest_cmd_result_t *result) {
  const char **args;
  size_t i;
  int rc;

  if (env == NULL || input_file == NULL || index_dir == NULL || result == NULL) {
    errno = EINVAL;
    return -1;
  }

  args = (const char **)calloc(6U + extra_args_len, sizeof(char *));
  if (args == NULL) {
    return -1;
  }

  args[0] = "-f";
  args[1] = input_file;
  args[2] = "-d";
  args[3] = index_dir;
  for (i = 0; i < extra_args_len; i++) {
    args[4U + i] = extra_args[i];
  }

  rc = run_tool(env, "yappo_makeindex", args, 4U + extra_args_len, NULL, result);
  free((void *)args);
  return rc;
}

int ytest_search_run(const ytest_env_t *env, const char *index_dir, const char *const args[],
                     size_t args_len, ytest_cmd_result_t *result) {
  const char **argv;
  size_t i;
  int rc;

  if (env == NULL || index_dir == NULL || result == NULL) {
    errno = EINVAL;
    return -1;
  }

  argv = (const char **)calloc(args_len + 3U, sizeof(char *));
  if (argv == NULL) {
    return -1;
  }

  argv[0] = "-l";
  argv[1] = index_dir;
  for (i = 0; i < args_len; i++) {
    argv[i + 2U] = args[i];
  }

  rc = run_tool(env, "search", argv, args_len + 2U, NULL, result);
  free((void *)argv);
  return rc;
}

int ytest_mergepos_run(const ytest_env_t *env, const char *const args[], size_t args_len,
                       ytest_cmd_result_t *result) {
  return run_tool(env, "yappo_mergepos", args, args_len, NULL, result);
}
