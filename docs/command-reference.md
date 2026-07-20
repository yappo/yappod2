# コマンドリファレンス

この文書では、Yappod2がインストールする各コマンドの書式、入力、出力、副作用、失敗条件を説明します。例ではリポジトリのルートディレクトリにいるものとし、ビルドした実行ファイルを`./build/...`で呼び出します。インストール済みなら先頭の`./build/`を省略できます。

## 共通する終了状態と出力

- 正常終了は終了状態`0`です。`yappo_makeindex`の最上位、`yappo_makeindex verify`、`search`、`yappod_core`、`yappod_front`では`--help`または`-h`を指定すると使い方を表示して`0`を返します。`prepare`、`build`、`update`、`yappo_compact`には専用のヘルプオプションがなく、必要な引数を省略すると使い方を標準エラー出力へ表示して失敗します。
- 引数、設定、入力、索引、I/Oのいずれかに問題があれば0以外を返し、理由を標準エラー出力へ書きます。
- JSONを返すコマンドの正常結果は標準出力へ1行で書きます。運用スクリプトでは、標準出力と標準エラー出力を分けて扱ってください。
- パスを取るオプション自体は、通常はコマンドを実行したディレクトリを基準にします。ただし、TOML内の相対パスはTOMLのディレクトリを基準に解決します。

## 索引へ登録するNDJSON

`yappo_makeindex build`と`update`が読む正式な入力は、UTF-8のNDJSONです。空行を含めず、1行に一つのJSONオブジェクトを書きます。この1行を、この文書では「操作」と呼びます。操作は、文書を登録または置換する`upsert`と、文書を削除する`delete`の総称です。

### `upsert`

次の例は読みやすいように改行しています。NDJSONファイルへ保存するときは、このオブジェクト全体を1行にします。

```json
{
  "operation": "upsert",
  "id": "doc-1",
  "url": "https://example.test/1",
  "title": "検索入門",
  "body": "検索対象となる本文です。",
  "metadata": {
    "language": "ja"
  },
  "updated_at_unix_ms": 1784516400000
}
```

| フィールド | 必須 | 型と上限 | 説明 |
|---|---|---|---|
| `operation` | 必須 | 文字列`upsert` | 同じ`id`の古い文書を置き換えます。 |
| `id` | 必須 | 空でない文字列、最大255バイト | 索引内で一意な文書IDです。 |
| `url` | 任意 | 文字列、最大8192バイト、デフォルトは`""` | 検索結果と取得結果へ返す出典URLです。URLとしての構文検証はしません。 |
| `title` | 任意 | 文字列、最大255バイト、デフォルトは`""` | 語彙検索のtitleフィールドと表示に使います。 |
| `body` | 必須 | 文字列、最大1048576バイト | 検索対象本文です。空文字は形式上受理されますが、本文断片を生成できないため実用上は本文を入れてください。 |
| `metadata` | 任意 | JSONオブジェクト、正規化後最大1048576バイト、デフォルトは`{}` | 文書コンポーネントへ保存し、`filterable_fields`で選んだ値を絞り込み索引へ入れます。現行の検索レスポンスには返しません。 |
| `updated_at_unix_ms` | 任意 | 0以上のJSON整数、デフォルトは`0` | 更新時刻をUnix epochからのミリ秒で表します。値の意味は呼び出し側で管理します。負の値は拒否されます。 |
| `vectors` | ベクトル対応索引では必須 | 数値の二次元配列 | `[chunking]`で生成される本文断片順に並べます。行数は本文断片数、各行の要素数は`vector.dimensions`と一致させ、全要素を有限な浮動小数点数にします。 |

オブジェクト内と、その中に含まれるオブジェクト内の重複キーは拒否されます。未知のトップレベルフィールドも拒否されます。`metadata`はキー順へ正規化して保存されます。

### `delete`

```json
{
  "operation": "delete",
  "id": "doc-2"
}
```

`delete`は`operation`と`id`の二つだけを許可します。削除対象が現在の索引にない場合でも削除標識として公開されます。同じ更新バッチ内で同じIDを二度指定することはできません。

