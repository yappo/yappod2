/*
 *
 *DB回りの処理を行なう
 *
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "yappo_db.h"
#include "yappo_index.h"
#include "yappo_index_pos.h"
#include "yappo_index_filedata.h"
#include "yappo_index_deletefile.h"
#include "yappo_linklist.h"
#include "yappo_alloc.h"
#include "yappo_io.h"
#include "yappo_stat.h"
#include "yappo_ngram.h"

static void YAP_Error(char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

static FILE *yap_fopen_or_die(const char *path, const char *mode) {
  FILE *fp = fopen(path, mode);
  if (fp == NULL) {
    YAP_Error("fopen error");
  }
  return fp;
}

static void yap_touch_file_if_missing(const char *path) {
  FILE *fp;
  if (YAP_is_reg(path)) {
    return;
  }
  fp = yap_fopen_or_die(path, "w");
  fclose(fp);
}

static FILE *yap_open_managed_file(const char *path, int db_mode, long write_seek) {
  FILE *fp;

  yap_touch_file_if_missing(path);
  if (db_mode == YAPPO_DB_WRITE) {
    fp = yap_fopen_or_die(path, "r+");
    if (write_seek >= 0 && YAP_fseek_set(fp, write_seek) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    fp = yap_fopen_or_die(path, "r");
  }
  return fp;
}

static int yap_read_int_or_zero(const char *path) {
  int value = 0;
  FILE *fp;

  if (!YAP_is_reg(path)) {
    return 0;
  }
  fp = yap_fopen_or_die(path, "r");
  if (YAP_fread_exact(fp, &value, sizeof(int), 1) != 0) {
    fclose(fp);
    YAP_Error("fread error");
  }
  fclose(fp);
  return value;
}

static void yap_write_int_value(const char *path, int value) {
  FILE *fp = yap_fopen_or_die(path, "w");
  if (YAP_fwrite_exact(fp, &value, sizeof(int), 1) != 0) {
    fclose(fp);
    YAP_Error("fwrite error");
  }
  fclose(fp);
}

static char *yap_alloc_printf(const char *fmt, ...) {
  int needed;
  size_t buf_size;
  char *buf;
  va_list ap;
  va_list ap_copy;

  va_start(ap, fmt);
  va_copy(ap_copy, ap);
  needed = vsnprintf(NULL, 0, fmt, ap_copy);
  va_end(ap_copy);
  if (needed < 0) {
    va_end(ap);
    fprintf(stderr, "ERROR: format failed\n");
    exit(EXIT_FAILURE);
  }

  buf_size = (size_t)needed + 1;
  buf = (char *)YAP_malloc(buf_size);
  if (vsnprintf(buf, buf_size, fmt, ap) < 0) {
    va_end(ap);
    free(buf);
    fprintf(stderr, "ERROR: format failed\n");
    exit(EXIT_FAILURE);
  }
  va_end(ap);
  return buf;
}

/*
 *YAPPO_DB_FILESに必要なファイル名をセットする
 */
