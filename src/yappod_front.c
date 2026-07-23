#include "yappo_core_http_v2.h"
#include "yappo_http_v2.h"
#include "yappo_net.h"
#include "yappo_observability_v2.h"
#include "yappo_runtime_policy_v2.h"
#include "yappo_application_config.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_FRONT_PORT 18400
#define DEFAULT_CORE_PORT 18401
#define FRONT_WORKERS 16U
#define MAX_HTTP_LINE_BYTES 8192U
#define MAX_HTTP_HEADER_BYTES 65536U

typedef enum {
  ENDPOINT_UNKNOWN = 0,
  ENDPOINT_LIVE,
  ENDPOINT_READY,
  ENDPOINT_METRICS,
  ENDPOINT_SEARCH,
  ENDPOINT_RETRIEVE,
  ENDPOINT_PREPARE,
  ENDPOINT_INGEST
} endpoint_t;

typedef struct {
  char method[16];
  char target[256];
  endpoint_t endpoint;
  size_t content_length;
  int have_content_length;
  int json_content_type;
  char authorization[YAP_V2_AUTHORIZATION_MAX_BYTES + 1U];
} http_request_t;

typedef struct {
  int status;
  unsigned char *body;
  size_t body_bytes;
} core_result_t;

typedef struct {
  size_t id;
  int listen_socket;
  const char *index_dir;
  const char *core_host;
  int core_port;
} worker_t;

static volatile sig_atomic_t shutdown_requested = 0;
static int listen_socket = -1;
static char pid_file[YAP_APPLICATION_PATH_BYTES] = "front.pid";
static char log_file[YAP_APPLICATION_PATH_BYTES] = "front.log";
static char error_file[YAP_APPLICATION_PATH_BYTES] = "front.error";
static YAP_V2_RUNTIME_POLICY runtime_policy;
static YAP_V2_RUNTIME_LIMITER runtime_limiter;
static YAP_V2_METRICS metrics;
static void usage(FILE *output, const char *program) {
  fprintf(output,
          "Usage: %s (--config CONFIG | --index INDEX_DIR --core-host HOST [--port PORT] "
          "[--core-port PORT])\n"
          "  --index INDEX_DIR  Valid v2 index snapshot (required)\n"
          "  --core-host HOST   yappod_core host (required)\n"
          "  --config CONFIG    Shared application TOML\n"
          "  --port PORT        HTTP port (default: %d)\n"
          "  --core-port PORT   Internal HTTP port (default: %d)\n",
          program, DEFAULT_FRONT_PORT, DEFAULT_CORE_PORT);
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
      snprintf(pid_file, sizeof(pid_file), "%s/front.pid", directory) >= (int)sizeof(pid_file) ||
      snprintf(log_file, sizeof(log_file), "%s/front.log", directory) >= (int)sizeof(log_file) ||
      snprintf(error_file, sizeof(error_file), "%s/front.error", directory) >= (int)sizeof(error_file))
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

static int read_line(FILE *stream, char **line_out) {
  char chunk[1024];
  char *line = NULL;
  size_t used = 0U;
  *line_out = NULL;
  for (;;) {
    size_t bytes;
    char *next;
    if (fgets(chunk, sizeof(chunk), stream) == NULL) {
      free(line);
      return ferror(stream) ? -1 : used == 0U ? 0 : -1;
    }
    bytes = strlen(chunk);
    if (bytes > MAX_HTTP_LINE_BYTES - used) { free(line); return -2; }
    next = realloc(line, used + bytes + 1U);
    if (next == NULL) { free(line); return -1; }
    line = next;
    memcpy(line + used, chunk, bytes + 1U);
    used += bytes;
    if (used != 0U && line[used - 1U] == '\n') break;
    if (bytes < sizeof(chunk) - 1U) { free(line); return -1; }
  }
  *line_out = line;
  return 1;
}

