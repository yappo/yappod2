# サンプルで問題が発生したときの調べ方

この文書では、local-files、Wikipedia、search-webで表示されるエラーの読み方と、安全な復旧手順を説明します。
コマンドは、特記がない限りリポジトリのルートディレクトリで実行します。

## エラー表示の読み方

各サンプルは、通常の失敗を次の形式で標準エラー出力へ表示します。存在しない項目は表示されません。

```text
<application>: error: <失敗した操作>
Reason: <原因>
Config: <設定ファイルの絶対パス>
Input: <入力の絶対パス>
Output: <出力の絶対パス>
How to fix:
  1. <最初に確認する内容>
  2. <修正後に実行する操作>
```

`Reason`は失敗の直接原因です。`Config`、`Input`、`Output`などは、実際に解決された絶対パスです。
相対パスの解決先を思い込みで判断せず、まずここに表示されたパスを確認してください。`How to fix`は、上から順に実行します。

### 標準出力、標準エラー出力、終了状態

- 正常時の要約JSONや通常の進行表示は標準出力へ書きます。
- エラー本文は標準エラー出力へ書きます。正常時の要約JSONと同じ形式ではありません。
- 正常終了は終了状態0、失敗は0以外です。シェルスクリプトから使う場合は、表示文字列ではなく終了状態で成否を判定してください。
- `failures-*.ndjson`は変換できなかった個別ファイルの記録です。処理全体の標準エラー出力ではありません。

`YAPPOD_EXAMPLE_DEBUG=1`は、通常の設定誤りに対する最初の対処ではありません。既知のエラー分類に当てはまらない例外を
調べるときにだけ指定すると、PythonのトレースバックまたはJavaScriptのスタックトレースを追加表示します。

```sh
YAPPOD_EXAMPLE_DEBUG=1 python3 examples/local-files/local_files.py convert \
  --config examples/local-files/local-files.toml
```

デバッグ出力を共有する場合は、URL、入力パス、文書本文、環境変数から読まれた認証情報が含まれていないか確認してください。

## local-files

### 実際のエラー表示

存在しない設定ファイルを指定すると、現在の実装は次の形式で表示します。Pythonの実行ファイル名とパスは環境により変わります。

```text
local-files: error: 'convert' command failed
Reason: cannot load config /private/tmp/yappod2-docs-missing.toml: [Errno 2] No such file or directory: '/private/tmp/yappod2-docs-missing.toml'
Config: /private/tmp/yappod2-docs-missing.toml
How to fix:
  1. Check that the --config file exists and is readable: /private/tmp/yappod2-docs-missing.toml
  2. Compare it with examples/local-files/local-files.toml.
```

英語部分はプログラムが実際に出力する文面です。次の表では、`Reason`に含まれる文字列と確認箇所を対応させます。

### 起動、引数、設定

| 表示または症状 | 主な原因 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| `required Python package is not installed` | Python 3.9または3.10で`tomli`を利用できません。 | 実行中のPythonと`requirements-core.txt`を確認します。 | `python3 -m pip install -r examples/local-files/requirements-core.txt`を実行します。 |
| `invalid command arguments` | サブコマンド、必須引数、または排他的な引数が不正です。 | エラー直後に示されるヘルプ用コマンドを確認します。 | `python3 examples/local-files/local_files.py <command> --help`を実行し、引数を修正します。 |
| `cannot load config` | ファイルがない、読めない、またはTOMLとして解析できません。 | `Config`の絶対パスと読み取り権限を確認します。 | [`local-files.toml`](local-files/local-files.toml)と比較して、同じサブコマンドを再実行します。 |
| `unknown key`、`is required`、`must be` | キー名、型、範囲、必須項目のいずれかが不正です。 | [`local-filesの設定リファレンス`](local-files/docs/configuration.md)で、表示された完全なキー名を確認します。 | キーだけを直して再実行します。使わない任意セクションは、空で残さず見出しごと削除します。 |
| `environment variable ... is not set` | `authorization_token_env`が指す環境変数がありません。 | TOMLにはトークンではなく環境変数名だけがあることを確認します。 | 例として`export EMBEDDING_API_TOKEN='...'`のように現在のシェルへ設定します。実値をTOMLへ書きません。 |

