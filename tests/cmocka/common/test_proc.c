#include "test_proc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_cli.h"
#include "test_fs.h"
#include "test_http.h"

static int read_pid_file_once(const char *path, pid_t *pid_out) {
  FILE *fp = NULL;
  long v;

  if (path == NULL || pid_out == NULL) {
    errno = EINVAL;
    return -1;
  }

  fp = fopen(path, "r");
  if (fp == NULL) {
    return -1;
  }
  if (fscanf(fp, "%ld", &v) != 1) {
    fclose(fp);
    errno = EINVAL;
    return -1;
  }
  fclose(fp);

  if (v <= 0) {
    errno = EINVAL;
    return -1;
  }

  *pid_out = (pid_t)v;
  return 0;
}

int ytest_pick_unused_port(int *port_out) {
  int fd;
  struct sockaddr_in addr;
  socklen_t addr_len;
  int port;

  if (port_out == NULL) {
    errno = EINVAL;
    return -1;
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(fd);
    return -1;
  }
  close(fd);

  port = (int)ntohs(addr.sin_port);
  if (port <= 0) {
    errno = EADDRINUSE;
    return -1;
  }

  *port_out = port;
  return 0;
}

void ytest_daemon_stack_init(ytest_daemon_stack_t *stack) {
  if (stack == NULL) {
    return;
  }
  memset(stack, 0, sizeof(*stack));
}

int ytest_wait_for_port(int port, int retries, int sleep_ms) {
  int i;

  for (i = 0; i < retries; i++) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
      close(fd);
      return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      close(fd);
      return 0;
    }

    close(fd);
    usleep((useconds_t)(sleep_ms * 1000));
  }

  errno = ETIMEDOUT;
  return -1;
}

int ytest_read_pid_file(const char *run_dir, const char *name, int retries, int sleep_ms,
                        pid_t *pid_out) {
  char path[PATH_MAX];
  int i;
  int last_errno = ENOENT;

  if (run_dir == NULL || name == NULL || pid_out == NULL || retries <= 0 || sleep_ms < 0) {
    errno = EINVAL;
    return -1;
  }
  if (ytest_path_join(path, sizeof(path), run_dir, name) != 0) {
    return -1;
  }

  for (i = 0; i < retries; i++) {
    if (read_pid_file_once(path, pid_out) == 0) {
      return 0;
    }
    if (errno != 0) {
      last_errno = errno;
    }
    if (i + 1 < retries) {
      usleep((useconds_t)(sleep_ms * 1000));
    }
  }

  errno = last_errno;
  return -1;
}

int ytest_wait_file_contains(const char *path, const char *needle, int retries, int sleep_ms) {
  int i;

  if (path == NULL || needle == NULL || retries <= 0 || sleep_ms < 0) {
    errno = EINVAL;
    return -1;
  }

  for (i = 0; i < retries; i++) {
    char *buf = NULL;
    size_t len = 0U;
    if (ytest_read_file(path, &buf, &len) == 0) {
      int found = (buf != NULL && strstr(buf, needle) != NULL);
      free(buf);
      if (found) {
        return 0;
      }
    }
    if (i + 1 < retries) {
      usleep((useconds_t)(sleep_ms * 1000));
    }
  }

  errno = ENOENT;
  return -1;
}

void ytest_stop_pid_if_alive(pid_t pid, int retries, int sleep_ms) {
  int i;

  if (pid <= 0 || retries <= 0 || sleep_ms < 0) {
    return;
  }
  if (kill(pid, 0) != 0) {
    return;
  }

  kill(pid, SIGTERM);
  for (i = 0; i < retries; i++) {
    if (kill(pid, 0) != 0) {
      return;
    }
    usleep((useconds_t)(sleep_ms * 1000));
  }
  kill(pid, SIGKILL);
}