static endpoint_t endpoint_for(const char *method, const char *target) {
  if (strcmp(method, "GET") == 0) {
    if (strcmp(target, "/health/live") == 0) return ENDPOINT_LIVE;
    if (strcmp(target, "/health/ready") == 0) return ENDPOINT_READY;
    if (strcmp(target, "/metrics") == 0) return ENDPOINT_METRICS;
  }
  if (strcmp(method, "POST") == 0 || strcmp(method, "QUERY") == 0) {
    if (strcmp(target, "/v2/search") == 0) return ENDPOINT_SEARCH;
    if (strcmp(target, "/v2/retrieve") == 0) return ENDPOINT_RETRIEVE;
  }
  if (strcmp(method, "POST") == 0) {
    if (strcmp(target, "/v2/passages:prepare") == 0) return ENDPOINT_PREPARE;
    if (strcmp(target, "/v2/documents:batch") == 0) return ENDPOINT_INGEST;
  }
  return ENDPOINT_UNKNOWN;
}

static int is_known_target(const char *target) {
  return strcmp(target, "/health/live") == 0 || strcmp(target, "/health/ready") == 0 ||
         strcmp(target, "/metrics") == 0 || strcmp(target, "/v2/search") == 0 ||
         strcmp(target, "/v2/retrieve") == 0 ||
         strcmp(target, "/v2/passages:prepare") == 0 ||
         strcmp(target, "/v2/documents:batch") == 0;
}

static int parse_request_line(const char *line, http_request_t *request) {
  char version[32], trailing;
  memset(request, 0, sizeof(*request));
  if (sscanf(line, "%15s %255s %31s %c", request->method, request->target,
             version, &trailing) != 3 ||
      (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)) return -1;
  request->endpoint = endpoint_for(request->method, request->target);
  return 0;
}

static int value_start(const char *line, size_t prefix, const char **value) {
  const char *cursor = line + prefix;
  while (*cursor == ' ' || *cursor == '\t') cursor++;
  *value = cursor;
  return 0;
}

static int parse_content_length(const char *value, size_t *length) {
  char *end = NULL;
  unsigned long long parsed;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  while (*end == ' ' || *end == '\t') end++;
  if (errno != 0 || end == value || (*end != '\r' && *end != '\n') || parsed == 0U ||
      parsed > YAP_V2_HTTP_MAX_BODY_BYTES || parsed > SIZE_MAX) return -1;
  *length = (size_t)parsed;
  return 0;
}

static int parse_headers(FILE *stream, http_request_t *request) {
  size_t total = 0U;
  int have_type = 0, have_authorization = 0;
  for (;;) {
    char *line = NULL;
    const char *value;
    size_t bytes, value_bytes;
    int read_status = read_line(stream, &line);
    if (read_status <= 0) return -1;
    bytes = strlen(line);
    if (bytes > MAX_HTTP_HEADER_BYTES - total) { free(line); return -1; }
    total += bytes;
    if ((line[0] == '\r' && line[1] == '\n') ||
        (line[0] == '\n' && line[1] == '\0')) { free(line); return 0; }
    if (strncasecmp(line, "Content-Type:", 13U) == 0) {
      value_start(line, 13U, &value);
      if (have_type || strncasecmp(value, "application/json", 16U) != 0 ||
          (value[16] != '\r' && value[16] != '\n' && value[16] != ';')) {
        free(line);
        return -3;
      }
      have_type = 1;
      request->json_content_type = 1;
    } else if (strncasecmp(line, "Content-Length:", 15U) == 0) {
      value_start(line, 15U, &value);
      if (request->have_content_length || parse_content_length(value, &request->content_length) != 0) {
        int too_large = errno == ERANGE || strtoull(value, NULL, 10) > YAP_V2_HTTP_MAX_BODY_BYTES;
        free(line);
        return too_large ? -2 : -1;
      }
      request->have_content_length = 1;
    } else if (strncasecmp(line, "Transfer-Encoding:", 18U) == 0) {
      free(line);
      return -1;
    } else if (strncasecmp(line, "Authorization:", 14U) == 0) {
      value_start(line, 14U, &value);
      value_bytes = strcspn(value, "\r\n");
      if (have_authorization || value_bytes == 0U ||
          value_bytes > YAP_V2_AUTHORIZATION_MAX_BYTES) {
        free(line);
        return -1;
      }
      memcpy(request->authorization, value, value_bytes);
      request->authorization[value_bytes] = '\0';
      have_authorization = 1;
    }
    free(line);
  }
}

static const char *reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    default: return "Service Unavailable";
  }
}

