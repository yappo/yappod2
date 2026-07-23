#include "yappo_core_http_v2.h"
#include "yappo_http_v2.h"
#include "yappo_net.h"
#include "yappo_observability_v2.h"
#include "yappo_runtime_policy_v2.h"
#include "yappo_application_config.h"

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

#define DEFAULT_CORE_PORT 18401
#define CORE_WORKERS 16U

typedef struct {
  size_t id;
  int listen_socket;
  const char *index_dir;
  YAP_V2_HTTP_RUNTIME *http_runtime;
} worker_t;

static volatile sig_atomic_t shutdown_requested = 0;
static int listen_socket = -1;
static char pid_file[YAP_APPLICATION_PATH_BYTES] = "core.pid";
static char log_file[YAP_APPLICATION_PATH_BYTES] = "core.log";
static char error_file[YAP_APPLICATION_PATH_BYTES] = "core.error";
static YAP_V2_RUNTIME_POLICY runtime_policy;
static YAP_V2_RUNTIME_LIMITER runtime_limiter;

static void usage(FILE *output, const char *program) {
  fprintf(output,
          "Usage: %s (--config CONFIG | --index INDEX_DIR [--port PORT])\n"
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
  if (listen_socket >= 0) (void)close(listen_socket);
  listen_socket = -1;
}

static int install_signal_handlers(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = request_shutdown;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0)
    return -1;
  action.sa_handler = SIG_IGN;
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

static int make_error_json(const char *code, const char *message,
                           char **json, size_t *json_bytes) {
  int length = snprintf(NULL, 0, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                        code, message);
  if (length < 0) return -1;
  *json = malloc((size_t)length + 1U);
  if (*json == NULL) return -1;
  (void)snprintf(*json, (size_t)length + 1U,
                 "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
  *json_bytes = (size_t)length;
  return 0;
}

static const char *allow_for_target(const char *target) {
  if (strcmp(target, "/v2/search") == 0 || strcmp(target, "/v2/retrieve") == 0)
    return "QUERY";
  if (strcmp(target, "/v2/documents:batch") == 0) return "POST";
  if (strcmp(target, "/health/ready") == 0) return "GET";
  return NULL;
}

static int query_target(const char *target) {
  return strcmp(target, "/v2/search") == 0 || strcmp(target, "/v2/retrieve") == 0;
}

static int send_json(FILE *stream, int status, const char *allow, int accept_query,
                     const char *json, size_t json_bytes) {
  return YAP_V2_core_http_write_response(stream, status,
    "application/json; charset=utf-8", allow, accept_query, json, json_bytes);
}

static int send_error(FILE *stream, int status, const char *code, const char *message,
                      const char *allow, int accept_query) {
  char *json = NULL;
  size_t json_bytes = 0U;
  int result;
  if (make_error_json(code, message, &json, &json_bytes) != 0) return -1;
  result = send_json(stream, status, allow, accept_query, json, json_bytes);
  free(json);
  return result;
}

static int handle_request(FILE *stream, const char *index_dir,
                          YAP_V2_HTTP_RUNTIME *http_runtime) {
  YAP_V2_CORE_HTTP_REQUEST request;
  YAP_V2_HTTP_OPERATION operation = YAP_V2_HTTP_SEARCH;
  char *json = NULL;
  size_t json_bytes = 0U;
  int admitted = 0, http_status = 500;
  int status, result = -1;
  const char *allow;
  int accept_query;
  YAP_V2_core_http_request_init(&request);
  status = YAP_V2_core_http_read_request(stream, YAP_V2_HTTP_MAX_BODY_BYTES, &request);
  if (status == YAP_V2_CORE_HTTP_TOO_LARGE) {
    result = send_error(stream, 413, "body_too_large", "Content Too Large",
                        NULL, 0);
    goto done;
  }
  if (status != YAP_V2_CORE_HTTP_OK) {
    result = send_error(stream, 400, "invalid_request", "Bad Request", NULL, 0);
    goto done;
  }
  allow = allow_for_target(request.target);
  accept_query = query_target(request.target);
  if (allow == NULL) {
    result = send_error(stream, 404, "not_found", "Not Found", NULL, 0);
    goto done;
  }
  if ((accept_query && strcmp(request.method, "QUERY") != 0) ||
      (strcmp(request.target, "/v2/documents:batch") == 0 &&
       strcmp(request.method, "POST") != 0) ||
      (strcmp(request.target, "/health/ready") == 0 &&
       strcmp(request.method, "GET") != 0)) {
    result = send_error(stream, 405, "method_not_allowed", "Method Not Allowed",
                        allow, accept_query);
    goto done;
  }
  if (strcmp(request.target, "/health/ready") == 0) {
    YAP_V2_OPERATIONAL_STATE state, disk_state;
    char probe_error[256] = {0};
    memset(&state, 0, sizeof(state));
    memset(&disk_state, 0, sizeof(disk_state));
    if (request.have_content_length) {
      result = send_error(stream, 400, "invalid_request", "Bad Request", "GET", 0);
      goto done;
    }
    status = YAP_V2_http_runtime_state(http_runtime, &state);
    if (status == YAP_V2_OK &&
        YAP_V2_operational_probe_index(index_dir, &disk_state, probe_error,
                                       sizeof(probe_error)) == YAP_V2_OK) {
      state.compaction_state = disk_state.compaction_state;
      state.compaction_generation = disk_state.compaction_generation;
      state.compaction_updated_at_unix = disk_state.compaction_updated_at_unix;
    }
    http_status = status == YAP_V2_OK && state.ready ? 200 : 503;
    if (YAP_V2_operational_state_json(&state, "yappod_core", &json, &json_bytes) !=
        YAP_V2_OK) goto done;
  } else {
    if (!request.have_content_length || request.body_bytes == 0U) {
      result = send_error(stream, 400, "invalid_request", "Bad Request",
                          allow, accept_query);
      goto done;
    }
    if (!request.json_content_type) {
      result = send_error(stream, 415, "unsupported_media_type",
                          "Unsupported Media Type", allow, accept_query);
      goto done;
    }
    if (strcmp(request.target, "/v2/search") == 0) operation = YAP_V2_HTTP_SEARCH;
    else if (strcmp(request.target, "/v2/retrieve") == 0) operation = YAP_V2_HTTP_RETRIEVE;
    else operation = YAP_V2_HTTP_INGEST;
    if (YAP_V2_runtime_limiter_acquire(&runtime_limiter, request.body_bytes) != YAP_V2_OK) {
      http_status = 503;
      if (make_error_json("overloaded", "Service Unavailable", &json, &json_bytes) != 0) {
        goto done;
      }
    } else {
      admitted = 1;
      if (operation == YAP_V2_HTTP_INGEST &&
          YAP_V2_authorize_write(&runtime_policy,
            request.authorization[0] == '\0' ? NULL : request.authorization) != YAP_V2_OK) {
        http_status = 401;
        if (make_error_json("unauthorized", "Unauthorized", &json, &json_bytes) != 0) {
          goto done;
        }
      } else if (YAP_V2_http_runtime_execute(http_runtime, operation, request.body,
                                             request.body_bytes, &http_status,
                                             &json, &json_bytes) != 0) {
        goto done;
      }
    }
  }
  result = send_json(stream, http_status, NULL, accept_query, json, json_bytes);
done:
  if (admitted) YAP_V2_runtime_limiter_release(&runtime_limiter, request.body_bytes);
  free(json);
  YAP_V2_core_http_request_free(&request);
  return result;
}

static void *run_worker(void *opaque) {
  worker_t *worker = opaque;
  while (!shutdown_requested) {
    struct sockaddr_storage address;
    socklen_t address_bytes = sizeof(address);
    FILE *stream = NULL;
    int descriptor = -1;
    if (YAP_Net_accept_stream(worker->listen_socket, (struct sockaddr *)&address,
                              &address_bytes, &stream, &descriptor, "core",
                              (int)worker->id) != 0) {
      if (shutdown_requested) break;
      continue;
    }
    if (YAP_V2_socket_set_deadline(descriptor, runtime_policy.request_timeout_ms) == YAP_V2_OK)
      (void)handle_request(stream, worker->index_dir, worker->http_runtime);
    YAP_Net_close_stream(&stream, &descriptor);
  }
  return NULL;
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

int main(int argc, char **argv) {
  const char *index_dir = NULL, *config_path = NULL;
  const char *listen_host = NULL;
  YAP_APPLICATION_CONFIG application;
  int port = DEFAULT_CORE_PORT, i, daemon_status;
  char policy_error[256] = {0};
  int have_port = 0;
  YAP_V2_HTTP_RUNTIME http_runtime;
  pthread_t threads[CORE_WORKERS], reloader_thread;
  worker_t workers[CORE_WORKERS];
  size_t started = 0U;
  int reloader_started = 0;
  YAP_V2_http_runtime_init(&http_runtime);
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (strcmp(argv[i], "--index") == 0 || strcmp(argv[i], "--config") == 0) {
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
    if (set_run_paths(application.run_directory) != 0) {
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
  if (YAP_V2_runtime_limiter_init(&runtime_limiter, &runtime_policy) != YAP_V2_OK) {
    fprintf(stderr, "Invalid runtime policy: %s\n", policy_error);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  listen_socket = create_listener(listen_host, port);
  if (listen_socket < 0) {
    fprintf(stderr, "Cannot listen on port %d: %s\n", port, strerror(errno));
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  daemon_status = daemonize_process();
  if (daemon_status != 0) {
    (void)close(listen_socket);
    YAP_V2_http_runtime_close(&http_runtime);
    return daemon_status > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (install_signal_handlers() != 0) {
    (void)close(listen_socket); listen_socket = -1;
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  if (pthread_create(&reloader_thread, NULL, run_reloader, &http_runtime) == 0)
    reloader_started = 1;
  else
    request_shutdown(SIGTERM);
  for (started = 0U; started < CORE_WORKERS; started++) {
    workers[started].id = started;
    workers[started].listen_socket = listen_socket;
    workers[started].index_dir = index_dir;
    workers[started].http_runtime = &http_runtime;
    if (pthread_create(&threads[started], NULL, run_worker, &workers[started]) != 0) break;
  }
  if (started != CORE_WORKERS) request_shutdown(SIGTERM);
  for (i = 0; i < (int)started; i++) (void)pthread_join(threads[i], NULL);
  if (reloader_started) (void)pthread_join(reloader_thread, NULL);
  if (listen_socket >= 0) (void)close(listen_socket);
  YAP_V2_runtime_limiter_close(&runtime_limiter);
  YAP_V2_http_runtime_close(&http_runtime);
  return started == CORE_WORKERS && reloader_started ? EXIT_SUCCESS : EXIT_FAILURE;
}
