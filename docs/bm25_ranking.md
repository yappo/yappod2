# BM25語彙ランキング

> **実装状態:** BM25F scorer は v2 terms/postings、複数 immutable segment、metadata filter、
> block-max WAND top-k に接続されています。CLI と HTTP の lexical/hybrid mode が同じ scorer を使います。

v1の辞書・postingを使う語彙検索では、各検索語のpostingを読み込んだ時点でBM25スコアを計算します。検索結果のAND/OR/phraseマージと削除文書の除外は既存の処理を引き続き使います。

## スコア

各文書の項目は次のとおりです。

- `tf`: 文書内のposting出現数
- `df`: その語を含む文書数
- `N`: index全体の文書数
- `dl`: 文書の総token数（`filekeywordnum`）
- `avgdl`: index内の有効な文書の平均token数

```text
idf = ln(1 + (N - df + 0.5) / (df + 0.5))
norm = (1 - b) + b * dl / avgdl
tf_component = tf * (k1 + 1) / (tf + k1 * norm)
score = idf * tf_component * boost
```

既定値は `k1=1.2`、`b=0.75` です。`boost` は既存の `score` ファイルの値を `ln(boost)+1` に変換した値で、既存の外部スコアを検索ランキングへ引き続き反映します。NaN、無限大、0以下の入力は安全な既定値へフォールバックします。

## 欠損値と境界

- `avgdl` が計算できない場合は1として扱います。
- `dl=0` は1として扱います。
- `df=0`、`df>N`、`tf=0`、`N=0` は0点です。
- 計算結果が有限でない場合は0点です。

実装は [src/yappo_bm25.c](../src/yappo_bm25.c)、単体テストは `tests/bm25_score_test.c` にあります。
