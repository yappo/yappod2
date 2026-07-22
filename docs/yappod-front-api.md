# `yappod_front` HTTP APIリファレンス

`yappod_front`は外部クライアント向けのHTTP APIを提供し、検索や更新を`yappod_core`へ転送します。通常の
クライアントは、内部プロトコルではなくこのAPIを利用します。

## 共通仕様

- HTTP/1.0とHTTP/1.1を受理し、1回のレスポンスごとに接続を閉じます。
- POSTには`Content-Type: application/json`と、0より大きい`Content-Length`が必要です。
- `Transfer-Encoding`は受理しません。リクエスト本文の上限は1 MiBです。
- リクエスト行の上限は8192バイト、リクエストヘッダー全体の上限は65536バイトです。
- レスポンスには`Cache-Control: no-store`が付きます。
- レスポンスの`Server`は`Yappo Search/2.0`、HTTPの版は`HTTP/1.1`、`Connection`は`close`です。
- エラーは`{"error":{"code":"...","message":"..."}}`形式です。
- 検索と読み出しに認証はありません。文書の一括更新には、設定に応じてBearer認証が必要です。

| HTTP状態コード | 主な意味 |
|---:|---|
| 400 | JSON、フィールド、ベクトル、カーソル、一括更新の内容が不正です。 |
| 401 | 更新用Bearerトークンがないか、一致しません。 |
| 404 | パスが存在しません。 |
| 405 | パスは存在しますが、HTTPメソッドが異なります。 |
| 409 | 更新処理中にマニフェストの世代が変わりました。 |
| 413 | リクエスト本文が1 MiBを超えています。 |
| 415 | `Content-Type`が`application/json`ではありません。 |
| 500 | サーバー内部で処理を完了できませんでした。 |
| 503 | core、索引、同時処理枠のいずれかを利用できません。 |

`[daemon].max_inflight`と`max_inflight_bytes`を超えたリクエストは、`503`と`overloaded`エラーになります。
frontとcoreの間の処理を待つ時間は`request_timeout_ms`です。

| `error.code` | HTTP状態コード | 意味 |
|---|---:|---|
| `invalid_request` | 400 | HTTPまたはJSONの形式、検索条件、カーソルが不正です。 |
| `unauthorized` | 401 | 更新用Bearerトークンを確認できません。 |
| `not_found` | 404 | 指定したパスがありません。 |
| `method_not_allowed` | 405 | エンドポイントに対するHTTPメソッドが異なります。 |
| `body_too_large` | 413 | リクエスト本文が上限を超えています。 |
| `unsupported_media_type` | 415 | JSONとして受理できない`Content-Type`です。 |
| `invalid_batch` | 400 | 文書の一括更新を不可分な単位として検証できません。 |
| `generation_conflict` | 409 | 更新中に索引の世代が変わりました。 |
| `overloaded` | 503 | 同時処理数または本文バイト数の上限に達しました。 |
| `core_unavailable` | 503 | frontからcoreへの処理を完了できません。 |
| `prepare_unavailable` | 503 | 本文断片分割を実行できません。 |
| `search_unavailable` | 503 | 検証済みの検索用スナップショットを利用できません。 |
| `update_unavailable` | 503 | 索引の更新を完了できません。 |
| `reload_failed` | 503 | 更新は公開されましたが、新しいスナップショットを読み込めません。 |
| `internal_error` | 500 | front内部で処理を完了できません。 |

| エンドポイント | HTTPメソッド | 認証 |
|---|---|---|
| `/v2/search` | POST | 不要です。 |
| `/v2/retrieve` | POST | 不要です。 |
| `/v2/passages:prepare` | POST | 不要です。 |
| `/v2/documents:batch` | POST | `daemon.write_token`を設定した場合にBearer認証が必要です。 |
| `/health/live` | GET | 不要です。 |
| `/health/ready` | GET | 不要です。 |
| `/metrics` | GET | 不要です。 |

## 検索条件

