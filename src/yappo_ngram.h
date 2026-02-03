/*
 *N-gramに関する処理
 */
#ifndef YAPPO_NGRAM_H
#define YAPPO_NGRAM_H

/* N-gramで切り出す文字数（UTF-8コードポイント単位） */
#define Ngram_N 2

/*
 *出現キーワードと、出現位置を収めるリスト
 */
typedef struct ngram_list {
  unsigned char *keyword;
  int index_count;
  int *index;
  struct ngram_list *next;
} NGRAM_LIST;

/*
 *出現キーワードと出現位置を格納する
 */
typedef struct {
  unsigned char *keyword; /* キーワード */
  int pos;                /* 出現位置 */
} NGRAM_SEARCH_LIST;

NGRAM_SEARCH_LIST *YAP_Ngram_tokenize_search(char *body, int *keyword_num);
NGRAM_LIST *YAP_Ngram_tokenize(char *body, int *keyword_num);
void YAP_Ngram_List_free(NGRAM_LIST *list);

/* TODO: 将来的にトークナイザを差し替え可能にする */
unsigned char *YAP_Ngram_get_1byte(unsigned char *tokp);
unsigned char *YAP_Ngram_get_2byte(unsigned char *tokp);

#endif
