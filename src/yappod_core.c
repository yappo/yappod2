/*
  サーバ
  フロントサーバから受け取った検索条件で検索して結果をそのまま返す

*/
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <pthread.h>

#include "yappo_alloc.h"
#include "yappo_io.h"
#include "yappo_net.h"
#include "yappo_stat.h"
#include "yappo_search.h"
#include "yappo_db.h"
#include "yappo_index.h"
#include "yappo_proto.h"

#define BUF_SIZE 1024
#define PORT 10086
#define MAX_SOCKET_BUF (1024 * 1024)

/*
 *スレッド毎の構造
 */
typedef struct{
  int id;
  int socket;
  char *base_dir;
}YAP_THREAD_DATA;


/*スレッドの数*/
#define MAX_THREAD 5

/*キャッシュ*/
YAPPO_CACHE yappod_core_cache;

int count;


void YAP_Error( char *msg){
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(-1);
}



/*
 *URLデコードを行なう
 */
static char *urldecode (const char *p) {
  char *ret, *retp;
  char f, l;
  size_t in_len;

  if (p == NULL) {
    return NULL;
  }

  in_len = strlen(p);
  ret = (char *) YAP_malloc(in_len + 1);
  retp = ret;

  while (*p) {

    if (*p != '%') {
      *retp = *p;
      retp++;
      p++;
    } else {
      p++;
      f = tolower(*p);
      p++;
      l = tolower(*p);
      p++;

      if (f >= 'a' && f <= 'f') {
	*retp = ((f - 'a') + 10) * 16;
      } else if (f >= '0' && f <= '9') {
	*retp = (f - '0') * 16;
      } else {
	continue;
      }
      if (l >= 'a' && l <= 'f') {
	*retp += l - 'a' + 10;
      } else if (l >= '0' && l <= '9') {
	*retp += l - '0';
      } else {
	continue;
      }

      retp++;
    }
  }
  *retp = '\0';

  return ret;
}

static void YAP_free_keyword_list(char **keyword_list, int keyword_list_num)
{
  int i;
  if (keyword_list == NULL) {
    return;
  }
  for (i = 0; i < keyword_list_num; i++) {
    free(keyword_list[i]);
  }
  free(keyword_list);
}

static char *YAP_decode_keyword_slice(const char *start, size_t len)
{
  char *buf;
  char *decoded;

  buf = (char *) YAP_malloc(len + 1);
  memcpy(buf, start, len);
  buf[len] = '\0';
  decoded = urldecode(buf);
  free(buf);
  return decoded;
}

static int YAP_split_keyword_query(const char *keyword, char ***keyword_list_out, int *keyword_list_num_out)
{
  const char *p, *start;
  int keyword_list_num = 1;
  int i = 0;
  char **keyword_list;

  if (keyword == NULL) {
    return -1;
  }

  p = keyword;
  while (*p) {
    if (*p == '&') {
      keyword_list_num++;
    }
    p++;
  }

  keyword_list = (char **) YAP_malloc(sizeof(char *) * keyword_list_num);
  start = keyword;
  p = keyword;
  while (*p) {
    if (*p == '&') {
      keyword_list[i] = YAP_decode_keyword_slice(start, (size_t) (p - start));
      i++;
      start = p + 1;
    }
    p++;
  }
  keyword_list[i] = YAP_decode_keyword_slice(start, (size_t) (p - start));

  *keyword_list_out = keyword_list;
  *keyword_list_num_out = keyword_list_num;
  return 0;
}

/*
 *検索処理のメインルーチン
 */
SEARCH_RESULT *search_core (YAPPO_DB_FILES *ydfp, char *dict, int max_size, char *op, char *keyword)
{
  int f_op;
  char **keyword_list = NULL;
  SEARCH_RESULT *result;
  int keyword_list_num = 0;
  (void) dict;

  /*検索条件*/
  if (strcmp(op, "OR") == 0) {
    /*OR*/
    f_op = 1;
  } else {
    /*AND*/
    f_op = 0;
  }

  if (YAP_split_keyword_query(keyword, &keyword_list, &keyword_list_num) != 0) {
    return NULL;
  }

  printf( "max: %d\n", max_size);

  /*
   *検索を行なう
   */
  result = YAP_Search(ydfp, keyword_list, keyword_list_num, max_size, f_op);
  YAP_free_keyword_list(keyword_list, keyword_list_num);
  return result;
}



/*
 *サーバの本体
 */
