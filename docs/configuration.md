# 設定リファレンス

Yappod2のサンプルでは、索引、サーバー、Web画面、埋め込みサービス、LLMの設定を1つのTOMLへまとめられます。ただし、各プログラムが読む項目は異なります。この文書では、すべての設定キーについて、読み取るプログラム、必須条件、型、デフォルト、値の選び方、ほかの設定との関係を説明します。

設定表は「キー」「データ型」「入力可能値」「デフォルト値」「必須」「説明」の順です。「デフォルト値」が「なし」のキーは、省略時に値が補われません。バッククォートは設定ファイルへ記述する値やキーを示すMarkdown表記であり、それ自体がデフォルトを表すものではありません。

## 最初に理解しておくこと

設定ファイルには、用途が異なる二つの情報が含まれます。

- 索引形式の設定は、`[tokenizer]`、`[chunking]`、`[vector]`、`[metadata]`です。索引を作ると、適用済みの値が`<index-directory>/config.toml`へ保存されます。
- アプリケーションの設定は、`[index]`、`[daemon]`、`[web]`、`[embedding]`、`[llm]`などです。接続先や実行時の上限を定めます。これらは索引内の`config.toml`へ保存されません。

既存索引を開くときは、アプリケーション用TOMLの`[index].directory`をたどり、索引内の`config.toml`と`manifest.json`を読みます。アプリケーション用TOMLで`[vector].enabled = true`へ変更しても、既存索引にベクトル用ファイルは追加されません。

## 共通規則

- アプリケーション用TOMLの相対パスは、そのTOMLが置かれているディレクトリを基準に解決します。コマンドを実行したカレントディレクトリは基準になりません。
- Yappod2コマンドで使うTOMLには、`format_version`、`[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[daemon]`が必要です。`[metadata]`は省略できます。
- 任意機能を使わない場合は、セクションを見出しごと省略します。空の`[embedding]`や`[llm]`は「無効」を意味せず、必須キーが欠けた設定として拒否されます。
- `schema_version`は使用しません。現在の形式を表すキーは`format_version = 2`です。
- 外部APIのトークンをTOMLへ直接書く`authorization_token`は拒否されます。`authorization_token_env`へ環境変数名を書きます。
- `yappo_makeindex`、`search`、`yappo_compact`、`yappod_core`、`yappod_front`は、自身が読むセクション内の未知のキーを拒否します。search-webサーバーは最上位と各対象セクションの未知のキーも拒否します。
- 文字列の長さは、特記がない限りUTF-8にしたときのバイト数で判定します。

## どのプログラムが何を読むか

| 設定 | 主に読み取るプログラム | 用途 |
|---|---|---|
| 最上位、`[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[metadata]`、`[daemon]` | `yappo_makeindex`、`search`、`yappo_compact`、`yappod_core`、`yappod_front` | 索引の場所と形式、サーバーの動作を決めます。 |
| `[build]` | search-webの起動スクリプト、local-files、Wikipedia | 索引作成プログラムと入力ファイルを指定します。 |
| `[embedding]` | local-files、Wikipedia、search-webサーバー | 文または本文断片をベクトルへ変換する外部HTTP APIを指定します。Yappod2コマンドは読みません。 |
| `[web]` | search-webサーバー、search-webの起動スクリプト | Webサーバーと起動待ち時間を指定します。 |
| `[llm]` | search-webサーバー | RAG回答を生成するOpenAI互換APIを指定します。 |
| `[usage_log]` | local-files、Wikipedia、search-webサーバー | 外部APIの利用量をNDJSONへ記録します。 |
| `[mock]` | search-webサーバー、search-webの起動スクリプト | テスト用の模擬LLM・埋め込みサーバーを指定します。 |
| `[input]`、`[output]`、`[prepare]`、`[extract]`、`[formatters]` | local-files | ファイル収集、抽出、成果物を指定します。 |

## 最上位のキー

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `format_version` | 整数 | `2` | なし | Yappod2コマンドでは必須 | 設定と索引形式の版です。local-filesとWikipediaのPython処理はこの値自体を検証しませんが、同じTOMLをYappod2コマンドへ渡すため、サンプル設定にも`2`を記載します。 |
| `collection_id` | 文字列 | 1〜32文字。英字、数字、`.`、`_`、`-` | なし | local-filesでは必須 | 収集した文書集合を識別し、文書IDとメタデータへ反映します。Yappod2コマンドとsearch-webはこの値を索引設定としては使用しません。 |