void YAP_Db_filename_set(YAPPO_DB_FILES *p) {
  char *base = p->base_dir;

  /* URLとIDの対比表 */
  p->fileindex = yap_alloc_printf("%s/%s", base, FILEINDEX_NAME);
  p->fileindex_tmp = yap_alloc_printf("%s/%s_tmp", base, FILEINDEX_NAME);

  /* DOMAINとIDの対比表 */
  p->domainindex = yap_alloc_printf("%s/%s", base, DOMAININDEX_NAME);
  p->domainindex_tmp = yap_alloc_printf("%s/%s_tmp", base, DOMAININDEX_NAME);

  /* 削除URL */
  p->deletefile = yap_alloc_printf("%s/%s", base, DELETEFILE_NAME);
  p->deletefile_tmp = yap_alloc_printf("%s/%s_tmp", base, DELETEFILE_NAME);

  /* URLメタデータ */
  p->filedata = yap_alloc_printf("%s/%s", base, FILEDATA_NAME);
  p->filedata_tmp = yap_alloc_printf("%s/%s_tmp", base, FILEDATA_NAME);

  p->filedata_size = yap_alloc_printf("%s/%s", base, FILEDATA_SIZE_NAME);
  p->filedata_size_tmp = yap_alloc_printf("%s/%s_tmp", base, FILEDATA_SIZE_NAME);

  p->filedata_index = yap_alloc_printf("%s/%s", base, FILEDATA_INDEX_NAME);
  p->filedata_index_tmp = yap_alloc_printf("%s/%s_tmp", base, FILEDATA_INDEX_NAME);

  p->key1byte = yap_alloc_printf("%s/%s", base, KEYWORD_1BYTE_NAME);
  p->key1byte_tmp = yap_alloc_printf("%s/%s_tmp", base, KEYWORD_1BYTE_NAME);

  /* 登録URL数 */
  p->filenum = yap_alloc_printf("%s/%s", base, FILENUM_NAME);

  /* 登録DOMAIN数 */
  p->domainnum = yap_alloc_printf("%s/%s", base, DOMAINNUM_NAME);

  /* 登録キーワード数 */
  p->keywordnum = yap_alloc_printf("%s/%s", base, KEYWORDNUM_NAME);

  /* 各URLのサイズ */
  p->size = yap_alloc_printf("%s/%s", base, SIZE_NAME);

  /* 各URLのDOMAIN ID */
  p->domainid = yap_alloc_printf("%s/%s", base, DOMAINID_NAME);

  /* 各URLのスコア */
  p->score = yap_alloc_printf("%s/%s", base, SCORE_NAME);

  /* 各URLのキーワード数 */
  p->filekeywordnum = yap_alloc_printf("%s/%s", base, FILEKEYWORDNUM_NAME);

  /* URLの長さ */
  p->urllen = yap_alloc_printf("%s/%s", base, URLLEN_NAME);

  /* キーワードの総出現数 */
  p->keyword_totalnum = yap_alloc_printf("%s/%s", base, KEYWORD_TOTALNUM_NAME);

  /* キーワードの総出現URL数 */
  p->keyword_docsnum = yap_alloc_printf("%s/%s", base, KEYWORD_DOCSNUM_NAME);
}

/*
 *baseファイルが存在していたら、destファイルにコピーする
 *destが存在していたらなにもしない
 */
void _tmp_copy(char *base, char *dest) {

  printf("%s/%s\n", base, dest);
  if (YAP_is_reg(dest)) {
    return;
  }

  if (YAP_is_reg(base)) {
    if (fork()) {
      int s;
      wait(&s);
    } else {
      execl("/bin/cp", "/bin/cp", base, dest, (char *)0);
      perror("execl /bin/cp");
      _exit(EXIT_FAILURE);
    }
  }
}

/*
 *常時開いておくDBを開く
 */
