# Yappod2検索Web UI

search-webは、Yappod2の索引をブラウザーから検索し、RAG向けの参照資料を取得し、任意で回答生成と文書登録を行うサンプルです。
Node.jsとFastifyで作られたsearch-webサーバーが、ブラウザーへ画面を配信し、ブラウザーから受け取った要求を
`yappod_front`、埋め込みAPI、LLM APIへ送ります。

## 構成

```text
ブラウザー
   │ HTTP /api/*
   ▼
search-webサーバー ──────→ 埋め込みAPI（ベクトル検索と登録時だけ）
   │                  └→ LLM Chat Completions（回答生成時だけ）
   │ HTTP /v2/*
   ▼
yappod_front ──専用のTCP通信──→ yappod_core ──→ 索引
```

検索そのものと索引更新はYappod2が実行します。検索文と登録文書の埋め込み、RAG回答の生成はsearch-webサーバーが外部サービスへ依頼します。`yappod_core`と`yappod_front`はLLMへ接続しません。

この文書では、次の二つを区別します。

- **search-webサーバー**は、`examples/search-web/server`にあるNode.jsプログラムです。ブラウザー向けの`/api/*`を提供し、実際の検索や外部API呼び出しを仲介します。
- **search-webの起動スクリプト**は、`examples/search-web/scripts/stack.mjs`です。索引の確認、core、front、search-webサーバー、任意の模擬APIの起動と停止、PIDとログの管理を行います。検索リクエスト自体は処理しません。

利用者が通常実行するのは起動スクリプトです。起動後、ブラウザーはsearch-webサーバーへ接続し、search-webサーバーが
`yappod_front`へ接続します。

## 必要な環境

- リポジトリのルートでビルドした`yappod_core`と`yappod_front`が必要です。
- Node.js 22以上とnpmが必要です。
- 有効なv2索引、または索引を作るための`[build].input`が必要です。

リポジトリのルートで準備します。

```sh
cmake --build build -j
cd examples/search-web
npm ci
cd ../..
cp examples/search-web/config.example.toml /tmp/yappod-search-web.toml
```

実際には`/tmp`の例ではなく、検索対象に合う設定を保存してください。local-filesとWikipediaは、それぞれのアプリケーション用TOMLをそのまま利用できます。

## 起動に必要な設定

search-webは1つのアプリケーションTOMLをYappod2サーバー、索引作成、search-webサーバー、起動スクリプトで共有します。相対パスはTOMLがあるディレクトリを基準に解決します。

設定表は「キー」「データ型」「入力可能値」「デフォルト値」「必須」「説明」の順です。「デフォルト値」が「なし」のキーは、省略時に値が補われません。

### `[build]`

既存索引だけを使う場合は省略できます。`[index].directory`に索引がなく、起動スクリプトに索引を作らせる場合は両方を指定します。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `yappo_makeindex` | 文字列 | 実行可能な`yappo_makeindex`のパス | なし | 索引を自動作成する場合は必須 | 相対パスはTOMLを基準にします。 |
| `input` | 文字列 | 読み取り可能な正式文書NDJSON | なし | 索引を自動作成する場合は必須 | `yappo_makeindex build`へ渡します。ベクトル対応索引では`vectors`を含む必要があります。 |

### `[index]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `directory` | 文字列 | 既存索引、または自動作成する新しい索引のディレクトリ。TOMLからの相対パスを解決した後で4095バイト以下 | なし | 必須 | 既存索引には`config.toml`と`manifest.json`が必要です。 |

`[vector]`、`[tokenizer]`、`[chunking]`、`[metadata]`は索引構造を表します。search-webサーバーの機能を有効にするだけの設定ではありません。索引直下の`config.toml`と一致しない構成へ変える場合は、索引を別ディレクトリへ作り直します。

