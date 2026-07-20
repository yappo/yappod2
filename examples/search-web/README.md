# Yappod2検索Web UI

search-webは、Yappod2の索引をブラウザーから検索し、RAG向けの参照資料を取得し、任意で回答生成と文書登録を行うサンプルです。Node.js/FastifyのBFFが`yappod_front`を呼び、React/Viteで作った画面を配信します。

## 構成

```text
ブラウザー
   │ HTTP /api/*
   ▼
search-web BFF ──────→ 埋め込みAPI（ベクトル検索と登録時だけ）
   │                  └→ LLM Chat Completions（回答生成時だけ）
   │ HTTP /v2/*
   ▼
yappod_front ──内部フレーム──→ yappod_core ──→ 索引
```

検索そのものと索引更新はYappod2が実行します。検索文と登録文書の埋め込み、RAG回答の生成はBFFが外部サービスへ依頼します。`yappod_core`と`yappod_front`はLLMへ接続しません。

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

search-webは1つのアプリケーションTOMLをCデーモン、索引作成、BFF、起動スクリプトで共有します。相対パスはTOMLがあるディレクトリを基準に解決します。

### `[build]`

既存索引だけを使う場合は省略できます。`[index].directory`に索引がなく、起動スクリプトに索引を作らせる場合は両方を指定します。

| キー | 必須 | 説明 |
|---|---|---|
| `yappo_makeindex` | 自動作成時に必須 | `yappo_makeindex`実行ファイルへのパスです。相対パスはTOMLを基準にします。 |
| `input` | 自動作成時に必須 | `yappo_makeindex build`へ渡す正式な文書NDJSONです。ベクトル対応索引では`vectors`を含む必要があります。 |

### `[index]`

| キー | 必須 | 説明 |
|---|---|---|
| `directory` | 必須 | 接続する索引ディレクトリです。既存索引には`config.toml`と`manifest.json`が必要です。 |

`[vector]`、`[tokenizer]`、`[chunking]`、`[metadata]`は索引構造を表します。BFFの機能を有効にするだけの設定ではありません。索引直下の`config.toml`と一致しない構成へ変える場合は、索引を別ディレクトリへ作り直します。

### `[daemon]`

| キー | 既定値・範囲 | 説明 |
|---|---|---|
| `run_directory` | `./run` | core、front、Web、模擬のPIDとログを置きます。 |
| `core_host` | `127.0.0.1` | frontが接続し、coreが待ち受けるホストです。 |
| `core_port` | `18401`、1〜65535 | coreの内部プロトコル用ポートです。 |
| `front_host` | `127.0.0.1` | frontの待ち受け先であり、BFFの接続先です。 |
| `front_port` | `18400`、1〜65535 | frontのHTTPポートです。 |
| `max_inflight` | `4`、1〜1024 | frontとcoreが、それぞれ処理中として受理するリクエストの件数上限です。 |
| `max_inflight_bytes` | `4194304`、1〜1073741824 | frontとcoreが、それぞれ処理中として保持する本文または内部ペイロードの合計バイト数です。 |
| `request_timeout_ms` | `5000`、1〜60000 | coreとfrontが接続後のソケット送受信へ設定する期限です。 |
| `write_token` | 任意、16〜255バイト | 文書登録APIをBearer認証します。空白と制御文字は使えません。 |

### `[web]`

| キー | 既定値・範囲 | 説明 |
|---|---|---|
| `host` | `127.0.0.1` | BFFと静的画面の待ち受け先です。 |
| `port` | `4173`、1〜65535 | ブラウザーから接続するポートです。 |
| `yappod_timeout_ms` | `5000`、1〜600000 | BFFが1回のfront応答を待つ時間です。 |
| `startup_timeout_ms` | `8000`、100〜600000 | 起動スクリプトが模擬LLMとWebのヘルスチェックを待つ時間です。core/frontの初期待機は、これとは別の短い繰り返し確認です。 |

### `[embedding]`

ベクトル検索、複合検索、ベクトル対応索引への文書登録に必要です。語彙検索だけならセクション全体を省略します。空の`[embedding]`は無効化ではなく設定エラーです。

| キー | 必須・既定値 | 説明 |
|---|---|---|
| `provider` | 必須 | `lmstudio`、`ollama`、`openai`のいずれかです。 |
| `base_url` / `endpoint_url` | どちらか一方だけ必須 | `base_url`にはプロバイダー別パスを追加します。`endpoint_url`は完全な接続先です。 |
| `model` | 必須 | 接続先へ送る実際のモデル名です。 |
| `model_id` | 任意ですが推奨 | 索引の`[vector].model_id`と比較するYappod2側の識別子です。 |
| `dimensions` | `768`、1〜65536 | 応答ベクトルの要素数です。索引と一致させます。 |
| `prompt_profile` | `plain` | `plain`または`embeddinggemma`です。後者は検索文と文書用プロンプトを付けます。 |
| `timeout_ms` | `60000`、1000〜600000 | 埋め込みAPIへのHTTP要求1回の期限です。 |
| `batch_size` | `16`、1〜1024 | 1回に送る入力数です。 |
| `authorization_token_env` | 任意 | Bearerトークンを読む環境変数名です。トークン自体はTOMLへ書きません。 |

