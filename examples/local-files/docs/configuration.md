# local-files設定リファレンス

この文書では、`examples/local-files/local_files.py`が読み取る設定を説明します。索引形式、Yappod2サーバー、Web、埋め込み、LLMの共通項目は[Yappod2設定リファレンス](../../../docs/configuration.md)を参照してください。

すべての相対パスはTOMLが置かれているディレクトリを基準に解決されます。`local_files.py`を実行したディレクトリを基準にはしません。標準の設定例は[`local-files.toml`](../local-files.toml)です。

表の「型、デフォルト」欄では、型の後ろに省略時のデフォルトを書きます。たとえば「正の整数、`67108864`」は、
正の整数を指定でき、省略した場合は67108864を使用するという意味です。必須キーにはデフォルトがありません。

## `collection_id`

| キー | 必須 | 型と範囲 | 説明 |
|---|---|---|---|
| `collection_id` | 必須 | 1〜32文字。英数字、`.`、`_`、`-`だけを許可 | 文書集合を識別する安定した名前です。文書IDの生成と、各文書の`metadata.collection_id`に使用します。同じ入力を再開するときは変更しません。 |

## `[input]`

| キー | 必須 | 型、デフォルト | 説明と値の選び方 |
|---|---|---|---|
| `root` | 必須 | ディレクトリのパス | 収集を開始するディレクトリです。実行時に存在するディレクトリでなければなりません。 |
| `include` | 任意 | 文字列配列、`["*", "**/*"]` | 収集対象の相対パスに照合するglobパターンです。一つ以上に一致したファイルを候補にします。空配列なら`include`による制限を行いません。 |
| `exclude` | 任意 | 文字列配列、`[".git/**", ".venv/**", "__pycache__/**"]` | 候補から除くglobパターンです。出力ディレクトリ類は、この設定とは別に自動除外されます。 |
| `follow_symlinks` | 任意 | 真偽値、`false` | `false`ではシンボリックリンクを収集しません。`true`ではリンク先をたどり、デバイス番号とiノード番号で訪問済みディレクトリを判定して循環を防ぎます。意図しない領域を読む可能性があるため、必要な場合だけ有効にします。 |

globパターンは、`*`や`**`などのワイルドカードでパスを指定する文字列です。`root`からの相対パスをPOSIX形式の`/`区切りへ正規化して照合します。ディレクトリを除外するときは、ディレクトリ自身の相対パスと末尾に`/`を加えた形の両方を照合します。走査順はUnicode NFCで正規化した名前順です。これは再実行時の成果物を安定させるためです。

## `[output]`

`convert`が正式な文書NDJSONを作る場所です。

| キー | 必須 | 型、デフォルト、範囲 | 説明と値の選び方 |
|---|---|---|---|
| `directory` | 必須 | ディレクトリのパス | `documents-000001.ndjson`、`failures-000001.ndjson`、`manifest.json`を保存します。既に存在する場合は上書きしません。 |
| `shard_max_bytes` | 任意 | 正の整数、`67108864` | NDJSONを次の分割ファイルへ切り替える目安です。1レコード自体が上限より大きい場合は、そのレコードだけを含む大きい分割ファイルを許可し、マニフェストへ`oversized_records`を記録します。 |
| `body_max_bytes` | 任意 | 正の整数、`1000000`、最大`1048576` | 一つの`upsert`へ入れる`body`のUTF-8バイト数上限です。抽出本文が大きい場合は、文字を途中で壊さない位置で複数文書へ分割します。 |

`shard_max_bytes`は1文書の本文上限ではありません。`body_max_bytes`は索引実装の本文上限1 MiBを超えられません。

## `[prepare]`

| キー | 必須 | 型、デフォルト | 説明 |
|---|---|---|---|
| `directory` | `prepare`、`embed`、`rag`、`hybrid`で必須 | ディレクトリのパス | `output.directory`の文書を`[chunking]`に従って分けた`passages-*.ndjson`と`manifest.json`を保存します。既存ディレクトリは上書きしません。 |

語彙検索だけの`convert`と`build --target lexical`では省略できます。

## `[embedding]`

