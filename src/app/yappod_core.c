#include "server/yappo_http_v2.h"
#include "server/yappo_core_reactor_v2.h"
#include "server/yappo_executor_v2.h"
#include "config/yappo_runtime_policy_v2.h"
#include "config/yappo_application_config.h"
#include "indexing/yappo_compact_v2.h"

#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define DEFAULT_CORE_PORT 18401
#define MAINTENANCE_POLL_INTERVAL_MS 250U
#define MAINTENANCE_IDLE_SAMPLES 2U
#define INGEST_BATCH_DELAY_MICROSECONDS 10000U
typedef struct {
  const char *index_dir;
  YAP_V2_HTTP_RUNTIME *http_runtime;
  YAP_V2_RUNTIME_LIMITER *search_limiter;
  YAP_V2_RUNTIME_LIMITER *writer_limiter;
  YAP_V2_COMPACTION_POLICY policy;
} maintenance_t;

static volatile sig_atomic_t shutdown_requested = 0;
static int listen_socket = -1;
static char pid_file[YAP_APPLICATION_PATH_BYTES] = "core.pid";
static char log_file[YAP_APPLICATION_PATH_BYTES] = "core.log";
static char error_file[YAP_APPLICATION_PATH_BYTES] = "core.error";
static YAP_V2_RUNTIME_POLICY runtime_policy;
static YAP_V2_COMPACTION_POLICY compaction_policy;
static YAP_V2_RUNTIME_LIMITER runtime_limiter;
static YAP_V2_RUNTIME_LIMITER writer_limiter;

static void usage(FILE *output, const char *program) {
  fprintf(output,
          "Usage: %s [--foreground] (--config CONFIG | --index INDEX_DIR [--port PORT])\n"
          "  --foreground       Run without forking or redirecting output\n"
          "  --index INDEX_DIR  Valid v2 index snapshot (required)\n"
          "  --config CONFIG    Shared application TOML\n"
          "  --port PORT        Internal HTTP port (default: %d)\n",
          program, DEFAULT_CORE_PORT);
}

static int parse_port(const char *text, int *port) {
  char *end = NULL;
  long value;
  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) return -1;
  *port = (int)value;
  return 0;
}

static void request_shutdown(int signal_number) {
  (void)signal_number;
  shutdown_requested = 1;
  if (listen_socket >= 0) (void)shutdown(listen_socket, SHUT_RDWR);
}

static int prepare_signal_wait(sigset_t *shutdown_signals) {
  struct sigaction action;
  if (sigemptyset(shutdown_signals) != 0 ||
      sigaddset(shutdown_signals, SIGTERM) != 0 ||
      sigaddset(shutdown_signals, SIGINT) != 0 ||
      pthread_sigmask(SIG_BLOCK, shutdown_signals, NULL) != 0)
    return -1;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  return sigaction(SIGPIPE, &action, NULL);
}

static void remove_pid_file(void) { (void)unlink(pid_file); }

static int redirect_stream(FILE *stream, const char *path, const char *mode) {
  FILE *opened = fopen(path, mode);
  if (opened == NULL) return -1;
  if (dup2(fileno(opened), fileno(stream)) < 0) { (void)fclose(opened); return -1; }
  return fclose(opened);
}