### 入力の走査と本文抽出

| 表示または症状 | 主な原因 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| 入力が0件、または`no files` | `input.root`が誤っているか、`input.include`と`input.exclude`がすべてを除外しています。 | `Input root`、対象相対パス、globを確認します。 | まず狭い`include`を1件の既知ファイルへ合わせて`convert`を再実行し、必要なパターンを追加します。 |
| `cannot stat`、`cannot scan`、`cannot read` | パスがない、権限がない、または走査中にファイルが変わりました。 | `Reason`にあるパスと現在の利用者の権限を確認します。 | 入力を安定した状態にして同じ`convert`を再実行します。 |
| `failures-*.ndjson`が作られます | 個別ファイルの抽出に失敗しました。成功文書が1件以上あれば処理全体は成功します。 | 各行の`path`、`code`、`message`、任意の`extractor`を確認します。 | 必要な抽出器を準備するか、対象外なら`input.exclude`へ追加して再生成します。 |
| フォーマッターの失敗 | `[[formatters.rules]]`の選択条件、コマンド、制限時間、出力上限が合っていません。 | `name`、`command`内の`{path}`、`timeout_ms`、`max_stdout_bytes`を確認します。 | 設定と同じ作業ディレクトリで、対象ファイル1件をコマンドへ渡して確認してから`convert`を再実行します。 |
| Tikaの失敗 | JARまたはJavaがない、`{path}`が1回でない、時間超過、出力不正です。 | `extract.tika_command`と`extract.tika_timeout_ms`を確認します。 | 設定した引数列を対象ファイル1件で直接実行し、UTF-8の本文が標準出力へ出ることを確認します。 |
| `cannot split UTF-8 body` | `output.body_max_bytes`がUTF-8の1文字すら収められません。 | `output.body_max_bytes`を確認します。 | 1文字を収められる値へ増やして、文書成果物を作り直します。 |

### パッセージ、埋め込み、索引作成

| 表示または症状 | 主な原因 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| 埋め込みへの接続失敗、HTTPエラー、時間超過 | 接続先が停止しているか、URL、プロバイダー、モデル、制限時間が合っていません。 | `[embedding]`の`provider`、`base_url`または`endpoint_url`、`model`、`timeout_ms`を確認します。 | 接続先へ最小リクエストを送り、応答後に`embed`を再実行します。検証済みの途中成果物があれば再開します。 |
| ベクトルの要素数が不一致 | API応答のベクトル長と`dimensions`が違います。 | `[embedding].dimensions`、`[vector].dimensions`、API応答を確認します。 | 正しい次元数でパッセージからベクトルを作り直します。値だけを変えて既存ベクトルを再利用しません。 |
| `model_id`の不一致 | 索引互換性用の識別子が段階間で違います。 | `[embedding].model_id`と`[vector].model_id`を確認します。 | 同じ識別子を設定し、埋め込み付き文書と索引を新しく作ります。 |
| `yappo_makeindex`を起動できません | C実行ファイルが未作成、パスが不正、または実行権限がありません。 | `[build].yappo_makeindex`が指す通常ファイルを確認します。 | `cmake --build build -j --target yappo_makeindex`を実行してから`build`または`all`を再実行します。 |
| `verification failed` | 索引のマニフェスト、コンポーネント、チェックサム、件数が一致しません。 | `index.directory`と`local-files-build.json`を確認します。 | 壊れた索引へ追記せず、入力と設定を確認して未使用ディレクトリへ索引を作り直します。 |

### 既存成果物を再利用できない場合