void YAP_Db_base_open(YAPPO_DB_FILES *p) {
  u_int32_t mode = DB_RDONLY;
  char *fileindex, *domainindex, *deletefile, *key1byte;
  char *filedata, *filedata_size, *filedata_index;

  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみモード */
    mode = DB_CREATE;

    /* すでにファイルが存在している場合は、作業用ファイルをコピーする */
    _tmp_copy(p->fileindex, p->fileindex_tmp);
    _tmp_copy(p->domainindex, p->domainindex_tmp);
    _tmp_copy(p->deletefile, p->deletefile_tmp);
    _tmp_copy(p->filedata, p->filedata_tmp);
    _tmp_copy(p->filedata_size, p->filedata_size_tmp);
    _tmp_copy(p->filedata_index, p->filedata_index_tmp);
    _tmp_copy(p->key1byte, p->key1byte_tmp);

    /* ファイル名の設定 */
    fileindex = p->fileindex_tmp;
    domainindex = p->domainindex_tmp;
    deletefile = p->deletefile_tmp;
    filedata = p->filedata_tmp;
    filedata_size = p->filedata_size_tmp;
    filedata_index = p->filedata_index_tmp;
    key1byte = p->key1byte_tmp;
  } else {
    /* ファイル名の設定 */
    fileindex = p->fileindex;
    domainindex = p->domainindex;
    deletefile = p->deletefile;
    filedata = p->filedata;
    filedata_size = p->filedata_size;
    filedata_index = p->filedata_index;
    key1byte = p->key1byte;
  }

  /* URLとIDの対比表 */
  db_create(&(p->fileindex_db), NULL, 0);
  p->fileindex_db->open(p->fileindex_db, NULL, fileindex, NULL, DB_BTREE, mode, 0);

  /* DOMAINとIDの対比表 */
  db_create(&(p->domainindex_db), NULL, 0);
  p->domainindex_db->open(p->domainindex_db, NULL, domainindex, NULL, DB_BTREE, mode, 0);

  /* 削除URL */
  p->deletefile_file = yap_open_managed_file(deletefile, p->mode, -1);

  /* URLメタデータ */
  p->filedata_file = yap_open_managed_file(filedata, p->mode, -1);
  p->filedata_size_file = yap_open_managed_file(filedata_size, p->mode, -1);
  p->filedata_index_file = yap_open_managed_file(filedata_index, p->mode, -1);

  /* 辞書ファイル 1byte */
  db_create(&(p->key1byte_db), NULL, 0);
  p->key1byte_db->open(p->key1byte_db, NULL, key1byte, NULL, DB_BTREE, mode, 0);

  /* 登録URL数 */
  p->total_filenum = yap_read_int_or_zero(p->filenum);
  /* 登録DOMAIN数 */
  p->total_domainnum = yap_read_int_or_zero(p->domainnum);
  /* 登録キーワード数 */
  p->total_keywordnum = yap_read_int_or_zero(p->keywordnum);

  printf("url=%d:key=%d\n", p->total_filenum, p->total_keywordnum);

  /* 各URLのサイズ */
  p->size_file = yap_open_managed_file(p->size, p->mode, sizeof(int) * p->total_filenum);
  /* 各URLのDOMAIN ID */
  p->domainid_file = yap_open_managed_file(p->domainid, p->mode, sizeof(int) * p->total_domainnum);
  /* 各URLのスコア */
  p->score_file = yap_open_managed_file(p->score, p->mode, sizeof(double) * p->total_filenum);
  /* 各URLのキーワード数 */
  p->filekeywordnum_file =
    yap_open_managed_file(p->filekeywordnum, p->mode, sizeof(int) * p->total_filenum);
  /* URLの長さ */
  p->urllen_file = yap_open_managed_file(p->urllen, p->mode, sizeof(int) * p->total_filenum);
  /* キーワードの総出現数 */
  p->keyword_totalnum_file = yap_open_managed_file(p->keyword_totalnum, p->mode, -1);
  /* キーワードの総出現URL数 */
  p->keyword_docsnum_file = yap_open_managed_file(p->keyword_docsnum, p->mode, -1);
}

/*
 *常時開いておくDBを閉じる
 */
