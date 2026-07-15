#include "yappo_core_protocol_v2.h"
#include "yappo_http_v2.h"
#include "yappo_net.h"
#include "yappo_observability_v2.h"
#include "yappo_runtime_policy_v2.h"

#include <errno.h>
#include <netinet/in.h>
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
static const char *pid_file = "core.pid";
static YAP_V2_RUNTIME_POLICY runtime_policy;
static YAP_V2_RUNTIME_LIMITER runtime_limiter;

static void usage(FILE *output, const char *program) {
  fprintf(output,
          "Usage: %s --index INDEX_DIR [--port PORT]\n"
          "  --index INDEX_DIR  Valid v2 index snapshot (required)\n"
          "  --port PORT        Internal frame port (default: %d)\n",
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

static int daemonize_process(void) {
  pid_t child = fork();
  FILE *file;
  if (child < 0) return -1;
  if (child > 0) return 1;
  if (setsid() < 0 || redirect_stream(stdin, "/dev/null", "r") != 0 ||
      redirect_stream(stdout, "core.log", "a") != 0 ||
      redirect_stream(stderr, "core.error", "a") != 0) return -1;
  file = fopen(pid_file, "w");
  if (file == NULL || fprintf(file, "%ld\n", (long)getpid()) < 0 || fclose(file) != 0)
    return -1;
  if (atexit(remove_pid_file) != 0) return -1;
  return 0;
}

static int create_listener(int port) {
  struct sockaddr_in address;
  int descriptor, reuse = 1;
  descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0 ||
      setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    if (descriptor >= 0) (void)close(descriptor);
    return -1;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((unsigned short)port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(descriptor, SOMAXCONN) != 0) {
    (void)close(descriptor);
    return -1;
  }
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

static int handle_request(FILE *stream, const char *index_dir,
                          YAP_V2_HTTP_RUNTIME *http_runtime) {
  YAP_V2_CORE_FRAME request, response;
  YAP_V2_HTTP_OPERATION operation = YAP_V2_HTTP_SEARCH;
  char *json = NULL;
  size_t json_bytes = 0U;
  unsigned char *payload = NULL;
  const unsigned char *request_json = NULL;
  size_t request_json_bytes = 0U;
  int admitted = 0, health = 0, http_status = 500;
  int status;
  YAP_V2_core_frame_init(&request);
  YAP_V2_core_frame_init(&response);
  status = YAP_V2_core_frame_read(stream, YAP_V2_CORE_MAX_PAYLOAD_BYTES, &request);
  if (status != YAP_V2_CORE_FRAME_OK) goto done;
  if (request.type == YAP_V2_CORE_HEALTH_REQUEST) {
    YAP_V2_OPERATIONAL_STATE state, disk_state;
    char probe_error[256] = {0};
    if (request.payload_bytes != 0U) { status = YAP_V2_CORE_FRAME_INVALID; goto done; }
    health = 1;
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
        YAP_V2_OK) {
      status = YAP_V2_CORE_FRAME_NO_MEMORY;
      goto done;
    }
  } else {
    if (request.type == YAP_V2_CORE_SEARCH_REQUEST) operation = YAP_V2_HTTP_SEARCH;
    else if (request.type == YAP_V2_CORE_RETRIEVE_REQUEST) operation = YAP_V2_HTTP_RETRIEVE;
    else if (request.type == YAP_V2_CORE_INGEST_REQUEST) operation = YAP_V2_HTTP_INGEST;
    else { status = YAP_V2_CORE_FRAME_INVALID; goto done; }
    if (YAP_V2_runtime_limiter_acquire(&runtime_limiter, request.payload_bytes) != YAP_V2_OK) {
      http_status = 503;
      if (make_error_json("overloaded", "Service Unavailable", &json, &json_bytes) != 0) {
        status = YAP_V2_CORE_FRAME_NO_MEMORY;
        goto done;
      }
    } else {
      admitted = 1;
      request_json = request.payload;
      request_json_bytes = request.payload_bytes;
      if (operation == YAP_V2_HTTP_INGEST &&
          YAP_V2_ingest_envelope_unwrap(&runtime_policy, request.payload,
                                        request.payload_bytes, &request_json,
                                        &request_json_bytes) != YAP_V2_OK) {
        http_status = 401;
        if (make_error_json("unauthorized", "Unauthorized", &json, &json_bytes) != 0) {
          status = YAP_V2_CORE_FRAME_NO_MEMORY;
          goto done;
        }
      } else if (request_json_bytes > YAP_V2_HTTP_MAX_BODY_BYTES ||
                 YAP_V2_http_runtime_execute(http_runtime, operation, request_json,
                                             request_json_bytes, &http_status,
                                             &json, &json_bytes) != 0) {
        status = YAP_V2_CORE_FRAME_IO_ERROR;
        goto done;
      }
    }
  }
  if (json_bytes > UINT32_MAX - 2U) { status = YAP_V2_CORE_FRAME_TOO_LARGE; goto done; }
  payload = malloc(json_bytes + 2U);
  if (payload == NULL) { status = YAP_V2_CORE_FRAME_NO_MEMORY; goto done; }
  payload[0] = (unsigned char)((unsigned int)http_status >> 8);
  payload[1] = (unsigned char)http_status;
  memcpy(payload + 2U, json, json_bytes);
  response.type = http_status == 200 ?
    (health ? YAP_V2_CORE_HEALTH_RESPONSE :
     operation == YAP_V2_HTTP_SEARCH ? YAP_V2_CORE_SEARCH_RESPONSE :
     operation == YAP_V2_HTTP_RETRIEVE ? YAP_V2_CORE_RETRIEVE_RESPONSE :
     YAP_V2_CORE_INGEST_RESPONSE) : YAP_V2_CORE_ERROR_RESPONSE;
  response.request_id = request.request_id;
  response.payload = payload;
  response.payload_bytes = (uint32_t)(json_bytes + 2U);
  status = YAP_V2_core_frame_write(stream, &response, YAP_V2_CORE_MAX_PAYLOAD_BYTES);
  if (status == YAP_V2_CORE_FRAME_OK && fflush(stream) != 0)
    status = YAP_V2_CORE_FRAME_IO_ERROR;
done:
  if (admitted) YAP_V2_runtime_limiter_release(&runtime_limiter, request.payload_bytes);
  free(payload);
  free(json);
  YAP_V2_core_frame_free(&request);
  return status;
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
      while (!shutdown_requested &&
             handle_request(stream, worker->index_dir,
                            worker->http_runtime) == YAP_V2_CORE_FRAME_OK) {}
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
  const char *index_dir = NULL;
  int port = DEFAULT_CORE_PORT, i, daemon_status;
  char policy_error[256] = {0};
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
    if (strcmp(argv[i], "--index") == 0) {
      if (++i >= argc) { usage(stderr, argv[0]); return EXIT_FAILURE; }
      index_dir = argv[i];
    } else if (strcmp(argv[i], "--port") == 0) {
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
  if (index_dir == NULL || YAP_V2_http_runtime_open(&http_runtime, index_dir) != YAP_V2_OK) {
    fprintf(stderr, "Invalid v2 index\n");
    return EXIT_FAILURE;
  }
  memset(&runtime_limiter, 0, sizeof(runtime_limiter));
  if (YAP_V2_runtime_policy_load_env(&runtime_policy, policy_error, sizeof(policy_error)) !=
        YAP_V2_OK ||
      YAP_V2_runtime_limiter_init(&runtime_limiter, &runtime_policy) != YAP_V2_OK) {
    fprintf(stderr, "Invalid runtime policy: %s\n", policy_error);
    YAP_V2_http_runtime_close(&http_runtime);
    return EXIT_FAILURE;
  }
  listen_socket = create_listener(port);
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
