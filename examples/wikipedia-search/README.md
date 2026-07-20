# 日本語WikipediaをYappod2で検索する

このサンプルは、日本語Wikipediaの記事をYappod2の正式なNDJSONへ変換し、語彙検索、ベクトル検索、複合検索、RAG向け取得を試すためのものです。データ準備はPython、索引と検索はYappod2、画面と回答生成は共通の[search-web](../search-web/README.md)を使います。

## 処理の全体像

少量の動作確認にはAction API、大量データには公式ダンプを使います。

```text
Action API ────────────────→ documents.ndjson

公式XML dump → WikiExtractor → JSON Lines → documents.ndjson
                                               │
                                               ├→ 語彙索引
                                               │
                                               └→ passages.ndjson
                                                     │ 埋め込みAPI
                                                     ▼
                                              documents.vector.ndjson
                                                     │
                                                     └→ ベクトル対応索引
```

`wikipedia_data.py`はAction APIからの記事取得、ダンプのダウンロード、WikiExtractor出力の変換、埋め込みを担当します。XMLダンプから本文を抽出するWikiExtractor自体は実行しません。

## 必要な環境

- Python 3.9以上が必要です。
- ビルド済みの`yappo_makeindex`、`search`、`yappod_core`、`yappod_front`が必要です。
- Web UIにはNode.js 22以上とnpmが必要です。
- ダンプ変換には別途WikiExtractorが必要です。
- ベクトルを作る場合はLM Studio、Ollama、またはOpenAI互換の埋め込みエンドポイントが必要です。

リポジトリのルートで準備します。

```sh
cmake --build build -j
cp examples/wikipedia-search/wikipedia-search.example.toml \
  examples/wikipedia-search/wikipedia-search.toml
```

相対パスは`wikipedia-search.toml`があるディレクトリを基準に解決します。

## アプリケーションTOML

設定例は最初、語彙索引用です。

### 索引構造の設定

| キー | 必須・既定値・範囲 | 用途 |
|---|---|---|
| `format_version` | 必須、`2` | 現在の索引形式です。 |
| `index.directory` | 必須 | 作成または起動する索引ディレクトリです。既存の語彙索引をベクトル対応へ変更するときは別のパスを指定します。 |
| `tokenizer.id` | 既定値`unicode_nfkc_casefold_v2`、最大255バイト | 索引互換性を識別する値です。任意の文字列で別実装へ切り替わる設定ではありません。 |
| `chunking.max_chars` | 既定値`1200`、1〜1048576 | 一つのパッセージに含める最大文字数です。 |
| `chunking.overlap_chars` | 既定値`200`、0以上かつ`max_chars`未満 | 隣接するパッセージの重なりです。 |
| `vector.enabled` | 必須 | 語彙索引では`false`、ベクトル対応索引では`true`です。 |
| `vector.model_id` | ベクトル有効時に必須、1〜255バイト | 索引と検索ベクトルの生成規則を識別します。実モデル名とは別です。 |
| `vector.dimensions` | ベクトル有効時に必須、1〜65536 | 各ベクトルの要素数です。 |
| `vector.metric` | ベクトル有効時に必須 | `cosine`、`dot`、`l2`から選びます。 |
| `metadata.filterable_fields` | 既定値`[]`、最大64件、各1〜127バイト | `language`や`source`など、検索時に絞り込むメタデータのパスです。 |

`[tokenizer]`、`[chunking]`、`[vector]`はセクション自体が必要です。`[metadata]`は省略できます。索引作成後の実際の値は索引直下の`config.toml`に保存されます。

### 索引作成、デーモン、Webの設定

