# 設定リファレンス

Yappod2のサンプルでは、索引、デーモン、Web画面、埋め込みサービス、LLMの設定を1つのTOMLへまとめられます。ただし、各プログラムが読む項目は異なります。この文書では、すべての設定キーについて、読み取るプログラム、必須条件、型、既定値、値の選び方、ほかの設定との関係を説明します。

## 最初に理解しておくこと

設定ファイルには、用途が異なる二つの情報が含まれます。

- 索引形式の設定は、`[tokenizer]`、`[chunking]`、`[vector]`、`[metadata]`です。索引を作ると、適用済みの値が`<index-directory>/config.toml`へ保存されます。
- アプリケーションの設定は、`[index]`、`[daemon]`、`[web]`、`[embedding]`、`[llm]`などです。接続先や実行時の上限を定めます。これらは索引内の`config.toml`へ保存されません。

既存索引を開くときは、アプリケーション用TOMLの`[index].directory`をたどり、索引内の`config.toml`と`manifest.json`を読みます。アプリケーション用TOMLで`[vector].enabled = true`へ変更しても、既存索引にベクトル用ファイルは追加されません。

## 共通規則

- アプリケーション用TOMLの相対パスは、そのTOMLが置かれているディレクトリを基準に解決します。コマンドを実行したカレントディレクトリは基準になりません。
- Cコマンドで使うTOMLには、`format_version`、`[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[daemon]`が必要です。`[metadata]`は省略できます。
- 任意機能を使わない場合は、セクションを見出しごと省略します。空の`[embedding]`や`[llm]`は「無効」を意味せず、必須キーが欠けた設定として拒否されます。
- `schema_version`は使用しません。現在の形式を表すキーは`format_version = 2`です。
- 外部APIのトークンをTOMLへ直接書く`authorization_token`は拒否されます。`authorization_token_env`へ環境変数名を書きます。
- Cの設定解析は、自身が読むセクション内の未知のキーを拒否します。search-web BFFは最上位と各対象セクションの未知のキーも拒否します。
- 文字列の長さは、特記がない限りUTF-8にしたときのバイト数で判定します。

## どのプログラムが何を読むか

| 設定 | 主に読み取るプログラム | 用途 |
|---|---|---|
| 最上位、`[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[metadata]`、`[daemon]` | `yappo_makeindex`、`search`、`yappo_compact`、`yappod_core`、`yappod_front` | 索引の場所と形式、デーモンの動作を決めます。 |
| `[build]` | search-webの`stack.mjs`、local-files、Wikipedia | 索引作成プログラムと入力ファイルを指定します。 |
| `[embedding]` | local-files、Wikipedia、search-webのBFF | 文またはパッセージをベクトルへ変換する外部HTTP APIを指定します。Cプログラムは読みません。 |
| `[web]` | search-web BFF、`stack.mjs` | Webサーバーと起動待ち時間を指定します。 |
| `[llm]` | search-web BFF | RAG回答を生成するOpenAI互換APIを指定します。 |
| `[usage_log]` | local-files、Wikipedia、search-web BFF | 外部APIの利用量をNDJSONへ記録します。 |
| `[mock]` | search-webのBFF、`stack.mjs` | テスト用の模擬LLM・埋め込みサーバーを指定します。 |
| `[input]`、`[output]`、`[prepare]`、`[extract]`、`[formatters]` | local-files | ファイル収集、抽出、成果物を指定します。 |

## 最上位のキー

| キー | 必須 | 型、既定値、範囲 | 読み取る処理 | 説明 |
|---|---|---|---|---|
| `format_version` | Cコマンドでは必須 | 整数、`2`だけを許可 | Cコマンド | 設定と索引形式の版です。local-filesとWikipediaのPython処理はこの値自体を検証しませんが、同じTOMLをCコマンドへ渡すため、サンプル設定にも`2`を記載します。 |
| `collection_id` | local-filesでは必須 | 文字列、1〜32文字、`A-Z`、`a-z`、`0-9`、`.`、`_`、`-` | local-files | 収集した文書集合を識別し、文書IDとメタデータへ反映します。Cコマンドとsearch-webはこの値を索引設定としては使用しません。 |

