# local-filesで手元の文書を検索する

local-filesは、指定したディレクトリから文書を収集し、Yappod2の正式なNDJSON、パッセージ、埋め込み、索引を段階的に作るPythonコマンドです。入力ファイルは変更せず、各段階の成果物と検証情報を設定した出力先へ保存します。

## 対応する入力

組み込み処理では、UTF-8などとして読めるテキスト、ソースコード、JSON、JSON Lines、CSV、TSV、HTML、Markdown、PDF、docx、xlsx、pptxなどを扱います。形式と環境により、Pythonパッケージ、任意のMarkItDownプラグイン、Apache Tikaが必要です。未対応ファイルや抽出失敗は、全処理を黙って中止せず`failures-*.ndjson`へ記録します。

入力の絶対パスは文書IDやメタデータへ保存せず、`input.root`からの相対パスを`title`と`metadata.source_path`へ保存します。ただし文書本文自体に秘密情報や絶対パスが書かれていれば、その内容は検索対象になります。

## 必要な環境

リポジトリのルートで実行します。

```sh
cmake --build build -j
python3 -m venv examples/local-files/.venv
examples/local-files/.venv/bin/python -m pip install \
  -r examples/local-files/requirements-core.txt
```

Python 3.9と3.10ではTOML読込用の`tomli`も依存ファイルから導入します。PDF、Office、プラグインの追加依存は、実際に扱う形式に合わせて導入してください。Tikaは自動でダウンロードしません。

## このリポジトリ自身を検索する

`local-files-yappod2.toml`は、このリポジトリを入力にし、索引作成成果物、`.git`、`node_modules`などを除外する設定です。語彙索引を作ります。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target lexical
```

設定例では、語彙検索に不要な`[prepare]`と`[embedding]`をコメントにしています。`--target lexical`では埋め込みAPIを呼ばず、`[vector].enabled`は`false`のままにします。複合検索へ切り替える場合だけ、完全なセクションを有効にしてください。

```sh
./build/search \
  --config examples/local-files/local-files-yappod2.toml \
  --mode lexical \
  --query yappo_makeindex \
  --limit 10
```

## 自分のディレクトリを対象にする

`local-files.toml`を複製し、少なくとも次を変更します。相対パスは複製したTOMLのディレクトリを基準にします。

```toml
collection_id = "my-documents"

[input]
root = "/path/to/documents"
include = ["*", "**/*"]
exclude = ["**/.git/**", "**/node_modules/**"]
follow_symlinks = false

[output]
directory = "../data/local-files/documents"

[index]
directory = "../data/local-files/index"
```

`collection_id`は`[A-Za-z0-9._-]{1,32}`です。別の文書集合には別の値と出力ディレクトリを使います。`output.directory`、`prepare.directory`、`embedding.directory`、`index.directory`は、互いに包含しない別ディレクトリでなければなりません。

全キーの型、既定値、上限、フォーマッター規則は[local-files設定リファレンス](docs/configuration.md)を参照してください。

## `all`で作る範囲を選ぶ

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config CONFIG \
  --target TARGET
```

| `TARGET` | 実行する段階 | 最終成果物 |
|---|---|---|
| `documents` | `convert` | 文書NDJSONと抽出失敗記録です。索引は作りません。 |
| `lexical` | `convert` → `build` | 語彙検索用索引です。`[vector].enabled = false`が必要です。 |
| `rag` | `convert` → `prepare` → `build` | パッセージも準備しますが、索引はベクトルを持ちません。RAGの語彙検索経路を試せます。 |
| `hybrid` | `convert` → `prepare` → `embed` → `build` | パッセージベクトルとANNを持つ索引です。完全な`[embedding]`と`[vector].enabled = true`が必要です。 |

既存の段階がある場合、`all`はマニフェスト、チェックサム、設定の指紋値、元入力のスナップショットを検証してから再利用します。単にディレクトリが存在するだけでは完成済みと判断しません。

`convert`と`all`には`--content-match`または`--no-content-match`を付け、フォーマッターの`content_regex`判定をTOMLより一時的に優先できます。この指定も指紋値へ含むため、異なる指定の成果物は再利用しません。

## コマンドライン引数

すべてのサブコマンドで`--config`が必須です。設定ファイル内の相対パスは、コマンドを実行した場所ではなく、その設定ファイルがあるディレクトリを基準に解決します。

| サブコマンド | 引数 | 必須条件と意味 |
|---|---|---|
| `convert` | `--config PATH` | 必須です。収集、抽出、出力先を含むTOMLを読みます。 |
| `convert` | `--content-match` / `--no-content-match` | どちらか一方を指定できます。`formatters.content_match_enabled`を今回の実行だけ上書きします。 |
| `prepare` | `--config PATH` | 必須です。完成済み文書成果物を検証してパッセージを作ります。 |
| `embed` | `--config PATH` | 必須です。完成済み文書とパッセージを検証してベクトルを付けます。 |
| `build` | `--config PATH` | 必須です。完成済みの前段成果物と索引設定を読みます。 |
| `build` | `--target lexical\|rag\|hybrid` | 必須です。索引へ渡す前段成果物と、必要なベクトル構成を選びます。 |
| `all` | `--config PATH` | 必須です。必要な段階を順に実行します。 |
| `all` | `--target documents\|lexical\|rag\|hybrid` | 必須です。最終的に作る成果物を選びます。 |
| `all` | `--content-match` / `--no-content-match` | どちらか一方を指定できます。`convert`段階の内容照合を上書きします。 |

