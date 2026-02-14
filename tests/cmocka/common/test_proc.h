#ifndef YTEST_PROC_H
#define YTEST_PROC_H

#include <limits.h>
#include <stdio.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  char run_dir[PATH_MAX];
  pid_t core_pid;
  pid_t front_pid;
  int core_port;
  int front_port;
} ytest_daemon_stack_t;

void ytest_daemon_stack_init(ytest_daemon_stack_t *stack);
int ytest_pick_unused_port(int *port_out);
int ytest_wait_for_port(int port, int retries, int sleep_ms);
int ytest_read_pid_file(const char *run_dir, const char *name, int retries, int sleep_ms,
                        pid_t *pid_out);
int ytest_wait_file_contains(const char *path, const char *needle, int retries, int sleep_ms);
void ytest_stop_pid_if_alive(pid_t pid, int retries, int sleep_ms);
int ytest_daemon_stack_start(ytest_daemon_stack_t *stack, const char *build_dir,
                             const char *index_dir, const char *run_dir);
int ytest_daemon_stack_alive(const ytest_daemon_stack_t *stack);
void ytest_daemon_stack_stop(ytest_daemon_stack_t *stack);
void ytest_daemon_stack_dump_logs(const ytest_daemon_stack_t *stack, FILE *out);

#endif
