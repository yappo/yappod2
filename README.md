# Yappod2

Yappod2は、1台のサーバーで動作する全文検索エンジンです。UTF-8の文書を索引へ登録し、
語句による検索、ベクトルによる類似検索、両者を組み合わせた検索を利用できます。
検索結果から引用可能な本文断片を取得できるため、RAGの検索部分にも利用できます。

索引は作成後に変更しないセグメントと、世代管理されたmanifestで構成します。文書の追加・更新・削除は新しい
generationとして公開され、検索処理には公開前または公開後の完全な状態だけが見えます。

## 主な機能

- BM25Fによる語彙検索
- USearch HNSWと全件走査によるベクトル検索
- Reciprocal Rank Fusion（RRF）による複合検索
- 文書単位とパッセージ単位の検索
- メタデータフィルター、フレーズ検索、カーソルによる続きの取得
- RAG向けの本文断片と出典情報の取得
- NDJSONによる文書の追加、更新、削除
- 変更しないセグメントのコンパクションと破損検証
- `yappod_front`が提供するHTTP APIとPrometheus形式のメトリクス

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

`--scope documents`が既定値です。パッセージ単位で検索する場合は`--scope passages`を指定します。
`--limit`には1から100までを指定でき、既定値は10です。

## ベクトルを使った検索

`examples/config.toml`は3次元の動作確認用ベクトルを使う設定です。入力NDJSONの`vectors`には、
文書をチャンクへ分割した後のパッセージ順でベクトルを指定します。

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

Yappod2の検索APIは、検索文を外部のembeddingサービスへ送信しません。実際のアプリケーションでは、
索引作成時と同じモデルから検索ベクトルを生成して渡してください。search-webでは
`[embedding]`を設定すると、この処理をBFFが担当します。

設定ファイルで`[vector].enabled = true`へ変更しても、既存の語彙索引にベクトルは追加されません。
ベクトルを含むNDJSONを用意し、新しいディレクトリへ索引を作成する必要があります。詳しくは
[設定リファレンス](docs/configuration.md)を参照してください。

## 設定ファイル

`yappo_makeindex`、`search`、`yappo_compact`、`yappod_core`、`yappod_front`は、同じアプリケーション用
TOMLを`--config`で読みます。主なセクションは次のとおりです。

| セクション | 用途 |
|---|---|
| `[index]` | 索引ディレクトリを指定します。 |
| `[tokenizer]`、`[chunking]` | 文字列の正規化とパッセージ分割を指定します。 |
| `[vector]` | 索引へ保存するベクトルの互換条件を指定します。 |
| `[metadata]` | 検索時に絞り込み可能なメタデータのフィールドを指定します。 |
| `[daemon]` | coreとfrontの接続先、PID、ログ、処理上限を指定します。 |
| `[embedding]` | サンプルが利用するembedding APIへの接続を指定します。 |
| `[web]`、`[llm]` | search-webと回答生成サービスを指定します。 |

相対パスは設定ファイルがあるディレクトリを基準に解決します。各キーの型、既定値、範囲、利用する
プログラムは[設定リファレンス](docs/configuration.md)にまとめています。

## 正式な入力NDJSON

正式な入力は、1行に1操作を書くUTF-8のNDJSONです。

```json
{"operation":"upsert","id":"doc-1","url":"https://example.test/1","title":"検索入門","body":"検索対象となる本文です。","metadata":{"language":"ja"}}
{"operation":"delete","id":"doc-2"}
```

`upsert`では`id`と`body`が必須です。`url`、`title`、`metadata`、`updated_at_unix_ms`は省略できます。
ベクトル対応索引では、生成されるパッセージ数と同じ数の`vectors`が必要です。入力形式の詳細は
[索引作成](docs/indexing.md)を参照してください。

## 索引を更新する

```sh
./build/yappo_makeindex update \
  --config examples/config.lexical.toml \
  --input operations.ndjson
```

1回の更新では最大100操作を不可分に公開します。同じ文書IDを更新すると新しいセグメントが優先され、
削除操作は古い文書を検索結果から隠します。

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

`yappod_core`を先に起動し、その後で`yappod_front`を起動します。両プログラムはデーモンとして動作し、
`[daemon].run_directory`へPIDとログを保存します。

```sh
./build/yappod_core --config examples/config.lexical.toml
./build/yappod_front --config examples/config.lexical.toml
```

既定ではfrontが`127.0.0.1:18400`、coreが`127.0.0.1:18401`を使用します。

```sh
curl -sS -H 'Content-Type: application/json' \
  --data '{"query":"modern search","mode":"lexical","scope":"documents","limit":10}' \
  http://127.0.0.1:18400/v2/search
```

公開HTTP APIは[`yappod_front` APIリファレンス](docs/yappod-front-api.md)、front/core間の通信は
[`yappod_core`内部プロトコル](docs/yappod-core-protocol.md)で説明しています。

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
- デーモンが起動しない場合は、`[daemon].run_directory`にある`.error`と`.log`を確認してください。
- サンプルのエラーには`Reason`と`How to fix`が表示されます。詳しくは
  [サンプルの問題解決](examples/troubleshooting.md)を参照してください。

文書全体の案内は[ドキュメント一覧](docs/README.md)にあります。