## `[index]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `directory` | Cコマンドとsearch-web stackで必須 | 空でないパス、既定値なし、解決後4095バイト以下 | 索引ディレクトリです。`build`では存在していないパスを指定します。検索、更新、デーモンでは`config.toml`と`manifest.json`を持つ既存ディレクトリを指定します。 |

索引を切り替える場合は、この値だけを新しい索引ディレクトリへ変更します。旧索引へ設定ファイルだけを上書きしてはいけません。

## `[tokenizer]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `id` | セクションは必須、キーは省略可能 | 文字列、`unicode_nfkc_casefold_v2`、1〜255バイト | トークナイザーの互換性を表す識別子です。現在のC実装は値を実装一覧から選択せず、Unicode正規化・分かち書き処理を固定で使用します。値は索引の設定指紋に含まれるため、同じ処理に同じ識別子を使い、変更時は索引を作り直します。 |

リポジトリ内の古いテスト用設定には`unicode_nfkc_cf_v1`もあります。新しく作る設定では、既定値の`unicode_nfkc_casefold_v2`を使用してください。任意の文字列で別のトークナイザー実装へ切り替わるわけではありません。

## `[chunking]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `max_chars` | セクションは必須、キーは省略可能 | 整数、`1200`、1〜1048576 | 一つのパッセージへ入れるUnicode文字数の上限です。短くすると検索結果の引用単位は細かくなりますが、パッセージ数とベクトル数が増えます。 |
| `overlap_chars` | セクションは必須、キーは省略可能 | 整数、`200`、0以上かつ`max_chars`未満 | 隣り合うパッセージの重なりです。段落境界をまたぐ検索漏れを減らすために使います。0にすると重複させません。 |

両方の値は索引の互換条件です。変更後は元の文書と、必要なら埋め込みを使って新しい索引を作ります。

## `[vector]`

`[vector]`は索引へ保存するベクトルの形式を定めます。埋め込みAPIのURLや実際に呼び出すモデルを定める設定ではありません。

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `enabled` | 必須 | 真偽値 | `true`なら`vectors.yap2`と`vectors.usearch`を作ります。`false`なら語彙検索用の索引だけを作ります。 |
| `model_id` | `enabled = true`のとき必須 | 文字列、1〜255バイト | Yappod2が索引と検索ベクトルの互換性を識別する名前です。提供元のモデル名と同じである必要はありませんが、運用中に意味が変わらない名前を付けます。 |
| `dimensions` | `enabled = true`のとき必須 | 整数、1〜65536 | 各ベクトルの要素数です。入力NDJSON、埋め込みレスポンス、検索ベクトルのすべてが一致する必要があります。 |
| `metric` | `enabled = true`のとき必須 | `cosine`、`dot`、`l2` | 類似度の計算方法です。モデルの推奨値に合わせます。通常の正規化済み文章埋め込みでは`cosine`を選びます。 |

語彙検索だけを使う設定は次の形にします。無効時に`model_id`や0以外の`dimensions`を書くと設定エラーになります。

```toml
[vector]
enabled = false
```

ベクトル検索と複合検索に対応する例は次のとおりです。

```toml
[vector]
enabled = true
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
metric = "cosine"
```

## `[metadata]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `filterable_fields` | 任意 | 文字列配列、`[]`、最大64件、各1〜127バイト | 絞り込み用のメタデータ索引へ保存するJSONパスです。`language`や`author.country`のように指定します。 |

