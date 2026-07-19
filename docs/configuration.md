# 設定リファレンス

Yappod2では、索引作成、検索、デーモン、サンプルから同じアプリケーション用TOMLを参照できます。ただし、
すべてのセクションをすべてのプログラムが読むわけではありません。この文書では、どの設定を誰が利用するかを
区別して説明します。

## 基本規則

- 相対パスは、TOMLファイルがあるディレクトリを基準に解決します。
- 各プログラムは、自身が利用するセクションのキーを検証します。索引設定とsearch-webの設定では未知のキーを拒否します。Cプログラム用の設定解析では、サンプルが利用する追加セクションを同じTOMLへ記述できます。
- 任意の機能を使わない場合は、そのセクションを見出しごと省略します。空の`[embedding]`や`[llm]`は
  機能を無効にする設定ではなく、必須キーが足りない設定として拒否されます。
- 外部APIの秘密情報はTOMLへ直接書きません。`authorization_token_env`に環境変数名を指定します。
- `format_version = 2`が必要です。形式の版を表す別のキーは使用しません。

## 設定区分と利用するプログラム

| セクション | 主な利用者 | 必須条件 |
|---|---|---|
| 最上位、`[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[daemon]` | Cのコマンド、core、front | アプリケーション用TOMLでは必須です。 |
| `[metadata]` | 索引作成と検索 | 絞り込み対象がある場合に指定します。 |
| `[build]` | search-webの起動スクリプト、Wikipedia | 起動スクリプトから索引を作る場合に指定します。 |
| `[embedding]` | local-files、Wikipedia、search-web BFF | embeddingを使う処理で必要です。 |
| `[web]` | search-web BFFとlauncher | 省略時は既定値を使います。 |
| `[llm]` | search-web BFF | 回答生成を使う場合だけ指定します。 |
| `[usage_log]` | Python adapterとBFF | API利用量を記録する場合に指定します。 |
| `[mock]` | search-webのテストとデモ | 通常運用では省略します。 |
| `[input]`、`[output]`、`[prepare]` | local-files | 対応する処理で使います。 |
| `[extract]`、`[formatters]` | local-files | 文書抽出を調整する場合に使います。 |

## 最上位の設定

| キー | 型・既定値 | 説明 |
|---|---|---|
| `format_version` | 整数、必須、`2`固定 | アプリケーション設定と索引の形式を表す版です。 |
| `collection_id` | 文字列、任意 | local-filesが文書の集合を識別するために使います。C製品は無視します。 |

## `[index]`

| キー | 型・既定値 | 説明 |
|---|---|---|
| `directory` | 空でない文字列、必須 | 索引ディレクトリです。設定ファイルからの相対パスを使用できます。解決後のパスは4095バイトまでです。 |

## `[tokenizer]`と`[chunking]`

| キー | 型・既定値 | 説明 |
|---|---|---|
| `tokenizer.id` | 1〜255バイトの文字列、`unicode_nfkc_casefold_v2` | 正規化とトークン分割規則の識別子です。 |
| `chunking.max_chars` | 1〜1048576の整数、`1200` | 1つのパッセージに含めるUnicode文字数の上限です。 |
| `chunking.overlap_chars` | 0以上の整数、`200` | 隣接するパッセージで重ねる文字数の上限です。`max_chars`未満にします。 |

これらは索引の互換性に含まれます。値を変えた場合は元文書から索引を作り直します。

## `[vector]`

`[vector]`は索引に保存するベクトルの形式を定めます。外部サービスへの接続設定ではありません。

| キー | 型・既定値 | 説明 |
|---|---|---|
| `enabled` | 真偽値、必須 | `true`の場合だけベクトル用コンポーネントを作成します。 |
| `model_id` | 1〜255バイトの文字列 | `enabled = true`の場合に必須です。索引と検索ベクトルの互換性を表す識別子です。 |
| `dimensions` | 1〜65536の整数 | `enabled = true`の場合に必須です。ベクトルの要素数です。 |
| `metric` | `cosine`、`dot`、`l2` | `enabled = true`の場合に必須です。距離の計算方法です。 |

`enabled = false`の場合は`model_id`、`dimensions`、`metric`を書きません。

## `[metadata]`

| キー | 型・既定値 | 説明 |
|---|---|---|
| `filterable_fields` | 文字列配列、`[]` | 絞り込み可能なJSONパスです。最大64件、各127バイトまでで、重複は指定できません。 |

指定したフィールドだけがメタデータ索引へ保存され、`/v2/search`と`/v2/retrieve`の`filter`から参照できます。
`search`コマンドにはフィルターを指定するオプションがありません。
入れ子の値は`author.country`のようにピリオドで区切ります。先頭と末尾のピリオド、連続するピリオド、制御文字は
使用できません。

## `[daemon]`

| キー | 型・既定値 | 説明 |
|---|---|---|
| `run_directory` | 文字列、必須 | PIDとログを保存します。解決後のパスは4095バイトまでです。 |
| `core_host` | 1〜255バイトの文字列、必須 | coreの待ち受けアドレスです。 |
| `core_port` | 1〜65535、必須 | coreの内部プロトコル用ポートです。 |
| `front_host` | 1〜255バイトの文字列、必須 | frontの待ち受けアドレスです。 |
| `front_port` | 1〜65535、必須 | frontのHTTPポートです。 |
| `max_inflight` | 1〜1024、`4` | 同時処理するHTTPリクエスト数の上限です。 |
| `max_inflight_bytes` | 1〜1073741824、`4194304` | 同時に受け付ける本文の合計バイト数の上限です。 |
| `request_timeout_ms` | 1〜60000、`5000` | frontとcoreの間のソケット処理を待つ時間です。 |
| `write_token` | 16〜255バイト、任意 | 更新APIのBearerトークンです。空白と制御文字は使えません。 |

