# Yappod2

Yappod2は、1台のサーバーで動作する検索エンジンです。UTF-8の文書を索引へ登録し、
語句による検索、ベクトルによる類似検索、両者を組み合わせた検索を利用できます。
長い本文を検索しやすい長さに分割して保存できるため、RAGでLLMへ渡す参照資料の取得にも利用できます。

Yappod2では、本文を設定した長さで分割した一部分を「本文断片」と呼びます。設定ファイルやAPIでは、この本文断片を
`passage`または`passages`と表記します。文書単位の検索では文書ごとに結果をまとめ、本文断片単位の検索では分割した
各部分を別々の結果として返します。

## サーバーの構成

HTTP APIを提供するときは、`yappod_core`と`yappod_front`の二つのサーバーを起動します。

| サーバー | 役割 |
|---|---|
| `yappod_core` | 索引ファイルを開き、検索、本文断片の取得、文書の追加・更新・削除を実行します。外部向けのHTTP APIは提供しません。 |
| `yappod_front` | 利用者からHTTPリクエストを受け付け、内容を検証して`yappod_core`へ処理を依頼します。ヘルスチェックとPrometheus用メトリクスも提供します。 |

二つに分かれているため、HTTPの受付処理と索引を扱う処理を別々に監視し、負荷を制限できます。通常のアプリケーションは
`yappod_front`のHTTP APIへ接続します。`yappod_core`のポートは`yappod_front`との通信専用であり、ブラウザーや
一般のHTTPクライアントから直接接続するものではありません。frontとcoreの間ではHTTP/1.1を使用し、frontは
内部HTTPクライアントにlibcurlを使用します。検索とRAG向け取得はRFC 10008の`QUERY`、文書更新は`POST`で送ります。

索引は複数の「セグメント」から構成されます。セグメントは、ある時点で登録された文書、検索用の語句一覧、本文断片、
ベクトルなどを一組のファイルとして保存したものです。更新時は既存ファイルを直接書き換えず、新しいセグメントを追加します。
`manifest.json`には、現在の検索で使用するセグメントと索引の世代番号が記録されます。

## 主な機能

- 題名と本文の重要度を考慮したBM25Fによる語彙検索
- USearchを使ったベクトルの近似検索と、小規模データ向けの全件比較
- 語彙検索とベクトル検索の順位を組み合わせる複合検索
- 文書単位と本文断片単位の検索
- メタデータフィルター、フレーズ検索、カーソルによる続きの取得
- RAG向けの本文断片と出典情報の取得
- NDJSONによる文書の追加、更新、削除
- 変更しないセグメントのコンパクションと破損検証
- `yappod_front`が提供するHTTP APIとPrometheusテキスト形式のメトリクス

正式な索引形式はv2だけです。Berkeley DBを使った旧索引、旧検索プロトコル、旧コマンドとの互換性は
ありません。旧索引を利用している場合は、元文書から新しい索引を作成してください。

## 必要な環境

- CMake 3.20以上
- CおよびC++コンパイラー
- cmocka
- ICU4C
- libcurl
- libevent

macOSではHomebrewで依存ライブラリをインストールできます。

```sh
brew install cmake cmocka icu4c libevent curl
```

Ubuntuでは次のパッケージをインストールします。

```sh
sudo apt-get update
sudo apt-get install -y cmake g++ libcmocka-dev libicu-dev \
  libcurl4-openssl-dev libevent-dev
```

## ビルドとテスト

リポジトリのルートディレクトリで実行します。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

インストールする場合は次のように実行します。

```sh
cmake --install build --prefix "$HOME/yappod2"
```

依存関係とインストール内容は[インストール手順](INSTALL)で説明しています。

## 最初の索引を作成する

まず、語句による検索だけを使う小さな索引を作成します。設定例では索引を
`examples/index-lexical`へ作成します。

```sh
./build/yappo_makeindex build \
  --config examples/config.lexical.toml \
  --input examples/documents.lexical.ndjson
```

成功すると、索引ディレクトリに`config.toml`、`manifest.json`、`segments/`が作成されます。
出力先がすでに存在する場合は上書きしません。

```sh
./build/search \
  --config examples/config.lexical.toml \
  --mode lexical \
  --query "modern search"
```

`--scope documents`がデフォルトです。この指定では、一つの文書から複数の本文断片が一致しても文書ごとに結果を
まとめます。RAG用に一致した本文断片を一つずつ確認したい場合は`--scope passages`を指定します。
`--limit`には1から100までを指定でき、デフォルトは10です。

## ベクトルを使った検索

`examples/config.toml`は3次元の動作確認用ベクトルを使う設定です。Yappod2は文書の本文を`[chunking]`の設定に従って
複数の本文断片へ分割します。入力NDJSONの`vectors`には、先頭の本文断片から順番に対応するベクトルを指定します。