## `yappo_makeindex`

### サブコマンド一覧

```text
yappo_makeindex prepare ...
yappo_makeindex build ...
yappo_makeindex update ...
yappo_makeindex verify ...
```

サブコマンドを省略した場合や、未知のサブコマンドを指定した場合は、使い方を標準エラー出力へ書いて失敗します。

## `yappo_makeindex prepare`

### 書式

```text
yappo_makeindex prepare --config CONFIG --input INPUT --output OUTPUT
                        [--input-format ndjson|tsv]
```

### 役割

文書入力を`[chunking]`で分割し、埋め込み処理などへ渡す本文断片NDJSONを作ります。この出力は本文断片用の中間形式であり、`yappo_makeindex build`へそのまま渡す文書NDJSONではありません。

### オプション

| オプション | 必須 | 説明 |
|---|---|---|
| `--config CONFIG` | 必須 | 共通アプリケーションTOMLです。`[chunking]`を含む必須セクションをすべて読みます。 |
| `--input INPUT` | 必須 | 入力ファイルです。`--input-format`に応じてNDJSONまたはTSVとして読みます。標準入力の`-`は実装されていません。 |
| `--output OUTPUT` | 必須 | 本文断片NDJSONの出力ファイルです。標準出力の`-`は実装されていません。現行実装は`w`で開くため、既存ファイルを上書きします。必要なファイルを指定しないでください。 |
| `--input-format` | 任意 | `ndjson`または`tsv`です。デフォルトは`ndjson`です。 |

### NDJSON入力と出力

NDJSON入力は前述の`upsert`または`delete`です。`upsert`一件から本文断片ごとに次の行を出力します。

`ordinal`は、同じ文書の中で何番目の本文断片かを表すJSON項目です。番号は文書ごとに0から始まります。

```json
{
  "operation": "upsert",
  "document_id": "doc-1",
  "passage_id": "p-9a6d14af8d0bd5f4",
  "ordinal": 0,
  "start_char": 0,
  "end_char": 12,
  "text": "検索対象となる本文です。",
  "metadata": {
    "language": "ja"
  }
}
```

| 出力フィールド | 説明 |
|---|---|
| `document_id` | 元の文書IDです。 |
| `passage_id` | 文書ID、文書内の本文断片番号、分割後の本文からFNV-1a方式で計算した64 bit値を、`p-`に16桁の小文字16進数を続けた形で表します。上の値は、この例の文書IDと本文から実装と同じ手順で計算した値です。 |
| `ordinal` | 同じ文書の中で何番目の本文断片かを表す番号です。最初は`0`、次は`1`です。文書が変わると再び`0`から始まります。 |
| `start_char`、`end_char` | 元本文中のUnicode文字位置です。 |
| `text` | 分割後の本文です。 |
| `metadata` | 元文書のmetadataです。 |

`delete`は`{"operation":"delete","id":"..."}`のまま出力します。

### TSV入力アダプター

TSVは旧入力から移行するためのアダプターです。各行を次の5列で読みます。

```text
ID<TAB>ADD|DELETE<TAB>TITLE<TAB>BODY_BYTES<TAB>BODY
```

`ADD`ではIDをURLにも使い、metadataは`{}`になります。`BODY_BYTES`には、行末のCRとLFを除いた`BODY`のUTF-8バイト数を指定します。文字数ではありません。現行実装はCの`strlen`で照合するため、埋め込みNULを含む本文は扱えません。新しい連携ではNDJSONを使用してください。

### 成功と失敗

成功時の標準出力はありません。入力を途中まで処理した後で失敗した場合、出力ファイルには途中までの行が残ります。不可分な公開ではないため、失敗した出力を再利用せず削除または別名へ退避してから再実行してください。

## `yappo_makeindex build`

### 書式

```text
yappo_makeindex build --config CONFIG --input documents.ndjson
```

### オプション

| オプション | 必須 | 説明 |
|---|---|---|
| `--config CONFIG` | 必須 | `[index].directory`を含む共通TOMLです。 |
| `--input INPUT` | 必須 | `upsert`だけを含む文書NDJSONです。空行、`delete`、未知のフィールドを拒否します。 |

