#include "test_cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void ytest_cmd_result_init(ytest_cmd_result_t *result) {
  if (result == NULL) {
    return;
  }
  memset(result, 0, sizeof(*result));
}

void ytest_cmd_result_free(ytest_cmd_result_t *result) {
  if (result == NULL) {
    return;
  }
  free(result->output);
  ytest_cmd_result_init(result);
}

static int append_buf(ytest_cmd_result_t *result, const char *buf, size_t len) {
  char *p;

  if (len == 0U) {
    return 0;
  }

  p = (char *)realloc(result->output, result->output_len + len + 1U);
  if (p == NULL) {
    return -1;
  }
  result->output = p;
  memcpy(result->output + result->output_len, buf, len);
  result->output_len += len;
  result->output[result->output_len] = '\0';
  return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;

  while (len > 0U) {
    ssize_t w = write(fd, p, len);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    p += (size_t)w;
    len -= (size_t)w;
  }
  return 0;
}

int ytest_cmd_run(char *const argv[], const char *cwd, const void *stdin_data, size_t stdin_len,
                  ytest_cmd_result_t *result) {
  int out_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  int need_stdin = stdin_data != NULL && stdin_len > 0U;
  pid_t pid;
  int status = 0;
  char tmp[4096];

  if (argv == NULL || argv[0] == NULL || result == NULL) {
    errno = EINVAL;
    return -1;
  }

  ytest_cmd_result_free(result);

  if (pipe(out_pipe) != 0) {
    return -1;
  }
  if (need_stdin && pipe(in_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    if (need_stdin) {
      close(in_pipe[0]);
      close(in_pipe[1]);
    }
    return -1;
  }

  if (pid == 0) {
    if (cwd != NULL && chdir(cwd) != 0) {
      _exit(127);
    }

    if (need_stdin) {
      close(in_pipe[1]);
      if (dup2(in_pipe[0], STDIN_FILENO) < 0) {
        _exit(127);
      }
      close(in_pipe[0]);
    }

    close(out_pipe[0]);
    if (dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(out_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(out_pipe[1]);

    execv(argv[0], argv);
    _exit(127);
  }

  close(out_pipe[1]);
  if (need_stdin) {
    close(in_pipe[0]);
    if (write_all(in_pipe[1], stdin_data, stdin_len) != 0) {
      close(in_pipe[1]);
      close(out_pipe[0]);
      waitpid(pid, NULL, 0);
      return -1;
    }
    close(in_pipe[1]);
  }

  for (;;) {
    ssize_t r = read(out_pipe[0], tmp, sizeof(tmp));
    if (r == 0) {
      break;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(out_pipe[0]);
      waitpid(pid, NULL, 0);
      return -1;
    }
    if (append_buf(result, tmp, (size_t)r) != 0) {
      close(out_pipe[0]);
      waitpid(pid, NULL, 0);
      return -1;
    }
  }

  close(out_pipe[0]);

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }

  if (WIFEXITED(status)) {
    result->exited = 1;
    result->exit_code = WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    result->signaled = 1;
    result->term_signal = WTERMSIG(status);
  }

  return 0;
}