## `[index]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `directory` | 文字列 | 空でない文字列。TOMLからの相対パスを解決した後で4095バイト以下 | なし | Yappod2コマンドとsearch-webの起動スクリプトでは必須 | 索引を作成または読み込むディレクトリです。`build`では、まだ存在しないディレクトリを指定します。検索、更新、サーバー起動では、`config.toml`と`manifest.json`を持つ作成済みの索引を指定します。 |

たとえば、現在`./index-lexical`を使用していて、ベクトル検索に対応した索引を`./index-vector`へ新しく作った場合は、
アプリケーション用TOMLを`directory = "./index-vector"`へ変更してサーバーを起動し直します。
`./index-lexical/config.toml`だけを書き換えても、ベクトル検索に必要なファイルは作成されません。索引の構成を変える場合は、
元の文書から別のディレクトリへ索引を作成してください。

## `[tokenizer]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `id` | 文字列 | 1〜255バイト | `unicode_nfkc_casefold_v2` | 任意。ただし`[tokenizer]`セクション自体は必須 | 文書と検索文を同じ方法で正規化・分割したことを確認するための識別子です。現在はUnicode NFKC CasefoldとICUの単語境界を使う処理に固定されています。値を変更しても別の処理には切り替わりませんが、索引との互換性が変わるため索引の作り直しが必要です。 |

リポジトリ内の古いテスト用設定には`unicode_nfkc_cf_v1`もあります。新しく作る設定では、デフォルトの`unicode_nfkc_casefold_v2`を使用してください。任意の文字列で別のトークナイザー実装へ切り替わるわけではありません。

## `[chunking]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `max_chars` | 整数 | 1〜1048576 | `1200` | 任意。ただし`[chunking]`セクション自体は必須 | 一つの本文断片へ入れるUnicode文字数の上限です。短くすると検索結果の引用単位は細かくなりますが、本文断片数とベクトル数が増えます。 |
| `overlap_chars` | 整数 | 0以上かつ`max_chars`未満 | `200` | 任意。ただし`[chunking]`セクション自体は必須 | 隣り合う本文断片の重なりです。段落境界をまたぐ検索漏れを減らすために使います。0にすると重複させません。 |

両方の値は索引の互換条件です。変更後は元の文書と、必要なら埋め込みを使って新しい索引を作ります。

## `[vector]`

`[vector]`は索引へ保存するベクトルの形式を定めます。埋め込みAPIのURLや実際に呼び出すモデルを定める設定ではありません。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `enabled` | 真偽値 | `true`、`false` | なし | 必須 | `true`なら`vectors.yap2`と`vectors.usearch`を作ります。`false`なら語彙検索用の索引だけを作ります。 |
| `model_id` | 文字列 | 1〜255バイト | なし | `enabled = true`の場合は必須 | Yappod2が索引と検索ベクトルの互換性を識別する名前です。提供元のモデル名と同じである必要はありませんが、運用中に意味が変わらない名前を付けます。 |
| `dimensions` | 整数 | 1〜65536 | なし | `enabled = true`の場合は必須 | 各ベクトルの要素数です。入力NDJSON、埋め込みレスポンス、検索ベクトルのすべてが一致する必要があります。 |
| `metric` | 文字列 | `cosine`、`dot`、`l2` | なし | `enabled = true`の場合は必須 | 類似度の計算方法です。モデルの推奨値に合わせます。通常の正規化済み文章埋め込みでは`cosine`を選びます。 |

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

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `filterable_fields` | 文字列の配列 | 最大64件。各要素は1〜127バイト | `[]` | 任意 | 絞り込み用のメタデータ索引へ保存するJSONパスです。`language`や`author.country`のように指定します。 |

