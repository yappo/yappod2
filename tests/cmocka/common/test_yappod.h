#ifndef YTEST_YAPPOD_H
#define YTEST_YAPPOD_H

#include <stddef.h>

#include "test_cli.h"
#include "test_env.h"

int ytest_bin_path(const ytest_env_t *env, const char *bin_name, char *out, size_t out_size);

int ytest_tool_run(const ytest_env_t *env, const char *bin_name, const char *const args[],
                   size_t args_len, const char *cwd, ytest_cmd_result_t *result);

int ytest_makeindex_run(const ytest_env_t *env, const char *input_file, const char *index_dir,
                        const char *const extra_args[], size_t extra_args_len,
                        ytest_cmd_result_t *result);

int ytest_search_run(const ytest_env_t *env, const char *index_dir, const char *const args[],
                     size_t args_len, ytest_cmd_result_t *result);

int ytest_mergepos_run(const ytest_env_t *env, const char *const args[], size_t args_len,
                       ytest_cmd_result_t *result);

#endif
