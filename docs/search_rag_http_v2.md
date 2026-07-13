# v2 search and retrieval HTTP API

`yappod_front` は `POST /v2/search`、`POST /v2/retrieve`、
`POST /v2/passages:prepare`、`POST /v2/documents:batch` を受けます。search、retrieve、batchは内部の length-prefixed v2 frame で
`yappod_core` へ転送します。coreはリクエストごとに
`config.toml` と `manifest.json`、全component checksumを検証し、一つのgenerationの
snapshotと同じsegment順のlexical、vector/ANN、metadata readerを開いて
`YAP_V2_query_execute` を実行します。retrieveは同じsnapshotのpassage結果を
`YAP_V2_retrieve_context` へ渡します。

4 endpointは `Content-Type: application/json` と明示的な `Content-Length` を必須とし、
chunked request、重複したlength/type、1 MiBを超えるbody、不完全bodyを拒否します。
検索`limit`は1から100です。JSONの未知field、型不一致、非有限vector、設定dimensionと
異なるvector、modeに必要なquery/vectorの欠落もfail-closedで `400` を返します。
media type不一致は`415`、body上限超過は`413`、検証済みsnapshotを開けない場合は`503`です。
daemonのdeadline、in-flight request/byte上限、`/v2/documents:batch`のBearer tokenは
[runtime resource limits and write authentication](runtime_limits_security.md)を参照してください。

## Search request

```json
{"query":"apple","vector":[0.1,0.2],"mode":"hybrid","scope":"documents","filter":{"eq":{"field":"category","value":"fruit"}},"operator":"or","phrase":false,"limit":20}
```

`mode`は`lexical`、`vector`、`hybrid`、`scope`は`documents`、`passages`です。
lexicalは`query`、vectorは`vector`、hybridは両方を必須とします。`filter`はindexの
filterable metadata fieldだけを参照できます。レスポンスはsnapshot `generation` と、
各hitのID、親document ID、title、URL、plain-text snippet、lexical/vector/fused scoreを返します。

```json
{
  "api_version": 2,
  "generation": 7,
  "total": 1,
  "results": [{
    "id": "doc-1",
    "document_id": "doc-1",
    "title": "Apple guide",
    "url": "https://example.test/apple",
    "snippet": "An apple is an edible fruit...",
    "lexical_score": 1.25,
    "vector_score": 0.91,
    "fused_score": 0.032
  }],
  "next_cursor": null
}
```

document scopeのsnippetはdocument body、passage scopeでは該当passageから最大180 graphemeの
windowを返します。query文字列が本文中のgrapheme境界で一致する場合はその周辺を優先し、
vector-only検索は先頭から取得します。snippetはHTML highlight markerを追加しないJSON stringです。
クライアントはHTMLとして解釈せずtextとして描画してください。

検索responseの`next_cursor`は、次候補がある場合だけ文字列になります。cursorは
`v1.<generation>.<offset>.<sha256>`形式で、SHA-256 digestがsnapshot generation、defaults適用・
固定field順・filter key-sort済みのcanonical query、offsetを結び付けます。次pageでは元requestと
同じ`limit`を含む検索条件に`cursor`を追加します。別query、別generation、改変されたoffset/digest、
上限外offsetは`400 invalid_request`です。これは衝突耐性digestによるbindingであり、秘密鍵を使う
認証tokenではありません。

4 endpointへのPOST以外のmethodはJSONの`405`を返します。旧GET検索APIは存在しません。

## Retrieval request

```json
{"query":"apple","vector":[0.1,0.2],"mode":"hybrid","limit":10,"max_passages_per_document":2,"max_context_bytes":32768}
```

retrieveのscopeはpassageに固定されます。レスポンスは連結済み`context`とcitation配列を
返します。citationにはpassage/document ID、URL、title、本文、元文書のUnicode文字offset、
context内byte offset、lexical/vector/fused scoreが含まれます。回答生成は行いません。

vector queryは呼び出し側が事前計算値を渡します。これは公開 API の確定仕様です。
index 作成側はベンダー中立な HTTP embedding provider または precomputed vector を利用できますが、
検索 API は query text を外部 provider へ暗黙送信しません。

## Passage preparation

```json
{"id":"doc-1","body":"Indexへ登録する本文"}
```

`POST /v2/passages:prepare`は、接続中indexのtokenizer・chunk設定を使って本文を正規化・分割し、
各passageのID、ordinal、元本文内のUnicode文字offset、embedding対象textを返すread-only endpointです。
外部embedding providerは呼び出さず、indexも更新しません。

```json
{
  "model_id": "embeddinggemma-300m-768-local-v1",
  "dimensions": 768,
  "passages": [{
    "id": "passage-id",
    "ordinal": 0,
    "start_char": 0,
    "end_char": 14,
    "text": "indexへ登録する本文"
  }]
}
```

vector indexへ文書を追加するclientは、この順序のtextをembeddingし、得られた二次元配列を同じ文書の
`vectors`へ設定します。これによりclient側でUnicode chunkingを再実装せず、batch ingestが要求する
passage数とordinalを一致させられます。

## Atomic document batch

```json
{"operations":[
  {"operation":"upsert","id":"doc-1","url":"https://example.test/1","title":"Title","body":"Body","metadata":{"lang":"ja"},"vectors":[[0.1,0.2]]},
  {"operation":"delete","id":"doc-2"}
]}
```

`operations`は1件以上100件以下です。未知key、重複document ID、canonical ingest違反、
chunk後passage数とvector数の不一致、dimension違反が1件でもあれば`400 invalid_batch`で
batch全体を拒否します。成功時は`generation`、accepted/upsert/delete件数、segment IDを返します。

coreは全操作を検証してからdocuments、lexical postings/positions、metadata、vectors、HNSW、
tombstoneを一つのimmutable delta segmentへ書きます。全component完成後にgeneration CASで
manifestをatomic publishするため、失敗時にpartial batchは検索されません。検索runtimeは
各request開始時に最新manifestの全checksumを検証してsnapshotを開くため、成功応答後の次requestは
新generationを読みます。writer競合は`409 generation_conflict`、index I/O/検証不能は`503`です。
