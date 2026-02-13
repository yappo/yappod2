/*
 *URLメタデータを取扱う
 */

#include "yappo_index_filedata.h"
#include "yappo_alloc.h"
#include "yappo_io.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 *fileindexをキーに検索をしてFILEDATAを取得
 */
int YAP_Index_Filedata_get(YAPPO_DB_FILES *ydfp, int fileindex, FILEDATA *filedata) {
  int filedata_size, filedata_index;
  int seek;
  size_t str_len;
  unsigned char *buf, *bufp;

  if ((unsigned int)fileindex > ydfp->total_filenum) {
    /*対象となるIDは存在していない*/
    return -1;
  }

  seek = sizeof(int) * fileindex;

  /*サイズの読みこみ*/
  if (YAP_fseek_set(ydfp->filedata_size_file, seek) != 0) {
    return -1;
  }
  if (YAP_fread_try(ydfp->filedata_size_file, &filedata_size, sizeof(int), 1) != 1) {
    return -1;
  }

  if (filedata_size == 0) {
    /*サイズが0なので登録されていない*/
    return -1;
  }

  /*indexの読みこみ*/
  if (YAP_fseek_set(ydfp->filedata_index_file, seek) != 0) {
    return -1;
  }
  if (YAP_fread_try(ydfp->filedata_index_file, &filedata_index, sizeof(int), 1) != 1) {
    return -1;
  }

  /*データの読みこみ*/
  buf = (unsigned char *)YAP_malloc(filedata_size);
  if (YAP_fseek_set(ydfp->filedata_file, filedata_index) != 0 ||
      YAP_fread_exact(ydfp->filedata_file, buf, 1, filedata_size) != 0) {
    free(buf);
    buf = NULL;
    return -1;
  }

  memset(filedata, 0, sizeof(FILEDATA));

  bufp = buf;

  /*FIELDATAをシリアライズする
   * url\0title\0comment\0size\0keyword_num\0lastmod\0domainid\0other_len\0other
   */
  memcpy(&str_len, bufp, sizeof(size_t));
  bufp += sizeof(size_t);
  if (str_len > 0) {
    filedata->url = (char *)YAP_malloc((size_t)str_len + 1);
    memcpy(filedata->url, bufp, str_len);
    filedata->url[str_len] = '\0';
    bufp += str_len;
  } else {
    filedata->url = NULL;
  }

  memcpy(&str_len, bufp, sizeof(size_t));
  bufp += sizeof(size_t);
  if (str_len > 0) {
    filedata->title = (char *)YAP_malloc((size_t)str_len + 1);
    memcpy(filedata->title, bufp, str_len);
    filedata->title[str_len] = '\0';
    bufp += str_len;
  } else {
    filedata->title = NULL;
  }

  memcpy(&str_len, bufp, sizeof(size_t));
  bufp += sizeof(size_t);
  if (str_len > 0) {
    filedata->comment = (char *)YAP_malloc((size_t)str_len + 1);
    memcpy(filedata->comment, bufp, str_len);
    filedata->comment[str_len] = '\0';
    bufp += str_len;
  } else {
    filedata->comment = NULL;
  }

  memcpy(&(filedata->size), bufp, sizeof(int));
  bufp += sizeof(int);
  memcpy(&(filedata->keyword_num), bufp, sizeof(int));
  bufp += sizeof(int);
  memcpy(&(filedata->lastmod), bufp, sizeof(time_t));
  bufp += sizeof(time_t);
  memcpy(&(filedata->domainid), bufp, sizeof(int));
  bufp += sizeof(int);

  memcpy(&(filedata->other_len), bufp, sizeof(int));
  bufp += sizeof(int);
  if (filedata->other_len > 0) {
    filedata->other = (unsigned char *)YAP_malloc(filedata->other_len + 1);
    memcpy(filedata->other, bufp, filedata->other_len);
    filedata->other[filedata->other_len] = '\0';
    bufp += filedata->other_len;
  } else {
    filedata->other = NULL;
  }

  free(buf);

  return 0;
}

/*
 *fileindexをキーに検索をしてFILEDATAを設定
 */