static int mkdir_p(const char *path) {
  char copy[YAP_APPLICATION_PATH_BYTES];
  char *cursor;
  size_t length = strlen(path);
  if (length == 0U || length >= sizeof(copy)) return -1;
  memcpy(copy, path, length + 1U);
  for (cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) != 0 && errno != EEXIST) return -1;
    *cursor = '/';
  }
  return mkdir(copy, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int set_run_paths(const char *directory) {
  if (mkdir_p(directory) != 0 ||
      snprintf(pid_file, sizeof(pid_file), "%s/core.pid", directory) >= (int)sizeof(pid_file) ||
      snprintf(log_file, sizeof(log_file), "%s/core.log", directory) >= (int)sizeof(log_file) ||
      snprintf(error_file, sizeof(error_file), "%s/core.error", directory) >= (int)sizeof(error_file))
    return -1;
  return 0;
}

static int daemonize_process(void) {
  pid_t child = fork();
  FILE *file;
  if (child < 0) return -1;
  if (child > 0) return 1;
  if (setsid() < 0 || redirect_stream(stdin, "/dev/null", "r") != 0 ||
      redirect_stream(stdout, log_file, "a") != 0 ||
      redirect_stream(stderr, error_file, "a") != 0) return -1;
  file = fopen(pid_file, "w");
  if (file == NULL || fprintf(file, "%ld\n", (long)getpid()) < 0 || fclose(file) != 0)
    return -1;
  if (atexit(remove_pid_file) != 0) return -1;
  return 0;
}

static int create_listener(const char *host, int port) {
  struct addrinfo hints, *addresses = NULL, *address;
  char port_text[16];
  int descriptor = -1, reuse = 1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
  (void)snprintf(port_text, sizeof(port_text), "%d", port);
  if (getaddrinfo(host, port_text, &hints, &addresses) != 0) return -1;
  for (address = addresses; address != NULL; address = address->ai_next) {
    descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (descriptor < 0) continue;
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
        bind(descriptor, address->ai_addr, address->ai_addrlen) == 0 &&
        listen(descriptor, SOMAXCONN) == 0) break;
    (void)close(descriptor); descriptor = -1;
  }
  freeaddrinfo(addresses);
  return descriptor;
}

static void *run_reloader(void *opaque) {
  YAP_V2_HTTP_RUNTIME *http_runtime = opaque;
  struct timespec interval = {1, 0};
  while (!shutdown_requested) {
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR && !shutdown_requested) {}
    interval.tv_sec = 1; interval.tv_nsec = 0;
    if (!shutdown_requested) (void)YAP_V2_http_runtime_reload(http_runtime);
  }
  return NULL;
}

static void sleep_maintenance_interval(uint32_t interval_ms) {
  uint32_t remaining = interval_ms;
  while (!shutdown_requested && remaining > 0U) {
    uint32_t slice = remaining > 1000U ? 1000U : remaining;
    struct timespec interval;
    interval.tv_sec = (time_t)(slice / 1000U);
    interval.tv_nsec = (long)(slice % 1000U) * 1000000L;
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR &&
           !shutdown_requested) {}
    if (remaining >= slice) remaining -= slice;
  }
}

static int maintenance_has_foreground_work(maintenance_t *maintenance) {
  size_t search_inflight = 0U, search_bytes = 0U;
  size_t writer_inflight = 0U, writer_bytes = 0U;
  size_t ignored_count = 0U, ignored_bytes = 0U;
  if (YAP_V2_runtime_limiter_snapshot(
        maintenance->search_limiter, &search_inflight, &search_bytes,
        &ignored_count, &ignored_bytes) != YAP_V2_OK ||
      YAP_V2_runtime_limiter_snapshot(
        maintenance->writer_limiter, &writer_inflight, &writer_bytes,
        &ignored_count, &ignored_bytes) != YAP_V2_OK)
    return 1;
  return search_inflight != 0U || search_bytes != 0U ||
         writer_inflight != 0U || writer_bytes != 0U;
}

