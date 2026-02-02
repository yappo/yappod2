/*
 *
 *DB回りの処理を行なう
 *
 */
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
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

static void YAP_Error(char *msg)
{
  perror(msg);
  exit(-1);
}

/*
 *YAPPO_DB_FILESに必要なファイル名をセットする
 */
void YAP_Db_filename_set (YAPPO_DB_FILES *p)
{
  char *tmp;
  char *base = p->base_dir;
  int base_len = strlen(base) + 2;


  /* URLとIDの対比表 */
  tmp = (char *) YAP_malloc(base_len + strlen(FILEINDEX_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILEINDEX_NAME);
  p->fileindex = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(FILEINDEX_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, FILEINDEX_NAME);
  p->fileindex_tmp = tmp;

  /* DOMAINとIDの対比表 */
  tmp = (char *) YAP_malloc(base_len + strlen(DOMAININDEX_NAME) + 1);
  sprintf( tmp, "%s/%s", base, DOMAININDEX_NAME);
  p->domainindex = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(DOMAININDEX_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, DOMAININDEX_NAME);
  p->domainindex_tmp = tmp;

  /* 削除URL */
  tmp = (char *) YAP_malloc(base_len + strlen(DELETEFILE_NAME) + 1);
  sprintf( tmp, "%s/%s", base, DELETEFILE_NAME);
  p->deletefile = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(DELETEFILE_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, DELETEFILE_NAME);
  p->deletefile_tmp = tmp;

  /* URLメタデータ */
  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILEDATA_NAME);
  p->filedata = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, FILEDATA_NAME);
  p->filedata_tmp = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_SIZE_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILEDATA_SIZE_NAME);
  p->filedata_size = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_SIZE_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, FILEDATA_SIZE_NAME);
  p->filedata_size_tmp = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_INDEX_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILEDATA_INDEX_NAME);
  p->filedata_index = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(FILEDATA_INDEX_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, FILEDATA_INDEX_NAME);
  p->filedata_index_tmp = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(KEYWORD_1BYTE_NAME) + 1);
  sprintf( tmp, "%s/%s", base, KEYWORD_1BYTE_NAME);
  p->key1byte = tmp;
  tmp = (char *) YAP_malloc(base_len + strlen(KEYWORD_1BYTE_NAME) + 5);
  sprintf( tmp, "%s/%s_tmp", base, KEYWORD_1BYTE_NAME);
  p->key1byte_tmp = tmp;

  /* 登録URL数 */
  tmp = (char *) YAP_malloc(base_len + strlen(FILENUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILENUM_NAME);
  p->filenum = tmp;

  /* 登録DOMAIN数 */
  tmp = (char *) YAP_malloc(base_len + strlen(DOMAINNUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, DOMAINNUM_NAME);
  p->domainnum = tmp;

  /* 登録キーワード数 */
  tmp = (char *) YAP_malloc(base_len + strlen(KEYWORDNUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, KEYWORDNUM_NAME);
  p->keywordnum = tmp;

  /* 各URLのサイズ */
  tmp = (char *) YAP_malloc(base_len + strlen(SIZE_NAME) + 1);
  sprintf( tmp, "%s/%s", base, SIZE_NAME);
  p->size = tmp;

  /* 各URLのDOMAIN ID */
  tmp = (char *) YAP_malloc(base_len + strlen(DOMAINID_NAME) + 1);
  sprintf( tmp, "%s/%s", base, DOMAINID_NAME);
  p->domainid = tmp;

  /* 各URLのスコア */
  tmp = (char *) YAP_malloc(base_len + strlen(SCORE_NAME) + 1);
  sprintf( tmp, "%s/%s", base, SCORE_NAME);
  p->score = tmp;

  /* 各URLのキーワード数 */
  tmp = (char *) YAP_malloc(base_len + strlen(FILEKEYWORDNUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, FILEKEYWORDNUM_NAME);
  p->filekeywordnum = tmp;

  /* URLの長さ */
  tmp = (char *) YAP_malloc(base_len + strlen(URLLEN_NAME) + 1);
  sprintf( tmp, "%s/%s", base, URLLEN_NAME);
  p->urllen = tmp;

  /* キーワードの総出現数 */
  tmp = (char *) YAP_malloc(base_len + strlen(KEYWORD_TOTALNUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, KEYWORD_TOTALNUM_NAME);
  p->keyword_totalnum = tmp;

  /* キーワードの総出現URL数 */
  tmp = (char *) YAP_malloc(base_len + strlen(KEYWORD_DOCSNUM_NAME) + 1);
  sprintf( tmp, "%s/%s", base, KEYWORD_DOCSNUM_NAME);
  p->keyword_docsnum = tmp;
}

/*
 *baseファイルが存在していたら、destファイルにコピーする
 *destが存在していたらなにもしない
 */
void _tmp_copy (char *base, char *dest)
{
 
  printf("%s/%s\n", base, dest);
  if (YAP_is_reg(dest)) {
    return;
  }

  if (YAP_is_reg(base)) {
    if (fork()) {
      int s;
      wait(&s);
    } else {
      execl("/bin/cp", "/bin/cp", base, dest, (char *) 0);
      exit(0);
    }
  }
}

/*
 *常時開いておくDBを開く
 */
void YAP_Db_base_open (YAPPO_DB_FILES *p)
{
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
  p->fileindex_db->open(p->fileindex_db, NULL, fileindex, NULL,
                    DB_BTREE, mode, 0);

  /* DOMAINとIDの対比表 */
  db_create(&(p->domainindex_db), NULL, 0);
  p->domainindex_db->open(p->domainindex_db, NULL, domainindex, NULL,
                    DB_BTREE, mode, 0);

  /* 削除URL */
  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみ時 */
    if (!YAP_is_reg(deletefile)) {
      p->deletefile_file = fopen(deletefile, "w");
      if (p->deletefile_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->deletefile_file);
    }
    p->deletefile_file = fopen(deletefile, "r+");
    if (p->deletefile_file == NULL) {
      YAP_Error("fopen error");
    }
  } else {
    /* 読み込み時 */
    p->deletefile_file = fopen(deletefile, "r");
    if (p->deletefile_file == NULL) {
      YAP_Error("fopen error");
    }
  }


  /* URLメタデータ */
  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみ時 */
    if (!YAP_is_reg(filedata)) {
      p->filedata_file = fopen(filedata, "w");
      if (p->filedata_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->filedata_file);
    }
    if (!YAP_is_reg(filedata_size)) {
      p->filedata_size_file = fopen(filedata_size, "w");
      if (p->filedata_size_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->filedata_size_file);
    }
    if (!YAP_is_reg(filedata_index)) {
      p->filedata_index_file = fopen(filedata_index, "w");
      if (p->filedata_index_file == NULL) {
        YAP_Error("fopen error");
      }
      fclose(p->filedata_index_file);
    }

    p->filedata_file = fopen(filedata, "r+");
    p->filedata_size_file = fopen(filedata_size, "r+");
    p->filedata_index_file = fopen(filedata_index, "r+");
    if (p->filedata_file == NULL || p->filedata_size_file == NULL || p->filedata_index_file == NULL) {
      YAP_Error("fopen error");
    }
  } else {
    /* 読み込み時 */
    p->filedata_file = fopen(filedata, "r");
    p->filedata_size_file = fopen(filedata_size, "r");
    p->filedata_index_file = fopen(filedata_index, "r");
    if (p->filedata_file == NULL || p->filedata_size_file == NULL || p->filedata_index_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* 辞書ファイル 1byte */
  db_create(&(p->key1byte_db), NULL, 0);
  p->key1byte_db->open(p->key1byte_db, NULL, key1byte, NULL,
                    DB_BTREE, mode, 0);



  /* 登録URL数 */
  if (!YAP_is_reg(p->filenum)) {
    /* 新規作成 */
    p->total_filenum = 0;
  } else {
    /* 読み込み */
    p->filenum_file = fopen(p->filenum, "r");
    if (p->filenum_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fread_exact(p->filenum_file, &(p->total_filenum), sizeof(int), 1) != 0) {
      YAP_Error("fread error");
    }
    fclose(p->filenum_file);
  }

  /* 登録DOMAIN数 */
  if (!YAP_is_reg(p->domainnum)) {
    /* 新規作成 */
    p->total_domainnum = 0;
  } else {
    /* 読み込み */
    p->domainnum_file = fopen(p->domainnum, "r");
    if (p->domainnum_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fread_exact(p->domainnum_file, &(p->total_domainnum), sizeof(int), 1) != 0) {
      YAP_Error("fread error");
    }
    fclose(p->domainnum_file);
  }

  /* 登録キーワード数 */
  if (!YAP_is_reg(p->keywordnum)) {
    /* 新規作成 */
    p->total_keywordnum = 0;
  } else {
    /* 読み込み */
    p->keywordnum_file = fopen(p->keywordnum, "r");
    if (p->keywordnum_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fread_exact(p->keywordnum_file, &(p->total_keywordnum), sizeof(int), 1) != 0) {
      YAP_Error("fread error");
    }
    fclose(p->keywordnum_file);
  }

  printf("url=%d:key=%d\n", p->total_filenum, p->total_keywordnum);


  /* 各URLのサイズ */
  if (!YAP_is_reg(p->size)) {
    /* 新規作成 */
    p->size_file = fopen(p->size, "w");
    if (p->size_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->size_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->size_file = fopen(p->size, "r+");
    if (p->size_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fseek_set(p->size_file, sizeof(int) * p->total_filenum) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    p->size_file = fopen(p->size, "r");
    if (p->size_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* 各URLのDOMAIN ID */
  if (!YAP_is_reg(p->domainid)) {
    /* 新規作成 */
    p->domainid_file = fopen(p->domainid, "w");
    if (p->domainid_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->domainid_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->domainid_file = fopen(p->domainid, "r+");
    if (p->domainid_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fseek_set(p->domainid_file, sizeof(int) * p->total_domainnum) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    p->domainid_file = fopen(p->domainid, "r");
    if (p->domainid_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* 各URLのスコア */
  if (!YAP_is_reg(p->score)) {
    /* 新規作成 */
    p->score_file = fopen(p->score, "w");
    if (p->score_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->score_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->score_file = fopen(p->score, "r+");
    if (p->score_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fseek_set(p->score_file, sizeof(double) * p->total_filenum) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    p->score_file = fopen(p->score, "r");
    if (p->score_file == NULL) {
      YAP_Error("fopen error");
    }
  }


  /* 各URLのキーワード数 */
  if (!YAP_is_reg(p->filekeywordnum)) {
    /* 新規作成 */
    p->filekeywordnum_file = fopen(p->filekeywordnum, "w");
    if (p->filekeywordnum_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->filekeywordnum_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->filekeywordnum_file = fopen(p->filekeywordnum, "r+");
    if (p->filekeywordnum_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fseek_set(p->filekeywordnum_file, sizeof(int) * p->total_filenum) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    p->filekeywordnum_file = fopen(p->filekeywordnum, "r");
    if (p->filekeywordnum_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* URLの長さ */
  if (!YAP_is_reg(p->urllen)) {
    /* 新規作成 */
    p->urllen_file = fopen(p->urllen, "w");
    if (p->urllen_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->urllen_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->urllen_file = fopen(p->urllen, "r+");
    if (p->urllen_file == NULL) {
      YAP_Error("fopen error");
    }
    if (YAP_fseek_set(p->urllen_file, sizeof(int) * p->total_filenum) != 0) {
      YAP_Error("fseek error");
    }
  } else {
    p->urllen_file = fopen(p->urllen, "r");
    if (p->urllen_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* キーワードの総出現数 */
  if (!YAP_is_reg(p->keyword_totalnum)) {
    /* 新規作成 */
    p->keyword_totalnum_file = fopen(p->keyword_totalnum, "w");
    if (p->keyword_totalnum_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->keyword_totalnum_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->keyword_totalnum_file = fopen(p->keyword_totalnum, "r+");
    if (p->keyword_totalnum_file == NULL) {
      YAP_Error("fopen error");
    }
  } else {
    p->keyword_totalnum_file = fopen(p->keyword_totalnum, "r");
    if (p->keyword_totalnum_file == NULL) {
      YAP_Error("fopen error");
    }
  }

  /* キーワードの総出現URL数 */
  if (!YAP_is_reg(p->keyword_docsnum)) {
    /* 新規作成 */
    p->keyword_docsnum_file = fopen(p->keyword_docsnum, "w");
    if (p->keyword_docsnum_file == NULL) {
      YAP_Error("fopen error");
    }
    fclose(p->keyword_docsnum_file);
  }
  if (p->mode == YAPPO_DB_WRITE) {
    p->keyword_docsnum_file = fopen(p->keyword_docsnum, "r+");
    if (p->keyword_docsnum_file == NULL) {
      YAP_Error("fopen error");
    }
  } else {
    p->keyword_docsnum_file = fopen(p->keyword_docsnum, "r");
    if (p->keyword_docsnum_file == NULL) {
      YAP_Error("fopen error");
    }
  }
}


/*
 *常時開いておくDBを閉じる
 */
void YAP_Db_base_close (YAPPO_DB_FILES *p)
{

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
    dir_path = (char *) YAP_malloc(strlen(p->base_dir) + 1);
    sprintf(dir_path, "%s", p->base_dir);
    pos_dir = opendir(dir_path);
    while ((direntp = readdir(pos_dir)) != NULL) {
      char *name_tmp = direntp->d_name;
      int len = strlen(name_tmp);
      char *name = YAP_malloc(strlen(dir_path) + len + 2);
      sprintf(name, "%s/%s", dir_path, name_tmp);
      len = strlen(name);

      printf("name: %s\n", name);
      if (name[len-4] == '_' && name[len-3] == 't' && name[len-2] == 'm' && name[len-1] == 'p') {
	char *new_name = (char *) YAP_malloc(len + 1);
	strcpy(new_name, name);
	new_name[len-4] = 0;
	printf("/bin/mv %s %s\n", name, new_name);
	if (fork()) {
	  int s;
	  wait(&s);
	} else {
	  execl("/bin/mv", "/bin/mv", name, new_name, (char *) 0);
	  exit(0);
	}
	free(new_name);
      }
    }
    closedir(pos_dir);
    free(dir_path);

    /* 位置情報ファイルも元にもどす */
    dir_path = (char *) YAP_malloc(strlen(p->base_dir) + 5);
    sprintf(dir_path, "%s/pos", p->base_dir);
    pos_dir = opendir(dir_path);
    while ((direntp = readdir(pos_dir)) != NULL) {
      char *name_tmp = direntp->d_name;
      int len = strlen(name_tmp);
      char *name = YAP_malloc(strlen(dir_path) + len + 2);
      sprintf(name, "%s/%s", dir_path, name_tmp);
      len = strlen(name);

      printf("name: %s\n", name);
      if (name[len-4] == '_' && name[len-3] == 't' && name[len-2] == 'm' && name[len-1] == 'p') {
	char *new_name = (char *) YAP_malloc(len + 1);
	strcpy(new_name, name);
	new_name[len-4] = 0;
	new_name = (char *) YAP_realloc(new_name, strlen(new_name) + 1);
	printf("/bin/mv %s %s\n", name, new_name);
	if (fork()) {
	  int s;
	  wait(&s);
	} else {
	  execl("/bin/mv", "/bin/mv", name, new_name, (char *) 0);
	  exit(0);
	}
	free(new_name);
      }
    }
    closedir(pos_dir);
    free(dir_path);
  }

  /* 登録URL数 */
  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみモード */
    p->filenum_file = fopen(p->filenum, "w"); 
    if (p->filenum_file == NULL) {
      YAP_Error("fopen error");
    } else {
      if (YAP_fwrite_exact(p->filenum_file, &(p->total_filenum), sizeof(int), 1) != 0) {
        YAP_Error("fwrite error");
      }
      fclose(p->filenum_file);
    }
  }
  free(p->filenum);
  p->filenum = NULL;

  /* 登録DOMAIN数 */
  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみモード */
    p->domainnum_file = fopen(p->domainnum, "w"); 
    if (p->domainnum_file == NULL) {
      YAP_Error("fopen error");
    } else {
      if (YAP_fwrite_exact(p->domainnum_file, &(p->total_domainnum), sizeof(int), 1) != 0) {
        YAP_Error("fwrite error");
      }
      fclose(p->domainnum_file);
    }
  }
  free(p->domainnum);
  p->domainnum = NULL;

  /* 登録キーワード数 */
  if (p->mode == YAPPO_DB_WRITE) {
    /* 書きこみモード */
    p->keywordnum_file = fopen(p->keywordnum, "w");
    if (p->keywordnum_file == NULL) {
      YAP_Error("fopen error");
    } else {
      if (YAP_fwrite_exact(p->keywordnum_file, &(p->total_keywordnum), sizeof(int), 1) != 0) {
        YAP_Error("fwrite error");
      }
      fclose(p->keywordnum_file);
    }
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
void YAP_Db_linklist_open (YAPPO_DB_FILES *p)
{
  char *base = p->base_dir;
  int base_len = strlen(base);
  int ret;

  p->linklist = (char *) YAP_malloc(base_len + strlen(LINKLIST_NAME) + 2);
  sprintf(p->linklist, "%s/%s", base, LINKLIST_NAME);
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

  p->linklist_size = (char *) YAP_malloc(base_len + strlen(LINKLIST_SIZE_NAME) + 2);
  sprintf(p->linklist_size, "%s/%s", base, LINKLIST_SIZE_NAME);
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

  p->linklist_index = (char *) YAP_malloc(base_len + strlen(LINKLIST_INDEX_NAME) + 2);
  sprintf(p->linklist_index, "%s/%s", base, LINKLIST_INDEX_NAME);
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
void YAP_Db_linklist_close (YAPPO_DB_FILES *p)
{
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
int YAP_Db_pos_open (YAPPO_DB_FILES *p, int pos_id)
{
  char *tmp;
  char *base = p->base_dir;
  int base_len = strlen(base) + 2;
  char *pos, *pos_size, *pos_index;
  int ret;

  /* ファイル名作成 */
  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_NAME) + 1);
  sprintf( tmp, "%s/"POSTINGS_NAME, base, pos_id);

  if (p->mode == YAPPO_DB_READ) {
    /*
     *読み込みモードなら
     *開く前にファイルが存在しているか調べる
     */
    if (!YAP_is_reg(tmp)) {
      /* 存在しない */
      free(tmp);
      return 0;
    }
  }

  p->pos = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_NAME) + 5);
  sprintf( tmp, "%s/"POSTINGS_NAME"_tmp", base, pos_id);
  p->pos_tmp = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_SIZE_NAME) + 1);
  sprintf( tmp, "%s/"POSTINGS_SIZE_NAME, base, pos_id);
  p->pos_size = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_SIZE_NAME) + 5);
  sprintf( tmp, "%s/"POSTINGS_SIZE_NAME"_tmp", base, pos_id);
  p->pos_size_tmp = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_INDEX_NAME) + 1);
  sprintf( tmp, "%s/"POSTINGS_INDEX_NAME, base, pos_id);
  p->pos_index = tmp;

  tmp = (char *) YAP_malloc(base_len + strlen(POSTINGS_INDEX_NAME) + 5);
  sprintf( tmp, "%s/"POSTINGS_INDEX_NAME"_tmp", base, pos_id);
  p->pos_index_tmp = tmp;


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
void YAP_Db_pos_close (YAPPO_DB_FILES *p)
{


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
void YAP_Db_cache_init (YAPPO_CACHE *p) 
{
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
void YAP_Db_cache_destroy (YAPPO_CACHE *p) 
{
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
void YAP_Db_cache_load (YAPPO_DB_FILES *ydfp, YAPPO_CACHE *p) 
{

  if (ydfp->total_filenum != p->total_filenum ||
      ydfp->total_domainnum != p->total_domainnum ||
      ydfp->total_keywordnum != p->total_keywordnum) {
    /*
     *キャッシュ上の数値と実際の数値が食い違っている
     *ので読みこみ開始する
     */

    printf("CACHE LOAD\n");

    /* スコアファイルキャッシュ */
    pthread_mutex_lock(&(p->score_mutex));
    p->score = (double *) YAP_realloc(p->score, sizeof(double) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->score_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->score_file, p->score, sizeof(double), ydfp->total_filenum) != 0) {
      p->score_num = 0;
    } else {
      p->score_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->score_mutex));

    /* ファイルサイズキャッシュ */
    pthread_mutex_lock(&(p->size_mutex));
    p->size = (int *) YAP_realloc(p->size, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->size_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->size_file, p->size, sizeof(int), ydfp->total_filenum) != 0) {
      p->size_num = 0;
    } else {
      p->size_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->size_mutex));

    /* URL文字数ファイルキャッシュ */
    pthread_mutex_lock(&(p->urllen_mutex));
    p->urllen = (int *) YAP_realloc(p->urllen, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->urllen_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->urllen_file, p->urllen, sizeof(int), ydfp->total_filenum) != 0) {
      p->urllen_num = 0;
    } else {
      p->urllen_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->urllen_mutex));


    /* 各URLのキーワード数ファイルキャッシュ */
    pthread_mutex_lock(&(p->filekeywordnum_mutex));
    p->filekeywordnum = (unsigned int *) YAP_realloc(p->filekeywordnum, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->filekeywordnum_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->filekeywordnum_file, p->filekeywordnum, sizeof(int), ydfp->total_filenum) != 0) {
      p->filekeywordnum_num = 0;
    } else {
      p->filekeywordnum_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->filekeywordnum_mutex));
    

    /* domain idファイルキャッシュ */
    pthread_mutex_lock(&(p->domainid_mutex));
    p->domainid = (int *) YAP_realloc(p->domainid, sizeof(int) * ydfp->total_filenum);
    if (YAP_fseek_set(ydfp->domainid_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->domainid_file, p->domainid, sizeof(int), ydfp->total_filenum) != 0) {
      p->domainid_num = 0;
    } else {
      p->domainid_num = ydfp->total_filenum;
    }
    pthread_mutex_unlock(&(p->domainid_mutex));

    /* 削除URLファイルキャッシュ */
    pthread_mutex_lock(&(p->domainid_mutex));
    p->deletefile = (unsigned char *) YAP_realloc(p->deletefile, (ydfp->total_filenum / 8) + 1);
    if (YAP_fseek_set(ydfp->deletefile_file, 0L) != 0 ||
        YAP_fread_exact(ydfp->deletefile_file, p->deletefile, 1, (ydfp->total_filenum / 8) + 1) != 0) {
      p->deletefile_num = 0;
    } else {
      p->deletefile_num = (ydfp->total_filenum / 8) + 1;
    }
    pthread_mutex_unlock(&(p->domainid_mutex));

    printf("load delete: %d/%d\n", p->deletefile_num, (ydfp->total_filenum / 8) + 1);

    p->total_filenum = ydfp->total_filenum;
    p->total_domainnum = ydfp->total_domainnum;
    p->total_keywordnum = ydfp->total_keywordnum;
  }

}
