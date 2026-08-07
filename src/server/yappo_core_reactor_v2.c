#include "server/yappo_core_reactor_v2.h"

#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/yappo_types_v2.h"
#include "server/yappo_core_http_v2.h"
#include "server/yappo_observability_v2.h"

typedef struct reactor reactor_t;
typedef struct connection connection_t;

typedef enum {
  MESSAGE_ACCEPT = 1,
  MESSAGE_COMPLETE = 2
} message_type_t;

typedef struct message {
  struct message *next;
  message_type_t type;
} message_t;

typedef struct {
  message_t message;
  int descriptor;
} accept_message_t;

typedef struct execution {
  message_t message;
  reactor_t *reactor;
  connection_t *connection;
  YAP_V2_HTTP_OPERATION operation;
  int health_request;
  int limiter_acquired;
  int http_status;
  char *json;
  size_t json_bytes;
  int result;
} execution_t;

struct connection {
  connection_t *previous;
  connection_t *next;
  reactor_t *reactor;
  struct bufferevent *buffered_event;
  YAP_V2_CORE_HTTP_REQUEST request;
  execution_t *execution;
  int request_head_parsed;
  int inflight;
  int abandoned;
  int response_pending;
  int close_after_response;
  int writer_admitted;
};

typedef struct {
  int listen_socket;
  const char *index_dir;
  YAP_V2_HTTP_RUNTIME *runtime;
  YAP_V2_EXECUTOR *search_executor;
  YAP_V2_EXECUTOR *writer_executor;
  YAP_V2_RUNTIME_LIMITER *search_limiter;
  YAP_V2_RUNTIME_LIMITER *writer_limiter;
  YAP_V2_RUNTIME_POLICY runtime_policy;
  YAP_V2_COMPACTION_POLICY compaction_policy;
  reactor_t *reactors;
  size_t reactor_count;
  pthread_t acceptor_thread;
  size_t started_reactors;
  int acceptor_started;
  volatile sig_atomic_t stopping;
} server_state_t;

struct reactor {
  server_state_t *server;
  size_t id;
  struct event_base *base;
  struct event *notification_event;
  int notification_sockets[2];
  pthread_mutex_t mailbox_lock;
  message_t *mailbox_head;
  message_t *mailbox_tail;
  connection_t *connections;
  pthread_t thread;
  int started;
  volatile sig_atomic_t stopping;
};

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

