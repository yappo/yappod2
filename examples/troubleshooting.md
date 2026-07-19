# サンプルの問題解決

local-files、Wikipedia、search-webは、利用者が原因と次の操作を判断できるよう、失敗内容を共通形式で標準エラー出力へ
表示します。この文書のコマンドは、特記がない限りリポジトリのルートディレクトリで実行します。

## エラー表示の読み方

```text
<application>: error: <失敗した操作>
Reason: <原因>
Config: <設定パス>
Input: <入力パス>
Output: <出力パス>
How to fix:
  1. <確認する内容>
  2. <次に実行する操作>
```

表示される項目は失敗内容により異なります。正常終了時の要約JSONは標準出力、エラーは標準エラー出力へ書きます。
正常終了時の終了状態は0、失敗時は0以外です。パイプや自動処理では、表示文字列だけでなく終了状態も確認してください。

まず`Config`、`Input`、`Output`に表示された絶対パスを確認し、`How to fix`を上から実行します。
`YAPPOD_EXAMPLE_DEBUG=1`は、通常の設定誤りに対する最初の手段ではありません。原因を特定できない例外について、
PythonのトレースバックまたはJavaScriptのスタックトレースを追加で取得するときだけ使います。

## 実際の表示例

存在しない設定ファイルをlocal-filesへ渡すと、次のように表示されます。パスとPythonの実行ファイル名は環境により変わります。

```text
local-files: error: 'convert' command failed
Reason: cannot load config /private/tmp/yappod2-docs-missing.toml: [Errno 2] No such file or directory: '/private/tmp/yappod2-docs-missing.toml'
Config: /private/tmp/yappod2-docs-missing.toml
How to fix:
  1. Check that the --config file exists and is readable: /private/tmp/yappod2-docs-missing.toml
  2. Compare it with examples/local-files/local-files.toml.
```

英語のエラーメッセージは実装が出力する文字列です。内容を改変せず、その下に表示される確認手順と、この文書の説明を
対応させています。

## local-files

| 症状または`Reason` | 確認する対象 | 次に実行する操作 |
|---|---|---|
| `required Python package is not installed` | Python 3.9または3.10で`tomli`を導入しているか確認します。 | `python3 -m pip install -r examples/local-files/requirements-core.txt`を実行します。 |
| コマンドライン引数が不正です | サブコマンド、`--config`、`--target`を確認します。 | `python3 examples/local-files/local_files.py <command> --help`を実行します。 |
| `cannot load config`または設定キーのエラーです | TOMLの構文、必須セクション、表示されたキーを確認します。 | [`local-files.toml`](local-files/local-files.toml)と比較してから同じコマンドを再実行します。 |
| 入力ファイルが0件です | `input.root`が読めるか、`input.include`と`input.exclude`が対象を残すか確認します。 | `convert`を再実行し、必要なら対象パターンを狭い範囲から追加します。 |
| 抽出または整形に失敗します | `failures-*.ndjson`の`path`、`code`、`message`、`extractor`を確認します。 | 必要な追加パッケージ、整形コマンド、またはTikaを準備して`convert`を再実行します。 |
| Tikaを開始できません | `extract.tika_command`が`{path}`を1回だけ含むか、JARを読めるか確認します。 | 設定したコマンドを対象ファイル1件で直接確認してから再実行します。 |
| embeddingへ接続できません | `provider`、`base_url`または`endpoint_url`、`model`、`timeout_ms`とサービスの稼働状態を確認します。 | サービスの最小リクエストを確認してから`embed`を再実行します。 |
| `model_id`または`dimensions`が一致しません | `[vector]`と`[embedding]`、レスポンスのベクトル要素数を確認します。 | 同じモデルと次元数でパッセージからベクトルを作り直します。 |
| 認証用の環境変数がありません | `authorization_token_env`に書いた環境変数名を確認します。 | 現在のシェルへ環境変数を設定します。トークン自体はTOMLへ書きません。 |
| `yappo_makeindex`がないか実行できません | `build.yappo_makeindex`が指すファイルと実行権限を確認します。 | `cmake --build build -j --target yappo_makeindex`を実行します。 |
| 既存成果物を再利用できません | 入力スナップショット、manifest、チェックサム、設定の指紋値、チェックポイント、ジャーナルを確認します。 | 異なる入力や設定の成果物を混ぜず、必要な証跡を保存してから全再生成を判断します。 |

`output.directory`、`prepare.directory`、`embedding.directory`、`index.directory`は別々のディレクトリにします。
一部だけを削除すると、manifestとチェックポイントの対応を壊す可能性があります。復旧の詳しい条件は
[処理の流れと再生成](local-files/docs/pipeline-and-recovery.md)を参照してください。

## Wikipedia

存在しないWikiExtractor入力を指定した場合は、次の形式で前段階の実行を案内します。

```text
wikipedia-data: error: 'convert-dump' command failed
Reason: WikiExtractor input does not exist: /tmp/yappod2-docs-missing.jsonl
Input: /private/tmp/yappod2-docs-missing.jsonl
Output: /private/tmp/yappod2-docs-output.ndjson
How to fix:
  1. Confirm that the input exists and is readable: /private/tmp/yappod2-docs-missing.jsonl
  2. Run the preceding data preparation step if this file has not been generated yet.
```