void YAP_Db_base_close(YAPPO_DB_FILES *p) {

  /* URLとIDの対比表 */
  p->fileindex_db->close(p->fileindex_db, 0);
  free(p->fileindex);
  p->fileindex = NULL;
  free(p->fileindex_tmp);
  p->fileindex_tmp = NULL;

  /* DOMAINとIDの対比表 */
  p->domainindex_db->close(p->domainindex_db, 0);
  free(p->domainindex);
  p->domainindex = NULL;
  free(p->domainindex_tmp);
  p->domainindex_tmp = NULL;

  /* 削除URL */
  fclose(p->deletefile_file);
  free(p->deletefile);
  p->deletefile = NULL;
  free(p->deletefile_tmp);
  p->deletefile_tmp = NULL;

  /* URLメタデータ */
  fclose(p->filedata_file);
  fclose(p->filedata_size_file);
  fclose(p->filedata_index_file);

  if (p->mode == YAPPO_DB_WRITE) {
    /* データファイルの掃除 */
    YAP_Index_Filedata_gc(p, p->filedata_tmp, p->filedata_size_tmp, p->filedata_index_tmp);
  }

  free(p->filedata);
  p->filedata = NULL;
  free(p->filedata_tmp);
  p->filedata_tmp = NULL;
  free(p->filedata_size);
  p->filedata_size = NULL;
  free(p->filedata_size_tmp);
  p->filedata_size_tmp = NULL;
  free(p->filedata_index);
  p->filedata_index = NULL;
  free(p->filedata_index_tmp);
  p->filedata_index_tmp = NULL;

  p->key1byte_db->close(p->key1byte_db, 0);
  free(p->key1byte);
  p->key1byte = NULL;
  free(p->key1byte_tmp);
  p->key1byte_tmp = NULL;

  if (p->mode == YAPPO_DB_WRITE) {
    DIR *pos_dir;
    struct dirent *direntp;
    char *dir_path;

    /* tmpファイルを元にもどす */
    dir_path = yap_alloc_printf("%s", p->base_dir);
    pos_dir = opendir(dir_path);
    while ((direntp = readdir(pos_dir)) != NULL) {
      char *name_tmp = direntp->d_name;
      int len = strlen(name_tmp);
      char *name = yap_alloc_printf("%s/%s", dir_path, name_tmp);
      len = strlen(name);

      printf("name: %s\n", name);
      if (name[len - 4] == '_' && name[len - 3] == 't' && name[len - 2] == 'm' &&
          name[len - 1] == 'p') {
        char *new_name = yap_alloc_printf("%s", name);
        new_name[len - 4] = 0;
        printf("/bin/mv %s %s\n", name, new_name);
        if (fork()) {
          int s;
          wait(&s);
        } else {
          execl("/bin/mv", "/bin/mv", name, new_name, (char *)0);
          perror("execl /bin/mv");
          _exit(EXIT_FAILURE);
        }
        free(new_name);
      }
    }
    closedir(pos_dir);
    free(dir_path);

    /* 位置情報ファイルも元にもどす */
    dir_path = yap_alloc_printf("%s/pos", p->base_dir);
    pos_dir = opendir(dir_path);
    while ((direntp = readdir(pos_dir)) != NULL) {
      char *name_tmp = direntp->d_name;
      int len = strlen(name_tmp);
      char *name = yap_alloc_printf("%s/%s", dir_path, name_tmp);
      len = strlen(name);

      printf("name: %s\n", name);
      if (name[len - 4] == '_' && name[len - 3] == 't' && name[len - 2] == 'm' &&
          name[len - 1] == 'p') {
        char *new_name = yap_alloc_printf("%s", name);
        new_name[len - 4] = 0;
        new_name = (char *)YAP_realloc(new_name, strlen(new_name) + 1);
        printf("/bin/mv %s %s\n", name, new_name);
        if (fork()) {
          int s;
          wait(&s);
        } else {
          execl("/bin/mv", "/bin/mv", name, new_name, (char *)0);
          perror("execl /bin/mv");
          _exit(EXIT_FAILURE);
        }
        free(new_name);
      }
    }
    closedir(pos_dir);
    free(dir_path);
  }

  /* 登録URL数 */
  if (p->mode == YAPPO_DB_WRITE) {
    yap_write_int_value(p->filenum, p->total_filenum);
  }
  free(p->filenum);
  p->filenum = NULL;

  /* 登録DOMAIN数 */
  if (p->mode == YAPPO_DB_WRITE) {
    yap_write_int_value(p->domainnum, p->total_domainnum);
  }
  free(p->domainnum);
  p->domainnum = NULL;

  /* 登録キーワード数 */
  if (p->mode == YAPPO_DB_WRITE) {
    yap_write_int_value(p->keywordnum, p->total_keywordnum);
  }
  free(p->keywordnum);
  p->keywordnum = NULL;

  /* 各URLのサイズ */
  fclose(p->size_file);
  free(p->size);
  p->size = NULL;

  /* 各URLのDOMAIN ID */
  fclose(p->domainid_file);
  free(p->domainid);
  p->domainid = NULL;

  /* 各URLのスコア */
  fclose(p->score_file);
  free(p->score);
  p->score = NULL;

  /* 各URLのキーワード数 */
  fclose(p->filekeywordnum_file);
  free(p->filekeywordnum);
  p->filekeywordnum = NULL;

  /* URLの長さ */
  fclose(p->urllen_file);
  free(p->urllen);
  p->urllen = NULL;

  /* キーワードの総出現数 */
  fclose(p->keyword_totalnum_file);
  free(p->keyword_totalnum);
  p->keyword_totalnum = NULL;

  /* キーワードの総出現URL数 */
  fclose(p->keyword_docsnum_file);
  free(p->keyword_docsnum);
  p->keyword_docsnum = NULL;
}