同じ値を重複して指定できません。`.`はJSONオブジェクトをたどる区切りとして扱うため、`author.country`は`metadata.author.country`を参照します。先頭または末尾の`.`、連続する`.`、制御文字、`/`、`\`は拒否されます。読込時にUTF-8のバイト順へ並べ替えて保存するため、記載順は索引の互換性へ影響しません。対象値が配列なら、配列内の`null`、真偽値、数値、文字列を別々の値として索引へ保存します。オブジェクトと入れ子配列は直接の比較値にはなりません。

この設定は`QUERY /v2/search`と`QUERY /v2/retrieve`の`filter`で使います。互換用の`POST`でも同じです。`search`コマンドにはfilterを指定するオプションがありません。フィールドを後から追加する場合は、元文書から索引を作り直します。

## `[daemon]`

Yappod2コマンドの共通アプリケーション設定ではセクションと接続先のキーが必須です。search-webだけで解析する場合は表のデフォルトが使われますが、Yappod2サーバーも同じTOMLを読むため、通常はすべて明記してください。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `run_directory` | 文字列 | 空でない文字列。TOMLからの相対パスを解決した後で4095バイト以下 | search-webでは`./run`。Yappod2コマンドではなし | Yappod2サーバーでは必須 | core/frontの`*.pid`、`*.log`、`*.error`を置きます。search-webの起動スクリプトもプロセス管理に使います。 |
| `core_host` | 文字列 | 1〜255バイトのホスト名またはIPアドレス | search-webでは`127.0.0.1`。Yappod2コマンドではなし | Yappod2サーバーでは必須 | coreの待ち受け先であり、frontの接続先です。アプリケーションTOMLを使わず`--index`だけでcoreを起動した場合は待ち受けホストを指定できません。 |
| `core_port` | 整数 | 1〜65535 | search-webでは`18401`。Yappod2コマンドではなし | Yappod2サーバーでは必須 | frontからcoreへ検索や更新を依頼する内部HTTP/1.1ポートです。外部クライアントには公開しません。 |
| `front_host` | 文字列 | 1〜255バイトのホスト名またはIPアドレス | search-webでは`127.0.0.1`。Yappod2コマンドではなし | Yappod2サーバーでは必須 | frontの待ち受け先であり、search-webサーバーの接続先です。アプリケーションTOMLを使わず`--index`だけでfrontを起動した場合は待ち受けホストを指定できません。 |
| `front_port` | 整数 | 1〜65535 | search-webでは`18400`。Yappod2コマンドではなし | Yappod2サーバーでは必須 | frontのHTTPポートです。 |
| `max_inflight` | 整数 | 1〜1024 | `4` | 任意 | frontとcoreが、それぞれ同時に処理中として保持するリクエストの件数上限です。どちらかで上限に達すると`503 overloaded`になります。ヘルスチェックとメトリクスはこの制限の対象外です。 |
| `max_inflight_bytes` | 整数 | 1〜1073741824 | `4194304` | 任意 | frontは処理中のHTTP本文、coreは処理中の検索・更新データについて、それぞれの合計バイト数を制限します。1件の大きさが残量を超える場合も`503 overloaded`になります。 |
| `request_timeout_ms` | 整数 | 1〜60000 | `5000` | 任意 | frontが受理したクライアントソケット、frontのlibcurlによるcoreへの接続と要求全体、coreが受理したソケットの送受信期限です。LLMを待つ時間ではありません。 |
| `write_token` | 文字列 | 16〜255バイト。空白文字と制御文字は不可 | なし | 任意 | `POST /v2/documents:batch`をBearer認証します。省略時は更新APIを認証なしで受け付けます。外部API用の`authorization_token_env`とは別の機能です。 |

## `[build]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `yappo_makeindex` | 文字列 | 実行可能な`yappo_makeindex`のパス | なし | local-filesの`build`では必須。search-webでは索引が存在しない場合に必須 | 起動スクリプトまたはPythonアダプターが実行します。相対パスはTOML基準です。 |
| `input` | 文字列 | 読み取り可能な文書NDJSONのパス | なし | search-webが索引を自動作成する場合は必須 | search-webの起動スクリプトは、`[index].directory`に索引がない場合、このファイルを`yappo_makeindex build`へ渡します。作成済みの索引だけを使う場合は省略できます。 |

## `[embedding]`

