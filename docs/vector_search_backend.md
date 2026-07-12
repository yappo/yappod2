# Vector search backend

Task 8 では v2 vector 契約の参照実装として exact flat backend を追加しました。各 entry の float 配列を全件走査し、指定 metric で score を計算して top-k を返します。ANN/HNSW などの高速実装は同じ API の後段差し替え対象です。

## Metric

- `cosine`: cosine similarity。ゼロノルムは score `0` とします。
- `dot`: 内積。
- `l2`: 負の二乗距離（大きい score が近い結果）です。

`YAP_Vector_search` は query と全 entry の次元一致、有限値、空 ID を検証します。結果は score 降順、同点時は entry ordinal 昇順で決定的です。hit の ID view と score は入力 entry/呼び出し側バッファを所有せず、入力ベクトルの寿命中だけ有効です。

```c
YAP_VECTOR_HIT hits[10];
size_t hit_count;
YAP_Vector_search(entries, entry_count, query, dimensions, YAP_V2_VECTOR_COSINE,
                  10, hits, 10, &hit_count);
```

この flat backend は正確性・再現性を担保する基準実装です。大規模 index では後続タスクの ANN インデックスを導入し、metric・dimension・model ID を manifest/config と照合してから検索します。