static int send_response_headers(FILE *stream, int status, const char *content_type,
                                 const char *allow, int accept_query,
                                 const void *body, size_t body_bytes);
static int send_json_error_headers(FILE *stream, int status, const char *code,
                                   const char *message, const char *allow,
                                   int accept_query);

static int send_response(FILE *stream, int status, const char *content_type,
                         const void *body, size_t body_bytes) {
  return send_response_headers(stream, status, content_type, NULL, 0, body, body_bytes);
}

static int send_response_headers(FILE *stream, int status, const char *content_type,
                                 const char *allow, int accept_query,
                                 const void *body, size_t body_bytes) {
  if (fprintf(stream,
              "HTTP/1.1 %d %s\r\nServer: Yappo Search/2.0\r\n"
              "Content-Type: %s\r\nContent-Length: %zu\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n",
              status, reason_phrase(status), content_type, body_bytes) < 0 ||
      (allow != NULL && fprintf(stream, "Allow: %s\r\n", allow) < 0) ||
      (accept_query && fputs("Accept-Query: application/json\r\n", stream) == EOF) ||
      fputs("\r\n", stream) == EOF ||
      (body_bytes != 0U && fwrite(body, 1U, body_bytes, stream) != body_bytes) ||
      fflush(stream) != 0) return -1;
  return 0;
}

static int send_json_error(FILE *stream, int status, const char *code, const char *message) {
  return send_json_error_headers(stream, status, code, message, NULL, 0);
}

static int send_json_error_headers(FILE *stream, int status, const char *code,
                                   const char *message, const char *allow,
                                   int accept_query) {
  char body[512];
  int length = snprintf(body, sizeof(body),
                        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                        code, message);
  if (length < 0 || (size_t)length >= sizeof(body)) return -1;
  return send_response_headers(stream, status, "application/json; charset=utf-8",
                               allow, accept_query, body, (size_t)length);
}

static int discard_request_body(FILE *stream, size_t body_bytes) {
  /* Linux may reset a connection closed with unread request bytes and discard
   * the error response. Drain first into a bounded stack buffer. */
  unsigned char buffer[4096];
  size_t remaining = body_bytes;
  while (remaining > 0U) {
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t consumed = fread(buffer, 1U, chunk, stream);
    if (consumed == 0U) return -1;
    remaining -= consumed;
  }
  return 0;
}

static int core_roundtrip(const worker_t *worker, endpoint_t endpoint,
                          const unsigned char *body, size_t body_bytes,
                          const char *authorization,
                          core_result_t *result) {
  const char *method = endpoint == ENDPOINT_INGEST ? "POST" : "QUERY";
  const char *target = endpoint == ENDPOINT_SEARCH ? "/v2/search" :
                       endpoint == ENDPOINT_RETRIEVE ? "/v2/retrieve" :
                       "/v2/documents:batch";
  YAP_V2_CORE_HTTP_RESPONSE response;
  memset(result, 0, sizeof(*result));
  YAP_V2_core_http_response_init(&response);
  if (YAP_V2_core_http_client_request(worker->core_host, worker->core_port,
                                      runtime_policy.request_timeout_ms,
                                      method, target,
                                      endpoint == ENDPOINT_INGEST ? authorization : NULL,
                                      body, body_bytes, &response) != YAP_V2_CORE_HTTP_OK)
    return -1;
  result->status = response.status;
  result->body = response.body;
  result->body_bytes = response.body_bytes;
  response.body = NULL;
  YAP_V2_core_http_response_free(&response);
  return 0;
}

static int core_ready(const worker_t *worker) {
  int ready = 0;
  YAP_V2_CORE_HTTP_RESPONSE response;
  YAP_V2_core_http_response_init(&response);
  if (YAP_V2_core_http_client_request(worker->core_host, worker->core_port,
                                      runtime_policy.request_timeout_ms,
                                      "GET", "/health/ready", NULL, NULL, 0U,
                                      &response) == YAP_V2_CORE_HTTP_OK &&
      response.status == 200)
    ready = 1;
  YAP_V2_core_http_response_free(&response);
  return ready;
}