### 処理と上限

`build`は初回の索引作成に使うため、入力ファイルを最後まで読みます。多数の文書は最大10000件ずつ処理し、必要に応じて
複数のセグメントへ分けて保存します。文書数そのものに固定の上限はありませんが、各索引ファイルの256 MiB上限、
最大セグメント数、利用可能なディスク容量とメモリー量による制限を受けます。

索引作成は次の順序で公開します。

1. 最終出力先と同じ親ディレクトリに一時ディレクトリを作ります。
2. 索引設定だけを`config.toml`へ保存し、空の世代1のマニフェストを作ります。
3. 入力を読み、セグメントと新しいマニフェストを一時ディレクトリへ作ります。
4. 全処理が成功した場合だけ、一時ディレクトリを`[index].directory`へ名前変更します。

出力先が既に存在する場合は、空ディレクトリであっても失敗します。既存索引を上書きしません。失敗時は一時ディレクトリを回収します。

### 正常出力

```json
{
  "generation": 3,
  "accepted": 12345
}
```

`accepted`は入力で受理した`upsert`行の総数です。世代は内部バッチごとに進むため、入力件数が10000件を超えると2より大きくなる場合があります。

### 主な失敗

- 設定または入力を開けない場合
- 入力が空、空行を含む、または`delete`を含む場合
- `[index].directory`が既に存在する場合
- ベクトルの行数・次元数が生成本文断片と一致しない場合
- 一つの文書を一つのセグメントへ格納できない場合
- 一時セグメント、チェックサム、マニフェストの検証または公開に失敗した場合

## `yappo_makeindex update`

### 書式

```text
yappo_makeindex update --config CONFIG --input operations.ndjson
```

### オプション

| オプション | 必須 | 説明 |
|---|---|---|
| `--config CONFIG` | 必須 | 更新対象の`[index].directory`を含む共通TOMLです。 |
| `--input INPUT` | 必須 | 1〜100行の`upsert`または`delete`を含むNDJSONです。1行を文書1件の登録、更新、削除として数えます。 |

`update`は入力した1〜100件をまとめて検証し、すべて正常な場合だけ一度に公開します。101件以上を更新する場合は、
入力を複数のファイルへ分けて`update`を繰り返してください。この上限は1回の更新処理に対するものであり、索引全体の
文書数を100件へ制限するものではありません。

### 不可分性と競合

入力をすべて解析し、同じバッチ内の文書ID重複、本文断片、ベクトル、セグメント容量を検証してからマニフェストを公開します。一行でも不正なら、バッチ全体を公開しません。更新中は索引ディレクトリの書き込みロックを取得します。公開直前に世代が変わった場合は競合として失敗し、作成途中のセグメントを回収します。

更新は新しいセグメントを追加します。古い文書ファイルをその場で書き換えません。`upsert`と`delete`のどちらも、同じIDの古い版を検索結果から隠します。

### 正常出力

```json
{
  "generation": 3,
  "accepted": 2,
  "upserts": 1,
  "deletes": 1,
  "segment_ids": [
    "seg-00000000000000000003-AbCd12"
  ]
}
```

`segment_ids`は、このバッチから作成して新しいマニフェストに追加したセグメントです。一つのバッチでも容量に応じて複数になる場合があります。

## `yappo_makeindex verify`

### 書式

```text
yappo_makeindex verify --config CONFIG
```

検証する索引は、`CONFIG`内の`[index].directory`で指定します。

### 検証内容

- 索引内`config.toml`の型、値、互換条件
- `manifest.json`の形式、世代、設定指紋、セグメントとコンポーネント記述子
- マニフェストが参照する各ファイルの存在、大きさ、SHA-256
- `.yap2`共通ヘッダーのマジック値、版、種類、世代、ペイロード長、CRC32C
- 文書、語彙、転置リスト、出現位置、メタデータ、ベクトル、削除標識の内部境界と参照整合性
- ベクトルを高速に検索するための`vectors.usearch`と、元ベクトルを保存する`vectors.yap2`の次元数、距離方式、件数の一致