/*
 *linklistファイルを開く
 */
void YAP_Db_linklist_open(YAPPO_DB_FILES *p) {
  char *base = p->base_dir;
  int ret;

  p->linklist = yap_alloc_printf("%s/%s", base, LINKLIST_NAME);
  if (!YAP_is_reg(p->linklist)) {
    free(p->linklist);
    p->linklist = NULL;
    return;
  }
  p->linklist_file = fopen(p->linklist, "r");
  if (p->linklist_file == NULL) {
    free(p->linklist);
    p->linklist = NULL;
    return;
  }

  p->linklist_size = yap_alloc_printf("%s/%s", base, LINKLIST_SIZE_NAME);
  if (!YAP_is_reg(p->linklist_size)) {
    free(p->linklist);
    p->linklist = NULL;
    free(p->linklist_size);
    p->linklist_size = NULL;

    fclose(p->linklist_file);
    return;
  }
  p->linklist_size_file = fopen(p->linklist_size, "r");
  if (p->linklist_size_file == NULL) {
    free(p->linklist);
    p->linklist = NULL;
    free(p->linklist_size);
    p->linklist_size = NULL;
    fclose(p->linklist_file);
    return;
  }

  p->linklist_index = yap_alloc_printf("%s/%s", base, LINKLIST_INDEX_NAME);
  if (!YAP_is_reg(p->linklist_index)) {
    free(p->linklist);
    p->linklist = NULL;
    free(p->linklist_size);
    p->linklist_size = NULL;
    free(p->linklist_index);
    p->linklist_index = NULL;

    fclose(p->linklist_file);
    fclose(p->linklist_size_file);
    return;
  }
  p->linklist_index_file = fopen(p->linklist_index, "r");
  if (p->linklist_index_file == NULL) {
    free(p->linklist);
    p->linklist = NULL;
    free(p->linklist_size);
    p->linklist_size = NULL;
    free(p->linklist_index);
    p->linklist_index = NULL;
    fclose(p->linklist_file);
    fclose(p->linklist_size_file);
    return;
  }

  ret = YAP_fread_exact(p->linklist_file, &(p->linklist_num), sizeof(int), 1);
  if (ret != 0) {
    p->linklist_num = 0;
  }
}

/*
 *linklistファイルを閉じる
 */
void YAP_Db_linklist_close(YAPPO_DB_FILES *p) {
  if (p->linklist != NULL) {
    fclose(p->linklist_file);
    free(p->linklist);
    p->linklist = NULL;

    fclose(p->linklist_size_file);
    free(p->linklist_size);
    p->linklist_size = NULL;

    fclose(p->linklist_index_file);
    free(p->linklist_index);
    p->linklist_index = NULL;

    p->linklist_num = 0;
  }
}

/*
 *位置情報DBを開く
 */
