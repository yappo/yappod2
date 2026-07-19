# 日本語WikipediaをYappod2で検索する

このサンプルは、日本語Wikipediaの記事を正式な入力NDJSONへ変換し、Yappod2の索引を作成します。検索、質問、
文書登録には共通の[search-web](../search-web/README.md)を利用します。

## 必要な環境

- ビルド済みの`yappo_makeindex`、`yappod_core`、`yappod_front`が必要です。
- Python 3.9以上が必要です。
- Web UIにはNode.js 22以上とnpmが必要です。
- ダンプ変換にはWikiExtractorが必要です。
- embeddingにはLM Studio、Ollama、またはOpenAI互換エンドポイントが必要です。

```sh
cmake --build build -j
cp examples/wikipedia-search/wikipedia-search.example.toml \
  examples/wikipedia-search/wikipedia-search.toml
```

相対パスは`wikipedia-search.toml`があるディレクトリを基準に解決します。外部APIのトークンは
`authorization_token_env`から環境変数を参照します。

## Action APIから記事を取得する

次のコマンドは話題を分散させながら最大1000記事を取得し、正式な入力NDJSONへ保存します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api \
  --limit 1000 \
  --output examples/data/wikipedia-search/documents.ndjson
```

Wikimediaへ送るUser-Agentを変更する場合は`--user-agent`を指定します。連絡先を含む、自分の利用目的に合った値を
使ってください。ネットワークまたはHTTPエラーが発生した場合、途中結果を完成済みファイルとして公開しません。

## ダンプを変換する

ダンプを取得します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py download-dump \
  --output-dir examples/data/wikipedia-search/dump
```

チェックサムを確認した後、WikiExtractorのJSON Linesを正式な入力NDJSONへ変換します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py convert-dump \
  --input examples/data/wikipedia-search/wikiextractor.jsonl \
  --output examples/data/wikipedia-search/documents.ndjson
```

入力不足、不正なJSON、空のデータ集合、重複IDを検出した場合は出力を公開しません。

## 語句で検索する索引を作る

初期設定では`[vector].enabled = false`です。`[build].input`が取得済みNDJSONを指していることを確認します。

```sh
node examples/search-web/scripts/stack.mjs build \
  --config examples/wikipedia-search/wikipedia-search.toml
```

CLIから検索できます。

```sh
./build/search \
  --config examples/wikipedia-search/wikipedia-search.toml \
  --mode lexical \
  --query "検索エンジン"
```

## embeddingを付ける

ベクトル検索を利用する場合は、`[vector]`と`[embedding]`を完全に設定します。

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

`vector.model_id`は索引との互換性を確認するYappod2側の識別子、`embedding.model`は接続先へ送る実際のモデル名です。
`model_id`と`dimensions`は両セクションで一致させます。

まず、取得済みの文書を索引と同じ設定でパッセージへ分割します。出力先は未作成である必要があります。

```sh
./build/yappo_makeindex prepare \
  --config examples/wikipedia-search/wikipedia-search.toml \
  --input examples/data/wikipedia-search/documents.ndjson \
  --output examples/data/wikipedia-search/passages.ndjson
```

次に、文書とパッセージを同じ処理結果のままembeddingへ渡します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py embed \
  --documents examples/data/wikipedia-search/documents.ndjson \
  --passages examples/data/wikipedia-search/passages.ndjson \
  --output examples/data/wikipedia-search/documents.vector.ndjson \
  --config examples/wikipedia-search/wikipedia-search.toml
```

変換プログラムは文書とパッセージのID、`ordinal`、件数、`dimensions`、有限値、レスポンス内の順序を検証します。
`[build].input`を生成した`documents.vector.ndjson`へ変更し、既存索引とは別の`[index].directory`へ索引を作成します。

```sh
node examples/search-web/scripts/stack.mjs build \
  --config examples/wikipedia-search/wikipedia-search.toml
```

設定だけを変えて既存の語彙索引をベクトル対応へ変換することはできません。

## Web UIを起動する

```sh
cd examples/search-web
npm install
cd ../..

examples/wikipedia-search/scripts/start_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

既定のURLは`http://127.0.0.1:4173`です。索引がない場合は`[build].input`から作成し、有効な既存索引は
そのまま利用します。

```sh
examples/wikipedia-search/scripts/stop_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

## RAG回答を利用する

`[llm]`がなければ、質問画面は取得した参照資料だけを表示します。設定するとBFFがOpenAI互換Chat Completionsへ
回答生成を依頼します。設定と問題の確認は[LLM連携](../search-web/docs/llm-integration.md)を参照してください。

## 問題が発生した場合

データ取得、ダンプ変換、embedding、索引作成、Web起動は、失敗した操作、原因、対象パス、修正手順を標準エラー出力へ
表示します。英語のエラー本文は実装が出力する文字列で、追加の説明は
[サンプルの問題解決](../troubleshooting.md)にあります。

未知のPython例外を調べる場合だけ、同じコマンドへ`YAPPOD_EXAMPLE_DEBUG=1`を付けます。

## テスト

```sh
python3 -m unittest discover \
  -s examples/wikipedia-search/tests \
  -p 'test_*.py'
```