正常時は次の1行を出力します。

```text
verified generation=3 segments=2
```

失敗時は`verification failed: STATUS`を標準エラー出力へ書きます。verifyは修復を行いません。

## `search`

### 書式

```text
search (--config CONFIG | --index INDEX_DIR) --mode lexical|vector|hybrid
       [--query TEXT] [--vector N,N,...]
       [--scope documents|passages] [--limit 1..100]
```

### オプション

| オプション | 必須 | デフォルトと説明 |
|---|---|---|
| `--config CONFIG` | `--index`と一方だけ必須 | 共通TOMLから`[index].directory`を解決します。 |
| `--index INDEX_DIR` | `--config`と一方だけ必須 | 索引ディレクトリを直接指定します。アプリケーション設定との一致確認は行いません。 |
| `--mode` | 必須 | `lexical`、`vector`、`hybrid`のいずれかです。 |
| `--query TEXT` | 語彙検索と複合検索で必須 | 空でない検索文です。ベクトルでは省略できます。 |
| `--vector N,N,...` | ベクトルと複合検索で必須 | comma区切りの有限な浮動小数点数です。空要素、末尾comma、NaN、無限大、float範囲外を拒否します。要素数は索引のdimensionsと一致させます。 |
| `--scope` | 任意 | 検索結果を文書ごとにまとめる`documents`と、分割した本文を一つずつ返す`passages`があります。デフォルトは`documents`です。通常の検索結果一覧には`documents`、RAG用の参照箇所を探す場合には`passages`を使用します。 |
| `--limit` | 任意 | 1〜100の整数です。デフォルトは`10`です。 |

`search`は指定された索引ファイルを直接読み込み、検索結果を標準出力へ書きます。`yappod_core`や`yappod_front`を
起動せずに、索引作成後の動作確認ができます。メタデータによる絞り込み、フレーズ検索、AND/ORの指定、検索結果の
続きを取得する機能はコマンドラインから指定できないため、これらが必要なアプリケーションでは`POST /v2/search`を使用します。

