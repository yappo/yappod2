/*
  フロントサーバ
  HTTPにより検索条件を受けつけ、各検索コアサーバに要求を送り
  検索結果をまとめてクライアントに返す
*/
#include <stdio.h>
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
#define PORT 10080
#define CORE_PORT 10086

/*
 *スレッド毎の構造
 */
typedef struct{
  int id;
  int socket;
  int server_num;
  FILE **server_socket;
  int *server_fd;
  char **server_addr;
  char *base_dir;
}YAP_THREAD_DATA;


/* スレッドの数 */
#define MAX_THREAD 5


int count;


void YAP_Error( char *msg){
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(-1);
}

static int YAP_send_bad_request(int fd, int thread_id)
{
  const char *msg =
      "HTTP/1.0 400 Bad Request\r\n"
      "Server: Yappo Search/1.0\r\n"
      "Content-Type: text/html\r\n"
      "\r\n"
      "Bad Search Request<br>By Yappo Search";
  return YAP_Net_write_all(fd, msg, strlen(msg), "front", thread_id);
}


/*
 *検索結果を標示
 */
void search_result_print (YAPPO_DB_FILES *ydfp, FILE *socket, SEARCH_RESULT *p, int start, int end)
{
  int i;
  FILEDATA filedata;
  char *title;

  fprintf( socket, "HTTP/1.0 200 OK\r\nServer: Yappo Search/1.0\r\nContent-Type: text/plain\r\n\r\n");
  fflush(socket);

  if (p == NULL || start >= p->keyword_docs_num) {
    fprintf(socket, "0\n\n");
    fflush(socket);
  } else {

    if (end >= p->keyword_docs_num) {
      end = p->keyword_docs_num;
    }

    fprintf(socket, "%d\n%d\n\n", p->keyword_docs_num, end - start);
    fflush(socket);

    for (i = start; i < end; i++) {
      if (YAP_Index_Filedata_get(ydfp, p->docs_list[i].fileindex, &filedata) == 0) {
	title = filedata.title;
	if (title == NULL) {
	  title = filedata.url;
	}
        fprintf(socket, "%s\t%s\t%d\t%ld\t%.2f\n", filedata.url, title, filedata.size, (long) filedata.lastmod, p->docs_list[i].score); 
	fflush(socket);

	YAP_Index_Filedata_free(&filedata);
      } else {
	fprintf(socket, "%d\t%.2f\n", p->docs_list[i].fileindex, p->docs_list[i].score);

      }
      fflush(socket);
    }
  }
  fflush(socket);
}


/*
 *ファイルポインタから一行分読み込みメモリを割り当てて返す
 */