static int wait_front_ready(int front_port, int retries, int sleep_ms) {
  static const char *k_ready_request =
    "GET /d/1/OR/0-1?__ytest_ready__ HTTP/1.0\r\nHost: localhost\r\n\r\n";
  int i;

  if (front_port <= 0 || retries <= 0 || sleep_ms < 0) {
    errno = EINVAL;
    return -1;
  }

  for (i = 0; i < retries; i++) {
    char *response = NULL;
    int rc = ytest_http_send_text(front_port, k_ready_request, &response);

    if (rc == 0) {
      int ok = (response != NULL && strstr(response, "HTTP/1.0") != NULL);
      free(response);
      if (ok) {
        return 0;
      }
    } else {
      free(response);
    }

    usleep((useconds_t)(sleep_ms * 1000));
  }

  errno = ETIMEDOUT;
  return -1;
}

static int launch_core_daemon(const char *bin_path, const char *index_dir, int core_port,
                              const char *cwd) {
  ytest_cmd_result_t result;
  char core_port_str[16];
  char *argv_core[6];

  if (snprintf(core_port_str, sizeof(core_port_str), "%d", core_port) >=
      (int)sizeof(core_port_str)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  ytest_cmd_result_init(&result);
  argv_core[0] = (char *)bin_path;
  argv_core[1] = "-l";
  argv_core[2] = (char *)index_dir;
  argv_core[3] = "-p";
  argv_core[4] = core_port_str;
  argv_core[5] = NULL;

  if (ytest_cmd_run(argv_core, cwd, NULL, 0U, &result) != 0) {
    ytest_cmd_result_free(&result);
    return -1;
  }

  if (!result.exited || result.exit_code != 0) {
    ytest_cmd_result_free(&result);
    errno = ECHILD;
    return -1;
  }

  ytest_cmd_result_free(&result);
  return 0;
}

static int launch_front_daemon(const char *bin_path, const char *index_dir, const char *server,
                               int front_port, int core_port, const char *cwd) {
  ytest_cmd_result_t result;
  char front_port_str[16];
  char core_port_str[16];
  char *argv_front[10];

  if (server == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (snprintf(front_port_str, sizeof(front_port_str), "%d", front_port) >=
          (int)sizeof(front_port_str) ||
      snprintf(core_port_str, sizeof(core_port_str), "%d", core_port) >=
        (int)sizeof(core_port_str)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  ytest_cmd_result_init(&result);
  argv_front[0] = (char *)bin_path;
  argv_front[1] = "-l";
  argv_front[2] = (char *)index_dir;
  argv_front[3] = "-s";
  argv_front[4] = (char *)server;
  argv_front[5] = "-p";
  argv_front[6] = front_port_str;
  argv_front[7] = "-P";
  argv_front[8] = core_port_str;
  argv_front[9] = NULL;

  if (ytest_cmd_run(argv_front, cwd, NULL, 0U, &result) != 0) {
    ytest_cmd_result_free(&result);
    return -1;
  }

  if (!result.exited || result.exit_code != 0) {
    ytest_cmd_result_free(&result);
    errno = ECHILD;
    return -1;
  }

  ytest_cmd_result_free(&result);
  return 0;
}

int ytest_daemon_stack_start(ytest_daemon_stack_t *stack, const char *build_dir,
                             const char *index_dir, const char *run_dir) {
  char core_bin[PATH_MAX];
  char front_bin[PATH_MAX];
  char core_pid_path[PATH_MAX];
  char front_pid_path[PATH_MAX];
  int attempt;
  int last_errno = EADDRINUSE;

  if (stack == NULL || build_dir == NULL || index_dir == NULL || run_dir == NULL) {
    errno = EINVAL;
    return -1;
  }

  ytest_daemon_stack_stop(stack);

  if (snprintf(stack->run_dir, sizeof(stack->run_dir), "%s", run_dir) >=
      (int)sizeof(stack->run_dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  if (ytest_mkdir_p(stack->run_dir, 0700) != 0) {
    return -1;
  }

  if (ytest_path_join(core_bin, sizeof(core_bin), build_dir, "yappod_core") != 0 ||
      ytest_path_join(front_bin, sizeof(front_bin), build_dir, "yappod_front") != 0 ||
      ytest_path_join(core_pid_path, sizeof(core_pid_path), stack->run_dir, "core.pid") != 0 ||
      ytest_path_join(front_pid_path, sizeof(front_pid_path), stack->run_dir, "front.pid") !=
        0) {
    return -1;
  }

  for (attempt = 0; attempt < 20; attempt++) {
    int rc_core_port;
    int rc_front_port;
    int pick_errno = 0;

    unlink(core_pid_path);
    unlink(front_pid_path);

    stack->core_pid = 0;
    stack->front_pid = 0;
    stack->core_port = 0;
    stack->front_port = 0;

    rc_core_port = ytest_pick_unused_port(&stack->core_port);
    if (rc_core_port != 0) {
      pick_errno = errno;
    }
    rc_front_port = ytest_pick_unused_port(&stack->front_port);
    if (rc_front_port != 0 && pick_errno == 0) {
      pick_errno = errno;
    }

    if (rc_core_port != 0 || rc_front_port != 0 ||
        stack->core_port == stack->front_port) {
      if (stack->core_port == stack->front_port) {
        errno = EADDRINUSE;
      }
      if (pick_errno != 0) {
        errno = pick_errno;
      }
      if (errno != 0) {
        last_errno = errno;
      }
      usleep(200000);
      continue;
    }
    fprintf(stderr, "[TEST] daemon attempt=%d core_port=%d front_port=%d\n", attempt + 1,
            stack->core_port, stack->front_port);

    if (launch_core_daemon(core_bin, index_dir, stack->core_port, stack->run_dir) == 0 &&
        ytest_read_pid_file(stack->run_dir, "core.pid", 100, 20, &stack->core_pid) == 0 &&
        kill(stack->core_pid, 0) == 0 &&
        ytest_wait_for_port(stack->core_port, 50, 100) == 0 &&
        launch_front_daemon(front_bin, index_dir, "127.0.0.1", stack->front_port,
                            stack->core_port, stack->run_dir) == 0 &&
        ytest_read_pid_file(stack->run_dir, "front.pid", 100, 20, &stack->front_pid) == 0 &&
        kill(stack->front_pid, 0) == 0 &&
        ytest_wait_for_port(stack->front_port, 50, 100) == 0 &&
        wait_front_ready(stack->front_port, 50, 100) == 0) {
      return 0;
    }
    if (errno != 0) {
      last_errno = errno;
    }

    ytest_daemon_stack_stop(stack);
    usleep(200000);
  }

  errno = last_errno;
  return -1;
}

int ytest_daemon_stack_alive(const ytest_daemon_stack_t *stack) {
  if (stack == NULL || stack->core_pid <= 0 || stack->front_pid <= 0) {
    return 0;
  }
  return kill(stack->core_pid, 0) == 0 && kill(stack->front_pid, 0) == 0;
}

void ytest_daemon_stack_stop(ytest_daemon_stack_t *stack) {
  if (stack == NULL) {
    return;
  }

  ytest_stop_pid_if_alive(stack->front_pid, 20, 100);
  ytest_stop_pid_if_alive(stack->core_pid, 20, 100);
  stack->front_pid = 0;
  stack->core_pid = 0;
  stack->front_port = 0;
  stack->core_port = 0;
}

void ytest_daemon_stack_dump_logs(const ytest_daemon_stack_t *stack, FILE *out) {
  static const char *files[] = {
    "core.pid", "front.pid", "core.log", "core.error", "front.log", "front.error",
  };
  size_t i;

  if (stack == NULL || out == NULL || stack->run_dir[0] == '\0') {
    return;
  }

  fprintf(out, "[DEBUG] daemon run dir: %s\n", stack->run_dir);
  fprintf(out, "[DEBUG] daemon ports: front=%d core=%d\n", stack->front_port, stack->core_port);
  for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char path[PATH_MAX];
    char *data = NULL;
    size_t len = 0U;

    if (ytest_path_join(path, sizeof(path), stack->run_dir, files[i]) != 0) {
      continue;
    }
    if (ytest_read_file(path, &data, &len) != 0) {
      continue;
    }

    fprintf(out, "[DEBUG] ----- %s -----\n", files[i]);
    if (len > 0U) {
      fwrite(data, 1U, len, out);
      if (data[len - 1] != '\n') {
        fputc('\n', out);
      }
    }
    fprintf(out, "[DEBUG] --------------------\n");
    free(data);
  }
}