`write_token`はYappod2自身の更新認証です。外部API secretに使う`authorization_token_env`とは用途が異なります。

## `[embedding]`

`[embedding]`はパッセージや検索文をベクトルへ変換するHTTP APIへの接続です。Cの検索APIはこのセクションを
読み取って外部通信を行いません。

| キー | 型・既定値 | 説明 |
|---|---|---|
| `directory` | パス、local-filesの`embed`で必須 | local-filesがベクトル付き分割ファイルを保存するディレクトリです。search-webは利用しません。 |
| `provider` | `lmstudio`、`ollama`、`openai`。必須 | 接続先ごとのリクエストとレスポンスの形式を選びます。 |
| `base_url` | HTTP(S) URL | 接続先の基準URLです。`endpoint_url`とどちらか一方だけを指定します。 |
| `endpoint_url` | HTTP(S) URL | embedding用エンドポイント全体です。`base_url`とどちらか一方だけを指定します。 |
| `model` | 空でない文字列、必須 | 接続先へ送る実際のモデル名です。 |
| `model_id` | 文字列 | local-filesでは必須です。search-webでは任意ですが、指定時は`vector.model_id`と一致させます。 |
| `dimensions` | 1〜65536 | local-filesでは必須です。search-webの既定値は`768`です。`vector.dimensions`と一致させます。 |
| `prompt_profile` | `plain`、`embeddinggemma` | 入力文の組み立て方です。既定値は`plain`です。 |
| `authorization_token_env` | 環境変数名、任意 | Bearerトークンを読む環境変数です。 |
| `timeout_ms` | 1000〜600000、`60000` | 1回のembeddingリクエストを待つ時間です。 |
| `batch_size` | 1〜1024、`16` | 1回のリクエストへまとめる入力件数です。 |

## `[web]`、`[llm]`、`[usage_log]`、`[mock]`

| キー | 型・既定値 | 説明 |
|---|---|---|
| `web.host` | 文字列、`127.0.0.1` | BFFの待ち受けアドレスです。 |
| `web.port` | 1〜65535、`4173` | BFFとWeb UIのポートです。 |
| `web.yappod_timeout_ms` | 1〜600000、`5000` | BFFが`yappod_front`の応答を待つ時間です。 |
| `web.startup_timeout_ms` | 100〜600000、`8000` | 起動スクリプトが各プロセスの起動完了を待つ時間です。 |
| `llm.base_url` | HTTP(S) URL、必須 | OpenAI互換Chat Completionsの基準URLです。 |
| `llm.model` | 文字列、必須 | 回答生成に使うモデル名です。 |
| `llm.effort` | 文字列、任意 | 接続先へ`reasoning_effort`として渡します。値の妥当性は接続先が判断します。 |
| `llm.max_tokens` | 1〜131072、`8192` | 接続先へ送る出力トークン数の上限です。 |
| `llm.authorization_token_env` | 環境変数名、任意 | Bearerトークンを読む環境変数です。 |
| `llm.timeout_ms` | 1000〜600000、`30000` | LLMの応答を待つ時間です。 |
| `usage_log.path` | パス、任意 | APIの利用量をNDJSONで追記するパスです。 |
| `mock.enabled` | 真偽値、`false` | ローカルの模擬サーバーを起動します。テストとデモ専用です。 |
| `mock.host` | 文字列、`127.0.0.1` | 模擬サーバーの待ち受けアドレスです。 |
| `mock.port` | 1〜65535、`1234` | 模擬サーバーのポートです。 |
| `mock.model` | 文字列、`yappod-demo-mock` | 模擬サーバーが受理するモデル名です。 |
| `mock.answer` | 文字列 | 模擬サーバーが返す回答本文です。 |
| `mock.embedding_dimensions` | 1〜65536、`3` | 模擬ベクトルの次元数です。 |

## `[build]`とlocal-files固有のセクション

| キー | 型・既定値 | 説明 |
|---|---|---|
| `build.input` | パス、任意 | search-webの起動スクリプトが索引作成へ渡すNDJSONです。索引がなく、自動作成する場合に必要です。 |
| `build.yappo_makeindex` | パス、任意 | 起動スクリプトまたはlocal-filesが実行する`yappo_makeindex`です。索引を作成する処理で必要です。 |

local-filesの`[input]`、`[output]`、`[prepare]`、`[extract]`、`[formatters]`は
[local-filesの設定](../examples/local-files/docs/configuration.md)で、実際の収集手順と一緒に説明しています。

## 語彙索引からベクトル対応索引へ移行する

1. 元文書からパッセージを生成します。
2. 索引で利用するモデルから各パッセージのembeddingを生成します。
3. パッセージ順の`vectors`を含む正式な入力NDJSONを作成します。
4. `[vector]`を完全に設定し、必要なら`[embedding]`も設定します。
5. 既存索引とは別の`[index].directory`へ`yappo_makeindex build`を実行します。
6. 新しい索引の`config.toml`と`manifest.json`にベクトル用コンポーネントがあることを確認します。
7. アプリケーション用TOMLの`[index].directory`を新しい索引へ切り替えます。

既存索引を設定変更だけで変換したり、既存ディレクトリへ上書きして索引を作ったりはできません。
