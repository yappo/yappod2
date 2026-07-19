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

`wikipedia-search.toml`にWikipedia入力、index directoryと構造、embedding、daemon、Web、LLMを
まとめます。`yappo_makeindex`、daemon、Web UIは同じfileを`--config`で読みます。
外部APIのsecretは`authorization_token_env`で環境変数を参照します。

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
  --output examples/data/wikipedia-search/documents.ndjson
```

User-Agentを変更する場合は環境変数ではなく`--user-agent`を指定します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api \
  --user-agent 'my-project/1.0 (contact@example.com)' \
  --limit 1000 \
  --output examples/data/wikipedia-search/documents.ndjson
```

## dumpを変換

```sh
python3 examples/wikipedia-search/wikipedia_data.py download-dump \
  --output-dir examples/data/wikipedia-search/dump
```

WikiExtractorのJSON Lines出力をcanonical NDJSONへ変換します。

```sh
python3 examples/wikipedia-search/wikipedia_data.py convert-dump \
  --input examples/data/wikipedia-search/wikiextractor.jsonl \
  --output examples/data/wikipedia-search/documents.ndjson
```

## lexical indexとWeb UIを一括起動

最初にWeb依存関係をinstallします。

```sh
cd examples/search-web
npm install
cd ../..
```

`wikipedia-search.toml`の`[build].input`と`[index].directory`を使用します。indexがなければ作成し、
有効な既存indexはそのまま利用します。

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

## エラーからの復旧

data取得、dump変換、embedding、index/Web起動の各scriptは、失敗した操作、原因、対象path、修正手順をstderrへ
表示します。例えば存在しないWikiExtractor出力を指定すると、`--input`の実pathと、先にdata準備が必要なことを
表示します。

```text
wikipedia-data: error: 'convert-dump' command failed
Reason: WikiExtractor input does not exist: /path/to/wikiextractor.jsonl
Input: /path/to/wikiextractor.jsonl
Output: /path/to/documents.ndjson
How to fix:
  1. Confirm that the input exists and is readable: /path/to/wikiextractor.jsonl
  2. Run the preceding data preparation step if this file has not been generated yet.
```

未知のPython/JavaScript例外を調査する場合は、`YAPPOD_EXAMPLE_DEBUG=1`を付けて同じcommandを再実行すると
tracebackまたはstack traceを確認できます。

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

passageへembeddingを付与する場合も同じapplication設定を渡します。`[vector].enabled = true`にし、
`model_id`、`dimensions`、`metric`を`[embedding]`と一致させます。

```sh
python3 examples/wikipedia-search/wikipedia_data.py embed \
  --documents examples/data/wikipedia-search/documents.ndjson \
  --passages examples/data/wikipedia-search/passages.ndjson \
  --output examples/data/wikipedia-search/documents.vector.ndjson \
  --config examples/wikipedia-search/wikipedia-search.toml
```

vector indexをWebで使う場合は`wikipedia-search.toml`の`[index].directory`をそのindexへ向けます。
BFFは同じ`[embedding]`を使うため、model ID、dimensions、prompt profileの別設定は不要です。
`[build].input`を生成済み`documents.vector.ndjson`へ変更してから、同じapplication設定でbuildします。

```sh
node examples/search-web/scripts/stack.mjs build \
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
max_tokens = 8192
timeout_ms = 30000
# authorization_token_env = "LLM_API_KEY"
```

`max_tokens`はChat Completionsへ送る生成token数の上限で、既定値は8192です。値は1から131072まで
指定できます。上限まで必ず生成する指定ではなく、modelが回答を完了すればその時点で停止します。reasoningを
有効にするproviderでは内部思考もこの上限を消費する場合があるため、長いcontextでは回答本文の分も含めて設定します。

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