`POST /v2/search`と`POST /v2/retrieve`は、次の検索条件を共通して受け付けます。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `query` | 文字列 | 空でないUTF-8文字列 | なし | `mode`が`lexical`または`hybrid`の場合は必須 | 語彙検索に使う検索文です。リクエスト本文全体には1048576バイトの上限があります。 |
| `vector` | 浮動小数点数の配列 | 要素数は索引の`dimensions`と同数。各要素はfloat32で表現できる有限値 | なし | `mode`が`vector`または`hybrid`の場合は必須 | ベクトル検索に使う検索ベクトルです。 |
| `mode` | 文字列 | `lexical`、`vector`、`hybrid` | `hybrid` | 任意 | 使用する検索方式を指定します。 |
| `filter` | フィルターオブジェクト | 最大32階層、全体で最大1024ノード | なし | 任意 | `[metadata].filterable_fields`に登録したメタデータを使って候補を絞り込みます。 |
| `operator` | 文字列 | `or`、`and` | `or` | 任意 | 複数の検索語のいずれかへの一致またはすべてへの一致を選びます。 |
| `phrase` | 真偽値 | `true`、`false` | `false` | 任意 | `true`では単語位置を使ったフレーズ一致を要求します。 |
| `limit` | 整数 | 1〜100 | `20` | 任意 | 返す検索結果または採用する本文断片の最大件数です。 |

検索モードに不要な`query`や`vector`を同時に送ることはできますが、必要な値を省略すると
`400 invalid_request`になります。Yappod2は検索文から埋め込みを生成しません。`vector`と`hybrid`では、呼び出し側が
索引の`model_id`、`dimensions`、`metric`に合う検索ベクトルを用意します。

### メタデータのフィルター

フィルターは最大32階層、全体で最大1024ノードです。フィールド名は空にできず、値には文字列、数値、真偽値、
または`null`を指定できます。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `eq` | 等値条件オブジェクト | `field`と`value`を1つずつ持つオブジェクト。`value`は文字列、数値、真偽値、`null` | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | `field`の値が`value`と一致する文書を選びます。 |
| `in` | 候補条件オブジェクト | `field`と、1件以上のスカラー値を持つ`values`配列 | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | `values`のいずれかと一致する文書を選びます。 |
| `range` | 範囲条件オブジェクト | `field`と、数値の`gt`、`gte`、`lt`、`lte`のうち1つ以上 | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | 指定したすべての数値境界を満たす文書を選びます。 |
| `exists` | 存在条件オブジェクト | 空でない文字列の`field` | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | `field`が存在する文書を選びます。値が`null`でも存在として扱います。 |
| `not` | フィルターオブジェクト | 1つのフィルターノード | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | 入れ子にした条件を反転します。 |
| `and` | フィルターオブジェクトの配列 | 1件以上 | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | 配列内のすべての条件を満たす文書を選びます。 |
| `or` | フィルターオブジェクトの配列 | 1件以上 | なし | フィルターノードでは、表内の条件キーのいずれか1つが必須 | 配列内のいずれかの条件を満たす文書を選びます。 |

## `POST /v2/search`

文書または本文断片を検索します。`scope`は`documents`または`passages`で、デフォルトは`documents`です。
`cursor`を指定すると、同じ検索条件の続きを取得できます。

```json
{
  "query": "apple",
  "vector": [0.1, 0.2],
  "mode": "hybrid",
  "scope": "documents",
  "filter": {
    "eq": {
      "field": "category",
      "value": "fruit"
    }
  },
  "operator": "or",
  "phrase": false,
  "limit": 20
}
```

共通の検索条件に加えて、`POST /v2/search`では次のキーを指定できます。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `scope` | 文字列 | `documents`、`passages` | `documents` | 任意 | 検索結果を文書単位または本文断片単位のどちらで返すかを指定します。 |
| `cursor` | 文字列 | 直前の同じ検索条件に対して返された`next_cursor` | なし | 任意 | 続きの検索結果を取得します。値を組み立てたり、検索条件を変更して再利用したりしないでください。 |

成功時は`200`で、索引の版、総件数、検索結果、次ページのカーソルを返します。

```json
{
  "api_version": 2,
  "generation": 7,
  "total": 1,
  "results": [
    {
      "id": "doc-1",
      "document_id": "doc-1",
      "title": "Apple guide",
      "url": "https://example.test/apple",
      "snippet": "An apple is an edible fruit...",
      "lexical_score": 1.25,
      "vector_score": 0.91,
      "fused_score": 0.032
    }
  ],
  "next_cursor": null
}
```

