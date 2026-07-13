/*
  フロントサーバ
  HTTPにより検索条件を受けつけ、各検索コアサーバに要求を送り
  検索結果をまとめてクライアントに返す
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>

#include "yappo_alloc.h"
#include "yappo_io.h"
#include "yappo_net.h"
#include "yappo_stat.h"
#include "yappo_search.h"
#include "yappo_index.h"
#include "yappo_index_filedata.h"
#include "yappo_linklist.h"
#include "yappo_proto.h"
#include "yappo_search_api.h"
#include "yappo_core_protocol_v2.h"
#include "yappo_http_v2.h"
#include "yappo_observability_v2.h"
#include "yappo_runtime_policy_v2.h"

#define BUF_SIZE 1024
#define MAX_HTTP_LINE_SIZE (16 * 1024)
#define DEFAULT_FRONT_PORT 10080
#define DEFAULT_CORE_PORT 10086

/*
 *スレッド毎の構造
 */
typedef struct {
  int id;
  int socket;
  int server_num;
  int core_port;
  FILE **server_socket;
  int *server_fd;
  char **server_addr;
  char *base_dir;
  uint64_t next_request_id;
} YAP_THREAD_DATA;

/* スレッドの数 */
#define MAX_THREAD 5

int count;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signal = 0;
static const char *g_front_pidfile = "./front.pid";
static YAP_V2_RUNTIME_POLICY g_runtime_policy;
static YAP_V2_RUNTIME_LIMITER g_runtime_limiter;
static YAP_V2_METRICS g_v2_metrics;
static void YAP_request_shutdown(int sig);
static int YAP_readline_alloc(FILE *socket, char **line_out);

void YAP_Error(char *msg) {
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(EXIT_FAILURE);
}

static void YAP_print_usage(FILE *out, const char *prog) {
  fprintf(out,
          "Usage: %s -l <index_dir> -s <core_host> [-s <core_host> ...] "
          "[-p <front_port>] [-P <core_port>]\n"
          "  -l <index_dir>   Index directory path (required)\n"
          "  -s <core_host>   Core server host (required, repeatable)\n"
          "  -p <front_port>  Front listen port (default: %d)\n"
          "  -P <core_port>   Core connect port (default: %d)\n"
          "  -h, --help       Show this help\n",
          prog, DEFAULT_FRONT_PORT, DEFAULT_CORE_PORT);
}

static int YAP_parse_port(const char *value, int *port_out) {
  long port;
  char *endptr;

  if (value == NULL || port_out == NULL) {
    return -1;
  }

  port = strtol(value, &endptr, 10);
  if (*value == '\0' || *endptr != '\0' || port < 1 || port > 65535) {
    return -1;
  }
  *port_out = (int)port;
  return 0;
}

static int YAP_install_signal_handlers(void) {
  struct sigaction sa;
  struct sigaction ign_sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = YAP_request_shutdown;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
    return -1;
  }

  memset(&ign_sa, 0, sizeof(ign_sa));
  ign_sa.sa_handler = SIG_IGN;
  ign_sa.sa_flags = SA_RESTART;
  sigemptyset(&ign_sa.sa_mask);
  if (sigaction(SIGPIPE, &ign_sa, NULL) != 0) {
    return -1;
  }

  return 0;
}

static void YAP_log_thread_error(int thread_id, const char *msg) {
  fprintf(stderr, "ERROR: front thread %d %s\n", thread_id, msg);
}

static void YAP_request_shutdown(int sig) {
  g_shutdown_signal = sig;
  g_shutdown_requested = 1;
}

static void YAP_remove_pidfile(void) { unlink(g_front_pidfile); }

static int YAP_send_bad_request(int fd, int thread_id) {
  const char *msg = "HTTP/1.0 400 Bad Request\r\n"
                    "Server: Yappo Search/1.0\r\n"
                    "Content-Type: text/html\r\n"
                    "\r\n"
                    "Bad Search Request<br>By Yappo Search";
  return YAP_Net_write_all(fd, msg, strlen(msg), "front", thread_id);
}

static int YAP_send_http_payload(int fd, int thread_id, int status, const char *reason,
                                 const char *content_type, const char *body, size_t body_bytes) {
  char headers[768]; int header_bytes = snprintf(headers, sizeof(headers),
    "HTTP/1.1 %d %s\r\nServer: Yappo Search/2.0\r\nContent-Type: %s\r\n"
    "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
    status, reason, content_type, body_bytes);
  if (header_bytes < 0 || (size_t)header_bytes >= sizeof(headers) ||
      YAP_Net_write_all(fd, headers, (size_t)header_bytes, "front", thread_id) != 0) return -1;
  return YAP_Net_write_all(fd, body, body_bytes, "front", thread_id);
}