`[embedding]`は外部サービスへ接続して入力文をベクトルに変換する設定です。`yappod_core`と`yappod_front`はこのセクションを読みません。search-webサーバー、local-files、Wikipediaアダプターが使用します。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `directory` | 文字列 | 空でないパス | なし | local-filesの`embed`と`hybrid`では必須 | local-filesが埋め込み済みの分割NDJSON、マニフェスト、再開位置を保存します。search-webとWikipediaは値を無視します。 |
| `provider` | 文字列 | `lmstudio`、`ollama`、`openai` | なし | 必須 | 要求本文と応答の解釈方法を選びます。OpenAI互換APIなら`openai`または接続先に合わせた値を選びます。 |
| `base_url` | 文字列 | クエリー文字列とフラグメントを含まないHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | `endpoint_url`を指定しない場合は必須 | 基準URLです。`ollama`では`/api/embed`、それ以外では`/embeddings`を末尾へ付けます。末尾の`/`は除去されます。 |
| `endpoint_url` | 文字列 | 埋め込みAPI全体を表すHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | `base_url`を指定しない場合は必須 | 独自のパスを使う接続先で使用します。`base_url`とは併用できません。 |
| `model` | 文字列 | 空でない文字列 | なし | 必須 | プロバイダーへ送る実際のモデル名です。これはYappod2側の互換識別子ではありません。 |
| `model_id` | 文字列 | 1〜255バイト | なし | local-filesでは必須。search-webとWikipediaでは任意 | `[vector].model_id`と一致させるYappod2側の識別子です。search-webでは指定時に索引のモデルIDと照合します。 |
| `dimensions` | 整数 | 1〜65536 | search-webでは`768`。Wikipediaでは`vector.dimensions`の値。local-filesではなし | local-filesでは必須。search-webとWikipediaでは任意 | プロバイダーが返すベクトルの要素数です。`[vector].dimensions`と一致させます。 |
| `prompt_profile` | 文字列 | `plain`、`embeddinggemma` | `plain` | 任意 | `plain`は入力をそのまま送ります。`embeddinggemma`は検索文と文書に用途別の指示を付けます。索引作成と検索で同じ方針を使います。 |
| `authorization_token_env` | 文字列 | `[A-Za-z_][A-Za-z0-9_]*`に一致する環境変数名 | なし | 任意 | 指定した環境変数からBearerトークンを読みます。環境変数が未設定または空なら起動・処理に失敗します。 |
| `timeout_ms` | 整数 | search-webとWikipediaでは1000〜600000。local-filesでは1以上 | `60000` | 任意 | 埋め込みAPIへのHTTP要求1回を待つ上限です。全バッチを処理する総時間ではありません。同じ設定を複数のサンプルで共有する場合は、1000〜600000の範囲に収めます。 |
| `batch_size` | 整数 | 1〜1024 | `16` | 任意 | 1回のHTTPリクエストへまとめる入力件数です。接続先の上限、メモリー量、応答時間に合わせて調整します。 |

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

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `host` | 文字列 | 空でないホスト名またはIPアドレス | `127.0.0.1` | 任意 | search-webサーバーが待ち受けるアドレスです。外部へ公開する場合は、認証やリバースプロキシも含めて別途保護してください。 |
| `port` | 整数 | 1〜65535 | `4173` | 任意 | Web UIとsearch-webサーバーのAPIのポートです。 |
| `yappod_timeout_ms` | 整数 | 1〜600000 | `5000` | 任意 | search-webサーバーがfrontのHTTP応答を待つ上限です。front/core間の`daemon.request_timeout_ms`とは別です。 |
| `startup_timeout_ms` | 整数 | 100〜600000 | `8000` | 任意 | search-webの起動スクリプトがsearch-webサーバーと、必要なら模擬サービスのヘルスチェックを待つ上限です。coreのPID確認とfrontの準備完了確認は、これとは別の固定回数で繰り返します。検索やLLMの応答時間には使いません。 |

## `[llm]`

このセクションが存在すると、search-webサーバーはOpenAI互換の`/chat/completions`へRAG回答生成を依頼します。使わない場合はセクション全体を省略します。

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `base_url` | 文字列 | クエリー文字列とフラグメントを含まないHTTPS URL。HTTPはlocalhost、ループバックアドレス、プライベートネットワークだけ | なし | 必須 | APIの基準URLです。末尾の`/`は除去され、search-webサーバーは`/chat/completions`を付けます。 |
| `model` | 文字列 | 空でない文字列 | なし | 必須 | Chat Completionsへ送るモデル名です。 |
| `effort` | 文字列 | 接続先が`reasoning_effort`として受理する空でない文字列 | なし | 任意 | 値を`reasoning_effort`としてそのまま送ります。Yappod2側は列挙値を検証しません。 |
| `max_tokens` | 整数 | 1〜131072 | `8192` | 任意 | 接続先へ明示する最大出力トークン数です。入力トークン数の上限ではありません。モデルが扱える文脈長の上限も接続先で適用されます。 |
| `authorization_token_env` | 文字列 | `[A-Za-z_][A-Za-z0-9_]*`に一致する環境変数名 | なし | 任意 | Bearerトークンを環境変数から読みます。TOMLへトークンそのものは書けません。 |
| `timeout_ms` | 整数 | 1000〜600000 | `30000` | 任意 | search-webサーバーがLLMの1回の応答を待つ上限です。大きいモデルやコールドスタートでは、実測時間を確認して増やします。 |

現在のsearch-webサーバーは`temperature: 0`を常に送ります。`[llm].temperature`という設定キーは実装されていません。接続先の管理画面で別の値を設定しても、search-webサーバーのリクエストが優先されます。詳しくは[LLM連携](../examples/search-web/docs/llm-integration.md)を参照してください。