int YAP_Index_Filedata_put(YAPPO_DB_FILES *ydfp, int fileindex, FILEDATA *filedata) {
  int filedata_index;
  int seek;
  char *buf, *bufp;
  int buf_len = 0;
  size_t str_len;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  /*FIELDATAをシリアライズする
   *url\0title\0comment\0size\0keyword_num\0lastmod\0domainid\0other_len\0other
   */

  buf_len += sizeof(size_t);
  if (filedata->url != NULL) {
    buf_len += strlen(filedata->url);
  }
  buf_len += sizeof(size_t);
  if (filedata->title != NULL) {
    buf_len += strlen(filedata->title);
  }
  buf_len += sizeof(size_t);
  if (filedata->comment != NULL) {
    buf_len += strlen(filedata->comment);
  }
  buf_len += sizeof(filedata->size) + sizeof(filedata->keyword_num) + sizeof(filedata->lastmod) +
             sizeof(filedata->domainid) + sizeof(filedata->other_len);
  if (filedata->other != NULL) {
    buf_len += filedata->other_len;
  }

  buf = (char *)YAP_malloc(buf_len);
  bufp = buf;

  if (filedata->url != NULL) {
    str_len = strlen(filedata->url);
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
    memcpy(bufp, filedata->url, str_len);
    bufp += str_len;
  } else {
    str_len = 0;
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
  }
  if (filedata->title != NULL) {
    str_len = strlen(filedata->title);
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
    memcpy(bufp, filedata->title, str_len);
    bufp += str_len;
  } else {
    str_len = 0;
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
  }
  if (filedata->comment != NULL) {
    str_len = strlen(filedata->comment);
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
    memcpy(bufp, filedata->comment, str_len);
    bufp += str_len;
  } else {
    str_len = 0;
    memcpy(bufp, &str_len, sizeof(size_t));
    bufp += sizeof(size_t);
  }

  memcpy(bufp, &(filedata->size), sizeof(int));
  bufp += sizeof(int);
  memcpy(bufp, &(filedata->keyword_num), sizeof(int));
  bufp += sizeof(int);
  memcpy(bufp, &(filedata->lastmod), sizeof(time_t));
  bufp += sizeof(time_t);
  memcpy(bufp, &(filedata->domainid), sizeof(int));
  bufp += sizeof(int);

  memcpy(bufp, &(filedata->other_len), sizeof(int));
  bufp += sizeof(int);
  if (filedata->other != NULL) {
    memcpy(bufp, filedata->other, filedata->other_len);
  }

  /*登録*/

  seek = sizeof(int) * fileindex;

  /*サイズの書きこみ*/
  if (YAP_fseek_set(ydfp->filedata_size_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->filedata_size_file, &buf_len, sizeof(int), 1) != 0) {
    free(buf);
    return -1;
  }

  /*データの書きこみ*/
  if (YAP_fseek_end(ydfp->filedata_file, 0L) != 0) {
    free(buf);
    return -1;
  }
  filedata_index = ftell(ydfp->filedata_file);
  if (YAP_fwrite_exact(ydfp->filedata_file, buf, 1, buf_len) != 0) {
    free(buf);
    return -1;
  }

  /*indexの書きこみ*/
  if (YAP_fseek_set(ydfp->filedata_index_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->filedata_index_file, &filedata_index, sizeof(int), 1) != 0) {
    free(buf);
    return -1;
  }

  free(buf);

  return 0;
}

/*
 *fileindexをキーに検索をしてFILEDATAを削除
 */
int YAP_Index_Filedata_del(YAPPO_DB_FILES *ydfp, int fileindex) {
  int c = 0;
  int seek;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  seek = sizeof(int) * fileindex;

  /*サイズの書きこみ*/
  if (YAP_fseek_set(ydfp->filedata_size_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->filedata_size_file, &c, sizeof(int), 1) != 0) {
    return -1;
  }

  /*indexの書きこみ*/
  if (YAP_fseek_set(ydfp->filedata_index_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->filedata_index_file, &c, sizeof(int), 1) != 0) {
    return -1;
  }

  return 0;
}

/*
 *FILEDATAのメモリをクリアする
 */
int YAP_Index_Filedata_free(FILEDATA *p) {
  if (p->url != NULL) {
    free(p->url);
    p->url = NULL;
  }
  if (p->title != NULL) {
    free(p->title);
    p->title = NULL;
  }
  if (p->comment != NULL) {
    free(p->comment);
    p->comment = NULL;
  }
  if (p->other != NULL) {
    free(p->other);
    p->other = NULL;
  }
  p->lastmod = 0;
  p->size = 0;
  p->keyword_num = 0;
  p->other_len = 0;

  return 0;
}

/*
 *メタファイルのごみ整理を行なう
 *メタファイルをopenしているプロセスが無いことが前堤
 */