| キー | 必須・既定値・範囲 | 用途 |
|---|---|---|
| `build.yappo_makeindex` | 索引がない場合に必須 | search-webの索引作成処理が実行する`yappo_makeindex`です。 |
| `build.input` | 索引がない場合に必須 | 語彙索引では`documents.ndjson`、ベクトル対応索引では`documents.vector.ndjson`を指定します。 |
| `daemon.run_directory` | Cデーモンでは必須 | core、front、WebのPIDとログを保存します。 |
| `daemon.core_host` / `core_port` | Cデーモンでは必須、ポートは1〜65535 | coreの待ち受け先と、frontからの接続先です。 |
| `daemon.front_host` / `front_port` | Cデーモンでは必須、ポートは1〜65535 | frontの待ち受け先と、BFFからの接続先です。 |
| `daemon.max_inflight` | `4`、1〜1024 | frontとcoreがそれぞれ保持する処理中リクエスト件数の上限です。 |
| `daemon.max_inflight_bytes` | `4194304`、1〜1073741824 | frontとcoreがそれぞれ保持する処理中データ量の上限です。 |
| `daemon.request_timeout_ms` | `5000`、1〜60000 | 接続後のソケット送受信期限です。 |
| `daemon.write_token` | 任意、16〜255バイト | 文書登録APIのBearer認証に使います。空白と制御文字は使用できません。 |
| `web.host` / `web.port` | `127.0.0.1` / `4173` | BFFとWeb画面の待ち受け先です。 |
| `web.yappod_timeout_ms` | `5000`、1〜600000 | BFFがfrontの応答を待つ時間です。 |
| `web.startup_timeout_ms` | `8000`、100〜600000 | 起動スクリプトがBFFと模擬サービスの起動を待つ時間です。 |
| `usage_log.path` | 任意 | 埋め込みとLLMの`usage`を追記するJSONLです。 |

### 埋め込みの設定

ベクトルを作る場合だけ`[embedding]`を追加します。

| キー | 必須・既定値・範囲 | 用途 |
|---|---|---|
| `provider` | 必須 | `lmstudio`、`ollama`、`openai`から選びます。 |
| `base_url` / `endpoint_url` | どちらか一方だけ必須 | 基準URL、または埋め込みエンドポイント全体です。 |
| `model` | 必須 | 接続先へ送る実際のモデル名です。 |
| `model_id` | 任意 | 指定する場合は`vector.model_id`と一致させます。 |
| `dimensions` | 省略時は`vector.dimensions`、1〜65536 | 接続先が返すベクトル要素数です。`vector.dimensions`と一致させます。 |
| `prompt_profile` | `plain` | `plain`または`embeddinggemma`です。 |
| `batch_size` | `16`、1〜1024 | 1回のAPI呼び出しへまとめる本文数です。 |
| `timeout_ms` | `60000`、1000〜600000 | 埋め込みAPIの1回の応答を待つ時間です。 |
| `authorization_token_env` | 任意 | Bearerトークンを読む環境変数名です。トークン自体はTOMLへ書きません。 |

### LLMの設定

回答生成を使う場合だけ`[llm]`を追加します。

| キー | 必須・既定値・範囲 | 用途 |
|---|---|---|
| `base_url` | 必須 | OpenAI互換Chat Completions APIの基準URLです。 |
| `model` | 必須 | 接続先へ送るLLM名です。 |
| `effort` | 任意 | 指定時だけ`reasoning_effort`として送ります。 |
| `max_tokens` | `8192`、1〜131072 | 最大出力トークン数です。 |
| `timeout_ms` | `30000`、1000〜600000 | LLMの応答を待つ時間です。 |
| `authorization_token_env` | 任意 | Bearerトークンを読む環境変数名です。 |

`[embedding]`と`[llm]`を使わない場合は、空のセクションを残さず見出しごと省略します。設定全体の厳密な条件は[設定リファレンス](../../docs/configuration.md)、LLM固有の挙動は[LLM連携](../search-web/docs/llm-integration.md)を参照してください。

## Action APIから少量の記事を取得する

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api \
  --limit 1000 \
  --output examples/data/wikipedia-search/documents.ndjson
```

### `fetch-api`の引数

| 引数 | 必須・既定値 | 説明 |
|---|---|---|
| `--output PATH` | 必須 | 正式なNDJSONの出力先です。親ディレクトリは作成します。 |
| `--limit N` | 既定1000、1以上 | 書き出す記事数の上限です。APIから必ずこの件数を得られる保証はありません。 |
| `--topic TEXT` | 繰り返し指定可 | 検索話題です。省略時は歴史、地理、自然科学、情報技術など20の話題へ件数を配分します。 |
| `--api-url URL` | `https://ja.wikipedia.org/w/api.php` | Action APIの接続先です。通常は変更しません。 |
| `--user-agent TEXT` | リポジトリの例示値 | Wikimediaへ送るUser-Agentです。実利用では連絡先を含む値へ変更します。 |

各話題を生成器による検索へ渡し、記事ごとにプレーンテキストの抜粋、完全URL、リビジョンIDを取得します。同じページIDと空本文は読み飛ばします。成功時の標準出力は次の要約JSONです。

```json
{"output":"examples/data/wikipedia-search/documents.ndjson","skipped":12,"written":988}
```

出力は一時ファイルへ書いて`fsync`してから置き換えます。APIエラーや不正応答で失敗した途中ファイルを最終出力として公開しません。

