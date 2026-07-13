# Vector search backend

現在の v2 vector 契約には、正解集合を作る exact flat backend と、USearch HNSW backend があります。embedding入力は[HTTP batch providerとprecomputed reader](embedding_provider.md)で生成でき、`vectors.yap2` readerがmapped passage vectorを両backendへ接続します。HNSWはpassage ordinalをkeyにして`vectors.usearch`をatomic saveし、検索時にはread-only viewします。

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

ANN候補は`vectors.yap2`の元vectorで再採点するため、公開scoreはexact backendと同じcosine、dot、負のsquared-L2です。ANN handleがないsegmentはexact scanへfallbackします。複数segmentは各segmentからtop-k候補を取得して同じscoreでmergeし、同点はsegment ordinal、passage ordinal順です。重複IDとtombstoneのlatest-wins解決はsnapshot層の責務です。

`vectors.usearch`のvector件数・dimensionが`vectors.yap2`と一致しない場合はsegmentを開きません。HNSWの標準構築値はconnectivity 16、expansion-add 128、expansion-search 64を推奨し、recall評価に応じて検索時のexpansionを上げます。製品のquery embedding、filter、snapshot、API接続は[現代検索基盤の完成契約](modern_search_completion_contract.md)の後続taskで扱います。