同じ値を重複して指定できません。`.`はJSONオブジェクトをたどる区切りとして扱うため、`author.country`は`metadata.author.country`を参照します。先頭または末尾の`.`、連続する`.`、制御文字、`/`、`\`は拒否されます。読込時にUTF-8のバイト順へ並べ替えて保存するため、記載順は索引の互換性へ影響しません。対象値が配列なら、配列内の`null`、真偽値、数値、文字列を別々の値として索引へ保存します。オブジェクトと入れ子配列は直接の比較値にはなりません。

この設定は`POST /v2/search`と`POST /v2/retrieve`の`filter`で使います。`search`コマンドにはfilterを指定するオプションがありません。フィールドを後から追加する場合は、元文書から索引を作り直します。

## `[daemon]`

Cコマンドの共通アプリケーション設定ではセクションと接続先のキーが必須です。search-webだけで解析する場合は表の既定値が使われますが、Cデーモンも同じTOMLを読むため、通常はすべて明記してください。

| キー | C設定で必須 | 型、既定値、範囲 | 読み取る処理と意味 |
|---|---|---|---|
| `run_directory` | 必須 | パス、search-webでの既定値`./run`、解決後4095バイト以下 | core/frontの`*.pid`、`*.log`、`*.error`を置きます。search-webの起動スクリプトもプロセス管理に使います。 |
| `core_host` | 必須 | 空でない文字列、search-webで`127.0.0.1` | coreの待ち受け先であり、frontの接続先です。アプリケーションTOMLを使わず`--index`だけでcoreを起動した場合は待ち受けホストを指定できません。 |
| `core_port` | 必須 | 整数、search-webで`18401`、1〜65535 | front/core間の内部フレーム用ポートです。 |
| `front_host` | 必須 | 空でない文字列、search-webで`127.0.0.1` | frontの待ち受け先であり、search-webのBFFの接続先です。アプリケーションTOMLを使わず`--index`だけでfrontを起動した場合は待ち受けホストを指定できません。 |
| `front_port` | 必須 | 整数、search-webで`18400`、1〜65535 | `yappod_front`のHTTPポートです。 |
| `max_inflight` | 任意 | 整数、`4`、1〜1024 | frontとcoreが、それぞれ同時に処理中として保持するリクエストの件数上限です。どちらかで上限に達すると`503 overloaded`になります。ヘルスチェックとメトリクスはこの制限の対象外です。 |
| `max_inflight_bytes` | 任意 | 整数、`4194304`、1〜1073741824 | frontとcoreが、それぞれ処理中として保持するリクエスト本文または内部ペイロードの合計バイト数上限です。1件の大きさが残量を超える場合も`503 overloaded`になります。 |
| `request_timeout_ms` | 任意 | 整数、`5000`、1〜60000 | frontが受理したクライアントソケット、接続後のfront/core間ソケット、coreが受理したソケットの送受信期限です。coreへのTCP接続処理そのものへ独立した非同期期限を付ける設定ではなく、LLMを待つ時間でもありません。 |
| `write_token` | 任意 | 16〜255バイト、空白・制御文字不可 | `POST /v2/documents:batch`をBearer認証します。省略時は更新APIを認証なしで受け付けます。外部API用の`authorization_token_env`とは別の機能です。 |

## `[build]`

| キー | 必須 | 型、既定値 | 読み取る処理と意味 |
|---|---|---|---|
| `yappo_makeindex` | local-filesの`build`では必須。search-webでは索引がない場合に必須 | 実行ファイルのパス、既定値なし | 起動スクリプトまたはPythonアダプターが実行する`yappo_makeindex`です。相対パスはTOML基準です。 |
| `input` | search-webが索引を自動作成する場合に必須 | NDJSONのパス、既定値なし | `stack.mjs start`で`[index].directory`が存在しない場合、`yappo_makeindex build`へ渡します。既存索引を使うだけなら省略できます。 |

## `[embedding]`

`[embedding]`は外部サービスへ接続して入力文をベクトルに変換する設定です。`yappod_core`と`yappod_front`はこのセクションを読みません。search-webのBFF、local-files、Wikipediaアダプターが使用します。

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `directory` | local-filesの`embed`と`hybrid`で必須 | パス、既定値なし | local-filesが埋め込み済みの分割NDJSON、マニフェスト、再開位置を保存します。search-webとWikipediaは値を無視します。 |
| `provider` | 必須 | `lmstudio`、`ollama`、`openai` | 要求本文と応答の解釈方法を選びます。OpenAI互換APIなら`openai`または接続先に合わせた値を選びます。 |
| `base_url` | `endpoint_url`と一方だけ必須 | HTTP(S) URL | 基準URLです。`ollama`では`/api/embed`、それ以外では`/embeddings`を末尾へ付けます。末尾の`/`は除去され、クエリー文字列とフラグメントは許可されません。 |
| `endpoint_url` | `base_url`と一方だけ必須 | HTTP(S) URL | 埋め込みエンドポイント全体を指定します。独自のパスを使う接続先で使用します。 |
| `model` | 必須 | 空でない文字列 | プロバイダーへ送る実際のモデル名です。これはYappod2側の互換識別子ではありません。 |
| `model_id` | local-filesでは必須。ほかでは任意 | 空でない文字列 | `[vector].model_id`と一致させるYappod2側の識別子です。search-webでは指定時に索引のモデルIDと照合します。 |
| `dimensions` | local-filesでは必須。search-webとWikipediaでは省略可能 | 整数、search-webの既定値`768`、Wikipediaでは`vector.dimensions`、1〜65536 | プロバイダーが返すベクトルの要素数です。`[vector].dimensions`と一致させます。 |
| `prompt_profile` | 任意 | `plain`、`embeddinggemma`、既定値`plain` | `plain`は入力をそのまま送ります。`embeddinggemma`は検索文と文書に用途別の指示を付けます。索引作成と検索で同じ方針を使います。 |
| `authorization_token_env` | 任意 | 環境変数名 | 指定した環境変数からBearerトークンを読みます。環境変数が未設定または空なら起動・処理に失敗します。 |
| `timeout_ms` | 任意 | 整数、既定値`60000`。search-webとWikipediaは1000〜600000、local-filesは1以上 | 埋め込みAPIへのHTTP要求1回を待つ上限です。全バッチを処理する総時間ではありません。同じ設定を複数のサンプルで共有する場合は、1000〜600000の範囲に収めます。 |
| `batch_size` | 任意 | 整数、`16`、1〜1024 | 1回のHTTPリクエストへまとめる入力件数です。接続先の上限、メモリー量、応答時間に合わせて調整します。 |

平文HTTPは`localhost`、ループバックアドレス、IPv4のプライベートネットワーク、IPv6のユニークローカルアドレスだけで許可されます。それ以外の接続先にはHTTPSが必要です。

`model`と`model_id`は、たとえば次のように使い分けます。

```toml
[vector]
enabled = true
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
metric = "cosine"