static int handle_operational(FILE *stream, const worker_t *worker, endpoint_t endpoint) {
  static const char live[] = "{\"status\":\"live\",\"service\":\"yappod_front\"}";
  YAP_V2_OPERATIONAL_STATE state;
  char error[256] = {0}, *body = NULL;
  size_t body_bytes = 0U;
  int status;
  if (endpoint == ENDPOINT_LIVE)
    return send_response(stream, 200, "application/json; charset=utf-8", live,
                         sizeof(live) - 1U);
  status = YAP_V2_operational_probe_index(worker->index_dir, &state, error, sizeof(error));
  if (status != YAP_V2_OK || !core_ready(worker)) state.ready = 0;
  if (endpoint == ENDPOINT_READY) {
    if (YAP_V2_operational_state_json(&state, "yappod_front", &body, &body_bytes) != YAP_V2_OK)
      return send_json_error(stream, 500, "internal_error", "Internal Server Error");
    status = send_response(stream, state.ready ? 200 : 503,
                           "application/json; charset=utf-8", body, body_bytes);
  } else {
    size_t inflight, inflight_bytes, max_inflight, max_inflight_bytes;
    if (YAP_V2_runtime_limiter_snapshot(&runtime_limiter, &inflight, &inflight_bytes,
                                        &max_inflight, &max_inflight_bytes) != YAP_V2_OK ||
        YAP_V2_metrics_render(&metrics, &state, inflight, inflight_bytes, max_inflight,
                              max_inflight_bytes, &body, &body_bytes) != YAP_V2_OK)
      return send_json_error(stream, 500, "internal_error", "Internal Server Error");
    status = send_response(stream, 200, "text/plain; version=0.0.4; charset=utf-8",
                           body, body_bytes);
  }
  free(body);
  return status;
}

static YAP_V2_OBSERVE_OPERATION observe_operation(endpoint_t endpoint) {
  return endpoint == ENDPOINT_SEARCH ? YAP_V2_OBSERVE_SEARCH :
         (endpoint == ENDPOINT_RETRIEVE || endpoint == ENDPOINT_PREPARE) ?
         YAP_V2_OBSERVE_RETRIEVE : YAP_V2_OBSERVE_INGEST;
}

static const char *allow_for_target(const char *target) {
  if (strcmp(target, "/v2/search") == 0 || strcmp(target, "/v2/retrieve") == 0)
    return "QUERY, POST";
  if (strcmp(target, "/v2/passages:prepare") == 0 ||
      strcmp(target, "/v2/documents:batch") == 0)
    return "POST";
  if (strcmp(target, "/health/live") == 0 ||
      strcmp(target, "/health/ready") == 0 ||
      strcmp(target, "/metrics") == 0)
    return "GET";
  return NULL;
}

static int query_endpoint(endpoint_t endpoint) {
  return endpoint == ENDPOINT_SEARCH || endpoint == ENDPOINT_RETRIEVE;
}

static int send_endpoint_error(FILE *stream, endpoint_t endpoint, int status,
                               const char *code, const char *message) {
  return send_json_error_headers(stream, status, code, message, NULL,
                                 query_endpoint(endpoint));
}

