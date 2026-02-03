# Yappod2

Yappod2 は、旧 Yappod を現行環境（主に Clang/CMake）で動作させるための移植・整備プロジェクトです。

## プロダクト概要

Yappod2 は、**オフラインの索引作成** と **オンラインの検索提供** を分離した検索システムです。  
入力データを `yappo_makeindex` で索引化し、検索時は `yappod_front` / `yappod_core` の2層構成で応答します。  
具体的には、`yappod_front` がHTTPの受付とリクエスト中継・結果集約を担当し、`yappod_core` が索引に対する実検索を担当します。  
この分離により、重い索引更新バッチと検索API提供を独立して運用できるため、更新作業の影響を検索応答に出しにくい構成になります。  
単一構成（front 1台 + core 1台）でも動かせますし、core を複数指定して複数 core 構成へ拡張することもできます。

### コンポーネントの役割

- `yappo_makeindex`
  - 1行1ドキュメントの入力を読み取り、検索用インデックスを生成
  - `ADD` / `DELETE` を解釈して索引を更新
  - 不正行は警告してスキップし、処理全体は継続
- `yappod_core`（TCP `10086`）
  - 索引を読み込んで実検索を実行するワーカー
  - `AND` / `OR` やサイズ上限条件を使って結果を計算
- `yappod_front`（HTTP/TCP `10080`）
  - HTTPの検索リクエストを受ける入口
  - 受けたクエリを1台以上の `yappod_core` に中継し、結果を集約して返却

### サーバ構成時の動作（検索リクエストの流れ）

1. クライアントが `yappod_front` にHTTPリクエストを送信  
   例: `/yappo/100000/AND/0-10?キーワード`
2. `yappod_front` がリクエストをパースし、`-s` で指定された各 `yappod_core` へ同一クエリを送信
3. 各 `yappod_core` がローカル索引に対して検索を実行し、結果を返却
4. `yappod_front` が複数 core の結果をマージし、スコア順に整えてHTTPレスポンスとして返す

### 複数 core 利用の意図

- `yappod_front -s host1 -s host2 ...` のように複数指定すると、複数 core を束ねて扱えます
- 想定ユースケースは、索引の分散配置（シャーディング）や台数分散による処理能力の確保です
- 単一構成（front 1台 + core 1台）でも動作します

## 現在の前提

- 文字コードは **UTF-8 専用**。
- ビルドシステムは **CMake**。
- 検索デーモンのHTTPリクエストは標準形式（`/dict/max/op/start-end?query`）を使用。

## 必要環境

### macOS

- Command Line Tools
- Homebrew
- `cmake`, `berkeley-db`, `zlib`, `ninja`（Ninja利用時）

```bash
brew install cmake berkeley-db zlib ninja
```

### Linux (Ubuntu 例)

```bash
sudo apt-get update
sudo apt-get install -y cmake libdb-dev zlib1g-dev
```

## ビルド

### 通常ビルド

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### macOS(Homebrew)でBerkeley DBを明示する場合

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBDB_INCLUDE_DIR="$(brew --prefix berkeley-db)/include" \
  -DBDB_LIB="$(brew --prefix berkeley-db)/lib/libdb.dylib"
cmake --build build -j
```

### Ninjaジェネレータ

```bash
cmake -S . -B build -G Ninja ...
cmake --build build
```

## テスト

```bash
ctest --test-dir build --output-on-failure
```

## インストール

```bash
cmake --install build
```

インストール先を指定する場合:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/yappod" ...
cmake --build build -j
cmake --install build
```

## インデックス作成

### 入力フォーマット

1行1ドキュメント、タブ区切り:

```text
URL\tCOMMAND\tTITLE\tBODY_SIZE\tBODY
```

- `COMMAND`: `ADD` / `DELETE`
- `BODY` に改行・タブは含めない
- 不正行（列不足、`COMMAND`不正、`BODY_SIZE`非数値）は警告してスキップし、処理は継続

サンプル: `tests/fixtures/index.txt`

### インデックスディレクトリ作成

`pos/` サブディレクトリが必須です。

```bash
mkdir -p /tmp/yappoindex/pos
```

### 実行

```bash
yappo_makeindex -f tests/fixtures/index.txt -d /tmp/yappoindex
```

`-f` と `-l` は入力ソースの指定です。

- `-f <path>`: 単一の入力ファイルを処理
- `-l <dir>`: ディレクトリ内の `.gz` ファイルを順次処理

ディレクトリ一括処理の例:

```bash
yappo_makeindex -l /path/to/gz_dir -d /tmp/yappoindex
```

