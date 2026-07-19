# yappod search Web UI

任意のyappod2 indexを検索・質問・更新する共通Web UIです。TypeScript、Fastify BFF、React/Viteで構成し、
lexical、vector、hybrid検索、引用付きRAG、単一文書登録に対応します。Wikipedia固有の実装ではなく、
`examples/wikipedia-search`と`examples/local-files`の双方から同じapplicationを使用します。

## 共有設定

index生成adapterとWeb stackは同じapplication TOMLを`--config PATH`で読みます。

主なsectionは次のとおりです。

| section | 利用者 | 用途 |
|---|---|---|
| `[index]`、`[tokenizer]`、`[chunking]`、`[vector]`、`[metadata]` | C CLI・index生成 | index directoryとindex構造 |
| `[build]` | index生成・launcher | binaryと入力 |
| `[embedding]` | index生成・BFF | provider、model、model ID、次元、prompt、token |
| `[usage_log]` | index生成・BFF | API usage log |
| `[daemon]` | core・front・launcher・BFF | PID/log、port、runtime limit、write token |
| `[web]` | BFF・launcher | listen先とyappod timeout |
| `[llm]` | BFF | RAG回答生成model |
| `[mock]` | test/demoのみ | local mock LLM/embedding server |

設定例は[`config.example.toml`](config.example.toml)です。外部APIのsecretはTOMLへ直書きせず、`authorization_token_env`にtokenを保持する環境変数名を
指定します。daemonの`write_token`は共有設定へ記述します。`config.toml`は`.gitignore`対象です。
index、PID、log、API usage logなどの生成物は`examples/data/`配下に保存します。

embedding設定の名前は全処理で共通です。

```toml
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

RAG回答生成を有効にする場合は、OpenAI-compatibleなChat Completionsを設定します。

```toml
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "model-identifier"
effort = "low"
max_tokens = 8192
timeout_ms = 30000
# authorization_token_env = "LLM_API_KEY"
```

`max_tokens`は生成token数の上限で、未指定時は8192です。1から131072まで指定でき、上限まで必ず生成する
ものではありません。reasoningを有効にするproviderでは内部思考も上限を消費する場合があります。

## 起動と停止

依存関係とC binaryを準備します。

```sh
cmake --build build -j
cd examples/search-web
npm install
cd ../..
```

有効な`[index].directory`を持つ設定を指定して、core、front、production BFF/UIをまとめて起動します。
launcherは既存indexを上書きしません。

```sh
examples/search-web/scripts/start.sh \
  --config examples/local-files/local-files-yappod2.toml
```

設定した`[web].host`と`port`をBrowserで開きます。標準は`http://127.0.0.1:4173`です。
launcherがWebの起動を待つ時間は`[web].startup_timeout_ms`で指定します。標準は8000msです。
`[web].yappod_timeout_ms`は起動待ちではなく、起動後にWebから`yappod_front`へ送るrequestのtimeoutです。
起動に失敗した場合、launcherは`[daemon].run_directory/web.error`の末尾をエラーとして表示します。

```sh
examples/search-web/scripts/stop.sh \
  --config examples/local-files/local-files-yappod2.toml
```

`[daemon].run_directory`には`core.pid`、`front.pid`、`web.pid`と各logを保存します。同じ設定で二重起動した
場合は既存PID fileを検出して停止します。

## local-files index

local-filesのapplication TOMLは収集、embedding、index生成、Web起動で共有します。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target lexical

examples/search-web/scripts/start.sh \
  --config examples/local-files/local-files-yappod2.toml
```

local-files文書はURLを持たず、titleにroot相対pathを保存します。UIはpathをリンクなしで表示し、URL付き文書
だけを外部リンクにします。lexical indexでも検索とlexical RAGを利用できます。vector/hybridではindex生成時と
同じ`[embedding]`をBFFも読むため、model ID・次元・promptの二重設定はありません。

文書登録を使う場合は`[daemon].write_token`へ16文字以上のtokenを設定します。手動登録は現在のindexだけを
更新し、local-filesの入力fileや生成shardへは逆流しません。pipelineでindexを再生成すると手動登録分は失われます。

## Wikipedia index

Wikipedia sampleは`wikipedia-search.example.toml`をcopyした
`examples/wikipedia-search/wikipedia-search.toml`を共有設定として使用します。

```sh
examples/wikipedia-search/scripts/start_demo.sh \
  --config examples/wikipedia-search/wikipedia-search.toml
```

indexがなければ`[build].input`と同じapplication設定から作成し、存在する場合はそのまま起動します。
停止も同じ設定を指定します。

## 開発とテスト

開発時は`config.example.toml`を`config.toml`へcopyし、接続するindexとportを編集します。

```sh
cd examples/search-web
cp config.example.toml config.toml
npm run dev
```

```sh
npm run typecheck
npm test
npm run build
npm run test:e2e
```

E2Eは設定fileに一時port、mock、tokenを記述し、外部networkや環境変数を使わずにcore、front、BFF/UIを
起動します。URL付きWikipedia形式文書と、URLなしlocal-files形式文書の両方を確認します。

画面のtask、状態設計、wireframeは[UX設計](docs/ux-design.md)を参照してください。
