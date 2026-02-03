/*
 *protocol I/O helpers between front and core
 */
#include "yappo_proto.h"

#include <string.h>

#include "yappo_alloc.h"
#include "yappo_io.h"

static int proto_read_string(FILE *socket, int max_payload, char **out)
{
  int len;
  *out = NULL;
  if (YAP_fread_exact(socket, &len, sizeof(int), 1) != 0) {
    return -1;
  }
  if (len <= 0 || len > max_payload) {
    return -1;
  }
  *out = (char *) YAP_malloc((size_t) len + 1);
  if (YAP_fread_exact(socket, *out, sizeof(char), len) != 0) {
    free(*out);
    *out = NULL;
    return -1;
  }
  (*out)[len] = '\0';
  return 0;
}

int YAP_Proto_send_query(FILE *socket, const char *dict, int max_size, const char *op, const char *keyword)
{
  int cmd = 1;
  int len;

  len = (int) strlen(dict);
  if (YAP_fwrite_exact(socket, &cmd, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, &len, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, dict, sizeof(char), len) != 0) {
    return -1;
  }

  len = (int) strlen(op);
  if (YAP_fwrite_exact(socket, &len, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, op, sizeof(char), len) != 0) {
    return -1;
  }

  len = (int) strlen(keyword);
  if (YAP_fwrite_exact(socket, &len, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, keyword, sizeof(char), len) != 0 ||
      YAP_fwrite_exact(socket, &max_size, sizeof(int), 1) != 0) {
    return -1;
  }
  return 0;
}

int YAP_Proto_recv_query(FILE *socket, int max_payload, char **dict, int *max_size, char **op, char **keyword)
{
  int cmd = 0;
  *dict = NULL;
  *op = NULL;
  *keyword = NULL;

  if (YAP_fread_exact(socket, &cmd, sizeof(int), 1) != 0) {
    return -1;
  }
  if (cmd == 0) {
    return 0;
  }
  if (cmd != 1) {
    return -1;
  }

  if (proto_read_string(socket, max_payload, dict) != 0 ||
      proto_read_string(socket, max_payload, op) != 0 ||
      proto_read_string(socket, max_payload, keyword) != 0 ||
      YAP_fread_exact(socket, max_size, sizeof(int), 1) != 0) {
    if (*dict != NULL) free(*dict);
    if (*op != NULL) free(*op);
    if (*keyword != NULL) free(*keyword);
    *dict = NULL;
    *op = NULL;
    *keyword = NULL;
    return -1;
  }

  return 1;
}

int YAP_Proto_send_shutdown(FILE *socket)
{
  int cmd = 0;
  return YAP_fwrite_exact(socket, &cmd, sizeof(int), 1);
}

int YAP_Proto_send_result(FILE *socket, const SEARCH_RESULT *result)
{
  int i, code;

  if (result == NULL || result->keyword_docs_num == 0) {
    code = 0;
    return YAP_fwrite_exact(socket, &code, sizeof(int), 1);
  }

  code = 1;
  if (YAP_fwrite_exact(socket, &code, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, &(result->keyword_id), sizeof(unsigned long), 1) != 0 ||
      YAP_fwrite_exact(socket, &(result->keyword_total_num), sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, &(result->keyword_docs_num), sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(socket, result->docs_list, sizeof(SEARCH_DOCUMENT), result->keyword_docs_num) != 0) {
    return -1;
  }

  for (i = 0; i < result->keyword_docs_num; i++) {
    if (YAP_fwrite_exact(socket, result->docs_list[i].pos, sizeof(int), result->docs_list[i].pos_len) != 0) {
      return -1;
    }
  }

  return 0;
}

SEARCH_RESULT *YAP_Proto_recv_result(FILE *socket)
{
  int i, code;
  unsigned long keyword_id;
  int keyword_total_num, keyword_docs_num;
  SEARCH_RESULT *p = NULL;

  if (YAP_fread_exact(socket, &code, sizeof(int), 1) != 0) {
    return NULL;
  }
  if (code == 0) {
    return NULL;
  }

  p = (SEARCH_RESULT *) YAP_malloc(sizeof(SEARCH_RESULT));
  if (YAP_fread_exact(socket, &keyword_id, sizeof(unsigned long), 1) != 0 ||
      YAP_fread_exact(socket, &keyword_total_num, sizeof(int), 1) != 0 ||
      YAP_fread_exact(socket, &keyword_docs_num, sizeof(int), 1) != 0) {
    free(p);
    return NULL;
  }

  if (keyword_docs_num < 0) {
    free(p);
    return NULL;
  }

  p->keyword_id = keyword_id;
  p->keyword_total_num = keyword_total_num;
  p->keyword_docs_num = keyword_docs_num;
  p->docs_list = (SEARCH_DOCUMENT *) YAP_malloc(sizeof(SEARCH_DOCUMENT) * p->keyword_docs_num);

  if (YAP_fread_exact(socket, p->docs_list, sizeof(SEARCH_DOCUMENT), p->keyword_docs_num) != 0) {
    free(p->docs_list);
    free(p);
    return NULL;
  }

  for (i = 0; i < p->keyword_docs_num; i++) {
    if (p->docs_list[i].pos_len < 0) {
      int j;
      for (j = 0; j < i; j++) free(p->docs_list[j].pos);
      free(p->docs_list);
      free(p);
      return NULL;
    }
    p->docs_list[i].pos = (int *) YAP_malloc(sizeof(int) * p->docs_list[i].pos_len);
    if (YAP_fread_exact(socket, p->docs_list[i].pos, sizeof(int), p->docs_list[i].pos_len) != 0) {
      int j;
      free(p->docs_list[i].pos);
      for (j = 0; j < i; j++) free(p->docs_list[j].pos);
      free(p->docs_list);
      free(p);
      return NULL;
    }
  }

  return p;
}
