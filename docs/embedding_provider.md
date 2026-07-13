# v2 embedding provider

embedding 層は、passage/query の embedding 入力を同じ `YAP_EMBEDDING_RESULT` に変換する二つの
providerを実装しています。出力は入力順のrow-major `float` 配列で、呼び出し側は
`YAP_Embedding_result_init` 後にproviderを呼び、最後に `YAP_Embedding_result_free` します。

## HTTP batch provider

`YAP_Embedding_http` はベンダー中立な OpenAI-compatible JSON を使用します。

```json
{"model":"embed-v1","input":["first passage","second passage"]}
```

responseの `data[].index` を入力順へ戻し、各 `embedding` が設定dimensionと一致し、全値が
finite `float` 範囲内であることを検証します。indexの欠落・重複、未知key、JSON不正、件数・
dimension不一致はfail-closedです。

呼び出しは `batch_size` ごとに分割します。transport失敗、HTTP 408/429、5xxだけを
`max_retries` 回まで指数backoffで再試行します。その他の4xxと、成功responseのschema違反は
再試行しません。timeoutはconnectとrequest全体の両方へ適用します。API keyはJSONやerrorへ
含めず、Bearer headerだけに設定します。

testではtransportとsleep callbackを差し替えられます。productionで未指定の場合はlibcurlと
monotonicな待機を使用します。

## Precomputed NDJSON provider

`YAP_Embedding_precomputed_read` は次の1行1vector形式を読みます。

```json
{"id":"passage-1","embedding":[0.1,0.2,0.3]}
```

file順ではなくIDで入力へ結合します。duplicate ID、入力IDの欠落、未知key、dimension違反、
非有限値、空行、壊れたJSONは拒否します。HTTP providerと同じく、成功時だけ結果を公開します。