### `[llm]`

RAGの回答生成を使う場合だけ追加します。回答生成を使わない場合は、空のセクションを残さず見出しごと省略します。

| キー | 必須・既定値 | 説明 |
|---|---|---|
| `base_url` | 必須 | OpenAI互換APIの基準URLです。BFFが`chat/completions`を加えます。クエリーとフラグメントは指定できません。 |
| `model` | 必須 | 接続先へ送る実際のモデル名です。索引の`model_id`とは別です。 |
| `effort` | 任意 | 指定した文字列を`reasoning_effort`として送ります。使用できる値は接続先で確認します。 |
| `max_tokens` | `8192`、1〜131072 | 接続先へ毎回送る最大出力トークン数です。入力トークン数ではありません。 |
| `timeout_ms` | `30000`、1000〜600000 | 回答生成のHTTP応答を待つ時間です。初回生成が長い環境では実測して調整します。 |
| `authorization_token_env` | 任意 | Bearerトークンを読む環境変数名です。トークン自体はTOMLへ書きません。 |

BFFは`temperature: 0`を常に送ります。`[llm].temperature`は実装されていません。回答として採用するのは`choices[0].message.content`だけで、`reasoning_content`は採用しません。詳細と空回答の確認手順は[LLM連携](docs/llm-integration.md)を参照してください。

### `[usage_log]`

| キー | 必須・既定値 | 説明 |
|---|---|---|
| `path` | 任意 | 埋め込みとLLMの応答に含まれる`usage`を、1行1レコードのJSONLへ追記するパスです。相対パスはTOMLのディレクトリを基準にします。 |

記録しない場合は`[usage_log]`を省略します。空のセクションを置いても記録先にはなりません。

### `[mock]`

起動スクリプトとE2Eテストが使うローカルの模擬LLM・埋め込みサービスです。実サービスの設定と混同しないでください。

| キー | 既定値・範囲 | 説明 |
|---|---|---|
| `enabled` | `false` | `true`なら`stack.mjs start`が模擬サービスも起動します。 |
| `host` | `127.0.0.1` | 模擬サービスの待ち受けアドレスです。 |
| `port` | `1234`、1〜65535 | 模擬サービスのポートです。 |
| `model` | `yappod-demo-mock` | 模擬Chat Completionsが受理するモデル名です。 |
| `answer` | `参照資料から確認できる内容です。[1]` | 模擬回答の本文です。 |
| `embedding_dimensions` | `3`、1〜65536 | 模擬埋め込みが返す要素数です。索引と`[embedding].dimensions`に合わせます。 |

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
2. C実行ファイル、npm依存、既存PIDを確認します。
3. `npm run build`でclientとserverをビルドします。
4. coreを起動し、PIDを確認します。
5. frontを起動し、`/health/ready`を確認します。
6. `[mock].enabled = true`なら模擬サービスを起動します。
7. BFFを起動し、`/api/health`を確認します。

成功すると`http://<web.host>:<web.port>`を表示します。既定は`http://127.0.0.1:4173`です。

## 画面で利用できる機能

### 検索

語彙検索はすべての索引で使えます。ベクトル検索と複合検索は、索引がベクトルを持ち、`[embedding]`の`dimensions`と任意の`model_id`が索引に一致する場合だけ選択できます。BFFは検索文を埋め込み、frontの`POST /v2/search`へ検索ベクトルを送ります。既定では50件を文書単位で取得します。

### 質問

BFFはfrontの`POST /v2/retrieve`へ、最大20パッセージ、1文書あたり2パッセージ、コンテキスト32768バイトを指定します。`[llm]`がない場合も参照資料を表示します。設定がある場合だけ、質問と参照資料をLLMへ送り、回答中の`[1]`形式の参照番号を検証します。

### 文書登録

語彙索引では入力文書をそのまま`POST /v2/documents:batch`へ登録します。ベクトル対応索引では、先に`POST /v2/passages:prepare`でパッセージを作り、BFFが各パッセージを埋め込みしてから登録します。

手動登録は索引にだけ反映され、local-filesやWikipediaの元データには戻りません。元データから索引を再構築すると、手動登録分は失われます。

## BFFのHTTP API

ブラウザー画面は次のBFFエンドポイントを使います。Yappod2本体の公開APIとは別の、サンプル専用APIです。

