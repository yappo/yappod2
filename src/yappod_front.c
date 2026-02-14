/*
  フロントサーバ
  HTTPにより検索条件を受けつけ、各検索コアサーバに要求を送り
  検索結果をまとめてクライアントに返す
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
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
} YAP_THREAD_DATA;

/* スレッドの数 */
#define MAX_THREAD 5

int count;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signal = 0;
static const char *g_front_pidfile = "./front.pid";
static void YAP_request_shutdown(int sig);

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
    return -1;
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

/*
 *サーバの本体
 */
void *thread_server(void *ip) {
  struct sockaddr_in *yap_sin;
  YAPPO_DB_FILES yappo_db_files;
  YAP_THREAD_DATA *p = (YAP_THREAD_DATA *)ip;
  int i;

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
      YAP_Error("client connect error");
    }
  }

  printf("GO\n");

  while (1) {
    SEARCH_RESULT *result, *left, *right;
    socklen_t sockaddr_len = sizeof(yap_sin);
    int accept_socket = -1;
    int read_rc;
    int header_rc;
    char *line = NULL;
    FILE *socket = NULL;
    char *dict, *op, *keyword; /* リクエスト */
    int max_size;
    int start, end;

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

    /* バッファの初期化 */
    dict = (char *)YAP_malloc(BUF_SIZE);
    op = (char *)YAP_malloc(BUF_SIZE);
    keyword = (char *)YAP_malloc(BUF_SIZE);
    dict[0] = '\0';
    op[0] = '\0';
    keyword[0] = '\0';

    if (YAP_parse_request_line(line, dict, &max_size, op, &start, &end, keyword) != 0) {
      YAP_send_bad_request(accept_socket, p->id);
      printf("bad:%d:\n", p->id);
      free(dict);
      free(op);
      free(keyword);
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
    search_result_print(&yappo_db_files, socket, result, start, end);

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