static int YAP_send_json_error(int fd, int thread_id, int status, const char *reason,
                               const char *code) {
  char body[256], response[768]; int body_len, response_len;
  body_len = snprintf(body, sizeof(body), "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                      code, reason);
  if (body_len < 0 || (size_t)body_len >= sizeof(body)) return -1;
  response_len = snprintf(response, sizeof(response),
    "HTTP/1.1 %d %s\r\nServer: Yappo Search/2.0\r\nContent-Type: application/json; charset=utf-8\r\n"
    "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n%s",
    status, reason, body_len, body);
  if (response_len < 0 || (size_t)response_len >= sizeof(response)) return -1;
  return YAP_Net_write_all(fd, response, (size_t)response_len, "front", thread_id);
}

static void YAP_drain_oversized_line(FILE *socket, char *socket_buf, size_t chunk_len) {
  while (chunk_len == (size_t)(BUF_SIZE - 1) && socket_buf[chunk_len - 1] != '\n') {
    if (fgets(socket_buf, BUF_SIZE, socket) == NULL) {
      break;
    }
    chunk_len = strlen(socket_buf);
    if (chunk_len == 0) {
      break;
    }
  }
}

static int YAP_writef(FILE *socket, const char *fmt, ...) {
  int rc;
  va_list ap;
  va_start(ap, fmt);
  rc = vfprintf(socket, fmt, ap);
  va_end(ap);
  if (rc < 0) {
    perror("ERROR: front response write");
    return -1;
  }
  return 0;
}

static int YAP_connect_core_stream(const char *host, int core_port, FILE **stream_out, int *fd_out,
                                   int thread_id) {
  struct addrinfo hints;
  struct addrinfo *res = NULL, *rp;
  char port_str[16];
  int gai_rc;
  int fd = -1;
  FILE *stream;

  *stream_out = NULL;
  *fd_out = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(port_str, sizeof(port_str), "%d", core_port);
  gai_rc = getaddrinfo(host, port_str, &hints, &res);
  if (gai_rc != 0) {
    fprintf(stderr, "ERROR: front thread %d resolve %s failed: %s\n", thread_id, host,
            gai_strerror(gai_rc));
    fflush(stderr);
    return -1;
  }

  for (rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  if (fd < 0) {
    fprintf(stderr, "ERROR: front thread %d connect %s:%d failed\n", thread_id, host, core_port);
    fflush(stderr);
    return -1;
  }
  if (YAP_V2_socket_set_deadline(fd, g_runtime_policy.request_timeout_ms) != YAP_V2_OK) {
    close(fd); return -1;
  }

  stream = (FILE *)fdopen(fd, "r+");
  if (stream == NULL) {
    perror("ERROR: front core fdopen");
    close(fd);
    return -1;
  }

  *stream_out = stream;
  *fd_out = fd;
  return 0;
}

static int YAP_flush_or_log(FILE *socket) {
  if (fflush(socket) != 0) {
    perror("ERROR: front response flush");
    return -1;
  }
  return 0;
}

static int YAP_parse_v2_post_line(const char *line, YAP_V2_HTTP_OPERATION *operation) {
  char method[16], target[BUF_SIZE], version[32]; int consumed = 0;
  if (line == NULL || operation == NULL ||
      sscanf(line, "%15s %1023s %31s%n", method, target, version, &consumed) != 3 ||
      strcmp(method, "POST") != 0 ||
      (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) ||
      (line[consumed] != '\r' && line[consumed] != '\n' && line[consumed] != '\0')) return -1;
  if (strcmp(target, "/v2/search") == 0) *operation = YAP_V2_HTTP_SEARCH;
  else if (strcmp(target, "/v2/retrieve") == 0) *operation = YAP_V2_HTTP_RETRIEVE;
  else if (strcmp(target, "/v2/documents:batch") == 0) *operation = YAP_V2_HTTP_INGEST;
  else return -1;
  return 0;
}

static int YAP_is_v2_endpoint_with_non_post_method(const char *line) {
  char method[16], target[BUF_SIZE], version[32]; int consumed = 0;
  if (line == NULL || sscanf(line, "%15s %1023s %31s%n", method, target, version, &consumed) != 3 ||
      (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) ||
      (line[consumed] != '\r' && line[consumed] != '\n' && line[consumed] != '\0')) return 0;
  return strcmp(method, "POST") != 0 &&
         ((strncmp(target, "/v2/search", 10U) == 0 && (target[10] == '\0' || target[10] == '?')) ||
          (strncmp(target, "/v2/retrieve", 12U) == 0 && (target[12] == '\0' || target[12] == '?')) ||
          (strncmp(target, "/v2/documents:batch", 19U) == 0 &&
           (target[19] == '\0' || target[19] == '?')));
}

static int YAP_read_v2_headers(FILE *socket, size_t *content_length,
                               char authorization[YAP_V2_AUTHORIZATION_MAX_BYTES + 1U]) {
  int have_type = 0, have_length = 0, have_authorization = 0;
  *content_length = 0U; authorization[0] = '\0';
  while (1) {
    char *line = NULL, *end; unsigned long long parsed; int read_rc = YAP_readline_alloc(socket, &line);
    if (read_rc <= 0) return -1;
    if ((line[0] == '\r' && line[1] == '\n') || (line[0] == '\n' && line[1] == '\0')) {
      free(line); break;
    }
    if (strncasecmp(line, "Content-Type:", 13U) == 0) {
      const char *value = line + 13U; while (*value == ' ' || *value == '\t') value++;
      if (have_type || strncasecmp(value, "application/json", 16U) != 0 ||
          (value[16] != '\r' && value[16] != '\n' && value[16] != ';')) { free(line); return -2; }
      have_type = 1;
    } else if (strncasecmp(line, "Content-Length:", 15U) == 0) {
      const char *value = line + 15U; while (*value == ' ' || *value == '\t') value++;
      errno = 0; parsed = strtoull(value, &end, 10);
      while (*end == ' ' || *end == '\t') end++;
      if (have_length || errno != 0 || end == value || (*end != '\r' && *end != '\n') || parsed == 0U) {
        free(line); return -1;
      }
      if (parsed > YAP_V2_HTTP_MAX_BODY_BYTES) { free(line); return -3; }
      *content_length = (size_t)parsed; have_length = 1;
    } else if (strncasecmp(line, "Transfer-Encoding:", 18U) == 0) {
      free(line); return -1;
    } else if (strncasecmp(line, "Authorization:", 14U) == 0) {
      const char *value = line + 14U; size_t length;
      while (*value == ' ' || *value == '\t') value++;
      length = strcspn(value, "\r\n");
      if (have_authorization || length == 0U || length > YAP_V2_AUTHORIZATION_MAX_BYTES) {
        free(line); return -1;
      }
      memcpy(authorization, value, length); authorization[length] = '\0'; have_authorization = 1;
    }
    free(line);
  }
  return have_type && have_length ? 0 : -1;
}

static int YAP_send_v2_http(FILE *client, FILE *core, YAP_V2_HTTP_OPERATION operation,
                            const unsigned char *body, size_t body_bytes, uint64_t request_id,
                            int *http_status_out) {
  YAP_V2_CORE_FRAME request, response; uint16_t expected; int status, http_status;
  const char *reason; size_t json_bytes, request_bytes = body_bytes; unsigned char *envelope = NULL;
  YAP_V2_core_frame_init(&request); YAP_V2_core_frame_init(&response);
  if (http_status_out == NULL) return -1;
  *http_status_out = 503;
  request.type = operation == YAP_V2_HTTP_SEARCH ? YAP_V2_CORE_SEARCH_REQUEST :
                 operation == YAP_V2_HTTP_RETRIEVE ? YAP_V2_CORE_RETRIEVE_REQUEST :
                 YAP_V2_CORE_INGEST_REQUEST;
  if (operation == YAP_V2_HTTP_INGEST &&
      YAP_V2_ingest_envelope_wrap(&g_runtime_policy, body, body_bytes, &envelope,
                                  &request_bytes) != YAP_V2_OK) { status = -1; goto done; }
  request.request_id = request_id; request.payload = envelope == NULL ? (unsigned char *)body : envelope;
  request.payload_bytes = (uint32_t)request_bytes;
  status = YAP_V2_core_frame_write(core, &request, YAP_V2_CORE_MAX_PAYLOAD_BYTES);
  if (status != YAP_V2_CORE_FRAME_OK || fflush(core) != 0 ||
      YAP_V2_core_frame_read(core, YAP_V2_CORE_MAX_PAYLOAD_BYTES, &response) != YAP_V2_CORE_FRAME_OK ||
      response.request_id != request_id || response.payload_bytes < 2U) { status = -1; goto done; }
  expected = operation == YAP_V2_HTTP_SEARCH ? YAP_V2_CORE_SEARCH_RESPONSE :
             operation == YAP_V2_HTTP_RETRIEVE ? YAP_V2_CORE_RETRIEVE_RESPONSE :
             YAP_V2_CORE_INGEST_RESPONSE;
  if (response.type != expected && response.type != YAP_V2_CORE_ERROR_RESPONSE) { status = -1; goto done; }
  http_status = ((int)response.payload[0] << 8) | response.payload[1];
  *http_status_out = http_status;
  if ((response.type == expected && http_status != 200) ||
      (response.type == YAP_V2_CORE_ERROR_RESPONSE && http_status < 400)) { status = -1; goto done; }
  reason = http_status == 200 ? "OK" : http_status == 400 ? "Bad Request" :
           http_status == 401 ? "Unauthorized" :
           http_status == 409 ? "Conflict" : "Service Unavailable";
  json_bytes = response.payload_bytes - 2U;
  if (YAP_writef(client, "HTTP/1.1 %d %s\r\nServer: Yappo Search/2.0\r\n"
                 "Content-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", http_status, reason, json_bytes) != 0 ||
      fwrite(response.payload + 2U, 1U, json_bytes, client) != json_bytes || fflush(client) != 0)
    status = -1;
  else status = 0;
done:
  free(envelope); YAP_V2_core_frame_free(&response); return status;
}

/*
 *検索結果を標示
 */
void search_result_print(YAPPO_DB_FILES *ydfp, FILE *socket, SEARCH_RESULT *p, int start, int end) {
  int i;
  FILEDATA filedata;
  char *title;

  if (YAP_writef(
        socket,
        "HTTP/1.0 200 OK\r\nServer: Yappo Search/1.0\r\nContent-Type: text/plain\r\n\r\n") != 0 ||
      YAP_flush_or_log(socket) != 0) {
    return;
  }

  if (start < 0) {
    start = 0;
  }
  if (end < 0) {
    end = 0;
  }
  if (end < start) {
    end = start;
  }

  if (p == NULL || start >= p->keyword_docs_num) {
    if (YAP_writef(socket, "0\n\n") != 0 || YAP_flush_or_log(socket) != 0) {
      return;
    }
  } else {

    if (end >= p->keyword_docs_num) {
      end = p->keyword_docs_num;
    }

    if (YAP_writef(socket, "%d\n%d\n\n", p->keyword_docs_num, end - start) != 0 ||
        YAP_flush_or_log(socket) != 0) {
      return;
    }

    for (i = start; i < end; i++) {
      if (YAP_Index_Filedata_get(ydfp, p->docs_list[i].fileindex, &filedata) == 0) {
        const char *url = filedata.url;
        title = filedata.title;
        if (url == NULL) {
          url = "";
        }
        if (title == NULL) {
          title = (char *)url;
        }
        if (YAP_writef(socket, "%s\t%s\t%d\t%ld\t%.2f\n", url, title, filedata.size,
                       (long)filedata.lastmod, p->docs_list[i].score) != 0 ||
            YAP_flush_or_log(socket) != 0) {
          YAP_Index_Filedata_free(&filedata);
          return;
        }

        YAP_Index_Filedata_free(&filedata);
      } else {
        if (YAP_writef(socket, "%d\t%.2f\n", p->docs_list[i].fileindex, p->docs_list[i].score) !=
              0 ||
            YAP_flush_or_log(socket) != 0) {
          return;
        }
      }
    }
  }
  YAP_flush_or_log(socket);
}

static int YAP_hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int YAP_copy_query_value(const char *value, size_t value_len, char *out, size_t out_size) {
  size_t i;
  size_t out_len = 0U;

  if (value == NULL || out == NULL || out_size == 0U) return -1;
  for (i = 0U; i < value_len; i++) {
    unsigned char c = (unsigned char)value[i];
    if (c == '%') {
      int hi, lo;
      if (i + 2U >= value_len || (hi = YAP_hex_digit(value[i + 1U])) < 0 ||
          (lo = YAP_hex_digit(value[i + 2U])) < 0 || (hi == 0 && lo == 0)) {
        return -1;
      }
      c = (unsigned char)((hi << 4) | lo);
      i += 2U;
    }
    if (c < 0x20U || c == 0x7fU) return -1;
    if (out_len + 1U >= out_size) return -1;
    out[out_len++] = (char)c;
  }
  if (out_len == 0U) return -1;
  out[out_len] = '\0';
  return 0;
}

static int YAP_parse_positive_int(const char *value, size_t value_len, int *out) {
  char buf[64];
  char *endptr;
  long parsed;

  if (value_len == 0U || value_len >= sizeof(buf) || out == NULL) return -1;
  memcpy(buf, value, value_len);
  buf[value_len] = '\0';
  errno = 0;
  parsed = strtol(buf, &endptr, 10);
  if (errno != 0 || endptr == buf || *endptr != '\0' || parsed <= 0 || parsed > INT_MAX) return -1;
  *out = (int)parsed;
  return 0;
}

static int YAP_parse_v2_request_line(const char *line, char *dict, int *max_size, char *op,
                                     char *keyword, size_t *limit_out, char *cursor) {
  char method[16], target[BUF_SIZE], version[32];
  const char *query, *part;
  int have_dict = 0, have_op = 0, have_query = 0, have_max_size = 0, have_limit = 0,
      have_cursor = 0;
  int parsed_max_size = INT_MAX;
  size_t parsed_limit = YAP_SEARCH_API_DEFAULT_LIMIT;

  if (line == NULL || dict == NULL || max_size == NULL || op == NULL || keyword == NULL ||
      limit_out == NULL || cursor == NULL || sscanf(line, "%15s %1023s %31s", method, target,
                                                       version) != 3 ||
      strcmp(method, "GET") != 0 || strncmp(version, "HTTP/", 5) != 0) {
    return -1;
  }
  if (strncmp(target, "/v2/search?", 11) != 0 || target[11] == '\0') return -1;
  query = target + 11;
  dict[0] = op[0] = keyword[0] = cursor[0] = '\0';

  for (part = query; part != NULL && *part != '\0';) {
    const char *next = strchr(part, '&');
    const char *equals = memchr(part, '=', next == NULL ? strlen(part) : (size_t)(next - part));
    size_t segment_len = next == NULL ? strlen(part) : (size_t)(next - part);
    size_t key_len;
    const char *value;
    size_t value_len;
    if (equals == NULL || equals >= part + segment_len) return -1;
    key_len = (size_t)(equals - part);
    value = equals + 1;
    value_len = segment_len - key_len - 1U;
    if (key_len == 0U || value_len == 0U) return -1;
    if (key_len == 4U && strncmp(part, "dict", 4U) == 0) {
      if (have_dict || YAP_copy_query_value(value, value_len, dict, BUF_SIZE) != 0) return -1;
      have_dict = 1;
    } else if (key_len == 2U && strncmp(part, "op", 2U) == 0) {
      if (have_op || YAP_copy_query_value(value, value_len, op, BUF_SIZE) != 0 ||
          (strcmp(op, "AND") != 0 && strcmp(op, "OR") != 0)) return -1;
      have_op = 1;
    } else if (key_len == 1U && part[0] == 'q') {
      if (have_query || YAP_copy_query_value(value, value_len, keyword, BUF_SIZE) != 0) return -1;
      have_query = 1;
    } else if (key_len == 8U && strncmp(part, "max_size", 8U) == 0) {
      if (have_max_size || YAP_parse_positive_int(value, value_len, &parsed_max_size) != 0)
        return -1;
      have_max_size = 1;
    } else if (key_len == 5U && strncmp(part, "limit", 5U) == 0) {
      int parsed;
      if (have_limit || YAP_parse_positive_int(value, value_len, &parsed) != 0 ||
          parsed > YAP_SEARCH_API_MAX_LIMIT) return -1;
      parsed_limit = (size_t)parsed;
      have_limit = 1;
    } else if (key_len == 6U && strncmp(part, "cursor", 6U) == 0) {
      if (have_cursor || YAP_copy_query_value(value, value_len, cursor, BUF_SIZE) != 0) return -1;
      have_cursor = 1;
    } else {
      return -1;
    }
    part = next == NULL ? NULL : next + 1;
    if (part != NULL && *part == '\0') return -1;
  }
  if (!have_dict || !have_op || !have_query) return -1;
  *max_size = parsed_max_size;
  *limit_out = parsed_limit;
  return 0;
}

static int YAP_search_result_print_json(YAPPO_DB_FILES *ydfp, FILE *socket, SEARCH_RESULT *result,
                                        size_t limit, const char *cursor) {
  YAP_SEARCH_API_DOCUMENT documents[YAP_SEARCH_API_MAX_LIMIT];
  char *urls[YAP_SEARCH_API_MAX_LIMIT];
  char *titles[YAP_SEARCH_API_MAX_LIMIT];
  size_t total = result == NULL ? 0U : (size_t)result->keyword_docs_num;
  size_t start, end, i, count;

  memset(urls, 0, sizeof(urls));
  memset(titles, 0, sizeof(titles));
  if (YAP_Search_api_page(total, limit, cursor, &start, &end) != 0) {
    return -1;
  }
  count = end - start;
  for (i = 0U; i < count; i++) {
    FILEDATA filedata;
    const char *url = "";
    const char *title = "";
    memset(&filedata, 0, sizeof(filedata));
    if (YAP_Index_Filedata_get(ydfp, result->docs_list[start + i].fileindex, &filedata) == 0) {
      url = filedata.url == NULL ? "" : filedata.url;
      title = filedata.title == NULL ? url : filedata.title;
      documents[i].size = filedata.size;
      documents[i].lastmod = (long)filedata.lastmod;
    } else {
      documents[i].size = 0;
      documents[i].lastmod = 0L;
    }
    urls[i] = (char *)YAP_malloc(strlen(url) + 1U);
    titles[i] = (char *)YAP_malloc(strlen(title) + 1U);
    memcpy(urls[i], url, strlen(url) + 1U);
    memcpy(titles[i], title, strlen(title) + 1U);
    documents[i].url = urls[i];
    documents[i].title = titles[i];
    documents[i].score = result->docs_list[start + i].score;
    YAP_Index_Filedata_free(&filedata);
  }
  if (YAP_writef(socket,
                 "HTTP/1.0 200 OK\r\nServer: Yappo Search/2.0\r\n"
                 "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n\r\n") != 0) {
    for (i = 0U; i < count; i++) {
      free(urls[i]);
      free(titles[i]);
    }
    return -1;
  }
  if (YAP_Search_api_write_json(socket, total, start, end, limit, documents) != 0 ||
      YAP_flush_or_log(socket) != 0) {
    for (i = 0U; i < count; i++) {
      free(urls[i]);
      free(titles[i]);
    }
    return -1;
  }
  for (i = 0U; i < count; i++) {
    free(urls[i]);
    free(titles[i]);
  }
  return 0;
}

/*
 *ファイルポインタから一行分読み込みメモリを割り当てて返す
 */
static int YAP_readline_alloc(FILE *socket, char **line_out) {
  char *socket_buf, *line_buf;
  size_t line_len = 0;

  *line_out = NULL;
  socket_buf = (char *)YAP_malloc(BUF_SIZE);
  line_buf = (char *)YAP_malloc(BUF_SIZE);
  line_buf[0] = '\0';

  while (1) {
    size_t chunk_len;
    memset(socket_buf, 0, BUF_SIZE);
    if (fgets(socket_buf, BUF_SIZE, socket) == NULL) {
      if (ferror(socket)) {
        free(socket_buf);
        free(line_buf);
        return -1;
      }
      break;
    }

    chunk_len = strlen(socket_buf);
    if (line_len + chunk_len + 1 > MAX_HTTP_LINE_SIZE) {
      if (chunk_len > 0) {
        YAP_drain_oversized_line(socket, socket_buf, chunk_len);
      }
      free(socket_buf);
      free(line_buf);
      return -2;
    }
    line_buf = (char *)YAP_realloc(line_buf, line_len + chunk_len + 1);
    memcpy(line_buf + line_len, socket_buf, chunk_len + 1);
    line_len += chunk_len;

    if (chunk_len < (size_t)(BUF_SIZE - 1) || socket_buf[chunk_len - 1] == '\n') {
      break;
    }
  }

  free(socket_buf);
  if (line_len == 0) {
    free(line_buf);
    return 0;
  }
  *line_out = line_buf;
  return 1;
}

static int YAP_parse_request_target(const char *target, char *dict, int *max_size, char *op,
                                    int *start, int *end, char *keyword) {
  char path[BUF_SIZE];
  const char *query;
  const char *path_start;
  int path_len;
  int keyword_len;
  int n = 0;
  char fmt[96];
  int w = BUF_SIZE - 1;

  if (target == NULL || target[0] == '\0') {
    return -1;
  }

  path_start = target;
  while (*path_start == ' ') {
    path_start++;
  }
  while (*path_start == '/') {
    path_start++;
  }
  if (*path_start == '\0') {
    return -1;
  }

  query = strchr(path_start, '?');
  if (query == NULL) {
    return -1;
  }

  path_len = (int)(query - path_start);
  keyword_len = (int)strlen(query + 1);
  if (path_len <= 0 || path_len >= BUF_SIZE || keyword_len <= 0 || keyword_len >= BUF_SIZE) {
    return -1;
  }

  memcpy(path, path_start, (size_t)path_len);
  path[path_len] = '\0';
  memcpy(keyword, query + 1, (size_t)keyword_len + 1);

  snprintf(fmt, sizeof(fmt), "%%%d[^/]/%%d/%%%d[^/]/%%d-%%d%%n", w, w);
  if (sscanf(path, fmt, dict, max_size, op, start, end, &n) != 5) {
    return -1;
  }
  if (path[n] != '\0') {
    return -1;
  }
  if (dict[0] == '\0' || op[0] == '\0' || keyword[0] == '\0') {
    return -1;
  }
  if (*max_size <= 0 || *start < 0 || *end < 0 || *end < *start) {
    return -1;
  }

  return 0;
}

static int YAP_parse_request_line(const char *line, char *dict, int *max_size, char *op, int *start,
                                  int *end, char *keyword) {
  char method[16];
  char target[BUF_SIZE];
  char version[32];

  if (sscanf(line, "%15s %1023s %31s", method, target, version) == 3) {
    if (strcmp(method, "GET") != 0 || strncmp(version, "HTTP/", 5) != 0) {
      return -1;
    }
    if (YAP_parse_request_target(target, dict, max_size, op, start, end, keyword) == 0) {
      return 0;
    }
  }

  return -1;
}

typedef enum {
  YAP_OPERATIONAL_NONE = 0,
  YAP_OPERATIONAL_LIVE = 1,
  YAP_OPERATIONAL_READY = 2,
  YAP_OPERATIONAL_METRICS = 3
} YAP_OPERATIONAL_ENDPOINT;

static YAP_OPERATIONAL_ENDPOINT YAP_parse_operational_request_line(const char *line) {
  char method[16];
  char target[BUF_SIZE];
  char version[32];

  if (line == NULL || sscanf(line, "%15s %1023s %31s", method, target, version) != 3) {
    return YAP_OPERATIONAL_NONE;
  }
  if (strcmp(method, "GET") != 0 || strncmp(version, "HTTP/", 5) != 0)
    return YAP_OPERATIONAL_NONE;
  if (strcmp(target, "/health/live") == 0 || strcmp(target, "/healthz") == 0)
    return YAP_OPERATIONAL_LIVE;
  if (strcmp(target, "/health/ready") == 0) return YAP_OPERATIONAL_READY;
  if (strcmp(target, "/metrics") == 0) return YAP_OPERATIONAL_METRICS;
  return YAP_OPERATIONAL_NONE;
}

static int YAP_drain_http_headers(FILE *socket) {
  while (1) {
    char *line = NULL;
    int read_rc = YAP_readline_alloc(socket, &line);
    if (read_rc <= 0) {
      return read_rc;
    }
    if ((line[0] == '\r' && line[1] == '\n') || (line[0] == '\n' && line[1] == '\0')) {
      free(line);
      return 1;
    }
    free(line);
  }
}

static int YAP_cores_ready(YAP_THREAD_DATA *thread) {
  int i;
  for (i = 0; i < thread->server_num; i++) {
    YAP_V2_CORE_FRAME request, response; int http_status;
    YAP_V2_core_frame_init(&request); YAP_V2_core_frame_init(&response);
    request.type = YAP_V2_CORE_HEALTH_REQUEST; request.request_id = thread->next_request_id++;
    if (YAP_V2_core_frame_write(thread->server_socket[i], &request, YAP_V2_CORE_MAX_PAYLOAD_BYTES) != YAP_V2_CORE_FRAME_OK ||
        fflush(thread->server_socket[i]) != 0 ||
        YAP_V2_core_frame_read(thread->server_socket[i], YAP_V2_CORE_MAX_PAYLOAD_BYTES, &response) != YAP_V2_CORE_FRAME_OK ||
        response.request_id != request.request_id || response.payload_bytes < 2U) {
      YAP_V2_core_frame_free(&response); return 0;
    }
    http_status = ((int)response.payload[0] << 8) | response.payload[1];
    if (http_status != 200 || response.type != YAP_V2_CORE_HEALTH_RESPONSE) {
      YAP_V2_core_frame_free(&response); return 0;
    }
    YAP_V2_core_frame_free(&response);
  }
  return thread->server_num > 0;
}

static int YAP_send_operational_response(YAP_THREAD_DATA *thread, int client_fd,
                                         YAP_OPERATIONAL_ENDPOINT endpoint) {
  static const char live[] = "{\"status\":\"live\",\"service\":\"yappod_front\"}";
  YAP_V2_OPERATIONAL_STATE state; char probe_error[256] = {0}; char *body = NULL;
  size_t body_bytes = 0U, inflight = 0U, inflight_bytes = 0U, max_inflight = 0U, max_bytes = 0U;
  int ready, status;
  if (endpoint == YAP_OPERATIONAL_LIVE)
    return YAP_send_http_payload(client_fd, thread->id, 200, "OK",
      "application/json; charset=utf-8", live, sizeof(live) - 1U);
  ready = YAP_V2_operational_probe_index(thread->base_dir, &state, probe_error, sizeof(probe_error)) == YAP_V2_OK &&
          state.ready && YAP_cores_ready(thread);
  state.ready = ready;
  if (endpoint == YAP_OPERATIONAL_READY) {
    status = YAP_V2_operational_state_json(&state, "yappod_front", &body, &body_bytes);
    if (status != YAP_V2_OK) return -1;
    status = YAP_send_http_payload(client_fd, thread->id, ready ? 200 : 503,
      ready ? "OK" : "Service Unavailable", "application/json; charset=utf-8", body, body_bytes);
  } else {
    if (YAP_V2_runtime_limiter_snapshot(&g_runtime_limiter, &inflight, &inflight_bytes,
                                        &max_inflight, &max_bytes) != YAP_V2_OK) return -1;
    status = YAP_V2_metrics_render(&g_v2_metrics, &state, inflight, inflight_bytes,
                                   max_inflight, max_bytes, &body, &body_bytes);
    if (status != YAP_V2_OK) return -1;
    status = YAP_send_http_payload(client_fd, thread->id, 200, "OK",
      "text/plain; version=0.0.4; charset=utf-8", body, body_bytes);
  }
  free(body); return status;
}

static YAP_V2_OBSERVE_OPERATION YAP_observe_operation(YAP_V2_HTTP_OPERATION operation) {
  return operation == YAP_V2_HTTP_SEARCH ? YAP_V2_OBSERVE_SEARCH :
         operation == YAP_V2_HTTP_RETRIEVE ? YAP_V2_OBSERVE_RETRIEVE : YAP_V2_OBSERVE_INGEST;
}

/*
 *サーバの本体
 */
void *thread_server(void *ip) {
  struct sockaddr_in *yap_sin;
  YAPPO_DB_FILES yappo_db_files;
  YAP_THREAD_DATA *p = (YAP_THREAD_DATA *)ip;
  int i;
  int j;

  /*
   *データベースの準備
   */
  memset(&yappo_db_files, 0, sizeof(YAPPO_DB_FILES));
  yappo_db_files.base_dir = p->base_dir;
  yappo_db_files.mode = YAPPO_DB_READ;

  /*
   *各サーバとの接続を開始する
   */
  for (i = 0; i < p->server_num; i++) {
    if (YAP_connect_core_stream(p->server_addr[i], p->core_port, &(p->server_socket[i]),
                                &(p->server_fd[i]), p->id) != 0) {
      fprintf(stderr, "ERROR: front thread %d client connect error\n", p->id);
      fflush(stderr);
      for (j = 0; j < i; j++) {
        YAP_Net_close_stream(&(p->server_socket[j]), &(p->server_fd[j]));
      }
      return NULL;
    }
  }

  printf("GO\n");

  while (1) {
    SEARCH_RESULT *result, *left, *right;
    socklen_t sockaddr_len = sizeof(yap_sin);
    int accept_socket = -1;
    int read_rc;
    int header_rc;
    int is_json_request = 0;
    char *line = NULL;
    FILE *socket = NULL;
    char *dict, *op, *keyword; /* リクエスト */
    char *cursor;
    int max_size;
    int start, end;
    size_t api_limit = YAP_SEARCH_API_DEFAULT_LIMIT;
    YAP_V2_HTTP_OPERATION v2_operation;
    YAP_OPERATIONAL_ENDPOINT operational_endpoint;

    if (g_shutdown_requested) {
      break;
    }
    if (YAP_Net_accept_stream(p->socket, (struct sockaddr *)&yap_sin, &sockaddr_len, &socket,
                              &accept_socket, "front", p->id) != 0) {
      if (g_shutdown_requested) {
        break;
      }
      continue;
    }
    if (YAP_V2_socket_set_deadline(accept_socket, g_runtime_policy.request_timeout_ms) != YAP_V2_OK) {
      YAP_Net_close_stream(&socket, &accept_socket); continue;
    }

    read_rc = YAP_readline_alloc(socket, &line);
    if (read_rc <= 0) {
      if (read_rc == -2) {
        YAP_send_bad_request(accept_socket, p->id);
      } else if (read_rc < 0) {
        YAP_log_thread_error(p->id, "read request line failed");
      }
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    operational_endpoint = YAP_parse_operational_request_line(line);
    if (operational_endpoint != YAP_OPERATIONAL_NONE) {
      header_rc = YAP_drain_http_headers(socket);
      if (header_rc > 0) {
        if (YAP_send_operational_response(p, accept_socket, operational_endpoint) != 0)
          YAP_log_thread_error(p->id, "write operational response failed");
      } else if (header_rc == -2) {
        YAP_send_bad_request(accept_socket, p->id);
      } else if (header_rc < 0) {
        YAP_log_thread_error(p->id, "read health headers failed");
      }
      free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    if (YAP_is_v2_endpoint_with_non_post_method(line)) {
      YAP_send_json_error(accept_socket, p->id, 405, "Method Not Allowed", "method_not_allowed");
      free(line); YAP_Net_close_stream(&socket, &accept_socket); continue;
    }

    if (YAP_parse_v2_post_line(line, &v2_operation) == 0) {
      unsigned char *body = NULL; size_t body_bytes = 0U; int headers, admitted = 0;
      int response_status = 503; uint64_t request_started = YAP_V2_monotonic_microseconds();
      char authorization[YAP_V2_AUTHORIZATION_MAX_BYTES + 1U];
      headers = YAP_read_v2_headers(socket, &body_bytes, authorization);
      if (headers != 0) {
        if (headers == -2) { response_status = 415; YAP_send_json_error(accept_socket, p->id, 415, "Unsupported Media Type", "unsupported_media_type"); }
        else if (headers == -3) { response_status = 413; YAP_send_json_error(accept_socket, p->id, 413, "Payload Too Large", "payload_too_large"); }
        else { response_status = 400; YAP_send_json_error(accept_socket, p->id, 400, "Bad Request", "invalid_http_request"); }
      } else if (v2_operation == YAP_V2_HTTP_INGEST &&
                 YAP_V2_authorize_write(&g_runtime_policy,
                   authorization[0] == '\0' ? NULL : authorization) != YAP_V2_OK) {
        response_status = 401; YAP_send_json_error(accept_socket, p->id, 401, "Unauthorized", "unauthorized");
      } else if (YAP_V2_runtime_limiter_acquire(&g_runtime_limiter, body_bytes) != YAP_V2_OK) {
        response_status = 503; YAP_send_json_error(accept_socket, p->id, 503, "Service Unavailable", "overloaded");
      } else {
        admitted = 1;
        body = malloc(body_bytes);
        if (body == NULL || fread(body, 1U, body_bytes, socket) != body_bytes ||
            YAP_send_v2_http(socket, p->server_socket[0], v2_operation, body, body_bytes,
                             p->next_request_id++, &response_status) != 0)
          YAP_send_json_error(accept_socket, p->id, 503, "Service Unavailable", "core_unavailable");
      }
      if (admitted) YAP_V2_runtime_limiter_release(&g_runtime_limiter, body_bytes);
      {
        uint64_t request_finished = YAP_V2_monotonic_microseconds();
        YAP_V2_metrics_record(&g_v2_metrics, YAP_observe_operation(v2_operation), response_status,
          request_finished >= request_started ? request_finished - request_started : 0U);
      }
      free(body); free(line); YAP_Net_close_stream(&socket, &accept_socket); continue;
    }

    /* バッファの初期化 */
    dict = (char *)YAP_malloc(BUF_SIZE);
    op = (char *)YAP_malloc(BUF_SIZE);
    keyword = (char *)YAP_malloc(BUF_SIZE);
    cursor = (char *)YAP_malloc(BUF_SIZE);
    dict[0] = '\0';
    op[0] = '\0';
    keyword[0] = '\0';
    cursor[0] = '\0';

    if (YAP_parse_v2_request_line(line, dict, &max_size, op, keyword, &api_limit, cursor) == 0) {
      is_json_request = 1;
      start = 0;
      end = 0;
    } else if (YAP_parse_request_line(line, dict, &max_size, op, &start, &end, keyword) != 0) {
      YAP_send_bad_request(accept_socket, p->id);
      printf("bad:%d:\n", p->id);
      free(dict);
      free(op);
      free(keyword);
      free(cursor);
      free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    printf("ok:%d: %s/%d/%s/%s (%d-%d)\n", p->id, dict, max_size, op, keyword, start, end);

    if (strlen(dict) == 0 || max_size == 0 || strlen(op) == 0 || strlen(keyword) == 0) {
      YAP_send_bad_request(accept_socket, p->id);
      printf("bad:%d:\n", p->id);
      free(dict);
      free(op);
      free(keyword);
      free(cursor);
      free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    header_rc = YAP_drain_http_headers(socket);
    if (header_rc <= 0) {
      if (header_rc == -2) {
        YAP_send_bad_request(accept_socket, p->id);
      } else if (header_rc < 0) {
        YAP_log_thread_error(p->id, "read headers failed");
      }
      free(dict);
      free(op);
      free(keyword);
      free(cursor);
      free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    /*
     *各検索サーバに検索要求を出す。
     */
    for (i = 0; i < p->server_num; i++) {
      if (YAP_Proto_send_query(p->server_socket[i], dict, max_size, op, keyword) != 0) {
        fprintf(stderr, "ERROR: front thread %d send query to core[%d] failed\n", p->id, i);
        continue;
      }
      fflush(p->server_socket[i]);
    }

    YAP_Db_filename_set(&yappo_db_files);
    YAP_Db_base_open(&yappo_db_files);
    YAP_Db_linklist_open(&yappo_db_files);

    /* 検索結果を受け取りマージする */
    result = YAP_Proto_recv_result(p->server_socket[0]);

    for (i = 1; i < p->server_num; i++) {
      left = result;
      /* 右を検索 */
      right = YAP_Proto_recv_result(p->server_socket[i]);

      result = YAP_Search_op_or(left, right);
      if (result == NULL) {
        /* どちらかの検索結果が無かった */
        if (left != NULL) {
          result = left;
          left = NULL;
        } else if (right != NULL) {
          result = right;
          right = NULL;
        }
      }

      /* メモリ解放 */
      if (left != NULL) {
        YAP_Search_result_free(left);
        free(left);
      }
      if (right != NULL) {
        YAP_Search_result_free(right);
        free(right);
      }
      if (result == NULL) {
        /* 検索不一致 */
        break;
      }
    }

    /* 検索結果内のページ同士のリンク関係によりスコアを可変する */
    YAP_Linklist_Score(&yappo_db_files, result);
    /* スコア順ソート */
    YAP_Search_result_sort_score(result);
    /* 結果出力 */
    if (is_json_request) {
      if (YAP_search_result_print_json(&yappo_db_files, socket, result, api_limit, cursor) != 0) {
        YAP_send_bad_request(accept_socket, p->id);
      }
    } else {
      search_result_print(&yappo_db_files, socket, result, start, end);
    }

    if (result != NULL) {
      printf("hit %d\n", result->keyword_docs_num);
      YAP_Search_result_free(result);
      free(result);
    }

    YAP_Db_linklist_close(&yappo_db_files);
    YAP_Db_base_close(&yappo_db_files);

    free(dict);
    free(op);
    free(keyword);
    free(cursor);
    free(line);

    fflush(stdout);
    fflush(socket);
    YAP_Net_close_stream(&socket, &accept_socket);
  }

  /*
   *各サーバとの接続を閉じる
   */
  for (i = 0; i < p->server_num; i++) {
    if (YAP_Proto_send_shutdown(p->server_socket[i]) != 0) {
      fprintf(stderr, "ERROR: front thread %d send shutdown to core[%d] failed\n", p->id, i);
    }
    YAP_Net_close_stream(&(p->server_socket[i]), &(p->server_fd[i]));
  }

  return NULL;
}

void start_deamon_thread(char *indextexts_dirpath, int server_num, int *server_socket,
                         char **server_addr, int listen_port, int core_port) {
  int sock_optval = 1;
  int yap_socket;
  struct sockaddr_in yap_sin;
  int i;
  pthread_t *pthread;
  YAP_THREAD_DATA *thread_data;
  (void)server_socket;
  {
    char policy_error[256] = {0};
    memset(&g_runtime_limiter, 0, sizeof(g_runtime_limiter));
    if (YAP_V2_runtime_policy_load_env(&g_runtime_policy, policy_error, sizeof(policy_error)) != YAP_V2_OK ||
        YAP_V2_runtime_limiter_init(&g_runtime_limiter, &g_runtime_policy) != YAP_V2_OK ||
        YAP_V2_metrics_init(&g_v2_metrics) != YAP_V2_OK) {
      fprintf(stderr, "ERROR: runtime policy: %s\n", policy_error); exit(EXIT_FAILURE);
    }
  }

  /* ソケットの作成 */
  yap_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (yap_socket == -1)
    YAP_Error("socket open error");

  /* ソケットの設定 */
  if (setsockopt(yap_socket, SOL_SOCKET, SO_REUSEADDR, &sock_optval, sizeof(sock_optval)) == -1) {
    YAP_Error("setsockopt error");
  }

  /* bindする */
  yap_sin.sin_family = AF_INET;
  yap_sin.sin_port = htons((unsigned short)listen_port);
  yap_sin.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(yap_socket, (struct sockaddr *)&yap_sin, sizeof(yap_sin)) < 0) {
    fprintf(stderr, "ERROR: bind failed on port %d: %s\n", listen_port, strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* listen */
  if (listen(yap_socket, SOMAXCONN) == -1) {
    YAP_Error("listen error");
  }

  /* スレッドの準備 */
  pthread = (pthread_t *)YAP_malloc(sizeof(pthread_t) * MAX_THREAD);
  thread_data = (YAP_THREAD_DATA *)YAP_malloc(sizeof(YAP_THREAD_DATA) * MAX_THREAD);
  for (i = 0; i < MAX_THREAD; i++) {
    int ii;

    /* 起動準備 */
    thread_data[i].id = i;
    thread_data[i].base_dir = (char *)YAP_malloc(strlen(indextexts_dirpath) + 1);
    thread_data[i].next_request_id = ((uint64_t)(unsigned int)i + 1U) << 32;
    memcpy(thread_data[i].base_dir, indextexts_dirpath, strlen(indextexts_dirpath) + 1);
    thread_data[i].socket = dup(yap_socket);

    thread_data[i].server_num = server_num;
    thread_data[i].core_port = core_port;
    thread_data[i].server_socket = (FILE **)YAP_malloc(sizeof(FILE **) * server_num);
    thread_data[i].server_fd = (int *)YAP_malloc(sizeof(int) * server_num);
    thread_data[i].server_addr = (char **)YAP_malloc(sizeof(char **) * server_num);
    for (ii = 0; ii < server_num; ii++) {
      thread_data[i].server_addr[ii] = (char *)YAP_malloc(strlen(server_addr[ii]) + 1);
      memcpy(thread_data[i].server_addr[ii], server_addr[ii], strlen(server_addr[ii]) + 1);
    }

    printf("start: %d:%s\n", i, thread_data[i].base_dir);
    pthread_create(&(pthread[i]), NULL, thread_server, (void *)&(thread_data[i]));

    printf("GO: %d\n", i);
  }

  /*
   *メインループ
   */
  while (!g_shutdown_requested) {
    sleep(1);
  }

  if (g_shutdown_signal != 0) {
    fprintf(stderr, "INFO: front shutdown requested by signal %d\n", g_shutdown_signal);
  }
  close(yap_socket);
  printf("end\n");
}

int main(int argc, char *argv[]) {
  int i, pid;
  char *indextexts_dirpath = NULL;
  struct stat f_stats;
  int server_num = 0;
  int *server_socket = NULL;
  char **server_addr = NULL;
  int listen_port = DEFAULT_FRONT_PORT;
  int core_port = DEFAULT_CORE_PORT;

  /*
   *オプションを取得
   */
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      YAP_print_usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (!strcmp(argv[i], "-l")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ERROR: missing value for -l\n");
        YAP_print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
      indextexts_dirpath = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "-p")) {
      if (i + 1 >= argc || YAP_parse_port(argv[i + 1], &listen_port) != 0) {
        fprintf(stderr, "ERROR: invalid port for -p\n");
        YAP_print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
      i++;
      continue;
    }
    if (!strcmp(argv[i], "-P")) {
      if (i + 1 >= argc || YAP_parse_port(argv[i + 1], &core_port) != 0) {
        fprintf(stderr, "ERROR: invalid port for -P\n");
        YAP_print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
      i++;
      continue;
    }
    if (!strcmp(argv[i], "-s")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "ERROR: missing value for -s\n");
        YAP_print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
      }
      server_addr = (char **)YAP_realloc(server_addr, sizeof(char **) * (server_num + 1));
      server_addr[server_num] = (char *)YAP_malloc(strlen(argv[i + 1]) + 1);
      memcpy(server_addr[server_num], argv[i + 1], strlen(argv[i + 1]) + 1);
      server_num++;
      i++;
      continue;
    }

    fprintf(stderr, "ERROR: unknown option: %s\n", argv[i]);
    YAP_print_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  if (indextexts_dirpath == NULL) {
    fprintf(stderr, "ERROR: missing required option -l\n");
    YAP_print_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  if (server_num == 0) {
    fprintf(stderr, "ERROR: missing required option -s\n");
    YAP_print_usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  server_socket = (int *)YAP_malloc(sizeof(int) * server_num);

  if (YAP_stat(indextexts_dirpath, &f_stats) != 0 || !S_ISDIR(f_stats.st_mode)) {
    perror("ERROR: invalid index dir");
    fprintf(stderr, "%s\n", indextexts_dirpath);
    exit(EXIT_FAILURE);
  }

  count = 0;

  printf("Start\n");

  /*
   *デーモン化
   */

  fclose(stdin);
  fclose(stdout);
  fclose(stderr);
  stdout = fopen("./front.log", "a");
  if (stdout == NULL) {
    stdout = fopen("/dev/null", "a");
  }
  stderr = fopen("./front.error", "a");
  if (stderr == NULL) {
    stderr = fopen("/dev/null", "a");
  }
  pid = fork();
  if (pid) {
    FILE *pidf = fopen("./front.pid", "w");
    if (pidf != NULL) {
      fprintf(pidf, "%d", pid);
      fclose(pidf);
    }
    exit(EXIT_SUCCESS);
  }

  atexit(YAP_remove_pidfile);

  /*
   *シグナル処理
   */
  if (YAP_install_signal_handlers() != 0) {
    perror("ERROR: sigaction");
    return EXIT_FAILURE;
  }

  start_deamon_thread(indextexts_dirpath, server_num, server_socket, server_addr, listen_port,
                      core_port);
  return EXIT_SUCCESS;
}
