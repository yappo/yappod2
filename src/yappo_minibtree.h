/*
 *
 *小規模なbtree
 *追加時のバランスなどは行なわれない
 *
 */
#ifndef YAPPO_MINIBTREE_H
#define YAPPO_MINIBTREE_H

typedef struct yap_minibtree {
  unsigned char *key; /*キーワード*/
  unsigned long id;   /*一意なID*/
  void *data;         /*データ*/
  struct yap_minibtree *left;
  struct yap_minibtree *right;
  struct yap_minibtree *top;
} MINIBTREE;

MINIBTREE *YAP_Minibtree_init(void);
MINIBTREE *YAP_Minibtree_search(MINIBTREE *root, unsigned char *key);
int YAP_Minibtree_add(MINIBTREE *root, MINIBTREE *add);

#endif