static int handle_client(FILE *stream, const worker_t *worker) {
  http_request_t request;
  char *line = NULL;
  unsigned char *body = NULL;
  core_result_t result;
  int read_status, header_status, response_status = 500, admitted = 0;
  uint64_t started = 0U;
  read_status = read_line(stream, &line);
  if (read_status <= 0 || parse_request_line(line, &request) != 0) {
    free(line);
    return send_json_error(stream, 400, "invalid_request", "Bad Request");
  }
  free(line);
  header_status = parse_headers(stream, &request);
  if (header_status != 0) {
    response_status = header_status == -2 ? 413 : header_status == -3 ? 415 : 400;
    return send_json_error_headers(stream, response_status,
      response_status == 413 ? "body_too_large" :
      response_status == 415 ? "unsupported_media_type" : "invalid_request",
      reason_phrase(response_status), NULL, query_endpoint(request.endpoint));
  }
  if (request.endpoint == ENDPOINT_UNKNOWN) {
    int known = is_known_target(request.target);
    if (known && request.have_content_length)
      (void)discard_request_body(stream, request.content_length);
    return send_json_error_headers(stream, known ? 405 : 404,
      known ? "method_not_allowed" : "not_found",
      known ? "Method Not Allowed" : "Not Found",
      known ? allow_for_target(request.target) : NULL,
      strcmp(request.target, "/v2/search") == 0 ||
      strcmp(request.target, "/v2/retrieve") == 0);
  }
  if (request.endpoint == ENDPOINT_LIVE || request.endpoint == ENDPOINT_READY ||
      request.endpoint == ENDPOINT_METRICS) {
    if (request.have_content_length)
      return send_json_error(stream, 400, "invalid_request", "Bad Request");
    return handle_operational(stream, worker, request.endpoint);
  }
  started = YAP_V2_monotonic_microseconds();
  if (!request.json_content_type) {
    response_status = 415;
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              "unsupported_media_type", "Unsupported Media Type");
    goto observed;
  }
  if (!request.have_content_length) {
    response_status = 400;
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              "invalid_request", "Bad Request");
    goto observed;
  }
  if (request.endpoint == ENDPOINT_INGEST &&
      YAP_V2_authorize_write(&runtime_policy,
        request.authorization[0] == '\0' ? NULL : request.authorization) != YAP_V2_OK) {
    response_status = 401;
    (void)discard_request_body(stream, request.content_length);
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              "unauthorized", "Unauthorized");
    goto observed;
  }
  if (YAP_V2_runtime_limiter_acquire(&runtime_limiter, request.content_length) != YAP_V2_OK) {
    response_status = 503;
    (void)discard_request_body(stream, request.content_length);
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              "overloaded", "Service Unavailable");
    goto observed;
  }
  admitted = 1;
  body = malloc(request.content_length);
  if (body == NULL || fread(body, 1U, request.content_length, stream) != request.content_length) {
    response_status = body == NULL ? 500 : 400;
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              response_status == 500 ? "internal_error" : "invalid_request",
                              reason_phrase(response_status));
    goto observed;
  }
  if (request.endpoint == ENDPOINT_PREPARE) {
    char *prepared = NULL; size_t prepared_bytes = 0U;
    if (YAP_V2_http_execute(worker->index_dir, YAP_V2_HTTP_PREPARE, body,
                            request.content_length, &result.status, &prepared,
                            &prepared_bytes) != 0) {
      response_status = 503;
      (void)send_endpoint_error(stream, request.endpoint, response_status,
                                "prepare_unavailable", "Service Unavailable");
      goto observed;
    }
    result.body = (unsigned char *)prepared; result.body_bytes = prepared_bytes;
  } else if (core_roundtrip(worker, request.endpoint, body, request.content_length,
                            request.authorization[0] == '\0' ? NULL : request.authorization,
                            &result) != 0) {
    response_status = 503;
    (void)send_endpoint_error(stream, request.endpoint, response_status,
                              "core_unavailable", "Service Unavailable");
    goto observed;
  }
  response_status = result.status;
  (void)send_response_headers(stream, result.status,
                              "application/json; charset=utf-8", NULL,
                              query_endpoint(request.endpoint),
                              result.body, result.body_bytes);
  free(result.body);
observed:
  if (admitted) YAP_V2_runtime_limiter_release(&runtime_limiter, request.content_length);
  free(body);
  YAP_V2_metrics_record(&metrics, observe_operation(request.endpoint), response_status,
                        YAP_V2_monotonic_microseconds() - started);
  return 0;
}

