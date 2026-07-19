# local-files設定

この文書ではlocal-files固有のセクションを説明します。索引、デーモン、embedding、Webの共通設定は
[Yappod2設定リファレンス](../../../docs/configuration.md)を参照してください。

`collection_id`はlocal-filesでは必須です。英数字、ピリオド、アンダースコア、ハイフンを使った1〜32文字で、
同じ文書集合を識別する安定した値を指定します。

## `[input]`

| キー | 条件 | 説明 |
|---|---|---|
| `root` | パス、必須 | 収集を開始するディレクトリです。 |
| `include` | 文字列配列、既定値`["*", "**/*"]` | 対象にするglobパターンです。 |
| `exclude` | 文字列配列 | 対象から外すglobパターンです。既定では`.git`、`.venv`、`__pycache__`を除外します。 |
| `follow_symlinks` | 真偽値、既定値`false` | シンボリックリンクを辿るかを指定します。 |

`include`へ一致し、`exclude`へ一致しないファイルだけを処理します。相対パスは`root`を基準にメタデータへ保存します。

## `[output]`と`[prepare]`

| キー | 条件 | 説明 |
|---|---|---|
| `output.directory` | パス、必須 | 文書の分割ファイルを保存する場所です。 |
| `output.shard_max_bytes` | 正の整数、既定値67108864 | 1つの分割ファイルの目安です。 |
| `output.body_max_bytes` | 正の整数、既定値1000000、上限1048576 | 1文書を分割した各部分の本文上限です。 |
| `prepare.directory` | パス、任意 | パッセージの分割ファイルを保存する場所です。`rag`と`hybrid`で必要です。 |

出力ディレクトリは不可分に公開し、既存ディレクトリを無条件に上書きしません。

## `[extract]`

| キー | 条件 | 説明 |
|---|---|---|
| `max_extracted_bytes` | 正の整数、既定値67108864 | 抽出処理から受け取る本文の上限です。 |
| `enable_plugins` | 真偽値、既定値`false` | Pythonのentry pointによるプラグインを有効にします。 |
| `tika_command` | 文字列配列、任意 | `{path}`をちょうど1回含むTikaのコマンドです。 |
| `tika_timeout_ms` | 正の整数、既定値30000 | Tikaの完了を待つ時間です。 |

通常のテキストは組み込みの抽出処理で読みます。PDFやOffice文書には追加パッケージ、MarkItDownプラグイン、
またはApache Tikaが必要です。利用しない抽出処理の設定を残したまま有効にしないでください。

## `[formatters]`

| キー | 条件 | 説明 |
|---|---|---|
| `enabled` | 真偽値、既定値`true` | 外部の整形規則を利用するか指定します。 |
| `content_match_enabled` | 真偽値、既定値`false` | ファイル名だけでなく内容も規則の選択に使います。 |
| `content_scan_bytes` | 正の整数、既定値1048576 | 判定のために読む先頭バイト数です。 |
| `rules` | テーブルの配列、既定値`[]` | 外部整形コマンドの一覧です。 |

各規則には一意な`name`と、`{path}`をちょうど1回含む`command`が必要です。選択条件として
`basename_glob`、`path_glob`、`path_regex`、`content_regex`を指定できます。`timeout_ms`の既定値は30000、
`max_stdout_bytes`の既定値は67108864です。

外部コマンドには実ファイルのパスと制限時間を渡し、終了状態、出力上限、UTF-8を検証します。失敗したファイルは
失敗記録用の分割ファイルへ記録し、他の正常なファイルの処理を続けます。

## `[build]`

`build.yappo_makeindex`に実行ファイルのパスを指定します。設定ファイルからの相対パスです。実行ファイルが存在しない場合は、
リポジトリのルートで`cmake --build build -j`を実行してください。

## ディレクトリの分離

`output.directory`、`prepare.directory`、`embedding.directory`、`index.directory`は、同じパスや親子関係に
できません。処理段階ごとの不可分な公開と安全な再開を保つため、互いに独立したディレクトリを指定します。
