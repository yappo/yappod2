# local-filesで手元の文書を検索する

local-filesは、ディレクトリ内の文書を正式な入力NDJSONへ変換し、必要に応じてパッセージ生成、embedding、索引作成まで
実行するPythonコマンドです。テキスト、ソースコード、PDF、Office文書などを扱い、大きな入力は複数の分割ファイルへ分けます。

## 最初に試す

このリポジトリ自身を検索する設定`local-files-yappod2.toml`を用意しています。リポジトリのルートで準備します。

```sh
cmake --build build -j
python3 -m venv examples/local-files/.venv
examples/local-files/.venv/bin/python -m pip install \
  -r examples/local-files/requirements-core.txt
```

次のコマンドは文書収集から語彙索引の作成まで実行します。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target lexical
```

成功すると`examples/data/local-files`の下に文書の分割ファイルと索引が作られます。

```sh
./build/search \
  --config examples/local-files/local-files-yappod2.toml \
  --mode lexical \
  --query yappo_makeindex \
  --limit 10
```

## Web UIで検索する

```sh
cd examples/search-web
npm install
cd ../..

examples/search-web/scripts/start.sh \
  --config examples/local-files/local-files-yappod2.toml
```

既定では`http://127.0.0.1:4173`で検索できます。停止時も同じ設定を渡します。

```sh
examples/search-web/scripts/stop.sh \
  --config examples/local-files/local-files-yappod2.toml
```

local-filesの文書はURLを持たず、`title`に入力元からの相対パスを保存します。Web UIはこれを外部リンクにしません。

## 自分のディレクトリを対象にする

`local-files.toml`を複製し、少なくとも次を変更します。

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

入力ファイルの絶対パスはIDやメタデータへ保存しません。ただし、文書本文に秘密情報や絶対パスが書かれていれば検索対象に
なります。対象にすべきでないファイルは`input.exclude`へ追加してください。

## `all --target`

| 対象 | 作成する成果物 |
|---|---|
| `documents` | 文書の分割ファイルだけを作ります。 |
| `lexical` | 文書の分割ファイルと、ベクトルを使わない索引を作ります。 |
| `rag` | 文書とパッセージの分割ファイルを作ります。 |
| `hybrid` | パッセージのembeddingを含む索引まで作ります。 |

`hybrid`では`[vector]`と完全な`[embedding]`が必要です。`model_id`と`dimensions`を一致させてください。

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

外部APIを使う場合は`authorization_token_env`へ環境変数名を指定します。トークン自体はTOMLへ書きません。

## 個別のコマンド

```sh
# ファイルを文書の分割ファイルへ変換します。
examples/local-files/.venv/bin/python examples/local-files/local_files.py convert --config CONFIG

# 文書の分割ファイルからパッセージを作ります。
examples/local-files/.venv/bin/python examples/local-files/local_files.py prepare --config CONFIG

# パッセージへembeddingを付けます。
examples/local-files/.venv/bin/python examples/local-files/local_files.py embed --config CONFIG

# 検証済みの分割ファイルから索引を作ります。
examples/local-files/.venv/bin/python examples/local-files/local_files.py build --config CONFIG --target lexical
```

設定の全項目は[local-files設定](docs/configuration.md)、分割ファイル、manifest、チェックポイント、再開は
[処理の流れと再生成](docs/pipeline-and-recovery.md)を参照してください。

## 問題が発生した場合

コマンドは失敗した操作、原因、対象パス、修正手順を標準エラー出力へ表示します。Python 3.9/3.10でTOMLモジュールを
読み込めない場合、整形プログラムやTikaがない場合、embeddingや`yappo_makeindex`へ接続できない場合も、必要な準備を
案内します。

既存成果物を検証できない場合でも、すぐに削除しないでください。manifest、チェックサム、チェックポイント、入力スナップショットを
確認し、必要な成果物を保存してから全再生成を判断します。詳しくは
[サンプルの問題解決](../troubleshooting.md)を参照してください。
