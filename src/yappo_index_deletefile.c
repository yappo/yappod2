/*
 *削除URLの処理
 */

#include "yappo_db.h"
#include "yappo_io.h"
#include "yappo_index_deletefile.h"

/*
 *
 */
int YAP_Index_Deletefile_get(YAPPO_DB_FILES *ydfp, int fileindex)
{
  int seek, bit;
  unsigned char c = 0;

  if ((unsigned int) fileindex > ydfp->total_filenum) {
    /*対象となるIDは存在していない*/
    return -1;
  }

  seek = fileindex / 8;
  bit  = fileindex % 8;

  if (YAP_fseek_set(ydfp->deletefile_file, seek) != 0) {
    return -1;
  }
  if (YAP_fread_exact(ydfp->deletefile_file, &c, 1, 1) != 0) {
    return -1;
  }

  if (c & (1 << bit)) {
    /*削除済*/
    return 0;
  } else {
    /*未削除*/
    return -1;
  }
}

/*
 *
 */
int YAP_Index_Deletefile_put(YAPPO_DB_FILES *ydfp, int fileindex)
{
  int seek, bit;
  unsigned char c = 0;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  seek = fileindex / 8;
  bit  = fileindex % 8;

  if (YAP_fseek_set(ydfp->deletefile_file, seek) != 0) {
    return -1;
  }
  if (YAP_fread_exact(ydfp->deletefile_file, &c, 1, 1) != 0) {
    return -1;
  }

  if (c & (1 << bit)) {
    /*削除済*/
    return -1;
  } else {
    /*未削除*/
    c |= (1 << bit);
    /*fseek(ydfp->deletefile_file, -1L, SEEK_CUR);*/
    if (YAP_fseek_set(ydfp->deletefile_file, seek) != 0) {
      return -1;
    }
    if (YAP_fwrite_exact(ydfp->deletefile_file, &c, 1, 1) != 0) {
      return -1;
    }
    return 0;
  }
}

/*
 *
 */
int YAP_Index_Deletefile_del(YAPPO_DB_FILES *ydfp, int fileindex)
{
  int seek, bit;
  unsigned char c = 0;

  if (ydfp->mode == YAPPO_DB_READ) {
    /*読みこみモードではエラー*/
    return -1;
  }

  seek = fileindex / 8;
  bit  = fileindex % 8;

  if (YAP_fseek_set(ydfp->deletefile_file, seek) != 0) {
    return -1;
  }
  if (YAP_fread_exact(ydfp->deletefile_file, &c, 1, 1) != 0) {
    return -1;
  }

  if (c & (1 << bit)) {
    /*削除済*/
    c -= (1 << bit);
    if (fseek(ydfp->deletefile_file, -1L, SEEK_CUR) != 0) {
      return -1;
    }
    if (YAP_fwrite_exact(ydfp->deletefile_file, &c, 1, 1) != 0) {
      return -1;
    }
    return 0;
  } else {
    /*未削除*/
    return -1;
  }
}
