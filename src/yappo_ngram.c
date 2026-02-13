/*
 *N-gramに関する処理
 *
 *入力可能な文字コードはUTF-8のみ
 */
#include "yappo_ngram.h"
#include <ctype.h>
#include <string.h>
#include <wctype.h>
#include "yappo_alloc.h"

/* UTF-8 1文字のバイト長を返す（不正時は1） */
static int YAP_Utf8_char_len(unsigned char c) {
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    return 3;
  }
  if ((c & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

/* 不完全なUTF-8シーケンスでは、残りバイト数までに丸める */
static int YAP_Utf8_char_len_safe(const unsigned char *p) {
  size_t remain;
  int len;

  if (p == NULL || *p == '\0') {
    return 0;
  }

  remain = strlen((const char *)p);
  len = YAP_Utf8_char_len(*p);
  if ((size_t)len > remain) {
    return (int)remain;
  }
  return len;
}

/*
 *出現位置リストの初期化
 */
NGRAM_LIST *__YAP_Ngram_List_init(void) {
  NGRAM_LIST *p = (NGRAM_LIST *)YAP_malloc(sizeof(NGRAM_LIST));
  p->keyword = NULL;
  p->index_count = 0;
  p->index = NULL;
  p->next = NULL;
  return p;
}

/*
 *keywordが一致するレコードをかえす
 */
NGRAM_LIST *__YAP_Ngram_List_search(NGRAM_LIST *list, unsigned char *keyword) {
  NGRAM_LIST *next;

  if (list == NULL) {
    return NULL;
  }
  next = list->next;
  /*
   *一致するレコードが見つかるか、全てのレコードを探索するまで続ける
   */
  while (1) {
    if (next == NULL) {
      return NULL;
    }
    if (!strcmp((const char *)next->keyword, (const char *)keyword)) {
      /* 一致 */
      return next;
    }
    next = next->next;
  }
  return NULL;
}

/*
 *リストに追加
 *すでにkeywordが存在している場合は、indexのリストに追加する
 */
void __YAP_Ngram_List_add(NGRAM_LIST *list, unsigned char *keyword, int index) {
  NGRAM_LIST *p, *add, *next;
  add = __YAP_Ngram_List_search(list, keyword);

  if (add == NULL) {
    /* 新規 */
    p = __YAP_Ngram_List_init();
    p->keyword = (unsigned char *)YAP_malloc(strlen((const char *)keyword) + 1);
    memcpy(p->keyword, keyword, strlen((const char *)keyword) + 1);
    p->index = (int *)YAP_malloc(sizeof(int));
    p->index[p->index_count] = index;
    p->index_count++;

    /* 最後のレコードを検索して、追加する */
    next = list;
    while (1) {
      if (next->next == NULL) {
        break;
      }
      next = next->next;
    }
    next->next = p;
  } else {
    /* 追加 */
    add->index_count++;
    add->index = (int *)YAP_realloc(add->index, (sizeof(int) * (add->index_count)));
    add->index[add->index_count - 1] = index;
  }
}

/*
 *指定リスト以降に継れているリストの消去
 */
void YAP_Ngram_List_free(NGRAM_LIST *list) {
  NGRAM_LIST *this, *next;

  next = list;
  while (1) {
    if (next == NULL) {
      return;
    }
    this = next;
    next = this->next;
    free(this->keyword);
    this->keyword = NULL;
    free(this->index);
    this->index = NULL;
    this->index_count = 0;

    free(this);
    this = NULL;
  }
  return;
}

/*
 *N-gramで文字列を分解
 */
NGRAM_LIST *YAP_Ngram_tokenize(char *body, int *keyword_num) {
  unsigned char *tokp, *tokp_next, *gram;
  int pos = 0;
  NGRAM_LIST *ngram_item_list = __YAP_Ngram_List_init();

  tokp = (unsigned char *)body;

  while (*tokp) {
    int pos_before = pos;
    gram = NULL;

    if (*tokp <= 0x20) {
      /* エスケープ文字は無視する */
      tokp++;
      pos++;
      continue;
    } else if (*tokp < 0x80) {
      /* 1byte文字 */

      if (isalnum(*tokp)) {
        /*
   *アルファベットか数字
   *
   *1byte文字のN-gram取得
   */
        gram = YAP_Ngram_get_1byte(tokp);
        tokp_next = tokp + strlen((const char *)gram);
        pos += (int)strlen((const char *)gram);
      } else {
        /* 記号ならスキップ */
        tokp_next = tokp + 1;
        pos++;
      }
    } else {
      /* UTF-8 非ASCII */
      int step = YAP_Utf8_char_len_safe(tokp);
      gram = YAP_Ngram_get_2byte(tokp);
      tokp_next = tokp + step;
      pos++;
    }

    if (gram != NULL) {
      /* ngramリストに追加 */
      __YAP_Ngram_List_add(ngram_item_list, gram, pos_before);
      (*keyword_num)++;
      free(gram);
    }
    tokp = tokp_next;
  }

  return ngram_item_list;
}

/*
 *1byte文字の場合は、1byte文字(記号を除く)が続く限り切り出す。
 */
unsigned char *YAP_Ngram_get_1byte(unsigned char *tokp) {
  unsigned char *ret, *p, *a, *b;
  p = tokp;

  while (*p) {
    if (*p > 0x20 && *p < 0x80 && isalnum(*p)) {
      p++;
    } else {
      break;
    }
  }

  /* 小文字に変換しつつコピー */
  ret = (unsigned char *)YAP_malloc((p - tokp) + 1);
  a = ret;
  b = tokp;
  while (b < p) {
    *a = (unsigned char)tolower(*b);
    a++;
    b++;
  }
  return ret;
}

/*
 *UTF-8のN-gramを切り出す（非ASCIIは2文字=バイグラム）
 */
unsigned char *YAP_Ngram_get_2byte(unsigned char *tokp) {
  unsigned char *ret;
  int len1, len2, total;
  size_t remain;

  if (tokp == NULL || *tokp == '\0') {
    return NULL;
  }
  remain = strlen((const char *)tokp);

  len1 = YAP_Utf8_char_len(*tokp);
  if ((size_t)len1 >= remain) {
    return NULL;
  }

  len2 = YAP_Utf8_char_len(*(tokp + len1));
  total = len1 + len2;
  if ((size_t)total > remain) {
    return NULL;
  }

  ret = (unsigned char *)YAP_malloc((size_t)total + 1);
  memcpy(ret, tokp, (size_t)total);
  ret[total] = '\0';

  return ret;
}

/*
 *N-gramで文字列を分解
 *キーワード検索用のN-gramリストを作成
 */
NGRAM_SEARCH_LIST *YAP_Ngram_tokenize_search(char *body, int *keyword_num) {
  unsigned char *tokp, *tokp_next, *gram;
  NGRAM_SEARCH_LIST *list = NULL;
  int pos = 0;

  tokp = (unsigned char *)body;

  while (*tokp) {
    int pos_before = pos;
    gram = NULL;

    if (*tokp <= 0x20) {
      /* エスケープ文字は無視する */
      tokp++;
      pos++;
      continue;
    } else if (*tokp < 0x80) {
      /* 1byte文字 */

      if (isalnum(*tokp)) {
        /*
   *アルファベットか数字
   *
   *1byte文字のN-gram取得
   */
        gram = YAP_Ngram_get_1byte(tokp);

        tokp_next = tokp + strlen((const char *)gram);
        pos += (int)strlen((const char *)gram);
      } else {
        /* 記号ならスキップ */
        tokp_next = tokp + 1;
        pos++;
      }
    } else {
      /* UTF-8 非ASCII */
      int step = YAP_Utf8_char_len_safe(tokp);
      gram = YAP_Ngram_get_2byte(tokp);
      tokp_next = tokp + step;
      pos++;
    }

    if (gram != NULL) {
      /* リストに追加 */
      list = (NGRAM_SEARCH_LIST *)YAP_realloc(list, sizeof(NGRAM_SEARCH_LIST) * (*keyword_num + 1));

      list[*keyword_num].keyword = (unsigned char *)YAP_malloc(strlen((const char *)gram) + 1);
      memcpy(list[*keyword_num].keyword, gram, strlen((const char *)gram) + 1);
      list[*keyword_num].pos = pos_before;
      (*keyword_num)++;
      free(gram);
    }
    tokp = tokp_next;
  }

  return list;
}
