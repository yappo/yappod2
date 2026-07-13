# v2 search and retrieval HTTP API

`yappod_front` は `POST /v2/search` と `POST /v2/retrieve` を受け、Task 17 の
length-prefixed frameで `yappod_core` へ転送します。coreはリクエストごとに
`config.toml` と `manifest.json`、全component checksumを検証し、一つのgenerationの
snapshotと同じsegment順のlexical、vector/ANN、metadata readerを開いて
`YAP_V2_query_execute` を実行します。retrieveは同じsnapshotのpassage結果を
`YAP_V2_retrieve_context` へ渡します。旧Berkeley DB検索結果のJSON変換ではありません。

両endpointは `Content-Type: application/json` と明示的な `Content-Length` を必須とし、
chunked request、重複したlength/type、1 MiBを超えるbody、不完全bodyを拒否します。
検索`limit`は1から100です。JSONの未知field、型不一致、非有限vector、設定dimensionと
異なるvector、modeに必要なquery/vectorの欠落もfail-closedで `400` を返します。
media type不一致は`415`、body上限超過は`413`、検証済みsnapshotを開けない場合は`503`です。

## Search request

```json
{"query":"apple","vector":[0.1,0.2],"mode":"hybrid","scope":"documents","filter":{"eq":{"field":"category","value":"fruit"}},"operator":"or","phrase":false,"limit":20}
```

`mode`は`lexical`、`vector`、`hybrid`、`scope`は`documents`、`passages`です。
lexicalは`query`、vectorは`vector`、hybridは両方を必須とします。`filter`はindexの
filterable metadata fieldだけを参照できます。レスポンスはsnapshot `generation` と、
各hitのID、親document ID、lexical/vector/fused scoreを返します。

検索responseの`next_cursor`は、次候補がある場合だけ文字列になります。cursorは
`v1.<generation>.<offset>.<sha256>`形式で、SHA-256 digestがsnapshot generation、defaults適用・
固定field順・filter key-sort済みのcanonical query、offsetを結び付けます。次pageでは元requestと
同じ`limit`を含む検索条件に`cursor`を追加します。別query、別generation、改変されたoffset/digest、
上限外offsetは`400 invalid_request`です。これは衝突耐性digestによるbindingであり、秘密鍵を使う
認証tokenではありません。

`/v2/search`と`/v2/retrieve`へのPOST以外のmethodはlegacy parserへ渡さず、JSONの`405`を返します。

## Retrieval request

```json
{"query":"apple","vector":[0.1,0.2],"mode":"hybrid","limit":10,"max_passages_per_document":2,"max_context_bytes":32768}
```

retrieveのscopeはpassageに固定されます。レスポンスは連結済み`context`とcitation配列を
返します。citationにはpassage/document ID、URL、title、本文、元文書のUnicode文字offset、
context内byte offset、lexical/vector/fused scoreが含まれます。回答生成は行いません。

現時点ではvector queryは呼び出し側が事前計算値を渡します。HTTP embedding providerを
query textから呼ぶためのprovider URL/API keyはindex config schemaに存在しないため、
暗黙の外部通信は行いません。
