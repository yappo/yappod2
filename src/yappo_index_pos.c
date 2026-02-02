/*
 *キーワード出現位置データを取扱う
 */
#include <stdio.h>
#include <string.h>

#include "yappo_index_pos.h"
#include "yappo_index.h"
#include "yappo_alloc.h"
#include "yappo_io.h"



/*
 *keyword_idをキーに検索をして位置リストを取得
 */
int YAP_Index_Pos_get(YAPPO_DB_FILES *ydfp, unsigned long keyword_id,
		      unsigned char **postings_buf, int *postings_buf_len)
{
  int ret;
  int pos_size, pos_index;
  int seek;

  if (ydfp->pos_num < keyword_id) {
    /*対象となるIDは存在していない*/
    return -1;
  }

  seek = sizeof(int) * keyword_id;

  /*サイズの読みこみ*/
  if (YAP_fseek_set(ydfp->pos_size_file, seek) != 0) {
    return -1;
  }
  ret = YAP_fread_exact(ydfp->pos_size_file, &pos_size, sizeof(int), 1);
  if (ret != 0) {
    return -1;
  }

  if (pos_size == 0) {
    /*サイズが0なので登録されていない*/
    return -1;
  }


  /*indexの読みこみ*/
  if (YAP_fseek_set(ydfp->pos_index_file, seek) != 0) {
    return -1;
  }
  ret = YAP_fread_exact(ydfp->pos_index_file, &pos_index, sizeof(int), 1);
  if (ret != 0) {
    return -1;
  }


  /*データの読みこみ*/
  *postings_buf = (unsigned char *) YAP_malloc(pos_size);
  if (YAP_fseek_set(ydfp->pos_file, pos_index) != 0 ||
      YAP_fread_exact(ydfp->pos_file, *postings_buf, 1, pos_size) != 0) {
    free(*postings_buf);
    *postings_buf = NULL;
    return -1;
  }
  *postings_buf_len = pos_size;

  return 0;
}

/*
 *keyword_idをキーに検索をして位置リストを設定
 *常にファイルの末尾に追加する
 */
int YAP_Index_Pos_put(YAPPO_DB_FILES *ydfp, unsigned long keyword_id,
		      unsigned char *postings_buf, int postings_buf_len)
{
  int pos_index;
  int seek;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  seek = sizeof(int) * keyword_id;

  /*サイズの書きこみ*/
  if (YAP_fseek_set(ydfp->pos_size_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->pos_size_file, &postings_buf_len, sizeof(int), 1) != 0) {
    return -1;
  }

  /*データの書きこみ*/
  if (YAP_fseek_end(ydfp->pos_file, 0L) != 0) {
    return -1;
  }
  pos_index = ftell(ydfp->pos_file);
  if (YAP_fwrite_exact(ydfp->pos_file, postings_buf, 1, postings_buf_len) != 0) {
    return -1;
  }

  /*indexの書きこみ*/
  if (YAP_fseek_set(ydfp->pos_index_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->pos_index_file, &pos_index, sizeof(int), 1) != 0) {
    return -1;
  }

  return 0;
}

/*
 *keyword_idをキーに検索をして位置リストを削除
 */
int YAP_Index_Pos_del(YAPPO_DB_FILES *ydfp, unsigned long keyword_id)
{
  int c = 0;
  int seek;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  seek = sizeof(int) * keyword_id;

  /*サイズの書きこみ*/
  if (YAP_fseek_set(ydfp->pos_size_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->pos_size_file, &c, sizeof(int), 1) != 0) {
    return -1;
  }

  /*indexの書きこみ*/
  if (YAP_fseek_set(ydfp->pos_index_file, seek) != 0 ||
      YAP_fwrite_exact(ydfp->pos_index_file, &c, sizeof(int), 1) != 0) {
    return -1;
  }

  return 0;
}


/*
 *位置情報ファイルのごみ整理を行なう
 *位置情報ファイルをopenしているプロセスが無いことが前堤
 */