[embedding]
provider = "lmstudio"
base_url = "http://127.0.0.1:1234/v1"
model = "text-embedding-embeddinggemma-300m"
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
prompt_profile = "embeddinggemma"
batch_size = 16
timeout_ms = 60000
```

ここでは`embedding.model`がLM Studioへ送る名前で、二つの`model_id`はYappod2内部の互換性確認に使う名前です。

## `[web]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `host` | 任意 | 文字列、`127.0.0.1` | search-web BFFが待ち受けるアドレスです。外部へ公開する場合は、認証やリバースプロキシも含めて別途保護してください。 |
| `port` | 任意 | 整数、`4173`、1〜65535 | Web UIとBFF APIのポートです。 |
| `yappod_timeout_ms` | 任意 | 整数、`5000`、1〜600000 | BFFが`yappod_front`のHTTP応答を待つ上限です。front/core間の`daemon.request_timeout_ms`とは別です。 |
| `startup_timeout_ms` | 任意 | 整数、`8000`、100〜600000 | `stack.mjs`がBFFと、必要なら模擬サービスのヘルスチェックを待つ上限です。coreのPID確認とfrontの準備完了確認は、現行の起動スクリプトに固定された回数だけ繰り返します。検索やLLMの応答時間には使いません。 |

## `[llm]`

このセクションが存在すると、search-web BFFはOpenAI互換の`/chat/completions`へRAG回答生成を依頼します。使わない場合はセクション全体を省略します。

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `base_url` | 必須 | HTTP(S) URL | APIの基準URLです。末尾の`/`は除去され、クエリー文字列とフラグメントは許可されません。BFFは`/chat/completions`を付けます。 |
| `model` | 必須 | 空でない文字列 | Chat Completionsへ送るモデル名です。 |
| `effort` | 任意 | 空でない文字列 | 値を`reasoning_effort`としてそのまま送ります。Yappod2側は列挙値を検証しないため、接続先が受理する値を指定します。 |
| `max_tokens` | 任意 | 整数、`8192`、1〜131072 | 接続先へ明示する最大出力トークン数です。入力トークン数の上限ではありません。モデルが扱える文脈長の上限も接続先で適用されます。 |
| `authorization_token_env` | 任意 | 環境変数名 | Bearerトークンを環境変数から読みます。TOMLへトークンそのものは書けません。 |
| `timeout_ms` | 任意 | 整数、`30000`、1000〜600000 | BFFがLLMの1回の応答を待つ上限です。大きいモデルやコールドスタートでは、実測時間を確認して増やします。 |

現在のBFFは`temperature: 0`を常に送ります。`[llm].temperature`という設定キーは実装されていません。接続先の管理画面で別の値を設定しても、BFFのリクエストが優先されます。詳しくは[LLM連携](../examples/search-web/docs/llm-integration.md)を参照してください。

