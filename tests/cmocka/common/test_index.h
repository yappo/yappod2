#ifndef YTEST_INDEX_H
#define YTEST_INDEX_H

#include <stddef.h>

#include "test_cli.h"
#include "test_env.h"

int ytest_build_index(const ytest_env_t *env, const char *input_file, const char *index_dir,
                      const char *const extra_args[], size_t extra_args_len);

int ytest_search_capture(const ytest_env_t *env, const char *index_dir, const char *const args[],
                        size_t args_len, ytest_cmd_result_t *result);

int ytest_search_expect_contains(const ytest_env_t *env, const char *index_dir, const char *query,
                                const char *needle);

int ytest_search_expect_contains_args(const ytest_env_t *env, const char *index_dir,
                                     const char *const args[], size_t args_len,
                                     const char *needle);

int ytest_search_expect_not_contains_args(const ytest_env_t *env, const char *index_dir,
                                         const char *const args[], size_t args_len,
                                         const char *needle);

int ytest_search_expect_no_hit(const ytest_env_t *env, const char *index_dir, const char *query);

int ytest_search_expect_not_signaled(const ytest_env_t *env, const char *index_dir,
                                     const char *query);

#endif