## `[usage_log]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `path` | 文字列 | 作成または追記可能なファイルのパス | なし | 利用量を記録する場合は必須 | 埋め込みまたはLLMレスポンスに含まれる利用量を1行1レコードのNDJSONで追記します。相対パスはTOML基準です。 |

local-filesでは`[usage_log]`を記述した場合に`path`が必要です。空のセクションを置かず、記録しない場合はセクション全体を省略してください。

## `[mock]`

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `enabled` | 真偽値 | `true`、`false` | `false` | 任意 | `true`にするとsearch-webの起動スクリプトが模擬APIを起動します。テストとローカルデモ用です。 |
| `host` | 文字列 | 空でないホスト名またはIPアドレス | `127.0.0.1` | 任意 | 模擬APIの待ち受けアドレスです。 |
| `port` | 整数 | 1〜65535 | `1234` | 任意 | 模擬APIのポートです。 |
| `model` | 文字列 | 空でない文字列 | `yappod-demo-mock` | 任意 | 模擬Chat Completionsが受理するモデル名です。 |
| `answer` | 文字列 | 任意の文字列 | `参照資料から確認できる内容です。[1]` | 任意 | 模擬回答の本文です。 |
| `embedding_dimensions` | 整数 | 1〜65536 | `3` | 任意 | 模擬埋め込みが返すベクトルの次元数です。`[vector].dimensions`と合わせます。 |

## local-files固有の設定

local-filesの`[input]`、`[output]`、`[prepare]`、`[extract]`、`[formatters]`は、[local-files設定リファレンス](../examples/local-files/docs/configuration.md)で全キーと選び方を説明しています。

## タイムアウトの違い

| キー | データ型 | 入力可能値 | デフォルト値 | 必須 | 説明 |
|---|---|---|---|---|---|
| `daemon.request_timeout_ms` | 整数 | 1〜60000 | `5000` | 任意 | frontのlibcurlによるcoreへの接続と内部HTTP要求全体、およびcoreが受理したソケットの送受信期限です。core停止、内部処理の遅延、ネットワーク障害で期限を超える可能性があります。 |
| `web.yappod_timeout_ms` | 整数 | 1〜600000 | `5000` | 任意 | search-webサーバーがfrontの検索・取得・登録のHTTP応答を待つ時間です。 |
| `web.startup_timeout_ms` | 整数 | 100〜600000 | `8000` | 任意 | search-webの起動スクリプトがsearch-webサーバーと任意の模擬サービスを確認する時間です。 |
| `embedding.timeout_ms` | 整数 | search-webとWikipediaでは1000〜600000。local-filesでは1以上 | `60000` | 任意 | 埋め込みAPIの1リクエストを待つ時間です。接続先停止、大きいバッチ、モデルの読み込み待ちで期限を超える可能性があります。 |
| `llm.timeout_ms` | 整数 | 1000〜600000 | `30000` | 任意 | Chat Completionsの1リクエストを待つ時間です。長いRAG入力、コールドスタート、生成時間で期限を超える可能性があります。 |
| `extract.tika_timeout_ms` | 整数 | 1以上 | `30000` | 任意 | Tikaコマンド1回を待つ時間です。 |
| `formatters.rules[].timeout_ms` | 整数 | 1以上 | `30000` | 任意 | 独自フォーマッターコマンド1回を待つ時間です。 |

タイムアウトを大きくする前に、どの境界で時間を使っているかをログで確認してください。

## 語彙索引からベクトル対応索引へ移行する

設定変更だけでは移行できません。次の順序で別の索引を作ります。

1. 元文書から、現在の`[chunking]`に従う本文断片を作ります。
2. `[embedding]`で指定した実モデルから、各本文断片の埋め込みを生成します。
3. 本文断片順と同じ順序の`vectors`を各`upsert`へ加えたNDJSONを作ります。
4. `[vector]`へ`model_id`、`dimensions`、`metric`を設定します。
5. 既存索引とは異なる`[index].directory`を指定して`yappo_makeindex build`を実行します。
6. 新しい`config.toml`でベクトル設定を、`manifest.json`で`vectors.yap2`と`vectors.usearch`を確認します。
7. `yappo_makeindex verify`を実行します。
8. アプリケーション用TOMLの`[index].directory`を新しい索引へ切り替えます。

## 完成した設定例

語彙検索だけを使う設定は[`examples/config.lexical.toml`](../examples/config.lexical.toml)を参照してください。3次元の固定ベクトルでベクトルと複合検索を動作確認する設定は[`examples/config.toml`](../examples/config.toml)です。実際の埋め込みサービスを使う場合は、上記の`[vector]`と`[embedding]`の例を同じTOMLへ追加し、モデルIDと次元数を一致させてください。