サブコマンドごとの使い方は`python3 examples/local-files/local_files.py <サブコマンド> --help`で確認できます。引数不足や未知の引数は終了状態0以外となり、標準エラー出力に`Reason`と`How to fix`を含む案内を表示します。

## 段階ごとに実行する

### 1. 文書へ変換する

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py convert --config CONFIG
```

`input.include`へ一致し、`input.exclude`へ一致しないファイルを安定した順序で処理します。文書IDは文書集合の識別子、相対パス、分割番号から作り、本文が`output.body_max_bytes`を超える場合は複数文書へ分けます。成功時は`output.directory`を不可分に公開し、既存ディレクトリは上書きしません。

### 2. パッセージを作る

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py prepare --config CONFIG
```

文書マニフェストと現在の入力スナップショットを検証し、Cの`yappo_makeindex prepare`を各文書分割ファイルへ適用します。`[prepare].directory`が必要です。トークナイザーとチャンク分割の設定は同じアプリケーションTOMLから読みます。

### 3. 埋め込みを付ける

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py embed --config CONFIG
```

文書とパッセージのID、順序、件数を照合し、バッチごとに埋め込みAPIを呼びます。途中経過はジャーナルとチェックポイントへ保存し、同じ入力、設定、モデル、次元数であれば完了済み入力分割ファイルを再利用します。最終成果物は元の文書操作へ`vectors`を付けたNDJSONです。

### 4. 索引を作る

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py build \
  --config CONFIG \
  --target lexical
```

`--target`は`lexical`、`rag`、`hybrid`です。前段階の全分割ファイルをFIFOへ順に流し、1回の`yappo_makeindex build`を実行します。巨大な連結NDJSONをディスクへ作りません。索引作成後は、索引の設定、世代、文書数、各コンポーネントの大きさとSHA-256、必要なベクトルコンポーネントを検証し、`local-files-build.json`を保存します。

## 語彙検索用の設定

語彙索引では次の形にします。`[embedding]`はセクションごと省略します。将来用の設定例を残す場合も、見出しと全キーをコメントにして、空のセクションを有効にしないでください。

```toml
[vector]
enabled = false
```

`[vector].enabled = false`の索引にベクトルは保存されません。アプリケーションTOMLだけを`true`へ変えても複合検索索引にはなりません。

## ベクトル対応索引の設定

`[vector]`は索引へ保存する互換条件、`[embedding]`はAPI接続とベクトル生成方法です。

```toml
[vector]
enabled = true
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
metric = "cosine"

[embedding]
directory = "../data/local-files/vectors"
provider = "lmstudio"
base_url = "http://127.0.0.1:1234/v1"
model = "text-embedding-embeddinggemma-300m"
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
prompt_profile = "embeddinggemma"
timeout_ms = 60000
batch_size = 16
```

`embedding.model`は接続先へ送る実モデル名です。両セクションの`model_id`と`dimensions`は一致させます。外部APIのトークンは`authorization_token_env`へ環境変数名を指定し、トークン自体をTOMLへ書きません。

## フォーマッター、プラグイン、Tika

抽出は次の優先関係で行います。

1. 有効な`[[formatters.rules]]`のうちパスと任意の内容条件に一致したコマンドを実行します。
2. 対応する組み込み抽出処理を使います。
3. 組み込みで処理できず`extract.enable_plugins = true`ならMarkItDownプラグインを試します。
4. 抽出に失敗し`extract.tika_command`があればTikaコマンドを試します。
5. 最後にtextとして検出できるか確認し、できなければ失敗へ記録します。

外部コマンドの配列には`{path}`をちょうど1回含めます。シェル文字列として連結せず引数配列で実行します。タイムアウトと標準出力上限を超えた結果は失敗です。内容によるフォーマッター判定はファイルを追加で読むため、既定では無効です。

## 成果物

| 段階 | 主なファイル |
|---|---|
| `convert` | `documents-000001.ndjson`、`failures-000001.ndjson`、`manifest.json` |
| `prepare` | `passages-000001.ndjson`、`manifest.json` |
| 埋め込み | `documents-000001.ndjson`、`manifest.json`、処理中だけ使う作業ディレクトリ、チェックポイント、ジャーナル |
| `build` | Yappod2の`config.toml`、`manifest.json`、`segments/`、`local-files-build.json` |

各マニフェストの正確なフィールドと再開条件は[処理工程と再生成](docs/pipeline-and-recovery.md)で説明します。

## Web UIで検索する

```sh
cd examples/search-web
npm ci
cd ../..
examples/search-web/scripts/start.sh \
  --config examples/local-files/local-files-yappod2.toml
```

既定は`http://127.0.0.1:4173`です。local-files文書は外部URLを持たず、画面は`title`の相対パスを表示します。停止時も同じ設定を渡します。

```sh
examples/search-web/scripts/stop.sh \
  --config examples/local-files/local-files-yappod2.toml
```

## 失敗時の確認

標準出力には成功時の要約JSON、標準エラー出力には失敗した操作、`Reason`、`Config`、`Input`、`Output`、`How to fix`を表示します。抽出だけに失敗したファイルは`failures-*.ndjson`の`path`、`code`、`message`、`extractor`を確認します。

既存成果物を検証できない場合、個別のチェックポイントやマニフェストを手編集しないでください。必要な調査資料を保存し、全成果物が元入力から再生成できる場合だけ全体を作り直します。詳細は[処理工程と再生成](docs/pipeline-and-recovery.md)と[サンプルの問題解決](../troubleshooting.md)を参照してください。