共通キーの意味は[Yappod2設定リファレンス](../../../docs/configuration.md#embedding)を参照してください。local-filesでは次の条件が追加されます。

| キー | local-filesでの条件 |
|---|---|
| `directory` | `embed`と`build --target hybrid`で必須です。埋め込み付き`documents-*.ndjson`、マニフェスト、再開用ファイルを保存します。 |
| `provider` | `lmstudio`、`ollama`、`openai`のいずれかが必須です。 |
| `base_url` / `endpoint_url` | どちらか一方だけが必須です。 |
| `model` | プロバイダーへ送る実モデル名が必須です。 |
| `model_id` | `[vector].model_id`と一致する識別子が必須です。 |
| `dimensions` | 1〜65536の整数が必須です。`[vector].dimensions`と一致させます。 |
| `batch_size` | デフォルトは`16`、1〜1024です。 |
| `timeout_ms` | デフォルトは`60000`の正の整数です。1回のHTTPリクエストに適用します。 |
| `prompt_profile` | `plain`または`embeddinggemma`です。デフォルトは`plain`です。 |
| `authorization_token_env` | 任意の環境変数名です。`authorization_token`へ平文トークンを書くと拒否されます。 |

語彙索引またはRAG用成果物だけを作る場合は、空の`[embedding]`を残さずセクション全体を省略します。

## `[usage_log]`

| キー | 必須 | 型、デフォルト | 説明 |
|---|---|---|---|
| `path` | セクションを記述した場合は必須 | ファイルパス | 埋め込みAPI応答の`usage`をNDJSONで追記します。記録には時刻、処理名、プロバイダー、モデル、接続先が返した利用量が含まれます。書き込み失敗は警告として標準エラー出力へ出し、処理自体は継続します。 |

記録しない場合は空の`[usage_log]`を置かず、セクション全体を省略します。

## `[extract]`

| キー | 必須 | 型、デフォルト | 説明と値の選び方 |
|---|---|---|---|
| `max_extracted_bytes` | 任意 | 正の整数、`67108864` | 一つの入力ファイルから読み取る量、または抽出器から受け取るUTF-8出力の上限です。大きくすると一時ファイルとメモリーの使用量も増えます。 |
| `enable_plugins` | 任意 | 真偽値、`false` | Pythonのエントリーポイント`yappod.local_files_extractors`を読み込みます。信頼できるプラグインを明示的に導入した場合だけ有効にします。 |
| `tika_command` | 任意 | 空でない文字列の配列 | Apache Tikaを起動する引数列です。全要素を通じて`{path}`をちょうど1回含めます。シェルを介さず実行し、`{path}`を入力ファイルの絶対パスへ置換します。 |
| `tika_timeout_ms` | 任意 | 正の整数、`30000` | Tika 1回の完了待ち時間です。 |

組み込み処理はUTF-8テキスト、JSON、NDJSON、CSV、TSV、HTML、XMLなどを直接扱います。PDFやOffice文書は、導入済みの任意ライブラリ、プラグイン、または設定したTikaへ渡します。Tikaは自動でダウンロードされません。

```toml
[extract]
max_extracted_bytes = 67108864
enable_plugins = false
tika_command = ["java", "-jar", "/opt/tika/tika-app.jar", "--text", "{path}"]
tika_timeout_ms = 30000
```

抽出器の終了状態が0以外、時間超過、出力上限超過、UTF-8として不正、空白だけの出力の場合、そのファイルを`failures-*.ndjson`へ記録して次のファイルへ進みます。

## `[formatters]`

独自コマンドで特定ファイルを検索向けのテキストへ変換します。

| キー | 必須 | 型、デフォルト | 説明 |
|---|---|---|---|
| `enabled` | 任意 | 真偽値、`true` | `false`にするとすべての規則を無視し、組み込み抽出へ進みます。 |
| `content_match_enabled` | 任意 | 真偽値、`false` | `content_regex`のためにファイル内容を読むかを指定します。コマンドラインの`--content-match`と`--no-content-match`で実行時に上書きできます。 |
| `content_scan_bytes` | 任意 | 正の整数、`1048576` | `content_regex`へ渡すため、ファイル先頭から読む最大バイト数です。全文抽出の上限ではありません。 |
| `rules` | 任意 | テーブルの配列、`[]` | 上から順に評価し、最初に一致した規則だけを実行します。TOMLでは`[[formatters.rules]]`と書きます。 |

### `[[formatters.rules]]`

| キー | 必須 | 型、デフォルト | 照合または実行方法 |
|---|---|---|---|
| `name` | 必須 | 空でない一意な文字列 | マニフェストと失敗記録で抽出器を識別します。 |
| `command` | 必須 | 文字列配列 | 全要素を通じて`{path}`をちょうど1回含めます。シェルを介さず、TOMLのディレクトリをカレントディレクトリとして実行します。 |
| `basename_glob` | 任意 | 文字列配列、`[]` | ファイル名だけにglobパターンを照合します。一つ以上に一致する必要があります。 |
| `path_glob` | 任意 | 文字列配列、`[]` | `input.root`からの相対パスにglobパターンを照合します。一つ以上に一致する必要があります。 |
| `path_regex` | 任意 | 正規表現文字列の配列、`[]` | 相対パスを検索し、一つ以上に一致する必要があります。不正な正規表現は設定読込時に拒否します。 |
| `content_regex` | 任意 | 正規表現文字列の配列、`[]` | `content_match_enabled = true`のときだけ先頭部分へ照合し、一つ以上に一致する必要があります。無効時は、この条件を持つ規則自体が一致しません。 |
| `timeout_ms` | 任意 | 正の整数、`30000` | コマンド1回の完了待ち時間です。 |
| `max_stdout_bytes` | 任意 | 正の整数、`67108864` | 標準出力として受け取る最大バイト数です。標準エラー出力は失敗理由用に先頭4096バイトを読みます。 |

`basename_glob`、`path_glob`、`path_regex`の種類を複数指定した場合は、各種類がすべて一致する必要があります。同じ種類の配列内では、一つに一致すれば成立します。三種類をすべて省略した規則は、パス条件としては全ファイルに一致します。

```toml
[formatters]
enabled = true
content_match_enabled = true
content_scan_bytes = 1048576

[[formatters.rules]]
name = "special-c-source"
path_glob = ["src/**/*.c"]
content_regex = ["(?m)^SPECIAL_FORMAT="]
command = ["python3", "./tools/format_special.py", "{path}"]
timeout_ms = 30000
max_stdout_bytes = 67108864
```

## `[build]`

| キー | 必須 | 型、デフォルト | 説明 |
|---|---|---|---|
| `yappo_makeindex` | `build`と`all`で必須 | 実行ファイルのパス | `yappo_makeindex build`を実行するためのパスです。存在し、通常ファイルで、実行可能でなければなりません。 |

`index.directory`も`build`と`all`で必要です。`build --target lexical`と`build --target rag`では`[vector].enabled = false`、`build --target hybrid`では`[vector].enabled = true`にします。`rag`は本文断片成果物を準備したうえで、ベクトルを持たない検索索引も作ります。

## 成果物ディレクトリの関係

次のディレクトリは同一パスにも、互いの親子にもできません。

- `output.directory`
- `prepare.directory`
- `embedding.directory`
- `index.directory`

各段階は同じ親ディレクトリ内に一時ディレクトリを作り、ファイルを`fsync`した後に名前を変更して公開します。既存の完成済みディレクトリを無条件に上書きしません。このため、途中成果物を入力ディレクトリの内側へ置いたり、段階間で同じディレクトリを共有したりしないでください。

## 設定の指紋値と再開

local-filesはTOMLから`[daemon]`、`[web]`、`[llm]`、`[mock]`を除いた内容をJSONへ正規化し、SHA-256を設定の指紋値としてマニフェストへ保存します。収集・分割・埋め込みへ影響する設定が変わった状態で古い成果物を再利用しないためです。CLIで内容照合を上書きした場合は、その値も指紋へ含めます。

再開時は、設定の指紋値だけでなく、入力ファイルのパス、SHA-256、更新時刻から求めた入力スナップショット、前段マニフェスト、各分割ファイルの大きさ・件数・SHA-256、チェックポイント、ジャーナルも照合します。何を削除して再生成できるかは[処理段階と安全な再生成](pipeline-and-recovery.md)を参照してください。