| レスポンスのフィールド | 説明 |
|---|---|
| `api_version` | APIの版です。現在は`2`です。 |
| `generation` | 検索に使った索引の世代です。 |
| `total` | このレスポンスの`results`に入った件数です。索引全体の一致総数ではありません。 |
| `results` | 検索結果オブジェクトの配列です。各オブジェクトの内容は次の表で説明します。 |
| `next_cursor` | 続きがある場合のカーソルです。ない場合は`null`です。 |

`results`の各オブジェクトには次のフィールドが入ります。

| フィールド | 説明 |
|---|---|
| `id` | `scope = "documents"`では文書ID、`scope = "passages"`では本文断片IDです。 |
| `document_id` | 元の文書IDです。文書検索では`id`と同じ値、本文断片検索では本文断片の親文書IDです。 |
| `title` | 元文書の題名です。入力で省略した場合は空文字列です。 |
| `url` | 元文書のURLです。入力で省略した場合は空文字列です。 |
| `snippet` | 検索語付近を結果一覧へ表示するために短くした本文です。文書検索では文書本文、本文断片検索ではその本文断片から作ります。HTMLは含みません。 |
| `lexical_score` | BM25Fによる語彙検索のスコアです。語彙検索を使わない場合、または語彙検索側の候補にない場合は0です。 |
| `vector_score` | 索引で設定した`cosine`、`dot`、`l2`に基づくベクトル検索のスコアです。ベクトル検索を使わない場合、またはベクトル検索側の候補にない場合は0です。 |
| `fused_score` | 最終的な並び順に使うスコアです。複合検索では語彙検索とベクトル検索の順位から計算します。 |

`snippet`は原文の文字列なので、Web画面へ表示するときはHTMLの特殊文字をエスケープしてください。元本文を引用する用途では、
`POST /v2/retrieve`が返す`citations[].text`を使用します。

カーソルは世代、検索条件、読み出し位置をSHA-256で結び付けます。別の検索条件や世代では使えません。
改変を検出するための値であり、認証トークンではありません。カーソルの読み出し位置は最大10000件です。1ページを
取得する際も、読み出し位置、`limit`、続きの有無を調べる1件を合わせた値が内部上限を超えると`400`になります。

## `POST /v2/retrieve`

RAGへ渡す本文断片と出典情報を取得します。回答生成は行いません。このAPIは通常の検索結果一覧ではなく、質問と関係する
本文をLLMへ渡すためのものなので、検索結果を本文断片単位で選び、`context`と出典情報へまとめます。

```json
{
  "query": "apple",
  "vector": [0.1, 0.2],
  "mode": "hybrid",
  "limit": 10,
  "max_passages_per_document": 2,
  "max_context_bytes": 32768
}
```

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `query` | 文字列 | 空でないUTF-8文字列 | なし | `mode`が`lexical`または`hybrid`の場合は必須 | 検索する質問または語句です。リクエスト本文全体には1048576バイトの上限があります。 |
| `vector` | 浮動小数点数の配列 | 要素数は索引の`dimensions`と同数。各要素はfloat32で表現できる有限値 | なし | `mode`が`vector`または`hybrid`の場合は必須 | ベクトル検索に使います。 |
| `mode` | 文字列 | `lexical`、`vector`、`hybrid` | `hybrid` | 任意 | 本文断片を探す検索方式です。 |
| `filter` | フィルターオブジェクト | 最大32階層、全体で最大1024ノード | なし | 任意 | 登録済みのメタデータを使って検索対象を絞ります。 |
| `operator` | 文字列 | `or`、`and` | `or` | 任意 | 複数の検索語のいずれかへの一致またはすべてへの一致を選びます。 |
| `phrase` | 真偽値 | `true`、`false` | `false` | 任意 | `true`では検索語が同じ順序で連続する本文断片だけを候補にします。 |
| `limit` | 整数 | 1〜100 | `20` | 任意 | 採用する本文断片数の上限です。 |
| `max_passages_per_document` | 整数 | 1〜`limit` | `3` | 任意 | 1文書から採用する本文断片数の上限です。 |
| `max_context_bytes` | 整数 | 1〜1048576 | `16384` | 任意 | `context`へ連結する本文のUTF-8バイト数上限です。 |

本文を途中で切って上限へ合わせることはありません。次の本文断片が上限を超える場合は、その本文断片を採用しません。