| エンドポイント | 入力または用途 |
|---|---|
| `GET /api/health` | BFF自身の起動確認です。`{"ready":true}`を返します。 |
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
{"query":"検索エンジン","mode":"hybrid","limit":50,"cursor":"前回のnext_cursor"}
```

| 項目 | 必須・範囲 | 説明 |
|---|---|---|
| `query` | 必須、1〜500文字 | 前後の空白を除いた後も1文字以上必要です。 |
| `mode` | 任意、既定値`lexical` | `lexical`、`vector`、`hybrid`です。 |
| `limit` | 任意、既定値`50`、1〜100 | 1ページの最大件数です。 |
| `cursor` | 任意、1〜512文字 | 直前の応答に含まれる`next_cursor`です。 |

ベクトルを使う方式では、BFFが`query`を埋め込みしてから本体の`POST /v2/search`を呼びます。成功時の応答は本体APIの検索応答と同じです。

### `POST /api/documents`

```json
{"id":"manual-1","title":"手動登録","url":"https://example.test/1","body":"検索対象の本文です。"}
```

| 項目 | 必須・範囲 | 説明 |
|---|---|---|
| `id` | 必須、1〜200文字 | 前後の空白を除いて登録します。 |
| `title` | 必須、1〜500文字 | 前後の空白を除いて登録します。 |
| `url` | 任意、最大2048文字 | 空白だけなら省略します。URLの構文はBFFでは検証しません。 |
| `body` | 必須、1〜900000文字 | 本文は前後を削らず登録しますが、空白だけの本文は拒否します。 |

語彙索引では本体の`POST /v2/documents:batch`を直接呼びます。ベクトル対応索引では`passages:prepare`、文書用埋め込み、一括登録の順に実行します。登録メタデータには`{"source":"manual"}`を付けます。成功時は`generation`、`accepted`、`upserts`、`deletes`、`segment_ids`を返します。

BFFの表は文字数の上限ですが、本体APIではUTF-8のバイト数も検証します。特に`id`と`title`は本体側で最大255バイト、本文を含むHTTPリクエスト全体は最大1048576バイトです。日本語などのマルチバイト文字を多く含む入力は、BFFの文字数上限内でも本体APIで拒否される場合があります。

### `POST /api/rag`

```json
{"question":"Yappod2はどのように索引を更新しますか？","mode":"hybrid"}
```

| 項目 | 必須・範囲 | 説明 |
|---|---|---|
| `question` | 必須、1〜1000文字 | 前後の空白を除いた後も1文字以上必要です。 |
| `mode` | 任意、既定値`lexical` | 参照資料の取得に使う検索方式です。 |

応答には取得した`context`と`citations`、`question`、`retrieval_mode`、`answer`、`referenced_citations`、`generation_status`が含まれます。`generation_status`の値と意味は[LLM連携](docs/llm-integration.md#回答生成状態)で説明します。回答生成に失敗しても、取得済みの参照資料は応答へ残します。

### BFFのエラー

Fastifyによる入力検証エラーのほか、BFFは`{"code":"...","message":"..."}`を返します。主なコードは次のとおりです。

| コード | 主なHTTP状態 | 意味 |
|---|---:|---|
| `invalid_query`、`invalid_question`、`invalid_document` | 400 | 空白を除いた入力が空です。 |
| `embedding_unconfigured` | 503 | ベクトルを必要とする処理ですが`[embedding]`がありません。 |
| `index_vector_disabled` | 409 | 接続中の索引がベクトルに対応していません。 |
| `embedding_dimension_mismatch`、`embedding_model_mismatch` | 409または502 | 索引、BFF設定、埋め込み応答の互換条件が一致しません。 |
| `embedding_unavailable`、`embedding_failed` | 502または503 | 埋め込みサービスへ接続できないか、正常応答を得られません。 |
| `invalid_embedding_response`、`invalid_prepare_response` | 502 | 接続先またはfrontの応答を検証できません。 |
| `daemon_unavailable` | 503 | BFFからfrontへ接続できません。 |
| `invalid_daemon_response` | 502 | frontの応答をJSONとして読めません。 |
| `daemon_error` | frontが返した状態 | frontがエラー応答を返しました。現行BFFは本体の入れ子になった`error.code`をこの共通コードへまとめます。 |
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
| BFFと画面 | `web.pid` | `web.log` | `web.error` |
| mock | `mock-llm.pid` | `mock-llm.log` | `mock-llm.error` |

起動失敗時は表示されたヘルスチェックURLと`.error`の末尾を確認します。PIDファイルを手作業で消す前に、PIDとプロセスのコマンドラインを確認してください。症状別の手順は[サンプルの問題解決](../troubleshooting.md)にあります。

## 開発用起動

開発用サーバーは`config.toml`を既定で読みます。

```sh
cd examples/search-web
cp config.example.toml config.toml
npm run dev
```

`npm run dev`はTypeScriptサーバーを監視し、変更時に再起動します。同時にViteの開発サーバーも起動します。Cデーモンと索引は別途準備してください。配布用起動は`server/dist`と`client/dist`を使うため、変更後は次を実行します。

```sh
npm run typecheck
npm test
npm run build
npm run test:e2e
```

E2Eは一時ポートと模擬を利用し、外部APIへ接続しません。画面の情報設計は[UX設計](docs/ux-design.md)を参照してください。