```sh
./build/yappo_makeindex build \
  --config examples/config.toml \
  --input examples/documents.ndjson
```

類似検索では、索引と同じ次元の検索ベクトルを渡します。

```sh
./build/search \
  --config examples/config.toml \
  --mode vector \
  --vector "1,0,0"
```

語句とベクトルの両方を使う場合は次のように実行します。

```sh
./build/search \
  --config examples/config.toml \
  --mode hybrid \
  --query "modern search" \
  --vector "1,0,0"
```

Yappod2の検索APIは、検索文を外部の埋め込みサービスへ送信しません。実際のアプリケーションでは、
索引作成時と同じモデルから検索ベクトルを生成して渡してください。search-webでは
`[embedding]`を設定すると、この処理をsearch-webサーバーが担当します。

設定ファイルで`[vector].enabled = true`へ変更しても、既存の語彙索引にベクトルは追加されません。
ベクトルを含むNDJSONを用意し、新しいディレクトリへ索引を作成する必要があります。詳しくは
[設定リファレンス](docs/configuration.md)を参照してください。

## 設定ファイル

`yappo_makeindex`、`search`、`yappo_compact`、`yappod_core`、`yappod_front`は、同じアプリケーション用
TOMLを`--config`で読みます。主なセクションは次のとおりです。

| セクション | 用途 |
|---|---|
| `[index]` | 索引ディレクトリを指定します。 |
| `[tokenizer]`、`[chunking]` | 文字列の正規化と本文断片分割を指定します。 |
| `[vector]` | 索引へ保存するベクトルの互換条件を指定します。 |
| `[metadata]` | 検索時に絞り込み可能なメタデータのフィールドを指定します。 |
| `[daemon]` | coreとfrontの接続先、PID、ログ、処理上限を指定します。 |
| `[embedding]` | サンプルが利用する埋め込みAPIへの接続を指定します。 |
| `[web]`、`[llm]` | search-webと回答生成サービスを指定します。 |

相対パスは設定ファイルがあるディレクトリを基準に解決します。各キーの型、デフォルト、範囲、利用する
プログラムは[設定リファレンス](docs/configuration.md)にまとめています。

### 語彙検索用の完成した設定

次は索引作成、直接検索、core/frontの起動に使える最小構成です。`directory`と`run_directory`は、このTOMLを
置く場所に合わせて変更してください。

```toml
format_version = 2

[index]
directory = "./index"

[tokenizer]
id = "unicode_nfkc_casefold_v2"

[chunking]
max_chars = 1200
overlap_chars = 200

[vector]
enabled = false

[metadata]
filterable_fields = ["category", "language"]

[daemon]
run_directory = "./run"
core_host = "127.0.0.1"
core_port = 18401
front_host = "127.0.0.1"
front_port = 18400
max_inflight = 4
max_inflight_bytes = 4194304
request_timeout_ms = 5000
```

`filterable_fields`は、入力の`metadata`にある同名フィールドを検索時の絞り込み対象へします。省略または空配列にすると、
メタデータ自体は文書へ保存しますが、`filter`条件には使えません。`write_token`を設定しない場合、文書更新APIには認証が
ありません。ループバックアドレス以外へfrontを公開する場合は、運用方針に合わせて認証とTLSも用意してください。

### ベクトル対応索引へ変更する設定

新しい索引ディレクトリを指定し、`[vector]`を次のように置き換えます。

```toml
[vector]
enabled = true
model_id = "my-embedding-768-v1"
dimensions = 768
metric = "cosine"
```

`model_id`は、同じ規則で生成したベクトルかをYappod2側で識別する名前です。外部APIへ送るモデル名ではありません。
入力の各ベクトルと検索ベクトルは`dimensions`個の有限値を持ち、`metric`は`cosine`、`dot`、`l2`のいずれかです。

search-webに検索文の埋め込みを生成させる場合は、さらに完全な`[embedding]`を追加します。

```toml
[embedding]
provider = "openai"
base_url = "https://embedding.example.test/v1"
model = "provider-model-name"
model_id = "my-embedding-768-v1"
dimensions = 768
prompt_profile = "plain"
timeout_ms = 60000
batch_size = 16
authorization_token_env = "EMBEDDING_API_KEY"
```

`model`はプロバイダーへ送る実際の名前、`model_id`は索引との互換性を確認する名前です。`model_id`と`dimensions`は
`[vector]`と一致させます。`authorization_token_env`にはトークンそのものではなく、トークンを保持する環境変数名を
指定します。語彙検索だけを使う場合は`[embedding]`を空で残さず、セクション全体を省略します。

## 正式な入力NDJSON

正式な入力は、1行に一つのJSONオブジェクトを書くUTF-8のNDJSONです。次の例は読みやすいように整形しています。
実際のNDJSONファイルでは、各オブジェクトを改行なしの1行にします。