static void *run_worker(void *opaque) {
  worker_t *worker = opaque;
  while (!shutdown_requested) {
    struct sockaddr_storage address;
    socklen_t address_bytes = sizeof(address);
    FILE *stream = NULL;
    int descriptor = -1;
    if (YAP_Net_accept_stream(worker->listen_socket, (struct sockaddr *)&address,
                              &address_bytes, &stream, &descriptor, "front",
                              (int)worker->id) != 0) {
      if (shutdown_requested) break;
      continue;
    }
    if (YAP_V2_socket_set_deadline(descriptor, runtime_policy.request_timeout_ms) == YAP_V2_OK)
      (void)handle_client(stream, worker);
    YAP_Net_close_stream(&stream, &descriptor);
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *index_dir = NULL, *core_host = NULL, *config_path = NULL;
  const char *listen_host = NULL;
  YAP_APPLICATION_CONFIG application;
  int port = DEFAULT_FRONT_PORT, core_port = DEFAULT_CORE_PORT, i, daemon_status;
  char policy_error[256] = {0}, probe_error[256] = {0};
  YAP_V2_OPERATIONAL_STATE state;
  pthread_t threads[FRONT_WORKERS];
  worker_t workers[FRONT_WORKERS];
  size_t started = 0U;
  int have_port = 0, have_core_port = 0;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (strcmp(argv[i], "--index") == 0 || strcmp(argv[i], "--core-host") == 0 ||
        strcmp(argv[i], "--config") == 0) {
      const char **target = strcmp(argv[i], "--index") == 0 ? &index_dir :
                            strcmp(argv[i], "--core-host") == 0 ? &core_host : &config_path;
      if (++i >= argc) { usage(stderr, argv[0]); return EXIT_FAILURE; }
      *target = argv[i];
    } else if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--core-port") == 0) {
      int *target = strcmp(argv[i], "--port") == 0 ? &port : &core_port;
      if (strcmp(argv[i], "--port") == 0) have_port = 1; else have_core_port = 1;
      if (++i >= argc || parse_port(argv[i], target) != 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (config_path != NULL &&
      (index_dir != NULL || core_host != NULL || have_port || have_core_port)) {
    fputs("--config cannot be combined with legacy daemon options\n", stderr);
    return EXIT_FAILURE;
  }
  if (config_path != NULL) {
    if (YAP_application_config_load(config_path, &application, policy_error,
                                    sizeof(policy_error)) != YAP_V2_OK) {
      fprintf(stderr, "Invalid application config: %s\n", policy_error);
      return EXIT_FAILURE;
    }
    index_dir = application.index_directory;
    core_host = application.core_host;
    core_port = application.core_port;
    listen_host = application.front_host;
    port = application.front_port;
    runtime_policy = application.runtime_policy;
    if (set_run_paths(application.run_directory) != 0) {
      fprintf(stderr, "Cannot create run directory: %s\n", strerror(errno));
      return EXIT_FAILURE;
    }
  } else {
    YAP_V2_runtime_policy_init(&runtime_policy);
  }
  if (index_dir == NULL || core_host == NULL ||
      YAP_V2_operational_probe_index(index_dir, &state, probe_error, sizeof(probe_error)) !=
        YAP_V2_OK || !state.ready || state.segment_count == 0U) {
    fprintf(stderr, "Invalid v2 index: %s\n", probe_error);
    return EXIT_FAILURE;
  }
  memset(&runtime_limiter, 0, sizeof(runtime_limiter));
  if (YAP_V2_runtime_limiter_init(&runtime_limiter, &runtime_policy) != YAP_V2_OK ||
      YAP_V2_metrics_init(&metrics) != YAP_V2_OK) {
    fprintf(stderr, "Invalid runtime policy: %s\n", policy_error);
    return EXIT_FAILURE;
  }
  listen_socket = create_listener(listen_host, port);
  if (listen_socket < 0) {
    fprintf(stderr, "Cannot listen on port %d: %s\n", port, strerror(errno));
    YAP_V2_metrics_close(&metrics);
    YAP_V2_runtime_limiter_close(&runtime_limiter);
    return EXIT_FAILURE;
  }
  daemon_status = daemonize_process();
  if (daemon_status != 0) {
    (void)close(listen_socket);
    return daemon_status > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (install_signal_handlers() != 0) return EXIT_FAILURE;
  for (started = 0U; started < FRONT_WORKERS; started++) {
    workers[started].id = started;
    workers[started].listen_socket = listen_socket;
    workers[started].index_dir = index_dir;
    workers[started].core_host = core_host;
    workers[started].core_port = core_port;
    if (pthread_create(&threads[started], NULL, run_worker, &workers[started]) != 0) break;
  }
  if (started != FRONT_WORKERS) request_shutdown(SIGTERM);
  for (i = 0; i < (int)started; i++) (void)pthread_join(threads[i], NULL);
  if (listen_socket >= 0) (void)close(listen_socket);
  YAP_V2_metrics_close(&metrics);
  YAP_V2_runtime_limiter_close(&runtime_limiter);
  return started == FRONT_WORKERS ? EXIT_SUCCESS : EXIT_FAILURE;
}