```json
{
  "api_version": 2,
  "generation": 7,
  "context": "An apple is an edible fruit...",
  "citations": [
    {
      "passage_id": "p-0123456789abcdef",
      "document_id": "doc-1",
      "url": "https://example.test/apple",
      "title": "Apple guide",
      "text": "An apple is an edible fruit...",
      "start_char": 0,
      "end_char": 30,
      "context_start": 0,
      "context_end": 30,
      "lexical_score": 1.25,
      "vector_score": 0.91,
      "fused_score": 0.032
    }
  ]
}
```

| 応答フィールド | 説明 |
|---|---|
| `api_version` | APIの版です。現在は2です。 |
| `generation` | 取得に使った索引の世代です。 |
| `context` | 選んだ本文断片を空行で区切って連結したUTF-8文字列です。LLMへ渡す参照資料として使えます。 |
| `citations` | `context`へ採用した本文断片と出典の配列です。検索順位と同じ順番です。 |
| `citations[].passage_id` | 本文断片IDです。 |
| `citations[].document_id` | 元文書のIDです。 |
| `citations[].url`、`citations[].title` | 元文書のURLと題名です。 |
| `citations[].text` | 本文断片の全文です。 |
| `citations[].start_char`、`citations[].end_char` | 元文書の本文内で、この本文断片が占める文字位置です。 |
| `citations[].context_start`、`citations[].context_end` | 連結後の`context`内で、この本文断片が占めるUTF-8バイト位置です。 |
| `citations[].lexical_score`、`vector_score`、`fused_score` | 検索結果と同じ三種類のスコアです。 |

## `POST /v2/passages:prepare`

`/v2/passages:prepare`はコロンを含む文字列全体が実際のURLパスです。`passages`というリソースに対して、登録前の
準備処理`prepare`を実行することを表しています。コロンを別の値へ置き換える必要はありません。

登録前の文書を、接続中の索引と同じトークン分割・チャンク設定で本文断片へ分割します。索引は更新せず、
埋め込みAPIも呼びません。

```json
{
  "id": "doc-1",
  "body": "索引へ登録する本文です。"
}
```

リクエストは`id`と`body`の二つで構成します。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `id` | 文字列 | 1〜255バイト | なし | 必須 | 本文断片IDを生成するときに使う文書IDです。索引は更新しません。 |
| `body` | 文字列 | 1〜1048576バイト | なし | 必須 | 接続中の索引と同じ設定で本文断片へ分割する本文です。 |

成功時は`200`で、次の形式を返します。

```json
{
  "model_id": "example-model-v1",
  "dimensions": 2,
  "passages": [
    {
      "id": "p-624da837772a06f3",
      "ordinal": 0,
      "start_char": 0,
      "end_char": 12,
      "text": "索引へ登録する本文です。"
    }
  ]
}
```

`model_id`と`dimensions`は、接続中の索引がベクトルに対応している場合の設定です。`passages`は分割した本文の配列で、
`ordinal`は同じ文書の中で何番目の本文断片かを表す番号です。最初の本文断片は`0`、次は`1`です。`start_char`と`end_char`は元本文内の位置です。ベクトル対応索引へ登録する
クライアントは、配列の順番を変えずに各`text`の埋め込みを生成し、同じ順番で`vectors`へ入れます。

## `POST /v2/documents:batch`

1〜100件の`upsert`または`delete`を、1つの世代として不可分に公開します。

```json
{
  "operations": [
    {
      "operation": "upsert",
      "id": "doc-1",
      "title": "Title",
      "body": "Body",
      "metadata": {
        "lang": "ja"
      },
      "vectors": [
        [0.1, 0.2]
      ]
    },
    {
      "operation": "delete",
      "id": "doc-2"
    }
  ]
}
```

同じ一括更新内の文書IDは重複できません。`upsert`では`id`と`body`が必須です。ベクトル対応索引では、分割後の
本文断片数と`vectors`の件数を一致させ、各ベクトルを索引の`dimensions`に合わせます。1件でも入力形式、
本文断片数、ベクトルに問題があれば、全件を`400 invalid_batch`として拒否します。

トップレベルのリクエストは次のキーを持ちます。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `operations` | 更新操作オブジェクトの配列 | 1〜100件。同じ文書IDは重複不可 | なし | 必須 | 配列内の操作をすべて検証し、すべて正常な場合だけ一度に公開します。 |