### `[daemon]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `run_directory` | 文字列 | 空でないパス。TOMLからの相対パスを解決した後で4095バイト以下 | search-webでは`./run`。Yappod2サーバーではなし | Yappod2サーバーを起動する場合は必須 | core、front、Web、模擬のPIDとログを置きます。 |
| `core_host` | 文字列 | 1〜255バイトのホスト名またはIPアドレス | search-webでは`127.0.0.1`。Yappod2サーバーではなし | Yappod2サーバーを起動する場合は必須 | frontが接続し、coreが待ち受けるホストです。 |
| `core_port` | 整数 | 1〜65535 | search-webでは`18401`。Yappod2サーバーではなし | Yappod2サーバーを起動する場合は必須 | frontからcoreへ検索や更新を依頼する内部HTTP/1.1ポートです。外部クライアントには公開しません。 |
| `front_host` | 文字列 | 1〜255バイトのホスト名またはIPアドレス | search-webでは`127.0.0.1`。Yappod2サーバーではなし | Yappod2サーバーを起動する場合は必須 | frontの待ち受け先であり、search-webサーバーの接続先です。 |
| `front_port` | 整数 | 1〜65535 | search-webでは`18400`。Yappod2サーバーではなし | Yappod2サーバーを起動する場合は必須 | frontのHTTPポートです。 |
| `max_inflight` | 整数 | 1〜1024 | `4` | 任意 | frontとcoreが、それぞれ処理中として受理するリクエストの件数上限です。 |
| `max_inflight_bytes` | 整数 | 1〜1073741824 | `4194304` | 任意 | frontは処理中のHTTP本文、coreは処理中の検索・更新データについて、それぞれの合計バイト数を制限します。 |
| `request_timeout_ms` | 整数 | 1〜60000 | `5000` | 任意 | frontのlibcurlによるcoreへの接続と内部HTTP要求全体、およびcoreが受理したソケットの送受信期限です。 |
| `write_token` | 文字列 | 16〜255バイト。空白文字と制御文字は不可 | なし | 任意 | 文書登録APIをBearer認証します。 |

### `[web]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `host` | 文字列 | 空でないホスト名またはIPアドレス | `127.0.0.1` | 任意 | search-webサーバーと静的画面の待ち受け先です。 |
| `port` | 整数 | 1〜65535 | `4173` | 任意 | ブラウザーから接続するポートです。 |
| `yappod_timeout_ms` | 整数 | 1〜600000 | `5000` | 任意 | search-webサーバーが1回のfront応答を待つ時間です。 |
| `startup_timeout_ms` | 整数 | 100〜600000 | `8000` | 任意 | 起動スクリプトが模擬LLMとWebのヘルスチェックを待つ時間です。core/frontの初期待機は、これとは別の短い繰り返し確認です。 |

### `[embedding]`

ベクトル検索、複合検索、ベクトル対応索引への文書登録に必要です。語彙検索だけならセクション全体を省略します。空の`[embedding]`は無効化ではなく設定エラーです。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `provider` | 文字列 | `lmstudio`、`ollama`、`openai` | なし | `[embedding]`を使用する場合は必須 | 要求本文と応答の形式を選びます。 |
| `base_url` | 文字列 | クエリー文字列とフラグメントを含まないHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | `endpoint_url`を指定しない場合は必須 | プロバイダー別の埋め込みパスを追加する基準URLです。 |
| `endpoint_url` | 文字列 | 埋め込みAPI全体を表すHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | `base_url`を指定しない場合は必須 | 独自のパスを使う接続先で指定します。`base_url`とは併用できません。 |
| `model` | 文字列 | 空でない文字列 | なし | `[embedding]`を使用する場合は必須 | 接続先へ送る実際のモデル名です。 |
| `model_id` | 文字列 | 1〜255バイト | なし | 任意 | 索引の`[vector].model_id`と比較するYappod2側の識別子です。索引との取り違えを検出するため指定を推奨します。 |
| `dimensions` | 整数 | 1〜65536 | `768` | 任意 | 応答ベクトルの要素数です。索引と一致させます。 |
| `prompt_profile` | 文字列 | `plain`、`embeddinggemma` | `plain` | 任意 | `embeddinggemma`では検索文と文書用の指示を付けます。 |
| `timeout_ms` | 整数 | 1000〜600000 | `60000` | 任意 | 埋め込みAPIへのHTTP要求1回の期限です。 |
| `batch_size` | 整数 | 1〜1024 | `16` | 任意 | 1回に送る入力数です。 |
| `authorization_token_env` | 文字列 | `[A-Za-z_][A-Za-z0-9_]*`に一致する環境変数名 | なし | 任意 | Bearerトークンを環境変数から読みます。トークン自体はTOMLへ書きません。 |

### `[llm]`

