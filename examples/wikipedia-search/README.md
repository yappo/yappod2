# 日本語Wikipedia検索・RAG sample

日本語Wikipediaの記事をcanonical NDJSONへ変換し、yappod2 indexを作成するsampleです。検索・質問・文書登録の
Web UIはWikipedia固有実装ではなく、[`../search-web`](../search-web/README.md)の共通applicationを使用します。

## 必要環境

- repository rootでbuild済みの`yappo_makeindex`、`yappod_core`、`yappod_front`
- Python 3.9以上
- Web UIを使う場合はNode.js 22以上とnpm
- dump変換ではWikiExtractor
- vector/hybridではLM Studio、Ollama、またはOpenAI互換embedding endpoint

```sh
cmake --build build -j
```

## 設定file

このsampleでは役割を次の2種類に分けます。

| file | 用途 |
|---|---|
| `wikipedia-search.toml` | Wikipedia入力、生成index、embedding、daemon、Web、LLMをまとめたapplication設定 |
| `config.toml` / `config.vector.toml` | `yappo_makeindex`が読むindex構造設定 |

index構造用TOMLは従来どおり分離します。それ以外のindex生成とWeb起動は同じ
`wikipedia-search.toml`を`--config`で読みます。port、run directory、timeout、write tokenを
環境変数で上書きする仕組みはありません。外部APIのsecretだけは`authorization_token_env`で環境変数を参照します。

設定例をcopyして使用します。`wikipedia-search.toml`は`.gitignore`対象です。認証tokenを使う場合は
このcommit対象外の設定へ直接記述し、所有者だけが読めるpermissionにしてください。

```sh
cp examples/wikipedia-search/wikipedia-search.example.toml \
  examples/wikipedia-search/wikipedia-search.toml
```

## Action APIから記事を取得

既定ではtopicを分散させながら最大1,000記事を取得します。出力は1行1 operationのcanonical NDJSONです。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api \
  --limit 1000 \
  --output examples/wikipedia-search/data/documents.ndjson
```

User-Agentを変更する場合は環境変数ではなく`--user-agent`を指定します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api \
  --user-agent 'my-project/1.0 (contact@example.com)' \
  --limit 1000 \
  --output examples/wikipedia-search/data/documents.ndjson
```

## dumpを変換

```sh
python3 examples/wikipedia-search/wikipedia_data.py download-dump \
  --output-dir examples/wikipedia-search/data/dump
```

WikiExtractorのJSON Lines出力をcanonical NDJSONへ変換します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py convert-dump \
  --input examples/wikipedia-search/data/wikiextractor.jsonl \
  --output examples/wikipedia-search/data/documents.ndjson
```

## lexical indexとWeb UIを一括起動

最初にWeb依存関係をinstallします。

```sh
cd examples/search-web
npm install
cd ../..
```

`wikipedia-search.toml`の`[build].input`、`[build].index_config`、
`[build].index_directory`を使用します。indexがなければ作成し、有効な既存indexはそのまま利用します。

```sh
examples/wikipedia-search/scripts/start_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

標準URLは`http://127.0.0.1:4173`です。停止時も同じ設定を指定します。

```sh
examples/wikipedia-search/scripts/stop_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

PIDとlogは`[daemon].run_directory`に保存します。portやlisten addressは`[daemon]`と`[web]`で変更します。

## vector/hybrid index

詳細は[vector検索手順](docs/vector-search.md)を参照してください。embeddingはindex生成とWebで同じsectionを
共有します。

```toml
[embedding]
provider = "lmstudio"
base_url = "http://127.0.0.1:1234/v1"
model = "LM Studioのmodel identifier"
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
prompt_profile = "embeddinggemma"
timeout_ms = 60000
batch_size = 16
```

passageへembeddingを付与する場合も同じapplication設定を渡し、index構造だけ
`config.vector.toml`として分離します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py embed \
  --documents examples/wikipedia-search/data/documents.ndjson \
  --passages examples/wikipedia-search/data/passages.ndjson \
  --output examples/wikipedia-search/data/documents.vector.ndjson \
  --index-config examples/wikipedia-search/config.vector.toml \
  --config examples/wikipedia-search/wikipedia-search.toml
```

vector indexをWebで使う場合は`wikipedia-search.toml`の`[build].index_directory`をそのindexへ向けます。
BFFは同じ`[embedding]`を使うため、model ID、dimensions、prompt profileの別設定は不要です。
`[build].input`を生成済み`documents.vector.ndjson`、`[build].index_config`を`config.vector.toml`へ変更してから、
同じapplication設定でbuildします。

```sh
examples/wikipedia-search/scripts/build_index.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

## RAGと文書登録

`[llm]`がなければ質問画面は取得した参照資料だけを表示します。設定した場合はOpenAI-compatibleな
Chat Completionsへ質問とcitationを送り、`[1]`形式の参照番号を検証してから回答を表示します。

```toml
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "model-identifier"
effort = "low"
timeout_ms = 30000
# authorization_token_env = "LLM_API_KEY"
```

文書登録を認証する場合は`[daemon].write_token`へ16文字以上のtokenを設定します。core、front、BFFは同じ
設定を読むため、tokenの二重設定はありません。

## テスト

```sh
python3 -m unittest discover \
  -s examples/wikipedia-search/tests \
  -p 'test_*.py'

cd examples/search-web
npm run typecheck
npm test
npm run build
npm run test:e2e
```

Web E2Eは一時設定fileだけでport、mock、tokenを設定し、環境変数や外部networkを使用しません。
