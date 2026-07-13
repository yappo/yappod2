# Vector search backend

現在の v2 vector 契約には、参照実装として exact flat backend があります。各 entry の float 配列を全件走査し、指定 metric で score を計算して top-k を返します。embedding入力は[HTTP batch providerとprecomputed reader](embedding_provider.md)で生成でき、`vectors.yap2` readerがmapped passage vectorをこのbackendへ接続します。このモジュール単体では、snapshot横断の検索 runtime接続とANN/HNSWは提供しません。

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

この flat backend は正確性・再現性を担保する基準実装です。製品として vector 検索を利用可能にするには、[現代検索基盤の完成契約](modern_search_completion_contract.md) に従って永続 vector、embedding provider、HNSW、manifest/config 検証、検索 API へ接続する必要があります。