void *thread_server (void *ip) 
{
  struct sockaddr_in *yap_sin;
  YAPPO_DB_FILES yappo_db_files;
  YAP_THREAD_DATA *p = (YAP_THREAD_DATA *) ip;
  
  /*
   *データベースの準備
   */
  memset(&yappo_db_files, 0, sizeof(YAPPO_DB_FILES)); 
  yappo_db_files.base_dir = p->base_dir;
  yappo_db_files.mode = YAPPO_DB_READ;
  yappo_db_files.cache = &yappod_core_cache;

  printf("GO\n");

  while (1) {
    SEARCH_RESULT *result;
    socklen_t sockaddr_len = sizeof(yap_sin);
    int accept_socket = -1;
    FILE *socket = NULL;
    char *dict, *op, *keyword;/*リクエスト*/
    int recv_code;
    int max_size;

    if (YAP_Net_accept_stream(p->socket, (struct sockaddr *)&yap_sin, &sockaddr_len,
                              &socket, &accept_socket, "core", p->id) != 0) {
      continue;
    }

    printf("accept: %d\n", p->id);

    while (1) {
      /*永遠と処理を続ける*/
      recv_code = YAP_Proto_recv_query(socket, MAX_SOCKET_BUF, &dict, &max_size, &op, &keyword);
      if (recv_code <= 0) {
        if (recv_code < 0) {
          fprintf(stderr, "ERROR: core thread %d failed to receive query payload\n", p->id);
        }
        break;
      }

      printf("OK: %d/%p\n", p->id, (void *) socket);
      
      printf("ok:%d: %s/%d/%s/%s\n", p->id, dict, max_size, op, keyword);

      YAP_Db_filename_set(&yappo_db_files);
      YAP_Db_base_open(&yappo_db_files);
      /*キャッシュ読みこみ*/
      YAP_Db_cache_load(&yappo_db_files, &yappod_core_cache);
      
      result = search_core(&yappo_db_files, dict, max_size, op, keyword);

      /*結果出力*/
      if (YAP_Proto_send_result(socket, result) != 0) {
        if (result != NULL) {
          YAP_Search_result_free(result);
          free(result);
        }
        free(dict);
        free(op);
        free(keyword);
        YAP_Db_base_close(&yappo_db_files);
        break;
      }
      fflush(socket);
    
      if (result != NULL) {
	YAP_Search_result_free(result);
	free(result);
      }
      free(dict); free(op); free(keyword);
      fflush(stdout);

      YAP_Db_base_close(&yappo_db_files);
    }

    fflush(stdout);
    YAP_Net_close_stream(&socket, &accept_socket);
  }

  return NULL;
}

void start_deamon_thread(char *indextexts_dirpath) 
{
  int sock_optval = 1;
  int yap_socket;
  struct sockaddr_in yap_sin;
  int i;
  pthread_t *pthread;
  YAP_THREAD_DATA *thread_data;

  /*ソケットの作成*/
  yap_socket = socket( AF_INET, SOCK_STREAM, 0);
  if (yap_socket == -1)
    YAP_Error( "socket open error");

  /*ソケットの設定*/
  if (setsockopt(yap_socket, SOL_SOCKET, SO_REUSEADDR,
		&sock_optval, sizeof(sock_optval)) == -1) {
    YAP_Error( "setsockopt error");
  }

  /*bindする*/
  yap_sin.sin_family = AF_INET;
  yap_sin.sin_port = htons(PORT);
  yap_sin.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(yap_socket, (struct sockaddr *)&yap_sin, sizeof(yap_sin)) < 0) {
    YAP_Error( "bind error");
  }

  /*listen*/
  if (listen(yap_socket, SOMAXCONN) == -1) {
    YAP_Error( "listen error");
  }

  /*スレッドの準備*/
  pthread = (pthread_t *) YAP_malloc(sizeof(pthread_t) * MAX_THREAD);
  thread_data  = (YAP_THREAD_DATA *) YAP_malloc(sizeof(YAP_THREAD_DATA) * MAX_THREAD);
  for (i = 0; i < MAX_THREAD; i++) {

    /*起動準備*/
    thread_data[i].id = i;
    thread_data[i].base_dir = (char *) YAP_malloc(strlen(indextexts_dirpath) + 1);
    strcpy(thread_data[i].base_dir, indextexts_dirpath);
    thread_data[i].socket = dup(yap_socket);
    

    /*thread_server(dup(yap_socket), &yap_sin);*/
    printf( "start: %d:%s\n", i, thread_data[i].base_dir);
    pthread_create(&(pthread[i]), NULL, thread_server, (void *) &(thread_data[i]));

    /*    pthread_join(pthread[i], NULL); 
    //pthread_join(t1, NULL);
    */
    printf( "GO: %d\n", i);
  }

  /*メインループ*/
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


  /*
   *オプションを取得
   */
  if (argc > 1) {
    i = 1;
    while (1) {
      if ( argc == i)
	break;

      if (! strcmp(argv[i], "-l")) {
	/*インデックスディレクトリを取得*/
	i++;
	if (argc == i)
	  break;
	indextexts_dirpath = argv[i];
      }

      i++;
    }
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
  stdout = fopen("./core.log", "a");
  if (stdout == NULL) {
    stdout = fopen("/dev/null", "a");
  }
  stderr = fopen("./core.error", "a");
  if (stderr == NULL) {
    stderr = fopen("/dev/null", "a");
  }
  pid = fork();
  if (pid) {
    FILE *pidf = fopen("./core.pid", "w");
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


  YAP_Db_cache_init(&yappod_core_cache);

  start_deamon_thread(indextexts_dirpath);

  YAP_Db_cache_destroy(&yappod_core_cache); 
}
