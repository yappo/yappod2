#include "test_index.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "test_fs.h"
#include "test_yappod.h"

int ytest_build_index(const ytest_env_t *env, const char *input_file, const char *index_dir,
                      const char *const extra_args[], size_t extra_args_len) {
  char pos_dir[PATH_MAX];
  ytest_cmd_result_t result;

  if (env == NULL || input_file == NULL || index_dir == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (ytest_mkdir_p(index_dir, 0700) != 0 ||
      ytest_path_join(pos_dir, sizeof(pos_dir), index_dir, "pos") != 0 ||
      ytest_mkdir_p(pos_dir, 0700) != 0) {
    return -1;
  }

  ytest_cmd_result_init(&result);
  if (ytest_makeindex_run(env, input_file, index_dir, extra_args, extra_args_len, &result) != 0 ||
      !result.exited || result.exit_code != 0) {
    ytest_cmd_result_free(&result);
    errno = EINVAL;
    return -1;
  }

  ytest_cmd_result_free(&result);
  return 0;
}

int ytest_search_capture(const ytest_env_t *env, const char *index_dir, const char *const args[],
                        size_t args_len, ytest_cmd_result_t *result) {
  if (env == NULL || index_dir == NULL || result == NULL) {
    errno = EINVAL;
    return -1;
  }

  ytest_cmd_result_init(result);
  if (ytest_search_run(env, index_dir, args, args_len, result) != 0 || !result->exited) {
    ytest_cmd_result_free(result);
    return -1;
  }

  return 0;
}

int ytest_search_expect_contains_args(const ytest_env_t *env, const char *index_dir,
                                     const char *const args[], size_t args_len,
                                     const char *needle) {
  ytest_cmd_result_t result;
  int ok;

  if (needle == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (ytest_search_capture(env, index_dir, args, args_len, &result) != 0) {
    return -1;
  }
  ok = (result.output != NULL && strstr(result.output, needle) != NULL);
  ytest_cmd_result_free(&result);
  return ok ? 0 : -1;
}

int ytest_search_expect_not_contains_args(const ytest_env_t *env, const char *index_dir,
                                         const char *const args[], size_t args_len,
                                         const char *needle) {
  ytest_cmd_result_t result;
  int ok;

  if (needle == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (ytest_search_capture(env, index_dir, args, args_len, &result) != 0) {
    return -1;
  }
  ok = (result.output == NULL || strstr(result.output, needle) == NULL);
  ytest_cmd_result_free(&result);
  return ok ? 0 : -1;
}

int ytest_search_expect_contains(const ytest_env_t *env, const char *index_dir, const char *query,
                                const char *needle) {
  const char *args[1];

  if (query == NULL) {
    errno = EINVAL;
    return -1;
  }
  args[0] = query;
  return ytest_search_expect_contains_args(env, index_dir, args, 1U, needle);
}

int ytest_search_expect_no_hit(const ytest_env_t *env, const char *index_dir, const char *query) {
  ytest_cmd_result_t result;
  const char *args[1];
  int ok;

  if (query == NULL) {
    errno = EINVAL;
    return -1;
  }
  args[0] = query;

  if (ytest_search_capture(env, index_dir, args, 1U, &result) != 0) {
    return -1;
  }

  ok = (result.output != NULL &&
        (strstr(result.output, "Hit num: 0") != NULL || strstr(result.output, "not found") != NULL));
  ytest_cmd_result_free(&result);
  return ok ? 0 : -1;
}

int ytest_search_expect_not_signaled(const ytest_env_t *env, const char *index_dir,
                                     const char *query) {
  ytest_cmd_result_t result;
  const char *args[1];

  if (query == NULL) {
    errno = EINVAL;
    return -1;
  }
  args[0] = query;

  ytest_cmd_result_init(&result);
  if (ytest_search_run(env, index_dir, args, 1U, &result) != 0) {
    return -1;
  }
  if (result.signaled) {
    ytest_cmd_result_free(&result);
    errno = EINTR;
    return -1;
  }

  ytest_cmd_result_free(&result);
  return 0;
}
