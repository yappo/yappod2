/*
 *リンクリストの操作を行なう
 */
#ifndef YAPPO_LINKLIST_H
#define YAPPO_LINKLIST_H

#include "yappo_search.h"

/* リンクリスト本体 */
#define LINKLIST_NAME "linklist"
/* 各URL毎のリストサイズ */
#define LINKLIST_SIZE_NAME "linklist_size"
/* 各URL毎のlinklistファイル中の位置 */
#define LINKLIST_INDEX_NAME "linklist_index"

void YAP_Linklist_Score(YAPPO_DB_FILES *p, SEARCH_RESULT *result);

#endif