## 公式ダンプをダウンロードする

```sh
python3 examples/wikipedia-search/wikipedia_data.py download-dump \
  --output-dir examples/data/wikipedia-search/dump
```

| 引数 | 必須・既定値 | 説明 |
|---|---|---|
| `--output-dir DIR` | 必須 | チェックサムとダンプを保存します。 |
| `--base-url URL` | `https://dumps.wikimedia.org/jawiki/latest` | `jawiki-latest`の基準URLです。 |
| `--user-agent TEXT` | リポジトリの例示値 | HTTP User-Agentです。 |

最初に`jawiki-latest-sha1sums.txt`を取得し、その中の`jawiki-YYYYMMDD-pages-articles-multistream.xml.bz2`のSHA-1を読みます。ダンプは`.part`へダウンロードし、通信再試行時にはHTTP Rangeで続行を試みます。SHA-1が一致した場合だけ`jawiki-latest-pages-articles-multistream.xml.bz2`として公開します。

HTTP 429または5xxと一時的なネットワークエラーは最大4回再試行し、待ち時間を増やします。チェックサム不一致では`.part`を削除し、完成済みダンプへ置き換えません。

## WikiExtractorで本文を抽出する

ダウンロードしたファイルを、使用しているWikiExtractorのJSON Lines出力へ変換します。必要なレコードフィールドは次のとおりです。

| フィールド | 必須条件 | Yappod2への対応 |
|---|---|---|
| `id` | 文字列または整数 | `jawiki:<id>`という文書IDになります。 |
| `title` | 空でない文字列 | 文書タイトルになります。 |
| `text` | 文字列 | 前後の空白を除き、空なら読み飛ばします。 |
| `url` | `http://`または`https://` | 文書URLになります。 |
| `revid` | 任意の整数 | メタデータの`wikipedia_revision_id`になります。 |

WikiExtractorはこのリポジトリの依存に固定されておらず、CLIも版によって異なるため、この文書では未確認のコマンドラインを固定しません。導入した版の`--help`でJSON出力と出力先を確認し、生成したファイルまたはディレクトリを次の`--input`へ渡してください。

## WikiExtractor出力を正式なNDJSONへ変換する

```sh
python3 examples/wikipedia-search/wikipedia_data.py convert-dump \
  --input examples/data/wikipedia-search/wikiextractor \
  --output examples/data/wikipedia-search/documents.ndjson
```

| 引数 | 必須・既定値 | 説明 |
|---|---|---|
| `--input PATH` | 必須 | 1ファイル、または複数出力を含むディレクトリです。`.bz2`と`.gz`も読めます。 |
| `--output PATH` | 必須 | Yappod2の正式なNDJSONです。 |
| `--limit N` | 任意、1以上 | 動作確認で書き出す記事数を制限します。 |

ディレクトリ入力では、隠しファイル以外を再帰的に名前順で読みます。不正JSON、オブジェクト以外のレコード、必須フィールド不正、読込失敗は全体を失敗させます。重複IDと空本文は読み飛ばします。正常記事が0件なら出力を公開しません。

生成する各行は次の形です。

```json
{"operation":"upsert","id":"jawiki:123","url":"https://ja.wikipedia.org/wiki/...","title":"記事名","body":"本文","metadata":{"language":"ja","source":"wikipedia-ja","wikipedia_page_id":"123","wikipedia_revision_id":456}}
```

## 語彙索引を作る

`[vector].enabled = false`、`[build].input`が`documents.ndjson`を指すことを確認します。

```sh
examples/wikipedia-search/scripts/build_index.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

このscriptは`node examples/search-web/scripts/stack.mjs build`を呼びます。`index.directory`がすでに存在すると上書きせず失敗します。

```sh
./build/search \
  --config examples/wikipedia-search/wikipedia-search.toml \
  --mode lexical \
  --query "検索エンジン"
```

## パッセージを作る

ベクトル対応索引を作る前に、索引と同じトークナイザーと文書分割設定でパッセージを準備します。

```sh
./build/yappo_makeindex prepare \
  --config examples/wikipedia-search/wikipedia-search.toml \
  --input examples/data/wikipedia-search/documents.ndjson \
  --output examples/data/wikipedia-search/passages.ndjson
```

`prepare`の出力はパッセージ用の中間NDJSONであり、そのまま`build`へは渡しません。次の`embed`が元文書と結合します。出力ファイルは上書きされるため、既存成果物を保存する必要がある場合は別パスを指定してください。

## 埋め込みを付ける

アプリケーションTOMLへ完全な`[vector]`と`[embedding]`を追加します。

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
timeout_ms = 60000
batch_size = 16
```