int YAP_Db_pos_open(YAPPO_DB_FILES *p, int pos_id) {
  char *base = p->base_dir;
  char *pos, *pos_size, *pos_index;
  int ret;

  /* ファイル名作成 */
  p->pos = yap_alloc_printf("%s/" POSTINGS_NAME, base, pos_id);

  if (p->mode == YAPPO_DB_READ) {
    /*
     *読み込みモードなら
     *開く前にファイルが存在しているか調べる
     */
    if (!YAP_is_reg(p->pos)) {
      /* 存在しない */
      free(p->pos);
      p->pos = NULL;
      return 0;
    }
  }

  p->pos_tmp = yap_alloc_printf("%s/" POSTINGS_NAME "_tmp", base, pos_id);
  p->pos_size = yap_alloc_printf("%s/" POSTINGS_SIZE_NAME, base, pos_id);
  p->pos_size_tmp = yap_alloc_printf("%s/" POSTINGS_SIZE_NAME "_tmp", base, pos_id);
  p->pos_index = yap_alloc_printf("%s/" POSTINGS_INDEX_NAME, base, pos_id);
  p->pos_index_tmp = yap_alloc_printf("%s/" POSTINGS_INDEX_NAME "_tmp", base, pos_id);

  /*
   *DBを開く
   */
  if (p->mode == YAPPO_DB_WRITE) {
    /*
     *書きこみモード
     *すでにファイルが存在している場合は、作業用ファイルをコピーする
     */
    _tmp_copy(p->pos, p->pos_tmp);
    _tmp_copy(p->pos_size, p->pos_size_tmp);
    _tmp_copy(p->pos_index, p->pos_index_tmp);

    /* ファイル名の設定 */
    pos = p->pos_tmp;
    pos_size = p->pos_size_tmp;
    pos_index = p->pos_index_tmp;
  } else {
    /* ファイル名の設定 */
    pos = p->pos;
    pos_size = p->pos_size;
    pos_index = p->pos_index;
  }

  if (p->mode == YAPPO_DB_WRITE) {
    /* 書き込み時 */
    if (!YAP_is_reg(pos)) {
      int i = 0;
      long l = 0;
      p->pos_file = fopen(pos, "w");
      if (p->pos_file == NULL) {
        YAP_Error("fopen error");
      }
      if (YAP_fwrite_exact(p->pos_file, &l, sizeof(long), 1) != 0 ||
          YAP_fwrite_exact(p->pos_file, &i, sizeof(int), 1) != 0 ||
          YAP_fwrite_exact(p->pos_file, &i, sizeof(int), 1) != 0) {
        YAP_Error("fwrite error");
      }
      fclose(p->pos_file);
    }
    if (!YAP_is_reg(pos_size)) {
      p->pos_size_file = fopen(pos_size, "w");
      if (p->pos_size_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->pos_size_file);
    }
    if (!YAP_is_reg(pos_index)) {
      p->pos_index_file = fopen(pos_index, "w");
      if (p->pos_index_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->pos_index_file);
    }

    p->pos_file = fopen(pos, "r+");
    p->pos_size_file = fopen(pos_size, "r+");
    p->pos_index_file = fopen(pos_index, "r+");
    if (p->pos_file == NULL || p->pos_size_file == NULL || p->pos_index_file == NULL) {
      YAP_Error("fopen error");
    }
  } else {
    /* 読み込み時 */
    p->pos_file = fopen(pos, "r");
    p->pos_size_file = fopen(pos_size, "r");
    p->pos_index_file = fopen(pos_index, "r");
    if (p->pos_file == NULL || p->pos_size_file == NULL || p->pos_index_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* 各種情報を読みこむ */
  ret = YAP_fread_exact(p->pos_file, &(p->pos_num), sizeof(long), 1);
  if (ret != 0) {
    p->pos_num = 0;
  }
  ret = YAP_fread_exact(p->pos_file, &(p->pos_fileindex_start), sizeof(int), 1);
  if (ret != 0) {
    p->pos_fileindex_start = 0;
  }
  ret = YAP_fread_exact(p->pos_file, &(p->pos_fileindex_end), sizeof(int), 1);
  if (ret != 0) {
    p->pos_fileindex_end = 0;
  }

  return 1;
}

/*
 *位置情報DBを閉じる
 */
void YAP_Db_pos_close(YAPPO_DB_FILES *p) {

  /* 各種情報を書きこむ */
  if (p->mode == YAPPO_DB_WRITE) {

    if (p->pos_fileindex_start == 0) {
      p->pos_fileindex_start = p->pos_fileindex_start_w;
    }
    if (p->pos_fileindex_end < p->pos_fileindex_end_w) {
      p->pos_fileindex_end = p->pos_fileindex_end_w;
    }

    if (YAP_fseek_set(p->pos_file, 0L) != 0 ||
        YAP_fwrite_exact(p->pos_file, &(p->pos_num), sizeof(long), 1) != 0 ||
        YAP_fwrite_exact(p->pos_file, &(p->pos_fileindex_start), sizeof(int), 1) != 0 ||
        YAP_fwrite_exact(p->pos_file, &(p->pos_fileindex_end), sizeof(int), 1) != 0) {
      YAP_Error("pos header write error");
    }
  }

  /* ファイルを閉じる */
  fclose(p->pos_file);
  fclose(p->pos_size_file);
  fclose(p->pos_index_file);

  /* データの整理を行なう */
  if (p->mode == YAPPO_DB_WRITE) {
    YAP_Index_Pos_gc(p, p->pos_tmp, p->pos_size_tmp, p->pos_index_tmp);
  }

  /*
   *メモリクリア
   */
  free(p->pos);
  p->pos = NULL;
  free(p->pos_tmp);
  p->pos_tmp = NULL;
  free(p->pos_size);
  p->pos_size = NULL;
  free(p->pos_size_tmp);
  p->pos_size_tmp = NULL;
  free(p->pos_index);
  p->pos_index = NULL;
  free(p->pos_index_tmp);
  p->pos_index_tmp = NULL;
}

/*
 *キャッシュの初期化
 */
void YAP_Db_cache_init(YAPPO_CACHE *p) {
  memset(p, 0, sizeof(YAPPO_CACHE));

  pthread_mutex_init(&(p->score_mutex), NULL);
  pthread_mutex_init(&(p->size_mutex), NULL);
  pthread_mutex_init(&(p->urllen_mutex), NULL);
  pthread_mutex_init(&(p->filekeywordnum_mutex), NULL);
  pthread_mutex_init(&(p->domainid_mutex), NULL);
  pthread_mutex_init(&(p->deletefile_mutex), NULL);

  p->score = NULL;
  p->size = NULL;
  p->urllen = NULL;
  p->filekeywordnum = NULL;
  p->domainid = NULL;
  p->deletefile = NULL;

  p->score_num = 0;
  p->size_num = 0;
  p->urllen_num = 0;
  p->filekeywordnum_num = 0;
  p->domainid_num = 0;
  p->deletefile_num = 0;

  p->total_filenum = 0;
  p->total_domainnum = 0;
  p->total_keywordnum = 0;
}

/*
 *キャッシュの破棄
 */
void YAP_Db_cache_destroy(YAPPO_CACHE *p) {
  pthread_mutex_destroy(&(p->score_mutex));
  pthread_mutex_destroy(&(p->size_mutex));
  pthread_mutex_destroy(&(p->urllen_mutex));
  pthread_mutex_destroy(&(p->filekeywordnum_mutex));
  pthread_mutex_destroy(&(p->domainid_mutex));
  pthread_mutex_destroy(&(p->deletefile_mutex));

  if (p->score != NULL) {
    free(p->score);
    p->score = NULL;
  }
  if (p->size != NULL) {
    free(p->size);
    p->size = NULL;
  }
  if (p->urllen != NULL) {
    free(p->urllen);
    p->urllen = NULL;
  }
  if (p->filekeywordnum != NULL) {
    free(p->filekeywordnum);
    p->filekeywordnum = NULL;
  }
  if (p->domainid != NULL) {
    free(p->domainid);
    p->domainid = NULL;
  }
  if (p->deletefile != NULL) {
    free(p->deletefile);
    p->deletefile = NULL;
  }

  p->score_num = 0;
  p->size_num = 0;
  p->urllen_num = 0;
  p->filekeywordnum_num = 0;
  p->domainid_num = 0;
  p->deletefile_num = 0;

  p->total_filenum = 0;
  p->total_domainnum = 0;
  p->total_keywordnum = 0;
}

/*
 *必要ならば各ファイルをメモリ上にキャッシュする
 */
void YAP_Db_cache_load(YAPPO_DB_FILES *ydfp, YAPPO_CACHE *p) {

  if (ydfp->total_filenum != p->total_filenum || ydfp->total_domainnum != p->total_domainnum ||
      ydfp->total_keywordnum != p->total_keywordnum) {
    /*
     *キャッシュ上の数値と実際の数値が食い違っている
     *ので読みこみ開始する
     */

    printf("CACHE LOAD\n");

    /* スコアファイルキャッシュ */
    pthread_mutex_lock(&(p->score_mutex));
    p->score = (double *)YAP_realloc(p->score, sizeof(double) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->score_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->score_file, p->score, sizeof(double), ydfp->total_filenum) != 0) {
      p->score_num = 0;
    } else {
      p->score_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->score_mutex));

    /* ファイルサイズキャッシュ */
    pthread_mutex_lock(&(p->size_mutex));
    p->size = (int *)YAP_realloc(p->size, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->size_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->size_file, p->size, sizeof(int), ydfp->total_filenum) != 0) {
      p->size_num = 0;
    } else {
      p->size_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->size_mutex));

    /* URL文字数ファイルキャッシュ */
    pthread_mutex_lock(&(p->urllen_mutex));
    p->urllen = (int *)YAP_realloc(p->urllen, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->urllen_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->urllen_file, p->urllen, sizeof(int), ydfp->total_filenum) != 0) {
      p->urllen_num = 0;
    } else {
      p->urllen_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->urllen_mutex));

    /* 各URLのキーワード数ファイルキャッシュ */
    pthread_mutex_lock(&(p->filekeywordnum_mutex));
    p->filekeywordnum =
      (unsigned int *)YAP_realloc(p->filekeywordnum, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->filekeywordnum_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->filekeywordnum_file, p->filekeywordnum, sizeof(int),
                        ydfp->total_filenum) != 0) {
      p->filekeywordnum_num = 0;
    } else {
      p->filekeywordnum_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->filekeywordnum_mutex));

    /* domain idファイルキャッシュ */
    pthread_mutex_lock(&(p->domainid_mutex));
    p->domainid = (int *)YAP_realloc(p->domainid, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->domainid_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->domainid_file, p->domainid, sizeof(int), ydfp->total_filenum) != 0) {
      p->domainid_num = 0;
    } else {
      p->domainid_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->domainid_mutex));

    /* 削除URLファイルキャッシュ */
    pthread_mutex_lock(&(p->domainid_mutex));
    p->deletefile = (unsigned char *)YAP_realloc(p->deletefile, (ydfp->total_filenum / 8) + 1);
    memset(p->deletefile, 0, (ydfp->total_filenum / 8) + 1);
    if (YAP_fseek_set(ydfp->deletefile_file, 0L) != 0) {
      p->deletefile_num = 0;
    } else {
      size_t got =
        YAP_fread_try(ydfp->deletefile_file, p->deletefile, 1, (ydfp->total_filenum / 8) + 1);
      p->deletefile_num = (int)got;
    }
    pthread_mutex_unlock(&(p->domainid_mutex));

    printf("load delete: %d/%d\n", p->deletefile_num, (ydfp->total_filenum / 8) + 1);

    p->total_filenum = ydfp->total_filenum;
    p->total_domainnum = ydfp->total_domainnum;
    p->total_keywordnum = ydfp->total_keywordnum;
  }
}