## `[usage_log]`

| キー | 必須 | 型、既定値 | 説明 |
|---|---|---|---|
| `path` | 任意 | ファイルパス、既定値なし | 埋め込みまたはLLMレスポンスに含まれる利用量を1行1レコードのNDJSONで追記します。相対パスはTOML基準です。 |

local-filesでは`[usage_log]`を記述した場合に`path`が必要です。空のセクションを置かず、記録しない場合はセクション全体を省略してください。

## `[mock]`

| キー | 必須 | 型、既定値、範囲 | 説明 |
|---|---|---|---|
| `enabled` | 任意 | 真偽値、`false` | `true`にするとsearch-webの起動スクリプトが模擬APIを起動します。テストとローカルデモ用です。 |
| `host` | 任意 | 文字列、`127.0.0.1` | 模擬APIの待ち受けアドレスです。 |
| `port` | 任意 | 整数、`1234`、1〜65535 | 模擬APIのポートです。 |
| `model` | 任意 | 文字列、`yappod-demo-mock` | 模擬Chat Completionsが受理するモデル名です。 |
| `answer` | 任意 | 文字列、`参照資料から確認できる内容です。[1]` | 模擬回答の本文です。 |
| `embedding_dimensions` | 任意 | 整数、`3`、1〜65536 | 模擬埋め込みが返すベクトルの次元数です。`[vector].dimensions`と合わせます。 |

## local-files固有の設定

local-filesの`[input]`、`[output]`、`[prepare]`、`[extract]`、`[formatters]`は、[local-files設定リファレンス](../examples/local-files/docs/configuration.md)で全キーと選び方を説明しています。

## タイムアウトの違い

| キー | 待っている対象 | 典型的な失敗箇所 |
|---|---|---|
| `daemon.request_timeout_ms` | TCP接続後のfrontとcore間のフレーム送受信 | core停止、内部処理の遅延、ネットワーク障害。この値は独立した接続確立時間の上限ではありません。 |
| `web.yappod_timeout_ms` | search-web BFFからfrontへのHTTP応答 | front停止、検索・取得・登録の遅延 |
| `web.startup_timeout_ms` | `stack.mjs`がBFFと任意の模擬のヘルスチェック確認 | ビルド不足、ポート競合、起動直後の初期化 |
| `embedding.timeout_ms` | 埋め込みAPIの1リクエスト | 接続先停止、大きいバッチ、モデルの読み込み待ち |
| `llm.timeout_ms` | Chat Completionsの1リクエスト | 長いRAG入力、コールドスタート、生成時間超過 |
| `extract.tika_timeout_ms` | Tikaコマンド1回 | 大きい文書、Java/Tikaの停止 |
| `formatters.rules[].timeout_ms` | 独自フォーマッターコマンド1回 | フォーマッターの停止、処理時間超過 |

タイムアウトを大きくする前に、どの境界で時間を使っているかをログで確認してください。

## 語彙索引からベクトル対応索引へ移行する

設定変更だけでは移行できません。次の順序で別の索引を作ります。

1. 元文書から、現在の`[chunking]`に従うパッセージを作ります。
2. `[embedding]`で指定した実モデルから、各パッセージの埋め込みを生成します。
3. パッセージ順と同じ順序の`vectors`を各`upsert`へ加えたNDJSONを作ります。
4. `[vector]`へ`model_id`、`dimensions`、`metric`を設定します。
5. 既存索引とは異なる`[index].directory`を指定して`yappo_makeindex build`を実行します。
6. 新しい`config.toml`でベクトル設定を、`manifest.json`で`vectors.yap2`と`vectors.usearch`を確認します。
7. `yappo_makeindex verify`を実行します。
8. アプリケーション用TOMLの`[index].directory`を新しい索引へ切り替えます。

## 完成した設定例

語彙検索だけを使う設定は[`examples/config.lexical.toml`](../examples/config.lexical.toml)を参照してください。3次元の固定ベクトルでベクトルと複合検索を動作確認する設定は[`examples/config.toml`](../examples/config.toml)です。実際の埋め込みサービスを使う場合は、上記の`[vector]`と`[embedding]`の例を同じTOMLへ追加し、モデルIDと次元数を一致させてください。