int YAP_Index_Filedata_gc(YAPPO_DB_FILES *ydfp, char *filedata, char *filedata_size,
                          char *filedata_index) {
  int i;
  int seek, index, index_tmp, size;
  char *filedata_tmp, *filedata_index_tmp;
  FILE *filedata_file, *filedata_size_file, *filedata_index_file;
  FILE *filedata_tmp_file, *filedata_index_tmp_file;
  int buf_len = 0;
  char *buf = NULL;

  printf("Start YAP_Index_Filedata_gc\n");

  /*待避ファイル名の作成*/
  filedata_tmp = (char *)YAP_malloc(strlen(filedata) + 5);
  if (snprintf(filedata_tmp, strlen(filedata) + 5, "%s_tmp", filedata) < 0) {
    free(filedata_tmp);
    return -1;
  }
  filedata_index_tmp = (char *)YAP_malloc(strlen(filedata_index) + 5);
  if (snprintf(filedata_index_tmp, strlen(filedata_index) + 5, "%s_tmp", filedata_index) < 0) {
    free(filedata_tmp);
    free(filedata_index_tmp);
    return -1;
  }

  /*ファイルを開く*/
  filedata_file = fopen(filedata, "r");
  filedata_size_file = fopen(filedata_size, "r");
  filedata_index_file = fopen(filedata_index, "r");
  filedata_tmp_file = fopen(filedata_tmp, "w");
  filedata_index_tmp_file = fopen(filedata_index_tmp, "w");
  if (filedata_file == NULL || filedata_size_file == NULL || filedata_index_file == NULL ||
      filedata_tmp_file == NULL || filedata_index_tmp_file == NULL) {
    fprintf(stderr, "fopen error: filedata files\n");
    if (filedata_file != NULL)
      fclose(filedata_file);
    if (filedata_size_file != NULL)
      fclose(filedata_size_file);
    if (filedata_index_file != NULL)
      fclose(filedata_index_file);
    if (filedata_tmp_file != NULL)
      fclose(filedata_tmp_file);
    if (filedata_index_tmp_file != NULL)
      fclose(filedata_index_tmp_file);
    free(filedata_tmp);
    free(filedata_index_tmp);
    return -1;
  }

  if (YAP_fseek_set(filedata_size_file, sizeof(int)) != 0 ||
      YAP_fseek_set(filedata_index_file, sizeof(int)) != 0 ||
      YAP_fseek_set(filedata_index_tmp_file, sizeof(int)) != 0) {
    fclose(filedata_file);
    fclose(filedata_size_file);
    fclose(filedata_index_file);
    fclose(filedata_tmp_file);
    fclose(filedata_index_tmp_file);
    free(filedata_tmp);
    free(filedata_index_tmp);
    return -1;
  }

  /*位置情報のコピー*/
  for (i = 1; (unsigned int)i <= ydfp->total_filenum; i++) {
    seek = sizeof(int) * i;

    /*サイズの読みこみ*/
    if (YAP_fread_exact(filedata_size_file, &size, sizeof(int), 1) != 0) {
      break;
    }

    if (size > 0) {
      /*登録が有る*/

      /*indexの読みこみ*/
      if (YAP_fseek_set(filedata_index_file, seek) != 0 ||
          YAP_fread_exact(filedata_index_file, &index, sizeof(int), 1) != 0) {
        break;
      }

      /*データの読みこみ*/
      if (buf_len < size) {
        buf = (char *)YAP_realloc(buf, size);
        buf_len = size;
      }
      if (YAP_fseek_set(filedata_file, index) != 0 ||
          YAP_fread_exact(filedata_file, buf, 1, size) != 0) {
        break;
      }

      /*データの書きこみ*/
      index_tmp = ftell(filedata_tmp_file);
      if (YAP_fwrite_exact(filedata_tmp_file, buf, 1, size) != 0) {
        break;
      }
    } else {
      index_tmp = 0;
    }
    /*indexの書きこみ*/
    if (YAP_fwrite_exact(filedata_index_tmp_file, &index_tmp, sizeof(int), 1) != 0) {
      break;
    }
  }

  if (buf != NULL) {
    free(buf);
  }

  /*ファイルを閉じる*/
  fclose(filedata_file);
  fclose(filedata_size_file);
  fclose(filedata_index_file);
  fclose(filedata_tmp_file);
  fclose(filedata_index_tmp_file);

  /*ファイルを入れ換える*/
  if (fork()) {
    int s;
    wait(&s);
  } else {
    execl("/bin/mv", "/bin/mv", filedata_tmp, filedata, (char *)0);
    perror("execl /bin/mv");
    _exit(EXIT_FAILURE);
  }
  if (fork()) {
    int s;
    wait(&s);
  } else {
    execl("/bin/mv", "/bin/mv", filedata_index_tmp, filedata_index, (char *)0);
    perror("execl /bin/mv");
    _exit(EXIT_FAILURE);
  }

  free(filedata_tmp);
  free(filedata_index_tmp);

  printf("End YAP_Index_Filedata_gc\n");

  return 0;
}