`[vector]`は索引に保存する構造、`[embedding]`はAPI接続です。`embedding.model`は接続先へ送る実名、両セクションの`model_id`はYappod2が互換性を確認する識別子です。`model_id`と`dimensions`を一致させます。

外部APIでは`authorization_token_env = "EMBEDDING_API_KEY"`のように環境変数名を指定します。トークン自体はTOMLへ書きません。

```sh
python3 examples/wikipedia-search/wikipedia_data.py embed \
  --documents examples/data/wikipedia-search/documents.ndjson \
  --passages examples/data/wikipedia-search/passages.ndjson \
  --output examples/data/wikipedia-search/documents.vector.ndjson \
  --config examples/wikipedia-search/wikipedia-search.toml
```

| 引数 | 必須・既定値 | 説明 |
|---|---|---|
| `--documents PATH` | 必須 | 元の正式な登録・更新用NDJSONです。 |
| `--passages PATH` | 必須 | 同じ文書から`prepare`したパッセージNDJSONです。 |
| `--output PATH` | 必須 | `vectors`を付けた正式なNDJSONです。 |
| `--config PATH` | 既定は`wikipedia-search.toml` | `[embedding]`と`[usage_log]`を読みます。 |

全文書が一意な登録・更新操作であること、全パッセージが既知の文書を指すこと、通し番号が文書ごとに0から連続すること、全文書に1件以上のパッセージがあることを検証します。埋め込み応答は件数、索引順、次元数、有限値を検証します。

成功時は文書数とパッセージ数を要約JSONへ出します。出力は一時ファイルから不可分に置き換えます。現行のWikipedia `embed`はlocal-filesのようなチェックポイント再開を実装していないため、途中失敗時は同じ入力から再実行します。

## ベクトル対応索引を作る

`[build].input`を`documents.vector.ndjson`へ変更し、既存語彙索引とは別の`[index].directory`を指定します。

```sh
examples/wikipedia-search/scripts/build_index.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

設定だけを変更して既存の語彙索引へベクトルコンポーネントを追加することはできません。新しい索引の`config.toml`、`manifest.json`、`vectors.yap2`、`vectors.usearch`を確認し、`yappo_makeindex verify`を実行してください。

```sh
./build/search \
  --config examples/wikipedia-search/wikipedia-search.toml \
  --mode hybrid \
  --query "検索エンジン" \
  --vector "$(paste -sd, query-vector.txt)"
```

CLIは検索文から埋め込みを生成しません。この例の`query-vector.txt`には、索引と同じモデルで作った有限値を1行に1要素ずつ、`dimensions`行だけ用意します。`paste`でCLIが受け取るコンマ区切りへ変換しています。通常はsearch-webが質問を埋め込みしてベクトルを渡します。

## Web UIを起動する

```sh
cd examples/search-web
npm ci
cd ../..
examples/wikipedia-search/scripts/start_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

索引がなければ`[build]`から作り、有効な既存索引は再利用します。既定は`http://127.0.0.1:4173`です。

```sh
examples/wikipedia-search/scripts/stop_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

検索、RAG、登録、PID、ログの詳細は[search-web](../search-web/README.md)を参照してください。

## RAG回答を利用する

質問画面は`/v2/retrieve`でパッセージと出典を取得します。`[llm]`がなければ参照資料だけを表示します。追加した場合はBFFがOpenAI互換Chat Completionsへ回答を依頼します。LLMの全設定、固定temperature、空content、期限超過の確認は[LLM連携](../search-web/docs/llm-integration.md)を参照してください。

## 問題が発生した場合

成功時の要約は標準出力、失敗理由、対象パス、修正手順は標準エラー出力です。`fetch-api`と`download-dump`は外部通信を行うため、再試行前にUser-Agent、HTTP状態、ネットワーク、空き容量を確認します。`convert-dump`と`embed`は最終出力を不可分に置き換えます。

英語の実エラーと症状別の次の操作は[サンプルの問題解決](../troubleshooting.md)を参照してください。未知のPython例外を追加調査する場合だけ`YAPPOD_EXAMPLE_DEBUG=1`を付けます。

## テスト

外部APIへ接続しない単体テストを、リポジトリのルートで実行します。

```sh
python3 -m unittest discover \
  -s examples/wikipedia-search/tests \
  -p 'test_*.py'
```
