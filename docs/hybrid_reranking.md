# Hybrid reranking

Lexical BM25 と vector similarity はスコアの尺度が異なるため、現在の融合契約は Reciprocal Rank Fusion（RRF）を使用します。各入力リストの順位 `rank` に対し、候補の fused score は次で計算します。

```text
lexical_weight / (60 + lexical_rank)
+ vector_weight / (60 + vector_rank)
```

片方のリストにしかない候補は存在する側の寄与だけを持ち、両方に出た候補は寄与を加算します。結果は fused score 降順、同点時は最初に見つかった入力順で deterministic です。

`YAP_Hybrid_fuse_rrf` は入力リストを順位順（index 0 が rank 1）として扱い、ID view をコピーしません。weights は非負で、少なくとも一方を正にします。このモジュールは候補生成を行わないため、単体では hybrid 検索を提供しません。[現代検索基盤の完成契約](modern_search_completion_contract.md) に従って lexical/vector candidate、filter、document/passage 集約、検索 API へ接続します。