```json
{
  "operation": "upsert",
  "id": "doc-1",
  "url": "https://example.test/1",
  "title": "検索入門",
  "body": "検索対象となる本文です。",
  "metadata": {
    "language": "ja"
  }
}
```

削除する場合は、`operation`を`delete`にして文書IDを指定します。

```json
{
  "operation": "delete",
  "id": "doc-2"
}
```

`upsert`では`id`と`body`が必須です。`url`、`title`、`metadata`、`updated_at_unix_ms`は省略できます。
ベクトル対応索引では、生成される本文断片数と同じ数の`vectors`が必要です。入力形式の詳細は
[索引作成](docs/indexing.md)を参照してください。

## 索引を更新する

```sh
./build/yappo_makeindex update \
  --config examples/config.lexical.toml \
  --input operations.ndjson
```

NDJSONの1行を1件の更新操作として扱い、`update`は1回の実行で1〜100件を受け付けます。たとえば、文書を
3件登録して2件削除する入力は5件です。入力全体を検証してから1つの世代として公開するため、途中の
行までだけが検索可能になることはありません。同じ文書IDを更新すると新しいセグメントが優先され、削除操作は
古い文書を検索結果から隠します。大量の初期データには、この上限がない`build`を使用してください。

不要になった古いレコードをまとめる場合はコンパクションを実行します。

```sh
./build/yappo_compact --config examples/config.lexical.toml
```

## 索引を検証する

コンポーネントの大きさ、チェックサム、内部構造をすべて確認する場合は次を実行します。

```sh
./build/yappo_makeindex verify --config examples/config.lexical.toml
```

通常の検索では毎回この全検証を行いません。起動時と手動検証の違いは
[索引の更新と保守](docs/index-lifecycle.md)で説明しています。

## HTTP APIを起動する

`yappod_core`を先に起動し、その後で`yappod_front`を起動します。役割の違いは
[サーバーの構成](#サーバーの構成)を参照してください。両プログラムはバックグラウンドで動作し、
`[daemon].run_directory`へPIDとログを保存します。

```sh
./build/yappod_core --config examples/config.lexical.toml
./build/yappod_front --config examples/config.lexical.toml
```

デフォルトではfrontが`127.0.0.1:18400`、coreが`127.0.0.1:18401`を使用します。

```sh
curl -sS --request QUERY \
  -H 'Content-Type: application/json' \
  --data @- \
  http://127.0.0.1:18400/v2/search <<'JSON'
{
  "query": "modern search",
  "mode": "lexical",
  "scope": "documents",
  "limit": 10
}
JSON
```

`QUERY /v2/search`と`QUERY /v2/retrieve`は本文を持つ安全で冪等な検索です。既存クライアントとの互換性のため、
公開front APIは同じパスへの`POST`も受理します。frontからcoreへは常に`QUERY`で転送します。

準備状態とメトリクスは別々に確認できます。

```sh
curl -sS http://127.0.0.1:18400/health/ready
curl -sS http://127.0.0.1:18400/metrics
```

停止用コマンドはないため、PIDが対象プロセスを指すことを確認してからfront、coreの順に`SIGTERM`を送ります。具体的な
手順とPID再利用への注意は[Yappod2サーバーの運用](docs/operations.md)を参照してください。

公開HTTP APIは[`yappod_front` APIリファレンス](docs/yappod-front-api.md)、front/core間の通信は
[frontとcoreの通信仕様](docs/yappod-core-protocol.md)で説明しています。

`GET /metrics`で公開する全メトリクス名、ラベル、ヒストグラムのバケット、Prometheusの収集設定、値の読み方は
[監視とメトリクス](docs/observability.md)を参照してください。

## サンプル

| サンプル | 用途 |
|---|---|
| [`examples/local-files`](examples/local-files/README.md) | 手元の文書やソースコードを収集して検索します。 |
| [`examples/wikipedia-search`](examples/wikipedia-search/README.md) | 日本語Wikipediaから検索・RAG用索引を作成します。 |
| [`examples/search-web`](examples/search-web/README.md) | 索引を検索・質問・更新するWeb UIを起動します。 |

選び方と必要な準備は[サンプル一覧](examples/README.md)を参照してください。

## 困ったときは

- 設定を読み込めない場合は、相対パス、必須セクション、未知のキーを確認してください。
- ベクトル検索に失敗する場合は、索引の`model_id`、`dimensions`、`metric`と検索ベクトルを確認してください。
- `yappod_core`または`yappod_front`が起動しない場合は、`[daemon].run_directory`にある`.error`と`.log`を確認してください。
- サンプルのエラーには`Reason`と`How to fix`が表示されます。詳しくは
  [サンプルの問題解決](examples/troubleshooting.md)を参照してください。

文書全体の案内は[ドキュメント一覧](docs/README.md)にあります。