配列内の更新操作オブジェクトは次のキーを持ちます。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `operation` | 文字列 | `upsert`、`delete` | なし | 必須 | 登録・置換または削除を選びます。 |
| `id` | 文字列 | 1〜255バイト | なし | 必須 | 更新または削除する文書IDです。 |
| `url` | 文字列 | 0〜8192バイト | 空文字列 | `upsert`では任意。`delete`では指定不可 | 元文書のURLです。 |
| `title` | 文字列 | 0〜255バイト | 空文字列 | `upsert`では任意。`delete`では指定不可 | 題名です。 |
| `body` | 文字列 | 0〜1048576バイト | なし | `upsert`では必須。`delete`では指定不可 | 語彙索引では空文字列も形式上受理されますが、本文断片を生成できません。 |
| `metadata` | JSONオブジェクト | 正規化後0〜1048576バイト | `{}` | `upsert`では任意。`delete`では指定不可 | 絞り込みには索引作成時の`filterable_fields`が必要です。 |
| `updated_at_unix_ms` | 整数 | 0以上 | `0` | `upsert`では任意。`delete`では指定不可 | Unix epochからのミリ秒です。値の意味は呼び出し側で管理します。 |
| `vectors` | 浮動小数点数の二次元配列 | 行数は生成される本文断片数、各行の要素数は索引の`dimensions`と同数、各要素はfloat32で表現できる有限値 | なし | ベクトル対応索引の`upsert`では必須。`delete`では指定不可 | 本文断片と同じ順番でベクトルを並べます。 |

リクエストのトップレベルは`operations`配列です。配列内の形式は索引作成用NDJSONと同じであり、詳細な文字列制約は
[索引作成](indexing.md#正式な文書ndjson)を参照してください。

```json
{
  "generation": 8,
  "accepted": 2,
  "upserts": 1,
  "deletes": 1,
  "segment_ids": [
    "seg-00000000000000000008-AbCd12"
  ]
}
```

更新開始後に別の更新で世代が変わった場合は`409 generation_conflict`となり、公開しません。
`[daemon].write_token`を設定した場合は、次のヘッダーが必要です。

```text
Authorization: Bearer <write_token>
```

## `GET /health/live`

frontのプロセスがHTTPリクエストを処理できる場合に、`200`と
`{"status":"live","service":"yappod_front"}`を返します。この検査はcoreへの接続や
現在の索引の利用可否を確認しません。検索を受け付けられる状態かどうかは`/health/ready`で確認します。

## `GET /health/ready`

coreへ接続でき、検証済みの索引を利用できる場合に、`200`、`ready: true`、世代、セグメント数、埋め込みと
コンパクションの状態を返します。利用できない場合は`503`と`ready: false`を返します。`status`は`ready`または
`not_ready`、`service`は`yappod_front`です。

```json
{
  "status": "ready",
  "service": "yappod_front",
  "ready": true,
  "generation": 8,
  "segments": 2,
  "embedding": {
    "state": "precomputed_ready",
    "model_id": "example-model-v1",
    "dimensions": 768
  },
  "compaction": {
    "state": "idle",
    "generation": 0,
    "updated_at_unix": 0
  }
}
```

語彙索引では`embedding.state`が`disabled`、`model_id`が空文字列、`dimensions`が0です。`compaction.state`は
`idle`、`running`、`succeeded`、`failed`、`interrupted`、`unknown`のいずれかです。

## `GET /metrics`

Prometheusのテキスト形式で、リクエスト数、処理時間、処理中の件数とバイト数、マニフェストの世代、準備状態、
埋め込み状態、コンパクション状態を返します。全メトリクス名、ラベル、バケット、収集例は
[監視とメトリクス](observability.md)を参照してください。

## APIを公開するときの注意

front自身はTLS、利用者認証、検索APIの流量制限を提供しません。`write_token`は文書更新だけを保護します。
ループバックアドレス以外で待ち受ける場合は、TLS、接続元制限、必要な認証、本文と要求数の制限を行うリバースプロキシーの背後へ
配置してください。`/metrics`には秘密情報は含めませんが、索引の世代、処理量、上限を公開するため、監視サーバー
だけが取得できるようにします。