| 処理 | 症状と確認する対象 | 次に実行する操作 |
|---|---|---|
| `fetch-api` | Wikimedia APIのURL、ネットワーク、HTTP状態、`--user-agent`を確認します。 | 連絡先を含むUser-Agentを指定し、`fetch-api --help`で引数を確認して再実行します。 |
| `download-dump` | 空き容量、ダウンロード元、チェックサム不一致を確認します。 | 同じ`--output-dir`へ再実行します。不完全な一時出力は完成済みとして公開されません。 |
| `convert-dump` | WikiExtractorの入力不足、不正なJSON、空の本文、重複IDを確認します。 | 対象行を確認するか、同じダンプからWikiExtractorの出力を作り直します。 |
| `embed` | 文書とパッセージのID、`ordinal`、件数が同じ処理結果から作られたか確認します。 | 文書とパッセージを同じ入力から再生成し、順序を変えずに`embed`を再実行します。 |
| `embed` | embeddingの接続設定、認証用環境変数、モデル、次元数、レスポンスの`data[].index`を確認します。 | サービスを起動して最小リクエストを確認し、同じ設定で再実行します。 |

各サブコマンドの引数は次のように確認できます。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api --help
python3 examples/wikipedia-search/wikipedia_data.py download-dump --help
python3 examples/wikipedia-search/wikipedia_data.py convert-dump --help
python3 examples/wikipedia-search/wikipedia_data.py embed --help
```

## search-web

| 症状または`Reason` | 確認する対象 | 次に実行する操作 |
|---|---|---|
| `cannot read shared config` | `--config`のパスと読み取り権限を確認します。 | [`config.example.toml`](search-web/config.example.toml)と比較して再実行します。 |
| TOMLの検証に失敗します | 表示されたセクション、未知のキー、空の任意セクションを確認します。 | 不要な任意セクションは見出しごと削除し、必要なキーを揃えます。 |
| 索引パスはありますが索引ではありません | `index.directory`の`config.toml`、`manifest.json`、`segments/`を確認します。 | 既存ディレクトリを上書きせず、正しい入力から別の索引を作成して設定を切り替えます。 |
| Cの実行ファイルがありません | `build/yappod_core`と`build/yappod_front`を確認します。 | `cmake --build build -j`を実行します。 |
| npm依存パッケージがありません | `examples/search-web/node_modules`を確認します。 | `cd examples/search-web`で`npm install`を実行し、リポジトリのルートへ戻ります。 |
| すでに起動しています | 表示されたPIDとPIDファイル、実際のコマンドラインを確認します。 | 同じ設定で`examples/search-web/scripts/stop.sh --config CONFIG`を実行してから起動します。 |
| ポートを使用できません | `daemon.core_port`、`daemon.front_port`、`web.port`と、ポートを使うプロセスを確認します。 | プロセスの所有者を確認し、必要なら設定を未使用ポートへ変更します。 |
| 起動前にプロセスが終了しました | 表示された`error log`の末尾を確認します。 | ログの先頭の原因を修正し、`start`を再実行します。 |
| 起動待ちが期限を超えました | 表示された`health check` URL、`web.startup_timeout_ms`、`.error`を確認します。 | 原因を直します。正常に起動するものの時間だけが不足する場合は待ち時間を延ばします。 |
| BFFからfrontへ接続できません | frontの`/health/ready`と`web.yappod_timeout_ms`を確認します。 | core、front、BFFの順に準備状態を確認します。 |
| LLMの回答が空です | `content`、`reasoning_content`、`completion_tokens`、`llm.timeout_ms`を確認します。 | [LLM連携](search-web/docs/llm-integration.md)の順序でBFFと接続先を切り分けます。 |

`stack.mjs build`は索引だけを作成します。`start.sh`は必要なビルドを行って一式を起動し、`stop.sh`は同じ設定で
起動した一式を停止します。PIDが別のプロセスへ再利用されている場合、停止スクリプトは無関係なプロセスを停止しないため
処理を中止します。PIDファイルを手作業で削除する前に、参照先を確認してください。

PIDとログは`[daemon].run_directory`にあります。

| プロセス | PID | 標準出力 | 標準エラー出力 |
|---|---|---|---|
| core | `core.pid` | `core.log` | `core.error` |
| front | `front.pid` | `front.log` | `front.error` |
| Web | `web.pid` | `web.log` | `web.error` |
| 模擬LLM | `mock-llm.pid` | `mock-llm.log` | `mock-llm.error` |

## local-filesを全再生成する場合

設定または入力が変わり、既存の文書、パッセージ、ベクトル、索引を互換な成果物として検証できない場合だけ、
`examples/data/local-files`全体を再生成します。このディレクトリにはサンプルから再生成できる成果物が入りますが、
削除前に必要なファイルが混在していないことを確認してください。

```sh
rm -rf examples/data/local-files
```

このコマンドは文書、パッセージ、ベクトル、索引、manifest、チェックポイント、ジャーナルを削除します。元の入力
ディレクトリは削除しません。削除対象が再生成可能で、必要な調査資料を保存済みである場合だけ実行してください。
この文書の作業では実データの削除を実行していません。

## 認証情報

外部APIのトークンをTOMLへ直接書きません。`authorization_token_env`へ環境変数名を指定し、実際のトークンは
プロセスの環境から渡します。エラー出力やデバッグログを共有する前に秘密情報が含まれていないことを確認してください。