本文サイズフィルタ（デフォルト: `24`〜`102400` バイト）:

```bash
yappo_makeindex -f tests/fixtures/index.txt -d /tmp/yappoindex \
  --min-body-size 1 --max-body-size 200000
```

## pos 統合（`yappo_mergepos`）

`yappo_mergepos` は、`<index_dir>/pos/` 配下に分割された位置情報DBを1つにまとめる補助コマンドです。  
大量更新後のメンテナンスや、検索時のI/O断片化を減らしたいときに使います。

```bash
yappo_mergepos -l /tmp/yappoindex -d /tmp/yappoindex/pos/merged -s 0 -e 31
```

- `-l`: 入力インデックスディレクトリ（`deletefile` / `keywordnum` / `pos/` がある場所）
- `-d`: 出力ファイルのベース名（`<base>`, `<base>_index`, `<base>_size` を生成）
- `-s`, `-e`: 取り込む `pos` 番号の開始・終了（**両端を含む**）

補足:

- 指定範囲の入力は `pos/<n>`, `pos/<n>_index`, `pos/<n>_size` を読みます。
- `deletefile` を参照し、削除済みドキュメントの位置情報は除外して再生成します。
- 範囲内の一部ファイルが欠けていても、読めるものだけで処理を継続します。

主なエラー条件:

- `-l` が存在しないディレクトリ
- `-s` / `-e` が非数値、または `-s > -e`
- `-d` の出力先に書き込めない

## 検索

### CLI

```bash
search -l /tmp/yappoindex キーワード
```

複数語:

```bash
search -l /tmp/yappoindex -a キーワード1 キーワード2 キーワード3
search -l /tmp/yappoindex -o キーワード1 キーワード2
```

- `-a`: AND 検索（すべてのキーワードを含む文書のみヒット）
- `-o`: OR 検索（いずれかのキーワードを含む文書がヒット）
- 指定しない場合は AND（`-a`）として扱われます。

### デーモン

役割:

- `yappod_core`: 検索処理を実行するバックエンド（TCP `10086`）
- `yappod_front`: HTTPリクエスト受付と結果集約を行うフロント（TCP `10080`）

起動:

```bash
yappod_core -l /tmp/yappoindex
yappod_front -l /tmp/yappoindex -s localhost
```

`yappod_front` の `-s` は複数指定できます。複数の `yappod_core` へ同一クエリを送り、結果を集約します。

```bash
yappod_front -l /tmp/yappoindex \
  -s search-server1 \
  -s search-server2 \
  -s search-server3
```

HTTPクエリ例:

```text
http://localhost:10080/yappo/100000/AND/0-10?キーワード
```

HTTPリクエスト行としては次の形式です。

```text
GET /<dict>/<max>/<op>/<start>-<end>?<query> HTTP/1.1
```

各パラメータの意味:

- `dict`: 論理インデックス名（通常は `yappo` を指定）
- `max`: 対象にする文書サイズ上限（バイト）
- `op`: `AND` または `OR`
- `start` / `end`: 返却範囲（`start` 以上 `end` 未満）
- `query`: 検索語（URLエンコード前提）

補足:

- 複数語は `&` 区切りで指定できます（例: `...?検索&テスト`）
- `&` 自体を語として含めたい場合は `%26` を使ってください
- 実運用では `curl` などでURLエンコードした値を渡すのが安全です

`curl` 例:

```bash
curl 'http://localhost:10080/yappo/100000/AND/0-10?OpenAI2025'
curl 'http://localhost:10080/yappo/100000/OR/0-20?検索&テスト'
```

レスポンス形式:

```text
<総ヒット件数>
<今回返した件数>

URL\tタイトル\t文書サイズ\t最終更新時刻(epoch)\tスコア
...
```

`400 Bad Request` になりやすい条件:

- リクエスト行が `GET ... HTTP/x.y` 形式でない
- パスが `/<dict>/<max>/<op>/<start>-<end>?<query>` 形式でない
- `?` が無い（クエリ文字列が無い）
- `dict` / `op` / `query` のいずれかが空
- `max` が `0`
- パスやクエリが内部バッファ上限（1023文字）を超える

注意:

- `op` に `AND` / `OR` 以外を指定しても、現実装ではエラーではなく **AND 扱い** になります。

## トークナイズ方針（現状）

- 日本語などマルチバイト: バイグラム
- 英数字: 連続列を1トークン
- 記号（`-`, `_`, `/`, `+`, `.` など）: 既存互換ルール

将来的にトークナイザ差し替えを可能にする予定（TODO）。