RAGの回答生成を使う場合だけ追加します。回答生成を使わない場合は、空のセクションを残さず見出しごと省略します。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `base_url` | 文字列 | クエリー文字列とフラグメントを含まないHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | `[llm]`を使用する場合は必須 | OpenAI互換APIの基準URLです。search-webサーバーが`chat/completions`を加えます。 |
| `model` | 文字列 | 空でない文字列 | なし | `[llm]`を使用する場合は必須 | 接続先へ送る実際のモデル名です。索引の`model_id`とは別です。 |
| `effort` | 文字列 | 接続先が`reasoning_effort`として受理する空でない文字列 | なし | 任意 | 使用できる値は接続先で確認します。 |
| `max_tokens` | 整数 | 1〜131072 | `8192` | 任意 | 接続先へ毎回送る最大出力トークン数です。入力トークン数ではありません。 |
| `timeout_ms` | 整数 | 1000〜600000 | `30000` | 任意 | 回答生成のHTTP応答を待つ時間です。初回生成が長い環境では実測して調整します。 |
| `authorization_token_env` | 文字列 | `[A-Za-z_][A-Za-z0-9_]*`に一致する環境変数名 | なし | 任意 | Bearerトークンを環境変数から読みます。トークン自体はTOMLへ書きません。 |

search-webサーバーは`temperature: 0`を常に送ります。`[llm].temperature`は実装されていません。回答として採用するのは`choices[0].message.content`だけで、`reasoning_content`は採用しません。詳細と空回答の確認手順は[LLM連携](docs/llm-integration.md)を参照してください。

### `[usage_log]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `path` | 文字列 | 作成または追記可能なファイルのパス | なし | 利用量を記録する場合は必須 | 埋め込みとLLMの応答に含まれる`usage`を、1行1レコードのJSONLへ追記します。相対パスはTOMLのディレクトリを基準にします。 |

記録しない場合は`[usage_log]`を省略します。空のセクションを置いても記録先にはなりません。

### `[mock]`

起動スクリプトとE2Eテストが使うローカルの模擬LLM・埋め込みサービスです。実サービスの設定と混同しないでください。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `enabled` | 真偽値 | `true`、`false` | `false` | 任意 | `true`ならsearch-webの起動スクリプトが模擬サービスも起動します。 |
| `host` | 文字列 | 空でないホスト名またはIPアドレス | `127.0.0.1` | 任意 | 模擬サービスの待ち受けアドレスです。 |
| `port` | 整数 | 1〜65535 | `1234` | 任意 | 模擬サービスのポートです。 |
| `model` | 文字列 | 空でない文字列 | `yappod-demo-mock` | 任意 | 模擬Chat Completionsが受理するモデル名です。 |
| `answer` | 文字列 | 任意の文字列 | `参照資料から確認できる内容です。[1]` | 任意 | 模擬回答の本文です。 |
| `embedding_dimensions` | 整数 | 1〜65536 | `3` | 任意 | 模擬埋め込みが返す要素数です。索引と`[embedding].dimensions`に合わせます。 |

アプリケーションTOMLの全セクションと全キーは[設定リファレンス](../../docs/configuration.md)を参照してください。

## 索引だけを作る

`[build].input`と`[build].yappo_makeindex`がある設定では、次のコマンドで索引だけを作ります。

```sh
node examples/search-web/scripts/stack.mjs build \
  --config examples/wikipedia-search/wikipedia-search.toml
```

`build`は`index.directory`がすでに存在すると上書きせず失敗します。既存索引を利用する場合は`start`を使います。作り直す場合は、既存成果物を保存するか再生成可能であることを確認し、未使用の`index.directory`を設定してください。

## 一式を起動する

```sh
examples/search-web/scripts/start.sh \
  --config /absolute/path/to/application.toml
```

起動処理は次の順序です。

1. 索引の`manifest.json`を確認し、索引がない場合だけ`[build]`から作成します。
2. Yappod2実行ファイル、npm依存、既存PIDを確認します。
3. `npm run build`でclientとserverをビルドします。
4. coreを起動し、PIDを確認します。
5. frontを起動し、`/health/ready`を確認します。
6. `[mock].enabled = true`なら模擬サービスを起動します。
7. search-webサーバーを起動し、`/api/health`を確認します。

成功すると`http://<web.host>:<web.port>`を表示します。デフォルトは`http://127.0.0.1:4173`です。

## 画面で利用できる機能

### 検索

語彙検索はすべての索引で使えます。ベクトル検索と複合検索は、索引がベクトルを持ち、`[embedding]`の`dimensions`と任意の`model_id`が索引に一致する場合だけ選択できます。search-webサーバーは検索文を埋め込み、frontの`QUERY /v2/search`へ検索ベクトルを送ります。デフォルトでは50件を文書単位で取得します。

### 質問