static int YAP_readline_alloc(FILE *socket, char **line_out)
{
  char *socket_buf, *line_buf;
  size_t line_len = 0;

  *line_out = NULL;
  socket_buf = (char *) YAP_malloc(BUF_SIZE);
  line_buf = (char *) YAP_malloc(BUF_SIZE);
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
    line_buf = (char *) YAP_realloc(line_buf, line_len + chunk_len + 1);
    memcpy(line_buf + line_len, socket_buf, chunk_len + 1);
    line_len += chunk_len;

    if (chunk_len < (size_t) (BUF_SIZE - 1) || socket_buf[chunk_len - 1] == '\n') {
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

static int YAP_parse_request_line(const char *line, char *dict, int *max_size, char *op, int *start, int *end, char *keyword)
{
  char fmt[128];
  int w = BUF_SIZE - 1;

  snprintf(fmt, sizeof(fmt),
           "GET / %%%d[a-zA-Z]/%%d/%%%d[a-zA-Z]/%%d-%%d?%%%ds HTTP/1.0",
           w, w, w);
  if (sscanf(line, fmt, dict, max_size, op, start, end, keyword) == 6) {
    return 0;
  }
  return -1;
}

static int YAP_drain_http_headers(FILE *socket)
{
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
void *thread_server (void *ip) 
{
 struct sockaddr_in *yap_sin;
 YAPPO_DB_FILES yappo_db_files;
 YAP_THREAD_DATA *p = (YAP_THREAD_DATA *) ip;
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
    struct hostent *cl_host;
    struct sockaddr_in cl_sin;

    memset(&cl_sin, 0, sizeof(struct sockaddr_in));

    cl_host = gethostbyname(p->server_addr[i]);
    if (cl_host == NULL) {
      YAP_Error( "gethostbyname error");
    }
    cl_sin.sin_family = AF_INET;
    memcpy((char *) &cl_sin.sin_addr, cl_host->h_addr, cl_host->h_length);

    cl_sin.sin_port = htons(CORE_PORT);

    /* ソケット作成 */
    if ((p->server_fd[i] = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      YAP_Error( "client socket error");
    }

    /* 接続 */
    if (connect(p->server_fd[i], (struct sockaddr *) &cl_sin, sizeof(struct sockaddr_in)) == -1) {
      YAP_Error( "client connect error");
    }

    p->server_socket[i] = (FILE *) fdopen(p->server_fd[i], "r+");
    if (p->server_socket[i] == NULL) {
      perror("ERROR: front core fdopen");
      close(p->server_fd[i]);
      YAP_Error("client fdopen error");
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
    char *dict, *op, *keyword;/* リクエスト */
    int max_size;
    int start, end;


    if (YAP_Net_accept_stream(p->socket, (struct sockaddr *)&yap_sin, &sockaddr_len,
                              &socket, &accept_socket, "front", p->id) != 0) {
      continue;
    }

    read_rc = YAP_readline_alloc(socket, &line);
    if (read_rc <= 0) {
      if (read_rc < 0) {
        fprintf(stderr, "ERROR: front thread %d failed to read request line\n", p->id);
      }
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    /* バッファの初期化 */
    dict = (char *) YAP_malloc(BUF_SIZE);
    op = (char *) YAP_malloc(BUF_SIZE);
    keyword = (char *) YAP_malloc(BUF_SIZE);
    dict[0] = '\0';
    op[0] = '\0';
    keyword[0] = '\0';

    if (YAP_parse_request_line(line, dict, &max_size, op, &start, &end, keyword) != 0) {
      YAP_send_bad_request(accept_socket, p->id);
      printf("bad:%d:\n", p->id);
      free(dict); free(op); free(keyword); free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    printf("ok:%d: %s/%d/%s/%s (%d-%d)\n", p->id, dict, max_size, op, keyword, start, end);

    if (strlen(dict) == 0 || max_size == 0 || strlen(op) == 0 || strlen(keyword) == 0) {
      YAP_send_bad_request(accept_socket, p->id);
      printf("bad:%d:\n", p->id);
      free(dict); free(op); free(keyword); free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    header_rc = YAP_drain_http_headers(socket);
    if (header_rc <= 0) {
      if (header_rc < 0) {
        fprintf(stderr, "ERROR: front thread %d failed while reading headers\n", p->id);
      }
      free(dict); free(op); free(keyword); free(line);
      YAP_Net_close_stream(&socket, &accept_socket);
      continue;
    }

    /*
     *各検索サーバに検索要求を出す。
     */
    for (i = 0; i < p->server_num; i++) {
      if (YAP_Proto_send_query(p->server_socket[i], dict, max_size, op, keyword) != 0) {
        fprintf(stderr, "ERROR: front thread %d failed to send query to core[%d]\n", p->id, i);
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
      printf( "hit %d\n", result->keyword_docs_num);
      YAP_Search_result_free(result);
      free(result);
    }

    YAP_Db_linklist_close(&yappo_db_files);
    YAP_Db_base_close(&yappo_db_files);

    free(dict); free(op); free(keyword); free(line);

    fflush(stdout);
    fflush(socket);
    YAP_Net_close_stream(&socket, &accept_socket);
  }
  
  /*
   *各サーバとの接続を閉じる
   */
  for (i = 0; i < p->server_num; i++) {
    if (YAP_Proto_send_shutdown(p->server_socket[i]) != 0) {
      fprintf(stderr, "ERROR: front thread %d failed to send shutdown to core[%d]\n", p->id, i);
    }
    YAP_Net_close_stream(&(p->server_socket[i]), &(p->server_fd[i]));
  }

  return NULL;
}

void start_deamon_thread(char *indextexts_dirpath, int server_num, int *server_socket, char **server_addr) 
{
  int sock_optval = 1;
  int yap_socket;
  struct sockaddr_in yap_sin;
  int i;
  pthread_t *pthread;
  YAP_THREAD_DATA *thread_data;
  (void) server_socket;

  /* ソケットの作成 */
  yap_socket = socket( AF_INET, SOCK_STREAM, 0);
  if (yap_socket == -1)
    YAP_Error( "socket open error");

  /* ソケットの設定 */
  if (setsockopt(yap_socket, SOL_SOCKET, SO_REUSEADDR,
		&sock_optval, sizeof(sock_optval)) == -1) {
    YAP_Error( "setsockopt error");
  }

  /* bindする */
  yap_sin.sin_family = AF_INET;
  yap_sin.sin_port = htons(PORT);
  yap_sin.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(yap_socket, (struct sockaddr *)&yap_sin, sizeof(yap_sin)) < 0) {
    YAP_Error( "bind error");
  }

  /* listen */
  if (listen(yap_socket, SOMAXCONN) == -1) {
    YAP_Error( "listen error");
  }

  /* スレッドの準備 */
  pthread = (pthread_t *) YAP_malloc(sizeof(pthread_t) * MAX_THREAD);
  thread_data  = (YAP_THREAD_DATA *) YAP_malloc(sizeof(YAP_THREAD_DATA) * MAX_THREAD);
  for (i = 0; i < MAX_THREAD; i++) {
    int ii;

    /* 起動準備 */
    thread_data[i].id = i;
    thread_data[i].base_dir = (char *) YAP_malloc(strlen(indextexts_dirpath) + 1);
    strcpy(thread_data[i].base_dir, indextexts_dirpath);
    thread_data[i].socket = dup(yap_socket);

    thread_data[i].server_num = server_num;
    thread_data[i].server_socket = (FILE **) YAP_malloc(sizeof(FILE **) * server_num);
    thread_data[i].server_fd = (int *) YAP_malloc(sizeof(int) * server_num);
    thread_data[i].server_addr = (char **) YAP_malloc(sizeof(char **) * server_num);
    for (ii = 0; ii < server_num; ii++) {
      thread_data[i].server_addr[ii] = (char *) YAP_malloc(strlen(server_addr[ii]) + 1);
      strcpy(thread_data[i].server_addr[ii], server_addr[ii]);
    }

    printf( "start: %d:%s\n", i, thread_data[i].base_dir);
    pthread_create(&(pthread[i]), NULL, thread_server, (void *) &(thread_data[i]));

    printf( "GO: %d\n", i);
  }

  /*
   *メインループ
   */
  while(1){
    sleep(120);
  }
  printf("end\n");
}



int main(int argc, char *argv[])
{
  int i, pid;
  char *indextexts_dirpath = NULL;
  struct stat f_stats;
  int server_num = 0;
  int *server_socket = NULL;
  char **server_addr = NULL;


  /*
   *オプションを取得
   */
  if (argc > 1) {
    i = 1;
    while (1) {
      if ( argc == i)
	break;

      if (! strcmp(argv[i], "-l")) {
	/* インデックスディレクトリを取得 */
	i++;
	if (argc == i)
	  break;
	indextexts_dirpath = argv[i];
      } else if(! strcmp(argv[i], "-s")) {
	/* 検索サーバ指定 */
	i++;
	if (argc == i)
	  break;
	server_addr = (char **) YAP_realloc(server_addr, sizeof(char **) * (server_num + 1));
	server_addr[server_num] = (char *) YAP_malloc(strlen(argv[i]) + 1);
	strcpy(server_addr[server_num], argv[i]);
	server_num++;
      }

      i++;
    }
  }

  server_socket = (int *) YAP_malloc(sizeof(int) * server_num);

  if (server_num == 0) {
    printf("server option -s\n");
    exit(-1);
  }

  if (YAP_stat(indextexts_dirpath, &f_stats) != 0 || !S_ISDIR(f_stats.st_mode)) {
    perror("ERROR: invalid index dir");
    fprintf(stderr, "%s\n", indextexts_dirpath);
    exit(-1);
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
    exit(0);
  }
  
  /*
   *SIGPIPEを無視
   */
  signal(SIGPIPE, SIG_IGN);

  start_deamon_thread(indextexts_dirpath, server_num, server_socket, server_addr);

}