正常時は[`POST /v2/search`](yappod-front-api.md#post-v2search)と同じJSONを標準出力へ書きます。検索条件が不正でHTTP相当の状態が200以外になった場合は、JSONエラーを標準エラー出力へ書いて失敗します。

## `yappo_compact`

### 書式

```text
yappo_compact (--config CONFIG | --index INDEX_DIR)
```

`--config`と`--index`は一方だけ指定します。その他のオプションはありません。

### 処理

1. 書き込みロックを取得し、現在の`config.toml`、マニフェスト、全コンポーネントを検証します。
2. 最新のマニフェストで有効な文書と本文断片だけを収集します。
3. 現在の索引設定に従い、新しい一つ以上の`compact-*`セグメントへ再構築します。
4. 開始時の世代が変わっていない場合だけ、compactセグメントだけを参照する次のマニフェストを公開します。
5. 公開中マニフェストから参照されないセグメントディレクトリを回収します。

コンパクション中は`<index-directory>/compaction.state`へ状態を書きます。失敗しても開始時のマニフェストは維持されます。途中でプロセスが終了した場合、ヘルスチェックとメトリクスは実行中PIDの不在から`interrupted`を報告します。

### 正常出力

```json
{
  "generation": 4,
  "documents": 12000,
  "passages": 36000,
  "removed_segments": 7,
  "segment_ids": [
    "compact-00000000000000000004-XyZ123"
  ]
}
```

`removed_segments`はコンパクション開始前と公開後の回収で削除した、マニフェストから参照されないセグメントの合計です。

## `yappod_core`

### 書式

```text
yappod_core --config CONFIG
yappod_core --index INDEX_DIR [--port PORT]
```

| オプション | 条件 | 説明 |
|---|---|---|
| `--config CONFIG` | 推奨。ほかのオプションと併用不可 | 索引、coreのホスト名とポート、実行時ファイルのディレクトリ、処理上限、トークンを共通TOMLから読みます。 |
| `--index INDEX_DIR` | `--config`を使わない場合に必須 | 索引を直接指定します。実行時設定はデフォルトになります。 |
| `--port PORT` | `--index`形式だけで任意 | `yappod_front`から検索や更新の依頼を受ける専用ポートです。HTTPポートではありません。デフォルトは`18401`で、1〜65535を指定します。 |

`--config`形式では`daemon.core_host`を待ち受けアドレスとして使います。`--index`形式ではホストを指定せず、`getaddrinfo`で受動接続用のアドレスを取得して待ち受けます。

### 起動とプロセス

coreは索引を開いて検証し、ポートを確保してからforkします。親プロセスは成功状態で終了し、子プロセスが処理を継続します。`--config`形式では次を`daemon.run_directory`へ保存します。

- `core.pid`: 子プロセスのPIDです。正常終了時に削除します。
- `core.log`: 子プロセスの標準出力を追記します。
- `core.error`: 子プロセスの標準エラー出力を追記します。

`--index`形式ではこれらをコマンド実行時のディレクトリの`core.pid`、`core.log`、`core.error`へ作ります。通常は場所が明確な`--config`形式を使用してください。

coreは16本のワーカースレッドで内部接続を受け、1秒ごとに新しいマニフェストを確認します。公開済み世代を検出すると、検証に成功したスナップショットへ切り替えます。`SIGTERM`または`SIGINT`で待ち受けを閉じて終了します。

coreは外部クライアント向けHTTPサーバーではありません。通常の利用者はfrontへ接続してください。front/core間の形式は[frontとcoreの通信仕様](yappod-core-protocol.md)で説明します。

## `yappod_front`

### 書式

```text
yappod_front --config CONFIG
yappod_front --index INDEX_DIR --core-host HOST
             [--port PORT] [--core-port PORT]
```

| オプション | 条件 | 説明 |
|---|---|---|
| `--config CONFIG` | 推奨。ほかのオプションと併用不可 | 索引、frontのホスト名とポート、core接続先、実行時ファイルのディレクトリ、処理上限、書き込み用トークンを読みます。 |
| `--index INDEX_DIR` | 直接指定形式で必須 | front自身が準備完了とmetricsを判定する索引です。coreも同じ索引を開いている必要があります。 |
| `--core-host HOST` | 直接指定形式で必須 | coreへ接続するホストです。 |
| `--port PORT` | 直接指定形式で任意 | HTTPポートです。デフォルトは`18400`、範囲1〜65535です。 |
| `--core-port PORT` | 直接指定形式で任意 | frontからcoreへ検索や更新を依頼する専用ポートです。デフォルトは`18401`、範囲1〜65535です。 |

frontは索引に少なくとも一つのセグメントがあることを確認し、HTTPポートを確保してからforkします。`--config`形式では`front.pid`、`front.log`、`front.error`を`daemon.run_directory`へ保存します。直接指定形式では実行時のディレクトリへ保存します。

frontは16本のワーカースレッドでHTTP/1.xリクエストを処理します。検索、RAG向けの本文取得、文書更新はcoreへ依頼し、
結果をHTTPレスポンスへ変換します。文書を本文断片へ分割する`POST /v2/passages:prepare`、ヘルスチェック、メトリクスは
front自身が処理します。対応するHTTP仕様は[`yappod_front` APIリファレンス](yappod-front-api.md)、メトリクスは
[監視とメトリクス](observability.md)を参照してください。

## 安全な実行順序

初めて作る索引では次の順序で確認します。

```sh
./build/yappo_makeindex build \
  --config examples/config.lexical.toml \
  --input examples/documents.lexical.ndjson

./build/yappo_makeindex verify \
  --config examples/config.lexical.toml

./build/search \
  --config examples/config.lexical.toml \
  --mode lexical \
  --query "modern search"

./build/yappod_core --config examples/config.lexical.toml
./build/yappod_front --config examples/config.lexical.toml
```

既に索引ディレクトリがある場合は、`build`を再実行しないでください。更新なら`update`、不要セグメントの統合なら`yappo_compact`を使用します。