static uint64_t monotonic_milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void *run_maintenance(void *opaque) {
  maintenance_t *maintenance = opaque;
  uint64_t next_compaction = monotonic_milliseconds() +
                             maintenance->policy.check_interval_ms;
  unsigned int idle_samples = 0U;
#ifdef __APPLE__
  (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
  while (!shutdown_requested) {
    int compacted = 0;
    size_t small_segments = 0U;
    char error[256] = {0};
    YAP_V2_COMPACTION_RESULT result;
    uint64_t now;
    sleep_maintenance_interval(MAINTENANCE_POLL_INTERVAL_MS);
    if (shutdown_requested) break;
    if (maintenance_has_foreground_work(maintenance)) {
      YAP_V2_http_runtime_record_maintenance_deferral(
        maintenance->http_runtime);
      idle_samples = 0U;
      continue;
    }
    if (++idle_samples < MAINTENANCE_IDLE_SAMPLES) continue;
    idle_samples = 0U;
    (void)YAP_V2_http_runtime_maintain_ann(maintenance->http_runtime);
    now = monotonic_milliseconds();
    if (!maintenance->policy.enabled || now < next_compaction) continue;
    next_compaction = now + maintenance->policy.check_interval_ms;
    YAP_V2_compaction_result_init(&result);
    if (YAP_V2_compact_if_needed(
          maintenance->index_dir, &maintenance->policy, &result,
          &compacted, &small_segments, error, sizeof(error)) == YAP_V2_OK) {
      if (compacted)
        {
          (void)YAP_V2_http_runtime_reload(maintenance->http_runtime);
          (void)YAP_V2_http_runtime_maintain_ann(maintenance->http_runtime);
        }
    } else {
      fprintf(stderr,
              "Automatic compaction check or run failed for %zu small "
              "segments: %s\n",
              small_segments, error);
    }
    YAP_V2_compaction_result_free(&result);
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *index_dir = NULL, *config_path = NULL;
  const char *listen_host = NULL;
  YAP_APPLICATION_CONFIG application;
  int port = DEFAULT_CORE_PORT, i, daemon_status;
  char policy_error[256] = {0};
  int have_port = 0;
  sigset_t shutdown_signals;
  YAP_V2_HTTP_RUNTIME http_runtime;
  pthread_t reloader_thread, maintenance_thread;
  maintenance_t maintenance;
  size_t io_threads = YAP_APPLICATION_DEFAULT_IO_THREADS;
  size_t search_threads = YAP_APPLICATION_DEFAULT_SEARCH_THREADS;
  size_t writer_queue_capacity = 1U;
  size_t writer_queue_bytes = YAP_APPLICATION_DEFAULT_WRITER_QUEUE_BYTES;
  int foreground = 0, reloader_started = 0, maintenance_started = 0;
  YAP_V2_EXECUTOR search_executor, writer_executor;
  YAP_V2_CORE_REACTOR_SERVER reactor_server;
  YAP_V2_http_runtime_init(&http_runtime);
  YAP_V2_executor_init(&search_executor);
  YAP_V2_executor_init(&writer_executor);
  YAP_V2_core_reactor_server_init(&reactor_server);
  YAP_V2_compaction_policy_init(&compaction_policy);
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (strcmp(argv[i], "--foreground") == 0) {
      foreground = 1;
    } else if (strcmp(argv[i], "--index") == 0 || strcmp(argv[i], "--config") == 0) {
      if (++i >= argc) { usage(stderr, argv[0]); return EXIT_FAILURE; }
      if (strcmp(argv[i - 1], "--index") == 0) index_dir = argv[i]; else config_path = argv[i];
    } else if (strcmp(argv[i], "--port") == 0) {
      have_port = 1;
      if (++i >= argc || parse_port(argv[i], &port) != 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (config_path != NULL && (index_dir != NULL || have_port)) {
    fputs("--config cannot be combined with --index or --port\n", stderr);
    return EXIT_FAILURE;
  }
  if (config_path != NULL) {
    if (YAP_application_config_load(config_path, &application, policy_error,
                                    sizeof(policy_error)) != YAP_V2_OK) {
      fprintf(stderr, "Invalid application config: %s\n", policy_error);
      return EXIT_FAILURE;
    }
    index_dir = application.index_directory;
    listen_host = application.core_host;
    port = application.core_port;
    runtime_policy = application.runtime_policy;
    io_threads = application.core_io_threads;
    search_threads = application.core_search_threads;
    writer_queue_capacity = application.core_writer_queue_capacity;
    writer_queue_bytes = application.core_writer_queue_bytes;
    compaction_policy = application.compaction_policy;
    if (!foreground && set_run_paths(application.run_directory) != 0) {
      fprintf(stderr, "Cannot create run directory: %s\n", strerror(errno));
      return EXIT_FAILURE;
    }
  } else {
    YAP_V2_runtime_policy_init(&runtime_policy);
  }
  if (index_dir == NULL || YAP_V2_http_runtime_open(&http_runtime, index_dir) != YAP_V2_OK) {
    fprintf(stderr, "Invalid v2 index\n");
    return EXIT_FAILURE;
  }
  memset(&runtime_limiter, 0, sizeof(runtime_limiter));
  memset(&writer_limiter, 0, sizeof(writer_limiter));
  if (YAP_V2_runtime_limiter_init(&runtime_limiter, &runtime_policy) != YAP_V2_OK) {
    fprintf(stderr, "Invalid runtime policy: %s\n", policy_error);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  {
    YAP_V2_RUNTIME_POLICY writer_policy = runtime_policy;
    writer_policy.max_inflight = writer_queue_capacity + 1U;
    writer_policy.max_inflight_bytes = writer_queue_bytes;
    if (YAP_V2_runtime_limiter_init(&writer_limiter, &writer_policy) != YAP_V2_OK) {
      fputs("Invalid writer queue policy\n", stderr);
      YAP_V2_runtime_limiter_close(&runtime_limiter);
      YAP_V2_http_runtime_close(&http_runtime);
      return EXIT_FAILURE;
    }
  }
  listen_socket = create_listener(listen_host, port);
  if (listen_socket < 0) {
    fprintf(stderr, "Cannot listen on port %d: %s\n", port, strerror(errno));
    YAP_V2_executor_close(&writer_executor);
    YAP_V2_executor_close(&search_executor);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_runtime_limiter_close(&writer_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  if (!foreground) {
    daemon_status = daemonize_process();
    if (daemon_status != 0) {
      (void)close(listen_socket);
      YAP_V2_executor_close(&writer_executor);
      YAP_V2_executor_close(&search_executor);
      YAP_V2_runtime_limiter_close(&runtime_limiter);
      YAP_V2_runtime_limiter_close(&writer_limiter);
      YAP_V2_http_runtime_close(&http_runtime);
      return daemon_status > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }
  if (prepare_signal_wait(&shutdown_signals) != 0) {
    (void)close(listen_socket); listen_socket = -1;
    YAP_V2_executor_close(&writer_executor);
    YAP_V2_executor_close(&search_executor);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_runtime_limiter_close(&writer_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  if (YAP_V2_executor_open(&search_executor, search_threads,
                           runtime_policy.max_inflight) != YAP_V2_OK ||
      YAP_V2_executor_open_batch(
        &writer_executor, writer_queue_capacity + 1U,
        writer_queue_capacity + 1U, INGEST_BATCH_DELAY_MICROSECONDS,
        YAP_V2_core_reactor_execute_ingest_batch, NULL) != YAP_V2_OK) {
    fputs("Cannot start core executors\n", stderr);
    (void)close(listen_socket); listen_socket = -1;
    YAP_V2_executor_close(&writer_executor);
    YAP_V2_executor_close(&search_executor);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_runtime_limiter_close(&writer_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  if (YAP_V2_core_reactor_server_open(
        &reactor_server, listen_socket, index_dir, &http_runtime,
        &search_executor, &writer_executor, &runtime_limiter, &writer_limiter,
        &runtime_policy,
        &compaction_policy, io_threads) != YAP_V2_OK) {
    fputs("Cannot start core I/O reactors\n", stderr);
    (void)close(listen_socket); listen_socket = -1;
    YAP_V2_executor_close(&writer_executor);
    YAP_V2_executor_close(&search_executor);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_runtime_limiter_close(&writer_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  if (pthread_create(&reloader_thread, NULL, run_reloader, &http_runtime) == 0)
    reloader_started = 1;
  else
    request_shutdown(SIGTERM);
  maintenance.index_dir = index_dir;
  maintenance.http_runtime = &http_runtime;
  maintenance.search_limiter = &runtime_limiter;
  maintenance.writer_limiter = &writer_limiter;
  maintenance.policy = compaction_policy;
  if (!shutdown_requested) {
    if (pthread_create(&maintenance_thread, NULL, run_maintenance,
                       &maintenance) == 0)
      maintenance_started = 1;
    else
      request_shutdown(SIGTERM);
  }
  if (!reloader_started || !maintenance_started) {
    request_shutdown(SIGTERM);
  } else {
    int signal_number;
    if (sigwait(&shutdown_signals, &signal_number) != 0) signal_number = SIGTERM;
    request_shutdown(signal_number);
  }
  YAP_V2_core_reactor_server_stop_accepting(&reactor_server);
  listen_socket = -1;
  if (reloader_started) (void)pthread_join(reloader_thread, NULL);
  if (maintenance_started) (void)pthread_join(maintenance_thread, NULL);
  YAP_V2_executor_close(&writer_executor);
  YAP_V2_executor_close(&search_executor);
  YAP_V2_core_reactor_server_close(&reactor_server);
  YAP_V2_runtime_limiter_close(&runtime_limiter);
  YAP_V2_runtime_limiter_close(&writer_limiter);
  YAP_V2_http_runtime_close(&http_runtime);
  return reloader_started &&
    maintenance_started ?
    EXIT_SUCCESS : EXIT_FAILURE;
}
