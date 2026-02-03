/*
 *削除URLの処理
 */
#ifndef YAPPO_INDEX_DELETEFILE_H
#define YAPPO_INDEX_DELETEFILE_H

#define DELETEFILE_NAME "deletefile"

int YAP_Index_Deletefile_get(YAPPO_DB_FILES *ydfp, int fileindex);
int YAP_Index_Deletefile_put(YAPPO_DB_FILES *ydfp, int fileindex);
int YAP_Index_Deletefile_del(YAPPO_DB_FILES *ydfp, int fileindex);

#endif