search-webサーバーはfrontの`QUERY /v2/retrieve`へ、最大20件の本文断片、1文書あたり2件の本文断片、コンテキスト
32768バイトを指定します。`[llm]`がない場合も参照資料を表示します。設定がある場合だけ、質問と参照資料をLLMへ送り、
回答中の`[1]`形式の参照番号を検証します。

### 文書登録

語彙索引では入力文書をそのまま`POST /v2/documents:batch`へ登録します。ベクトル対応索引では、先に`POST /v2/passages:prepare`で本文断片を作り、search-webサーバーが各本文断片を埋め込みしてから登録します。

手動登録は索引にだけ反映され、local-filesやWikipediaの元データには戻りません。元データから索引を再構築すると、手動登録分は失われます。

## search-webサーバーのHTTP API

ブラウザー画面は次のsearch-webサーバーエンドポイントを使います。Yappod2本体の公開APIとは別の、サンプル専用APIです。

| エンドポイント | 入力または用途 |
|---|---|
| `GET /api/health` | search-webサーバー自身の起動確認です。`{"ready":true}`を返します。 |
| `GET /api/status` | frontの準備完了、世代、利用可能な検索方式、LLMと埋め込みの設定状態を返します。 |
| `POST /api/search` | `query` 1〜500文字、`limit` 1〜100、任意の`cursor`と`mode`を受けます。 |
| `POST /api/documents` | `id`、`title`、`body`と任意の`url`を受け、1文書を登録します。 |
| `POST /api/rag` | `question` 1〜1000文字と任意の`mode`を受けます。 |

### `GET /api/status`

正常時は次の項目を返します。

| 項目 | 説明 |
|---|---|
| `ready` | frontの`/health/ready`が返した準備状態です。 |
| `generation` | 接続中の索引世代です。 |
| `state` | frontが返した状態文字列です。省略される場合があります。 |
| `llm_configured` | `[llm]`を読み込んだかを表します。LLMの稼働確認ではありません。 |
| `embedding_configured` | `[embedding]`を読み込んだかを表します。埋め込みサービスの稼働確認ではありません。 |
| `index_embedding` | 索引のベクトル状態、`model_id`、`dimensions`です。 |
| `available_modes` | 利用可能な検索方式です。常に`lexical`を含み、索引と埋め込み設定が一致すると`vector`と`hybrid`も含みます。 |

frontへ接続できない場合もHTTP応答自体は返し、`ready: false`と`code`、`message`で理由を示します。

### `POST /api/search`

```json
{
  "query": "検索エンジン",
  "mode": "hybrid",
  "limit": 50,
  "cursor": "前回のnext_cursor"
}
```

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `query` | 文字列 | 1〜500文字。前後の空白を除いた後も1文字以上 | なし | 必須 | 検索する語句を指定します。 |
| `mode` | 文字列 | `lexical`、`vector`、`hybrid` | `lexical` | 任意 | 使用する検索方式を指定します。 |
| `limit` | 整数 | 1〜100 | `50` | 任意 | 1ページの最大件数です。 |
| `cursor` | 文字列 | 1〜512文字。直前の同じ検索条件に対して返された`next_cursor` | なし | 任意 | 続きの検索結果を取得します。 |

ベクトルを使う方式では、search-webサーバーが`query`を埋め込みしてから本体の`QUERY /v2/search`を呼びます。成功時の応答は本体APIの検索応答と同じです。検索と取得に限り、本体のfrontは互換用の`POST`も受理します。

### `POST /api/documents`

```json
{
  "id": "manual-1",
  "title": "手動登録",
  "url": "https://example.test/1",
  "body": "検索対象の本文です。"
}
```

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `id` | 文字列 | 1〜200文字。前後の空白を除いた後も1文字以上 | なし | 必須 | 前後の空白を除いて登録します。 |
| `title` | 文字列 | 1〜500文字。前後の空白を除いた後も1文字以上 | なし | 必須 | 前後の空白を除いて登録します。 |
| `url` | 文字列 | 0〜2048文字 | 空文字列 | 任意 | 空白だけなら省略します。URLの構文はsearch-webサーバーでは検証しません。 |
| `body` | 文字列 | 1〜900000文字。空白だけの文字列は不可 | なし | 必須 | 前後の空白を削らず登録します。 |

語彙索引では本体の`POST /v2/documents:batch`を直接呼びます。ベクトル対応索引では`passages:prepare`、文書用埋め込み、一括登録の順に実行します。登録メタデータには`{"source":"manual"}`を付けます。成功時は`generation`、`accepted`、`upserts`、`deletes`、`segment_ids`を返します。

