# Hybrid reranking

Lexical BM25 と vector similarity はスコアの尺度が異なるため、Task 9 の統合は Reciprocal Rank Fusion（RRF）を使用します。各入力リストの順位 `rank` に対し、候補の fused score は次で計算します。

```text
lexical_weight / (60 + lexical_rank)
+ vector_weight / (60 + vector_rank)
```

片方のリストにしかない候補は存在する側の寄与だけを持ち、両方に出た候補は寄与を加算します。結果は fused score 降順、同点時は最初に見つかった入力順で deterministic です。

`YAP_Hybrid_fuse_rrf` は入力リストを順位順（index 0 が rank 1）として扱い、ID view をコピーしません。weights は非負で、少なくとも一方を正にします。RRF は candidate generation と再ランキングを分離するため、後続の ANN や cross-encoder を候補側へ追加しても融合契約を保てます。
