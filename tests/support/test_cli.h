#ifndef YTEST_CLI_H
#define YTEST_CLI_H

#include <stddef.h>

typedef struct {
  int exited;
  int exit_code;
  int signaled;
  int term_signal;
  char *output;
  size_t output_len;
} ytest_cmd_result_t;

void ytest_cmd_result_init(ytest_cmd_result_t *result);
void ytest_cmd_result_free(ytest_cmd_result_t *result);

int ytest_cmd_run(char *const argv[], const char *cwd, const void *stdin_data, size_t stdin_len,
                  ytest_cmd_result_t *result);

#endif
