# Yappod2検索Web UI

search-webは、Yappod2の索引をブラウザーから検索・質問・更新するための共通Web UIです。FastifyのBFFと
React/Viteの画面で構成し、local-filesとWikipediaのどちらの索引にも接続できます。

## 必要な環境

- リポジトリのルートでビルドした`yappod_core`と`yappod_front`が必要です。
- Node.js 22以上とnpmが必要です。
- 検索対象の有効なv2索引が必要です。

```sh
cmake --build build -j
cd examples/search-web
npm install
cd ../..
```

## 設定

search-web、デーモン、索引作成用の変換プログラムは同じアプリケーション用TOMLを読みます。設定例は
[`config.example.toml`](config.example.toml)です。local-filesやWikipediaでは、それぞれのサンプルが用意する
TOMLをそのまま指定できます。

```sh
examples/search-web/scripts/start.sh \
  --config examples/local-files/local-files-yappod2.toml
```

主な設定は次のとおりです。

| セクション | 用途 |
|---|---|
| `[index]` | 接続する索引ディレクトリです。 |
| `[daemon]` | coreとfrontのアドレス、PID、ログ、処理上限です。 |
| `[web]` | BFFのアドレスと各種タイムアウトです。 |
| `[embedding]` | 検索文から検索ベクトルを生成する接続先です。 |
| `[llm]` | RAGの回答を生成する接続先です。 |
| `[usage_log]` | embeddingとLLMの利用量を記録するパスです。 |
| `[mock]` | テストとデモに使うローカルの模擬サービスです。 |

全キーは[設定リファレンス](../../docs/configuration.md)を参照してください。

## 起動

起動スクリプトはcore、front、配布用にビルドしたBFFと画面を起動し、ヘルスチェックが成功するまで待ちます。索引が
存在しない場合に作成する構成もありますが、有効な既存索引は上書きしません。

```sh
examples/search-web/scripts/start.sh --config /path/to/application.toml
```

既定のURLは`http://127.0.0.1:4173`です。`[web].startup_timeout_ms`は起動完了を待つ時間、
`yappod_timeout_ms`は起動後にBFFがfrontの応答を待つ時間です。

## 検索

語句による検索はベクトルを持たない索引でも利用できます。ベクトル検索と複合検索では、索引にベクトル用
コンポーネントがあり、`[embedding]`が索引と同じ`model_id`と`dimensions`で検索ベクトルを生成できる必要があります。

画面の検索結果にはタイトル、URLまたはローカルパス、抜粋を表示します。詳細表示ではgeneration、文書ID、各スコアを
確認できます。続きを取得するときはBFFがfrontのカーソルをそのまま利用します。

## 質問とRAG

質問画面は`/v2/retrieve`から本文断片と出典を取得します。`[llm]`がなければ参照資料だけを表示し、設定されている
場合はBFFがOpenAI互換Chat Completionsへ回答生成を依頼します。

回答中の`[1]`などの番号は、取得した参照資料と一致する場合だけ表示します。LLM接続、設定、空の回答、タイムアウトの
確認方法は[LLM連携](docs/llm-integration.md)を参照してください。

## 文書登録

文書登録は`/v2/passages:prepare`でパッセージを作り、必要ならBFFでembeddingを生成してから
`/v2/documents:batch`へ送ります。`[daemon].write_token`がある場合はBFFも同じ設定からトークンを読みます。

登録内容は現在の索引だけに反映されます。local-filesやWikipediaの元データへは戻らないため、一連の処理で索引を
再作成すると手動登録分は失われます。

## 停止

起動時と同じ設定ファイルを指定します。

```sh
examples/search-web/scripts/stop.sh --config /path/to/application.toml
```

PIDとログは`[daemon].run_directory`にあります。停止スクリプトはPIDが対象プロセスを指すことを確認してから停止します。

## 起動できない場合

起動失敗時は、失敗した操作、原因、設定パス、修正手順、ヘルスチェックURL、エラーログの末尾を標準エラー出力へ
表示します。まず表示された`.error`を確認してください。

```text
search-web: error: cannot start the example stack
Reason: Web application exited before becoming ready (PID 123)
Config: /path/to/application.toml
How to fix:
  1. Read the error log path shown above; its final lines contain the startup failure.
  2. Check daemon.core_port, daemon.front_port, web.port, and startup_timeout_ms in /path/to/application.toml.
```

英語のエラー本文は実装が出力する文字列です。意味と追加の確認手順は
[サンプルの問題解決](../troubleshooting.md)を参照してください。

## 開発

```sh
cd examples/search-web
cp config.example.toml config.toml
npm run dev
```

変更後は次を実行します。

```sh
npm run typecheck
npm test
npm run build
npm run test:e2e
```

E2Eは一時ポートと模擬サービスを使い、外部ネットワークへ接続しません。画面の情報設計は
[UX設計](docs/ux-design.md)を参照してください。