int YAP_Index_Pos_gc(YAPPO_DB_FILES *ydfp, char *pos, char *pos_size, char *pos_index)
{
  int i;
  long pos_num;
  int seek, index, index_tmp, size, tmp;
  char *pos_tmp, *pos_index_tmp;
  FILE *pos_file, *pos_size_file, *pos_index_file;
  FILE *pos_tmp_file, *pos_index_tmp_file;
  int buf_len = 0;
  char *buf = NULL;

  printf("Start YAP_Index_Pos_gc\n");

  /*待避ファイル名の作成*/
  pos_tmp = (char *) YAP_malloc(strlen(pos) + 5);
  sprintf(pos_tmp, "%s_tmp", pos);
  pos_index_tmp = (char *) YAP_malloc(strlen(pos_index) + 5);
  sprintf(pos_index_tmp, "%s_tmp", pos_index);

  /*ファイルを開く*/
  pos_file = fopen(pos, "r");
  pos_size_file = fopen(pos_size, "r");
  pos_index_file = fopen(pos_index, "r");
  pos_tmp_file = fopen(pos_tmp, "w");
  pos_index_tmp_file = fopen(pos_index_tmp, "w");
  if (pos_file == NULL ||
      pos_size_file == NULL ||
      pos_index_file == NULL ||
      pos_tmp_file == NULL ||
      pos_index_tmp_file == NULL) {
    fprintf(stderr, "fopen error: pos files\n");
    if (pos_file != NULL) fclose(pos_file);
    if (pos_size_file != NULL) fclose(pos_size_file);
    if (pos_index_file != NULL) fclose(pos_index_file);
    if (pos_tmp_file != NULL) fclose(pos_tmp_file);
    if (pos_index_tmp_file != NULL) fclose(pos_index_tmp_file);
    free(pos_tmp);
    free(pos_index_tmp);
    return -1;
  }


  /*基本情報をコピーする*/
  if (YAP_fread_exact(pos_file, &pos_num, sizeof(long), 1) != 0 ||
      YAP_fwrite_exact(pos_tmp_file, &pos_num, sizeof(long), 1) != 0 ||
      YAP_fread_exact(pos_file, &tmp, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(pos_tmp_file, &tmp, sizeof(int), 1) != 0 ||
      YAP_fread_exact(pos_file, &tmp, sizeof(int), 1) != 0 ||
      YAP_fwrite_exact(pos_tmp_file, &tmp, sizeof(int), 1) != 0) {
    fclose(pos_file);
    fclose(pos_size_file);
    fclose(pos_index_file);
    fclose(pos_tmp_file);
    fclose(pos_index_tmp_file);
    free(pos_tmp);
    free(pos_index_tmp);
    return -1;
  }


  if (YAP_fseek_set(pos_size_file, sizeof(int)) != 0 ||
      YAP_fseek_set(pos_index_file, sizeof(int)) != 0 ||
      YAP_fseek_set(pos_index_tmp_file, sizeof(int)) != 0) {
    fclose(pos_file);
    fclose(pos_size_file);
    fclose(pos_index_file);
    fclose(pos_tmp_file);
    fclose(pos_index_tmp_file);
    free(pos_tmp);
    free(pos_index_tmp);
    return -1;
  }

  /*位置情報のコピー*/
  for (i = 1; (unsigned int) i <= ydfp->total_keywordnum; i++) {
    seek = sizeof(int) * i;

    /*サイズの読みこみ*/
    if (YAP_fread_exact(pos_size_file, &size, sizeof(int), 1) != 0) {
      break;
    }

    if (size > 0) {
      /*登録が有る*/

      /*indexの読みこみ*/
      if (YAP_fseek_set(pos_index_file, seek) != 0 ||
          YAP_fread_exact(pos_index_file, &index, sizeof(int), 1) != 0) {
        break;
      }

      /*データの読みこみ*/
      if (buf_len < size) {
	buf = (char *) YAP_realloc(buf, size);
	buf_len = size;
      }
      if (YAP_fseek_set(pos_file, index) != 0 ||
          YAP_fread_exact(pos_file, buf, 1, size) != 0) {
        break;
      }

      /*データの書きこみ*/
      index_tmp = ftell(pos_tmp_file);
      if (YAP_fwrite_exact(pos_tmp_file, buf, 1, size) != 0) {
        break;
      }
    } else {
      index_tmp = 0;
    }
    /*indexの書きこみ*/

    if (YAP_fwrite_exact(pos_index_tmp_file, &index_tmp, sizeof(int), 1) != 0) {
      break;
    }
  }

  if (buf != NULL) {
    free(buf);
  }
  
  /*ファイルを閉じる*/
  fclose(pos_file);
  fclose(pos_size_file);
  fclose(pos_index_file);
  fclose(pos_tmp_file);
  fclose(pos_index_tmp_file);

  /*ファイルを入れ換える*/
  if (fork()) { int s; wait(&s);} else {
    execl("/bin/mv", "/bin/mv", pos_tmp, pos, (char *) 0);
    exit(0);
  }
  if (fork()) { int s; wait(&s);} else {
    execl("/bin/mv", "/bin/mv", pos_index_tmp, pos_index, (char *) 0);
    exit(0);
  }

  free(pos_tmp);
  free(pos_index_tmp);

  printf("End YAP_Index_Pos_gc\n");

  return 0;
}