`already exists`、`cannot reuse`、`configuration mismatch`、`input snapshot`、`manifest`、`checksum`、`checkpoint`、
`journal`を含むエラーは、異なる入力または設定で作った成果物を混ぜないための停止です。

1. [`処理工程、マニフェスト、再開`](local-files/docs/pipeline-and-recovery.md)で、失敗した段階と前段の依存関係を確認します。
2. 調査に必要な`manifest.json`、`progress.json`、`checkpoint.json`、`local-files-build.json`、標準エラー出力を退避します。
3. 入力、設定の指紋値、前段マニフェストのSHA-256、分割ファイルの件数とチェックサムを照合します。
4. 同じ入力と設定であることを確認できれば、その段階の再開を試します。確認できなければ全成果物を再生成します。

標準設定の成果物だけがあり、独自ファイルが混在せず、必要な調査資料を保存済みの場合に限り、次のコマンドで全再生成できます。

```sh
rm -rf examples/data/local-files
python3 examples/local-files/local_files.py all \
  --config examples/local-files/local-files.toml \
  --target hybrid
```

最初のコマンドは、抽出文書、失敗記録、パッセージ、ベクトル、索引、マニフェスト、チェックポイント、ジャーナル、
PID、ログを含む`examples/data/local-files`全体を削除します。`input.root`が別の場所なら元ファイルは削除しません。
ただし、そのディレクトリへ利用者のファイルを置いた場合は実行してはいけません。この文書の検証では削除コマンドを実行していません。

## Wikipedia

### 実際のエラー表示

存在しないWikiExtractor入力を指定した場合は、現在の実装が次のように表示します。

```text
wikipedia-data: error: 'convert-dump' command failed
Reason: WikiExtractor input does not exist: /tmp/yappod2-docs-missing.jsonl
Input: /private/tmp/yappod2-docs-missing.jsonl
Output: /private/tmp/yappod2-docs-output.ndjson
How to fix:
  1. Confirm that the input exists and is readable: /private/tmp/yappod2-docs-missing.jsonl
  2. Run the preceding data preparation step if this file has not been generated yet.
```

### 処理別の確認項目

| 処理 | 表示または症状 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| 共通 | `invalid command arguments` | 必須引数、正の整数である`--limit`、サブコマンド名を確認します。 | `python3 examples/wikipedia-search/wikipedia_data.py <command> --help`を実行します。 |
| `fetch-api` | HTTPエラー、通信失敗、時間超過 | `--api-url`、ネットワーク、HTTP状態、`--user-agent`を確認します。 | 連絡先を含む説明的なUser-Agentを指定し、同じ`--output`へ再実行します。 |
| `fetch-api` | 不正JSON、期待する記事がない | Wikimedia API応答と指定した話題を確認します。 | 公式エンドポイントで再実行し、繰り返す場合だけデバッグ出力で応答エラーを調べます。 |
| `download-dump` | ダウンロード失敗 | `--base-url`、空き容量、書き込み権限、ネットワークを確認します。 | 同じ`--output-dir`へ再実行します。不完全な一時ファイルは完成済みファイルとして公開されません。 |
| `download-dump` | `checksum`不一致 | ダウンロード済みファイルと公式チェックサムが一致しません。 | 再実行し、繰り返す場合は空き容量と、プロキシーが内容を書き換えていないか確認します。 |
| `convert-dump` | 入力がない、空です | WikiExtractorのJSONLが生成済みか確認します。 | 前段のWikiExtractor処理を実行してから`convert-dump`を再実行します。 |
| `convert-dump` | `invalid JSON`、`invalid UTF-8`、必須フィールド不正 | 報告された行の`id`、`url`、`title`、`text`を確認します。 | 同じダンプから該当JSONLを作り直します。空白だけの本文や不正な行を有効な文書として扱いません。 |
| `embed` | 文書とパッセージが対応しません | 文書ID、パッセージID、`ordinal`、並び順、件数を確認します。 | 文書とパッセージを同じ入力と設定から再生成し、混在や並べ替えをせずに再実行します。 |
| `embed` | 埋め込み接続またはレスポンス不正 | `[embedding]`のプロバイダー、URL、`model`、`model_id`、`dimensions`、`data[].index`を確認します。 | 接続先へ最小リクエストを送り、正しい次元数と順序のレスポンスを確認してから再実行します。 |
| `embed` | 認証用環境変数がありません | `authorization_token_env`に記載した環境変数名を確認します。 | 現在のシェルへ値を設定します。トークン自体はTOMLへ書きません。 |