static int make_error_json(const char *code, const char *message,
                           char **json, size_t *json_bytes) {
  int length = snprintf(NULL, 0, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                        code, message);
  if (length < 0) return YAP_V2_IO_ERROR;
  *json = malloc((size_t)length + 1U);
  if (*json == NULL) return YAP_V2_ALLOCATION_FAILED;
  (void)snprintf(*json, (size_t)length + 1U,
                 "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
  *json_bytes = (size_t)length;
  return YAP_V2_OK;
}

static void unlink_connection(connection_t *connection) {
  reactor_t *reactor = connection->reactor;
  if (connection->previous != NULL) connection->previous->next = connection->next;
  else reactor->connections = connection->next;
  if (connection->next != NULL) connection->next->previous = connection->previous;
  connection->previous = NULL;
  connection->next = NULL;
}

static void free_connection(connection_t *connection) {
  if (connection == NULL) return;
  if (connection->writer_admitted) {
    YAP_V2_runtime_limiter_release(connection->reactor->server->writer_limiter,
                                   connection->request.content_length);
    connection->writer_admitted = 0;
  }
  unlink_connection(connection);
  if (connection->buffered_event != NULL)
    bufferevent_free(connection->buffered_event);
  YAP_V2_core_http_request_free(&connection->request);
  free(connection);
}

static void reset_connection_request(connection_t *connection) {
  struct timeval timeout;
  if (connection->writer_admitted) {
    YAP_V2_runtime_limiter_release(connection->reactor->server->writer_limiter,
                                   connection->request.content_length);
    connection->writer_admitted = 0;
  }
  YAP_V2_core_http_request_free(&connection->request);
  YAP_V2_core_http_request_init(&connection->request);
  connection->request_head_parsed = 0;
  connection->response_pending = 0;
  connection->close_after_response = 0;
  timeout.tv_sec = (time_t)(
    connection->reactor->server->runtime_policy.request_timeout_ms / 1000U);
  timeout.tv_usec = (suseconds_t)(
    (connection->reactor->server->runtime_policy.request_timeout_ms % 1000U) *
    1000U);
  bufferevent_set_timeouts(connection->buffered_event, &timeout, &timeout);
}

static void abandon_connection(connection_t *connection) {
  if (connection->buffered_event != NULL) {
    bufferevent_free(connection->buffered_event);
    connection->buffered_event = NULL;
  }
  if (connection->inflight) connection->abandoned = 1;
  else free_connection(connection);
}

static int write_response(connection_t *connection, int status,
                          const char *allow, int accept_query,
                          const char *body, size_t body_bytes) {
  struct evbuffer *output;
  if (connection == NULL || connection->buffered_event == NULL ||
      (body_bytes != 0U && body == NULL) ||
      body_bytes > YAP_V2_CORE_HTTP_MAX_RESPONSE_BYTES)
    return YAP_V2_INVALID_ARGUMENT;
  output = evbuffer_new();
  if (output == NULL) return YAP_V2_ALLOCATION_FAILED;
  if (connection->reactor->stopping) connection->close_after_response = 1;
  if (evbuffer_add_printf(
        output,
        "HTTP/1.1 %d %s\r\nServer: Yappo Search Core/2.0\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: %s\r\n",
        status, reason_phrase(status), body_bytes,
        connection->close_after_response ? "close" : "keep-alive") < 0 ||
      (allow != NULL && evbuffer_add_printf(output, "Allow: %s\r\n", allow) < 0) ||
      (accept_query && evbuffer_add(output, "Accept-Query: application/json\r\n",
                                   sizeof("Accept-Query: application/json\r\n") - 1U) != 0) ||
      evbuffer_add(output, "\r\n", 2U) != 0 ||
      (body_bytes != 0U && evbuffer_add(output, body, body_bytes) != 0) ||
      bufferevent_write_buffer(connection->buffered_event, output) != 0) {
    evbuffer_free(output);
    return YAP_V2_IO_ERROR;
  }
  evbuffer_free(output);
  connection->response_pending = 1;
  (void)bufferevent_disable(connection->buffered_event, EV_READ);
  return YAP_V2_OK;
}

static void respond_error(connection_t *connection, int status,
                          const char *code, const char *message,
                          const char *allow, int accept_query) {
  char *json = NULL;
  size_t json_bytes = 0U;
  if (make_error_json(code, message, &json, &json_bytes) != YAP_V2_OK ||
      write_response(connection, status, allow, accept_query, json,
                     json_bytes) != YAP_V2_OK) {
    free(json);
    abandon_connection(connection);
    return;
  }
  free(json);
}

static void respond_fatal_error(connection_t *connection, int status,
                                const char *code, const char *message) {
  connection->close_after_response = 1;
  respond_error(connection, status, code, message, NULL, 0);
}

static void enqueue_message(reactor_t *reactor, message_t *message) {
  unsigned char notification = 1U;
  message->next = NULL;
  pthread_mutex_lock(&reactor->mailbox_lock);
  if (reactor->mailbox_tail != NULL) reactor->mailbox_tail->next = message;
  else reactor->mailbox_head = message;
  reactor->mailbox_tail = message;
  pthread_mutex_unlock(&reactor->mailbox_lock);
  if (write(reactor->notification_sockets[1], &notification,
            sizeof(notification)) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    /* The queued item remains visible to the reactor or final shutdown drain. */
  }
}

static void run_execution(void *opaque) {
  execution_t *execution = opaque;
  server_state_t *server = execution->reactor->server;
  connection_t *connection = execution->connection;
  if (execution->health_request) {
    YAP_V2_OPERATIONAL_STATE state, disk_state;
    char error[256] = {0};
    memset(&state, 0, sizeof(state));
    memset(&disk_state, 0, sizeof(disk_state));
    execution->result = YAP_V2_http_runtime_state(server->runtime, &state);
    if (execution->result == YAP_V2_OK &&
        YAP_V2_operational_probe_index_with_policy(
          server->index_dir, &server->compaction_policy, &disk_state,
          error, sizeof(error)) == YAP_V2_OK) {
      state.compaction_state = disk_state.compaction_state;
      state.compaction_generation = disk_state.compaction_generation;
      state.compaction_updated_at_unix = disk_state.compaction_updated_at_unix;
      if (state.generation == disk_state.generation) {
        state.document_records = disk_state.document_records;
        state.passage_records = disk_state.passage_records;
        state.tombstone_records = disk_state.tombstone_records;
        state.component_file_bytes = disk_state.component_file_bytes;
        state.smallest_segment_bytes = disk_state.smallest_segment_bytes;
        state.largest_segment_bytes = disk_state.largest_segment_bytes;
        state.small_segment_run = disk_state.small_segment_run;
        state.small_segment_threshold_bytes = disk_state.small_segment_threshold_bytes;
        state.auto_compaction_trigger_segments =
          disk_state.auto_compaction_trigger_segments;
        state.auto_compaction_enabled = disk_state.auto_compaction_enabled;
        state.auto_compaction_needed = disk_state.auto_compaction_needed;
      }
    }
    execution->http_status = execution->result == YAP_V2_OK && state.ready ? 200 : 503;
    if (YAP_V2_operational_state_json(&state, "yappod_core", &execution->json,
                                      &execution->json_bytes) != YAP_V2_OK)
      execution->result = YAP_V2_IO_ERROR;
  } else {
    execution->result = YAP_V2_http_runtime_execute(
      server->runtime, execution->operation, connection->request.body,
      connection->request.body_bytes, &execution->http_status,
      &execution->json, &execution->json_bytes);
  }
  enqueue_message(execution->reactor, &execution->message);
}

static const char *allow_for_target(const char *target) {
  if (strcmp(target, "/v2/search") == 0 || strcmp(target, "/v2/retrieve") == 0)
    return "QUERY";
  if (strcmp(target, "/v2/documents:batch") == 0) return "POST";
  if (strcmp(target, "/health/ready") == 0) return "GET";
  return NULL;
}

static int is_query_target(const char *target) {
  return strcmp(target, "/v2/search") == 0 ||
         strcmp(target, "/v2/retrieve") == 0;
}

static void submit_request(connection_t *connection) {
  reactor_t *reactor = connection->reactor;
  server_state_t *server = reactor->server;
  YAP_V2_CORE_HTTP_REQUEST *request = &connection->request;
  YAP_V2_EXECUTOR *executor;
  execution_t *execution;
  const char *allow = allow_for_target(request->target);
  int accept_query = is_query_target(request->target);
  int status;
  if (allow == NULL) {
    respond_error(connection, 404, "not_found", "Not Found", NULL, 0);
    return;
  }
  if ((accept_query && strcmp(request->method, "QUERY") != 0) ||
      (strcmp(request->target, "/v2/documents:batch") == 0 &&
       strcmp(request->method, "POST") != 0) ||
      (strcmp(request->target, "/health/ready") == 0 &&
       strcmp(request->method, "GET") != 0)) {
    respond_error(connection, 405, "method_not_allowed", "Method Not Allowed",
                  allow, accept_query);
    return;
  }
  if (strcmp(request->target, "/health/ready") == 0) {
    if (request->have_content_length) {
      respond_error(connection, 400, "invalid_request", "Bad Request", "GET", 0);
      return;
    }
  } else {
    if (!request->have_content_length || request->body_bytes == 0U) {
      respond_error(connection, 400, "invalid_request", "Bad Request",
                    allow, accept_query);
      return;
    }
    if (!request->json_content_type) {
      respond_error(connection, 415, "unsupported_media_type",
                    "Unsupported Media Type", allow, accept_query);
      return;
    }
  }
  execution = calloc(1U, sizeof(*execution));
  if (execution == NULL) {
    respond_error(connection, 503, "overloaded", "Service Unavailable", NULL, 0);
    return;
  }
  execution->message.type = MESSAGE_COMPLETE;
  execution->reactor = reactor;
  execution->connection = connection;
  execution->health_request = strcmp(request->target, "/health/ready") == 0;
  if (strcmp(request->target, "/v2/retrieve") == 0)
    execution->operation = YAP_V2_HTTP_RETRIEVE;
  else if (strcmp(request->target, "/v2/documents:batch") == 0)
    execution->operation = YAP_V2_HTTP_INGEST;
  else
    execution->operation = YAP_V2_HTTP_SEARCH;
  if (execution->operation == YAP_V2_HTTP_INGEST &&
      YAP_V2_authorize_write(
        &server->runtime_policy,
        request->authorization[0] == '\0' ? NULL : request->authorization) != YAP_V2_OK) {
    free(execution);
    respond_error(connection, 401, "unauthorized", "Unauthorized", NULL, 0);
    return;
  }
  if (!execution->health_request &&
      execution->operation != YAP_V2_HTTP_INGEST) {
    if (YAP_V2_runtime_limiter_acquire(server->search_limiter,
                                       request->body_bytes) != YAP_V2_OK) {
      free(execution);
      respond_error(connection, 503, "overloaded", "Service Unavailable", NULL, 0);
      return;
    }
    execution->limiter_acquired = 1;
  }
  executor = execution->operation == YAP_V2_HTTP_INGEST ?
             server->writer_executor : server->search_executor;
  connection->execution = execution;
  connection->inflight = 1;
  (void)bufferevent_disable(connection->buffered_event, EV_READ);
  status = YAP_V2_executor_try_submit(executor, run_execution, execution);
  if (status != YAP_V2_OK) {
    connection->execution = NULL;
    connection->inflight = 0;
    if (execution->limiter_acquired)
      YAP_V2_runtime_limiter_release(server->search_limiter,
                                     request->body_bytes);
    free(execution);
    respond_error(connection, 503, "overloaded", "Service Unavailable", NULL, 0);
  }
}

static void write_callback(struct bufferevent *buffered_event, void *opaque) {
  connection_t *connection = opaque;
  if (!connection->response_pending ||
      evbuffer_get_length(bufferevent_get_output(buffered_event)) != 0U)
    return;
  if (connection->close_after_response) {
    abandon_connection(connection);
    return;
  }
  reset_connection_request(connection);
  if (bufferevent_enable(buffered_event, EV_READ) != 0)
    abandon_connection(connection);
}

static void event_callback(struct bufferevent *buffered_event, short events,
                           void *opaque) {
  connection_t *connection = opaque;
  if ((events & BEV_EVENT_EOF) != 0 &&
      (connection->inflight || connection->response_pending)) {
    connection->close_after_response = 1;
    (void)bufferevent_disable(buffered_event, EV_READ);
    return;
  }
  if ((events & BEV_EVENT_EOF) != 0 && !connection->inflight &&
      !connection->response_pending) {
    size_t buffered_bytes = evbuffer_get_length(
      bufferevent_get_input(buffered_event));
    if ((!connection->request_head_parsed && buffered_bytes != 0U) ||
        (connection->request_head_parsed &&
         connection->request.have_content_length &&
         buffered_bytes < connection->request.content_length)) {
      abandon_connection(connection);
      return;
    }
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
    abandon_connection(connection);
}

static void read_callback(struct bufferevent *buffered_event, void *opaque) {
  connection_t *connection = opaque;
  server_state_t *server = connection->reactor->server;
  struct evbuffer *input = bufferevent_get_input(buffered_event);
  if (!connection->request_head_parsed) {
    struct evbuffer_ptr delimiter = evbuffer_search(input, "\r\n\r\n", 4U, NULL);
    size_t input_bytes = evbuffer_get_length(input);
    size_t head_bytes;
    const unsigned char *head;
    int status;
    if (delimiter.pos < 0) {
      if (input_bytes >= YAP_V2_CORE_HTTP_MAX_HEADER_BYTES)
        respond_fatal_error(connection, 413, "header_too_large",
                            "Content Too Large");
      return;
    }
    head_bytes = (size_t)delimiter.pos + 4U;
    if (head_bytes > YAP_V2_CORE_HTTP_MAX_HEADER_BYTES) {
      respond_fatal_error(connection, 413, "header_too_large",
                          "Content Too Large");
      return;
    }
    head = evbuffer_pullup(input, (ev_ssize_t)head_bytes);
    if (head == NULL) {
      abandon_connection(connection);
      return;
    }
    status = YAP_V2_core_http_parse_head(head, head_bytes, &connection->request);
    if (status != YAP_V2_CORE_HTTP_OK) {
      respond_fatal_error(connection, 400, "invalid_request", "Bad Request");
      return;
    }
    evbuffer_drain(input, head_bytes);
    connection->request_head_parsed = 1;
    connection->close_after_response = connection->request.close_connection;
    if (strcmp(connection->request.target, "/v2/documents:batch") == 0) {
      struct timeval ingest_timeout;
      ingest_timeout.tv_sec = (time_t)(
        server->runtime_policy.ingest_timeout_ms / 1000U);
      ingest_timeout.tv_usec = (suseconds_t)(
        (server->runtime_policy.ingest_timeout_ms % 1000U) * 1000U);
      bufferevent_set_timeouts(buffered_event, &ingest_timeout, &ingest_timeout);
    }
    if (connection->request.have_content_length) {
      size_t limit = strcmp(connection->request.target,
                            "/v2/documents:batch") == 0 ?
                     server->runtime_policy.ingest_max_body_bytes :
                     YAP_V2_HTTP_MAX_BODY_BYTES;
      if (connection->request.content_length > limit) {
        respond_fatal_error(connection, 413, "body_too_large",
                            "Content Too Large");
        return;
      }
      if (strcmp(connection->request.target, "/v2/documents:batch") == 0 &&
          connection->request.content_length != 0U) {
        if (YAP_V2_runtime_limiter_acquire(
              server->writer_limiter,
              connection->request.content_length) != YAP_V2_OK) {
          respond_fatal_error(connection, 503, "overloaded",
                              "Service Unavailable");
          return;
        }
        connection->writer_admitted = 1;
      }
    }
  }
  if (connection->request.have_content_length &&
      evbuffer_get_length(input) < connection->request.content_length)
    return;
  if (connection->request.have_content_length &&
      connection->request.content_length != 0U) {
    connection->request.body = malloc(connection->request.content_length);
    if (connection->request.body == NULL) {
      respond_fatal_error(connection, 503, "overloaded",
                          "Service Unavailable");
      return;
    }
    if (evbuffer_remove(input, connection->request.body,
                        connection->request.content_length) !=
        (ev_ssize_t)connection->request.content_length) {
      abandon_connection(connection);
      return;
    }
    connection->request.body_bytes = connection->request.content_length;
  }
  submit_request(connection);
}

static void add_connection(reactor_t *reactor, int descriptor) {
  connection_t *connection;
  struct timeval timeout;
  if (reactor->stopping) {
    (void)close(descriptor);
    return;
  }
  connection = calloc(1U, sizeof(*connection));
  if (connection == NULL) {
    (void)close(descriptor);
    return;
  }
  connection->reactor = reactor;
  YAP_V2_core_http_request_init(&connection->request);
  connection->buffered_event = bufferevent_socket_new(
    reactor->base, descriptor, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  if (connection->buffered_event == NULL) {
    free(connection);
    (void)close(descriptor);
    return;
  }
  connection->next = reactor->connections;
  if (reactor->connections != NULL) reactor->connections->previous = connection;
  reactor->connections = connection;
  timeout.tv_sec = (time_t)(reactor->server->runtime_policy.request_timeout_ms / 1000U);
  timeout.tv_usec = (suseconds_t)(
    (reactor->server->runtime_policy.request_timeout_ms % 1000U) * 1000U);
  bufferevent_set_timeouts(connection->buffered_event, &timeout, &timeout);
  bufferevent_setcb(connection->buffered_event, read_callback, write_callback,
                    event_callback, connection);
  if (bufferevent_enable(connection->buffered_event, EV_READ | EV_WRITE) != 0)
    abandon_connection(connection);
}

static void complete_execution(execution_t *execution) {
  connection_t *connection = execution->connection;
  server_state_t *server = execution->reactor->server;
  connection->execution = NULL;
  connection->inflight = 0;
  if (execution->limiter_acquired)
    YAP_V2_runtime_limiter_release(server->search_limiter,
                                   connection->request.body_bytes);
  if (connection->abandoned || connection->buffered_event == NULL) {
    free(execution->json);
    free(execution);
    free_connection(connection);
    return;
  }
  if (execution->result != YAP_V2_OK ||
      write_response(connection, execution->http_status, NULL,
                     is_query_target(connection->request.target),
                     execution->json, execution->json_bytes) != YAP_V2_OK) {
    free(execution->json);
    free(execution);
    abandon_connection(connection);
    return;
  }
  free(execution->json);
  free(execution);
}

static void notification_callback(evutil_socket_t descriptor, short events,
                                  void *opaque) {
  reactor_t *reactor = opaque;
  unsigned char notifications[128];
  message_t *messages;
  (void)events;
  while (read(descriptor, notifications, sizeof(notifications)) > 0) {}
  pthread_mutex_lock(&reactor->mailbox_lock);
  messages = reactor->mailbox_head;
  reactor->mailbox_head = NULL;
  reactor->mailbox_tail = NULL;
  pthread_mutex_unlock(&reactor->mailbox_lock);
  while (messages != NULL) {
    message_t *next = messages->next;
    if (messages->type == MESSAGE_ACCEPT) {
      accept_message_t *accepted = (accept_message_t *)messages;
      add_connection(reactor, accepted->descriptor);
      free(accepted);
    } else {
      complete_execution((execution_t *)messages);
    }
    messages = next;
  }
  if (reactor->stopping) {
    connection_t *connection = reactor->connections;
    while (connection != NULL) {
      connection_t *next = connection->next;
      abandon_connection(connection);
      connection = next;
    }
    event_base_loopbreak(reactor->base);
  }
}

static void *run_reactor(void *opaque) {
  reactor_t *reactor = opaque;
  (void)event_base_dispatch(reactor->base);
  return NULL;
}

static void *run_acceptor(void *opaque) {
  server_state_t *server = opaque;
  size_t next_reactor = 0U;
  while (!server->stopping) {
    struct pollfd readiness;
    int poll_status;
    readiness.fd = server->listen_socket;
    readiness.events = POLLIN;
    readiness.revents = 0;
    poll_status = poll(&readiness, 1U, 100);
    if (poll_status < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (poll_status == 0) continue;
    if ((readiness.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
    for (;;) {
      int descriptor = accept(server->listen_socket, NULL, NULL);
      accept_message_t *message;
      reactor_t *reactor;
      if (descriptor < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (server->stopping || errno == EBADF || errno == EINVAL) break;
        break;
      }
      if (evutil_make_socket_nonblocking(descriptor) != 0) {
        (void)close(descriptor);
        continue;
      }
      message = malloc(sizeof(*message));
      if (message == NULL) {
        (void)close(descriptor);
        continue;
      }
      reactor = &server->reactors[next_reactor];
      next_reactor = (next_reactor + 1U) % server->reactor_count;
      message->message.type = MESSAGE_ACCEPT;
      message->descriptor = descriptor;
      enqueue_message(reactor, &message->message);
    }
    if (server->stopping) break;
  }
  return NULL;
}

static int open_reactor(reactor_t *reactor, server_state_t *server, size_t id) {
  memset(reactor, 0, sizeof(*reactor));
  reactor->notification_sockets[0] = -1;
  reactor->notification_sockets[1] = -1;
  reactor->server = server;
  reactor->id = id;
  if (pthread_mutex_init(&reactor->mailbox_lock, NULL) != 0) return YAP_V2_IO_ERROR;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, reactor->notification_sockets) != 0 ||
      evutil_make_socket_nonblocking(reactor->notification_sockets[0]) != 0 ||
      evutil_make_socket_nonblocking(reactor->notification_sockets[1]) != 0)
    goto failed;
  reactor->base = event_base_new();
  if (reactor->base == NULL) goto failed;
  reactor->notification_event = event_new(
    reactor->base, reactor->notification_sockets[0], EV_READ | EV_PERSIST,
    notification_callback, reactor);
  if (reactor->notification_event == NULL ||
      event_add(reactor->notification_event, NULL) != 0)
    goto failed;
  if (pthread_create(&reactor->thread, NULL, run_reactor, reactor) != 0)
    goto failed;
  reactor->started = 1;
  return YAP_V2_OK;
failed:
  if (reactor->notification_event != NULL) event_free(reactor->notification_event);
  if (reactor->base != NULL) event_base_free(reactor->base);
  if (reactor->notification_sockets[0] >= 0) close(reactor->notification_sockets[0]);
  if (reactor->notification_sockets[1] >= 0) close(reactor->notification_sockets[1]);
  pthread_mutex_destroy(&reactor->mailbox_lock);
  memset(reactor, 0, sizeof(*reactor));
  return YAP_V2_IO_ERROR;
}

static void stop_reactor(reactor_t *reactor) {
  unsigned char notification = 1U;
  message_t *messages;
  if (!reactor->started) return;
  reactor->stopping = 1;
  (void)write(reactor->notification_sockets[1], &notification,
              sizeof(notification));
  (void)pthread_join(reactor->thread, NULL);
  reactor->started = 0;
  pthread_mutex_lock(&reactor->mailbox_lock);
  messages = reactor->mailbox_head;
  reactor->mailbox_head = NULL;
  reactor->mailbox_tail = NULL;
  pthread_mutex_unlock(&reactor->mailbox_lock);
  while (messages != NULL) {
    message_t *next = messages->next;
    if (messages->type == MESSAGE_ACCEPT) {
      accept_message_t *accepted = (accept_message_t *)messages;
      close(accepted->descriptor);
      free(accepted);
    } else {
      execution_t *execution = (execution_t *)messages;
      free(execution->json);
      free(execution);
    }
    messages = next;
  }
  event_free(reactor->notification_event);
  event_base_free(reactor->base);
  close(reactor->notification_sockets[0]);
  close(reactor->notification_sockets[1]);
  pthread_mutex_destroy(&reactor->mailbox_lock);
}

void YAP_V2_core_reactor_server_init(YAP_V2_CORE_REACTOR_SERVER *server) {
  if (server != NULL) server->state = NULL;
}

int YAP_V2_core_reactor_server_open(
  YAP_V2_CORE_REACTOR_SERVER *server, int listen_socket, const char *index_dir,
  YAP_V2_HTTP_RUNTIME *runtime, YAP_V2_EXECUTOR *search_executor,
  YAP_V2_EXECUTOR *writer_executor, YAP_V2_RUNTIME_LIMITER *search_limiter,
  YAP_V2_RUNTIME_LIMITER *writer_limiter,
  const YAP_V2_RUNTIME_POLICY *runtime_policy,
  const YAP_V2_COMPACTION_POLICY *compaction_policy, size_t reactor_threads) {
  server_state_t *state;
  size_t i;
  if (server == NULL || server->state != NULL || listen_socket < 0 ||
      index_dir == NULL || runtime == NULL || search_executor == NULL ||
      writer_executor == NULL || search_limiter == NULL || runtime_policy == NULL ||
      writer_limiter == NULL ||
      compaction_policy == NULL || reactor_threads == 0U ||
      reactor_threads > SIZE_MAX / sizeof(reactor_t))
    return YAP_V2_INVALID_ARGUMENT;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return YAP_V2_ALLOCATION_FAILED;
  state->reactors = calloc(reactor_threads, sizeof(*state->reactors));
  if (state->reactors == NULL) {
    free(state);
    return YAP_V2_ALLOCATION_FAILED;
  }
  state->listen_socket = listen_socket;
  state->index_dir = index_dir;
  state->runtime = runtime;
  state->search_executor = search_executor;
  state->writer_executor = writer_executor;
  state->search_limiter = search_limiter;
  state->writer_limiter = writer_limiter;
  state->runtime_policy = *runtime_policy;
  state->compaction_policy = *compaction_policy;
  state->reactor_count = reactor_threads;
  if (evutil_make_socket_nonblocking(listen_socket) != 0) {
    free(state->reactors);
    free(state);
    return YAP_V2_IO_ERROR;
  }
  for (i = 0U; i < reactor_threads; i++) {
    if (open_reactor(&state->reactors[i], state, i) != YAP_V2_OK) break;
    state->started_reactors++;
  }
  if (state->started_reactors != reactor_threads ||
      pthread_create(&state->acceptor_thread, NULL, run_acceptor, state) != 0) {
    state->stopping = 1;
    for (i = 0U; i < state->started_reactors; i++) stop_reactor(&state->reactors[i]);
    free(state->reactors);
    free(state);
    return YAP_V2_IO_ERROR;
  }
  state->acceptor_started = 1;
  server->state = state;
  return YAP_V2_OK;
}

void YAP_V2_core_reactor_server_stop_accepting(
  YAP_V2_CORE_REACTOR_SERVER *server) {
  server_state_t *state;
  if (server == NULL || server->state == NULL) return;
  state = server->state;
  state->stopping = 1;
  if (state->listen_socket >= 0) {
    (void)shutdown(state->listen_socket, SHUT_RDWR);
    (void)close(state->listen_socket);
    state->listen_socket = -1;
  }
  if (state->acceptor_started) {
    (void)pthread_join(state->acceptor_thread, NULL);
    state->acceptor_started = 0;
  }
}

void YAP_V2_core_reactor_server_close(YAP_V2_CORE_REACTOR_SERVER *server) {
  server_state_t *state;
  size_t i;
  if (server == NULL || server->state == NULL) return;
  state = server->state;
  YAP_V2_core_reactor_server_stop_accepting(server);
  for (i = 0U; i < state->started_reactors; i++)
    stop_reactor(&state->reactors[i]);
  free(state->reactors);
  free(state);
  server->state = NULL;
}