search-webサーバーの表は文字数の上限ですが、本体APIではUTF-8のバイト数も検証します。特に`id`と`title`は本体側で最大255バイト、本文を含むHTTPリクエスト全体は最大1048576バイトです。日本語などのマルチバイト文字を多く含む入力は、search-webサーバーの文字数上限内でも本体APIで拒否される場合があります。

### `POST /api/rag`

```json
{
  "question": "Yappod2はどのように索引を更新しますか？",
  "mode": "hybrid"
}
```

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `question` | 文字列 | 1〜1000文字。前後の空白を除いた後も1文字以上 | なし | 必須 | 参照資料を検索し、LLMへ送る質問です。 |
| `mode` | 文字列 | `lexical`、`vector`、`hybrid` | `lexical` | 任意 | 参照資料の取得に使う検索方式です。 |

応答には取得した`context`と`citations`、`question`、`retrieval_mode`、`answer`、`referenced_citations`、`generation_status`が含まれます。`generation_status`の値と意味は[LLM連携](docs/llm-integration.md#回答生成状態)で説明します。回答生成に失敗しても、取得済みの参照資料は応答へ残します。

### search-webサーバーのエラー

Fastifyによる入力検証エラーのほか、search-webサーバーは`{"code":"...","message":"..."}`を返します。主なコードは次のとおりです。

| コード | 主なHTTP状態 | 意味 |
|---|---:|---|
| `invalid_query`、`invalid_question`、`invalid_document` | 400 | 空白を除いた入力が空です。 |
| `embedding_unconfigured` | 503 | ベクトルを必要とする処理ですが`[embedding]`がありません。 |
| `index_vector_disabled` | 409 | 接続中の索引がベクトルに対応していません。 |
| `embedding_dimension_mismatch`、`embedding_model_mismatch` | 409または502 | 索引、search-webサーバー設定、埋め込み応答の互換条件が一致しません。 |
| `embedding_unavailable`、`embedding_failed` | 502または503 | 埋め込みサービスへ接続できないか、正常応答を得られません。 |
| `invalid_embedding_response`、`invalid_prepare_response` | 502 | 接続先またはfrontの応答を検証できません。 |
| `daemon_unavailable` | 503 | search-webサーバーからfrontへ接続できません。 |
| `invalid_daemon_response` | 502 | frontの応答をJSONとして読めません。 |
| `daemon_error` | frontが返した状態 | frontがエラー応答を返しました。現行search-webサーバーは本体の入れ子になった`error.code`をこの共通コードへまとめます。 |
| `internal_error` | 500 | 上記へ分類できない例外です。 |

本体のHTTP仕様と本体側エラーは[`yappod_front` API](../../docs/yappod-front-api.md)を参照してください。

## 停止

起動時と同じ設定を渡します。

```sh
examples/search-web/scripts/stop.sh \
  --config /absolute/path/to/application.toml
```

Web、模擬、front、coreの順に`SIGTERM`を送り、5秒程度待って終了しなければ`SIGKILL`を送ります。PIDが別のプロセスへ再利用されている場合は、コマンドラインを照合して無関係なプロセスを停止しません。

## PIDとログ

`[daemon].run_directory`には次を保存します。

| プロセス | PID | 標準出力 | 標準エラー出力 |
|---|---|---|---|
| core | `core.pid` | `core.log` | `core.error` |
| front | `front.pid` | `front.log` | `front.error` |
| search-webサーバーと画面 | `web.pid` | `web.log` | `web.error` |
| mock | `mock-llm.pid` | `mock-llm.log` | `mock-llm.error` |

起動失敗時は表示されたヘルスチェックURLと`.error`の末尾を確認します。PIDファイルを手作業で消す前に、PIDとプロセスのコマンドラインを確認してください。症状別の手順は[サンプルの問題解決](../troubleshooting.md)にあります。

## 開発用起動

開発用サーバーは`config.toml`をデフォルトで読みます。

```sh
cd examples/search-web
cp config.example.toml config.toml
npm run dev
```

`npm run dev`はTypeScriptサーバーを監視し、変更時に再起動します。同時にViteの開発サーバーも起動します。Yappod2サーバーと索引は別途準備してください。配布用起動は`server/dist`と`client/dist`を使うため、変更後は次を実行します。

```sh
npm run typecheck
npm test
npm run build
npm run test:e2e
```

E2Eは一時ポートと模擬を利用し、外部APIへ接続しません。画面の情報設計は[UX設計](docs/ux-design.md)を参照してください。