各サブコマンドの引数は次のコマンドで確認できます。

```sh
python3 examples/wikipedia-search/wikipedia_data.py fetch-api --help
python3 examples/wikipedia-search/wikipedia_data.py download-dump --help
python3 examples/wikipedia-search/wikipedia_data.py convert-dump --help
python3 examples/wikipedia-search/wikipedia_data.py embed --help
```

Wikipediaの処理は、既存の正式出力を無条件に上書きしません。削除や移動を行う前に、入力ダンプ、チェックサム、文書、
パッセージ、埋め込み付き文書が同じ処理系列であることを確認してください。

## search-web

### `build`、`start`、`stop`の違い

- `node examples/search-web/scripts/stack.mjs build --config PATH`は、`build.input`から`index.directory`へ索引だけを作ります。既存索引は上書きしません。
- `examples/search-web/scripts/start.sh --config PATH`は、必要なC実行ファイルとNode.js成果物を確認し、core、front、BFF、必要なら模擬LLMを起動します。既存索引を作り直しません。
- `examples/search-web/scripts/stop.sh --config PATH`は、同じ設定の`daemon.run_directory`にあるPIDファイルを使って一式を停止します。

### 設定、索引、依存関係

| 表示または症状 | 主な原因 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| `cannot read shared config` | `--config`がない、または読めません。 | `Config`の絶対パスを確認します。 | [`config.example.toml`](search-web/config.example.toml)と比較して同じコマンドを再実行します。 |
| `invalid shared config`、`unknown key`、`must be`、`is required` | TOML構文、キー名、型、範囲、必須項目が不正です。 | [`search-webの設定`](search-web/README.md#起動に必要な設定)で表示されたキーを確認します。 | 不要な任意セクションは見出しごと削除し、必要なキーを揃えます。 |
| `index path already exists` | `build`で既存索引を上書きしようとしました。 | `index.directory`と、その中の`manifest.json`を確認します。 | 既存索引を使うなら`start`、作り直すなら未使用の`index.directory`を指定します。 |
| `not a valid index` | ディレクトリはありますが、有効な索引ではありません。 | `config.toml`、`manifest.json`、`segments/`を確認します。 | ファイルを継ぎ足さず、正しい入力から別ディレクトリへ索引を作成します。 |
| `yappod binaries not found`または`cannot run` | C実行ファイルが未作成、または設定したパスが不正です。 | `build/yappod_core`、`build/yappod_front`と設定パスを確認します。 | `cmake --build build -j`を実行します。 |
| `dependencies not found` | search-webのnpm依存パッケージがありません。 | `examples/search-web/node_modules`を確認します。 | `(cd examples/search-web && npm ci)`を実行します。 |

### PID、ポート、起動待ち

| 表示または症状 | 主な原因 | 確認箇所 | 次に実行する操作 |
|---|---|---|---|
| `already running` | 同じPIDファイルが生存中の対象プロセスを指しています。 | 表示されたPID、PIDファイル、実際のコマンドラインを確認します。 | 同じ設定で`examples/search-web/scripts/stop.sh --config PATH`を実行してから起動します。 |
| `pid file`または`SIGKILL`を含む失敗 | PIDファイルが別プロセスを指すか、停止できません。 | PIDの所有者とコマンドラインを確認します。 | 無関係なプロセスではないと確認してから再試行します。PIDファイルを先に削除しません。 |
| `EADDRINUSE`または`address already in use` | core、front、Web、模擬LLMのいずれかのポートが使用中です。 | `daemon.core_port`、`daemon.front_port`、`web.port`、`mock.port`を確認します。 | 所有プロセスを確認し、同じスタックなら`stop`、別用途なら未使用ポートへ設定を変えます。 |
| `exited before becoming ready` | 起動したプロセスが準備完了前に終了しました。 | 表示された`error log`の末尾と、その前にある直接原因を確認します。 | ログの原因を直して`start`を再実行します。 |
| `did not become ready within ... ms` | ヘルスチェックが`web.startup_timeout_ms`内に成功しません。 | 表示された`health check` URL、PID、`error log`を確認します。 | 原因を直します。起動自体は正常で時間だけ不足する場合に限り`web.startup_timeout_ms`を増やします。 |

PIDとログは`daemon.run_directory`にあります。相対パスは設定ファイルのあるディレクトリを基準に解決されます。

| プロセス | PID | 標準出力 | 標準エラー出力 | 準備完了の確認先 |
|---|---|---|---|---|
| core | `core.pid` | `core.log` | `core.error` | TCP接続と内部ヘルスチェック要求です。 |
| front | `front.pid` | `front.log` | `front.error` | `http://HOST:FRONT_PORT/health/ready`です。 |
| Web BFF | `web.pid` | `web.log` | `web.error` | `http://HOST:WEB_PORT/api/status`です。 |
| 模擬LLM | `mock-llm.pid` | `mock-llm.log` | `mock-llm.error` | `http://HOST:MOCK_PORT/health`です。 |

たとえば標準設定のfrontを直接確認するには、次を実行します。

```sh
curl -i http://127.0.0.1:18400/health/ready
tail -n 80 examples/data/search-web/run/front.error
```

設定によってホスト、ポート、実行ディレクトリは変わります。エラー表示に出たURLとパスを優先してください。

### BFF、検索、LLM

| 症状 | 確認箇所 | 次に実行する操作 |
|---|---|---|
| BFFがfrontへ接続できません | frontの`/health/ready`、`web.yappod_timeout_ms`、`front.error`を確認します。 | core、front、BFFの順で準備状態を確認し、接続先と待ち時間を修正します。 |
| BFF起動時に`server startup failed`と表示されます | `Reason`、`Config`、`How to fix`を確認します。 | `node examples/search-web/server/dist/index.js --config PATH`を直接実行し、設定またはポートを修正します。 |
| LLMがHTTPエラーを返します | `llm.base_url`、`llm.model`、認証用環境変数、接続先ログを確認します。 | [LLM連携](search-web/docs/llm-integration.md)の最小curlを同じ設定で実行します。 |
| `finish_reason`が`stop`なのに回答が失敗します | `message.content`、`reasoning_content`、`completion_tokens`を確認します。 | `content`が空なら表示可能な本文はありません。出力上限、推論設定、モデルの応答を接続先で確認します。 |
| 生成が時間超過します | `llm.timeout_ms`と実測の生成時間、入力の`prompt_tokens`を確認します。 | 十分な待ち時間へ変更し、BFFを再起動してから再試行します。 |
| ソースを直しても挙動が変わりません | 実行中のclone、`server/src`、`server/dist`、PIDを確認します。 | `(cd examples/search-web && npm run build)`を実行し、同じ設定でスタックを停止してから起動します。 |

LLMの接続先画面で指定した値より、BFFがHTTPリクエストへ明示した`max_tokens`、`reasoning_effort`、
`temperature`などが優先されます。現在の実装が送る項目と空回答の詳しい切り分けは[LLM連携](search-web/docs/llm-integration.md)を参照してください。

## 認証情報を扱うときの注意

外部APIのトークンをTOMLへ直接書きません。`authorization_token_env`へ環境変数名を指定し、実際の値はプロセス環境から
渡します。設定、エラー、デバッグログを共有する前に、認証ヘッダーや入力本文が含まれていないことを確認してください。
