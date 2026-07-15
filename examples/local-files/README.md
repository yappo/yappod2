# ローカルファイルをshard化してyappod2で検索する

このexampleは、手元のdirectoryにある文書、設定、source code、PDF、Office文書などを検索可能な
canonical NDJSONへ変換し、必要に応じてpassage生成、embedding、index作成まで実行するPython CLIです。
大量の文書を1個の巨大なJSONへ詰めず、各stageを複数のNDJSON shardへ分けます。複数のdocument
shardはCLI内部のFIFOから既存の`yappo_makeindex build`へ1回だけ渡すため、shell scriptで連結する
必要はありません。

`yappo_makeindex`本体とそのCLIは変更せずに利用します。

## まず試す: yappod2 repository自身をindexにして検索する

このrepository専用の[`local-files-yappod2.toml`](local-files-yappod2.toml)を用意しています。
この設定はrepository rootを走査し、`.git/**`、`build/**`、`node_modules/**`、virtual environment、
IDE設定、生成済みdataなどを除外します。汎用の設定例`local-files.toml`やrootの
`examples/config.toml`をこの手順に使う必要はありません。

repository rootで、まずC binaryとPython環境を準備します。macOSでもPython executable名は
`python3`で構いません。

```sh
cmake --build build -j

python3 -m venv examples/local-files/.venv
examples/local-files/.venv/bin/python -m pip install \
  -r examples/local-files/requirements-core.txt
```

次の1 commandで、repositoryの変換からlexical index作成までを一気に実行します。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target lexical
```

成功すると、次が作られます。

```text
examples/local-files/data/
├── yappod2-documents/
│   ├── manifest.json
│   ├── documents-000001.ndjson
│   ├── documents-000002.ndjson  # shard上限を超えた場合
│   └── failures-000001.ndjson   # 抽出失敗があった場合
└── yappod2-index/
    ├── config.toml
    ├── manifest.json
    └── segments/
```

作成したindexを検索します。

```sh
./build/search \
  --index examples/local-files/data/yappod2-index \
  --mode lexical \
  --scope documents \
  --query yappo_makeindex \
  --limit 10
```

別のqueryとして`segment_write`、`BM25`、`unicode_nfkc_casefold_v2`なども試せます。titleには
`src/yappo_makeindex.c`のようなrepository root相対pathが入るため、file名も検索対象です。

入力fileの絶対pathはIDやmetadataへ保存しません。ただし、source codeや設定の原文自体に秘密情報や
絶対pathが書かれていれば、その文字列はbodyへ入ります。検索対象にすべきでないfileは
`local-files-yappod2.toml`の`input.exclude`へ追加してください。

出力directoryやindexが既にある場合は上書きしません。再実行する場合は既存の生成物を利用者自身で
退避・削除するか、設定内の出力pathを変更してください。

`local-files-yappod2.toml`を使ったpipelineを全て再生成する場合は、documentsだけでなくpassages、
vectors、indexも前回の生成物として残るため、`data` directory全体を削除してから再実行します。
個別のdirectoryだけを削除すると、後続stageが既存directoryを検出して停止します。

```sh
rm -rf examples/local-files/data

examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target hybrid
```

## targetと生成物

`all --target TARGET`は、選択した最終成果に必要なstageだけを順番に実行します。

| target | documents shard | 外部passage shard | embedding | index |
|---|---:|---:|---:|---|
| `documents` | あり | なし | なし | 作らない |
| `lexical` | あり | なし | なし | vector無効 |
| `rag` | あり | あり | なし | vector無効 |
| `hybrid` | あり | あり | あり | lexical＋vector |

たとえば変換結果だけが必要なら次を実行します。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target documents
```

`lexical`では外部passage shardやembeddingを作りません。ただし現行indexerはvector無効時にも内部で
本文をchunkし、index内部のpassageを保存します。これは既存buildの挙動であり、このexampleでは
変更しません。

`rag`は外部利用可能なpassage shardを保存した後、vector無効indexを作ります。index作成時には
既存buildへdocument shardを渡し、build自身も同じchunk設定でindex内部のpassageを作ります。

`hybrid`は外部passageをembeddingし、各documentのpassage ordinal順に`vectors`二次元配列へ戻した
vector付きdocument shardからindexを作ります。

## CLI command

一括実行のほか、各stageを個別に再現できます。

```sh
# local file -> documents-*.ndjson
python3 local_files.py convert --config local-files.toml

# documents shard -> passages-*.ndjson
python3 local_files.py prepare --config local-files.toml

# passage embedding -> vectors付きdocuments-*.ndjson
python3 local_files.py embed --config local-files.toml

# manifestで検証した複数document shard -> 1個のindex
python3 local_files.py build --config local-files.toml --target hybrid

# 必要stageを全て実行
python3 local_files.py all --config local-files.toml --target hybrid
```

各commandは成功時にsummary JSONをstdoutへ1行出力し、失敗理由をstderrへ出して非0で終了します。
stageを個別実行する場合も、前stageのmanifestとshard checksumを検証してから処理します。

## 必要環境

- text、source、JSON、HTML、XML、外部formatter: Python 3.9以上
- Python 3.9と3.10: `requirements-core.txt`の`tomli`
- PDF、DOCX、XLS/XLSX、PPTX: Python 3.10以上と`requirements.txt`のMarkItDown
- DOC/PPT、Keynote、Pages、Numbers: Javaと利用者が配置したApache Tika 3.3.1
- index作成: build済みの`yappo_makeindex`
- hybrid: LM Studio、Ollama、またはOpenAI互換embedding endpoint

基本機能だけなら次で準備できます。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-core.txt
```

PDFやOffice文書も扱う場合だけfull requirementsを追加します。

```sh
.venv/bin/python -m pip install -r requirements.txt
```

Tikaは自動downloadしません。

## 自分のdirectory用の設定

[`local-files.toml`](local-files.toml)は設定項目を説明するための汎用sampleです。そのまま実行する
前提のrepository専用設定ではありません。別名へcopyし、少なくともcollection、入力、出力、buildを
自分の環境に合わせます。相対pathは設定fileがあるdirectoryを基準に解決します。

```toml
schema_version = 1
collection_id = "my-files"

[input]
root = "/absolute/path/to/search-root"
include = ["*", "**/*"]
exclude = [".git/**", ".venv/**", "data/**"]
follow_symlinks = false

[output]
directory = "./data/documents"
shard_max_bytes = 67108864
body_max_bytes = 1000000

[prepare]
directory = "./data/passages"

[build]
yappo_makeindex = "../../build/yappo_makeindex"
index_config = "./config.lexical.toml"
hybrid_index_config = "./config.hybrid.toml"
index_directory = "./data/index"
```

`collection_id`は1〜32文字の英数字、`.`、`_`、`-`で指定します。異なるcollectionの同じ相対pathが
衝突しないためのnamespaceです。

### embedding設定

`hybrid` targetを使う場合だけ`[embedding]`が必要です。

```toml
[embedding]
directory = "./data/vectors"
provider = "lmstudio" # lmstudio | ollama | openai
base_url = "http://127.0.0.1:1234/v1"
model = "実際にendpointへ渡すmodel名"
model_id = "my-embedding-768-v1"
dimensions = 768
batch_size = 16
timeout_ms = 60000
prompt_profile = "plain" # plain | embeddinggemma
```

- LM StudioとOpenAI互換APIでは`base_url`へ`/embeddings`を追加して呼び出します。
- Ollamaでは通常`base_url = "http://127.0.0.1:11434"`とし、`/api/embed`を追加して呼び出します。
- bearer tokenが必要なら`authorization_token`を指定できます。設定fileの権限とversion管理に注意して
  ください。
- `embedding.model_id`と`dimensions`は`config.hybrid.toml`の`[vector]`と一致させます。
- lexical configとhybrid configの`[tokenizer]`、`[chunking]`も一致させます。異なる場合、CLIは
  passage/vector ordinalの不整合を避けるため失敗します。

yappod2自身をhybrid化する場合は、`local-files-yappod2.toml`のplaceholder `model`、`model_id`、
`dimensions`と、`config.hybrid.toml`の対応値を実際のembedding modelへ合わせてから実行します。

```sh
examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py all \
  --config examples/local-files/local-files-yappod2.toml \
  --target hybrid
```

検索時には、同じmodelとprompt profileでquery vectorを作り、comma区切りで渡します。

```sh
./build/search \
  --index examples/local-files/data/yappod2-index \
  --mode hybrid \
  --scope documents \
  --query yappo_makeindex \
  --vector 0.01,0.02,0.03 \
  --limit 10
```

上の3値は書式例です。実際にはindexの`dimensions`個の値が必要です。

## 抽出形式

- Markdown、source code、TOML/YAML/INIなどのtextはdecode後の原文をbodyへ保存します。
- JSON/JSON Lines/XMLは構造を検証します。
- HTML/XHTMLは`script`、`style`、`noscript`を除き、検索しやすいtextへ正規化します。
- PDF、DOCX、XLS/XLSX、PPTXはMarkItDownを使います。
- DOC/PPT、Keynote/Pages/Numbersは、明示設定したTika commandへfallbackします。
- 暗号化、破損、unsupported、timeout、空抽出などは`failures-*.ndjson`へ記録し、正常fileの
  処理は続けます。

OCR、一般archiveの再帰展開、symlink追跡、source codeのAST解析は初版の対象外です。

## 外部整形script

pathやfile先頭の内容に一致した場合、利用者指定commandのUTF-8 stdoutを本文にできます。独自形式だけで
なく、Tree-sitter、ctags、独自parserでsource codeを検索用textへ整形する用途にも使えます。

```toml
[formatters]
enabled = true
content_match_enabled = true
content_scan_bytes = 1048576

[[formatters.rules]]
name = "special-source"
basename_glob = ["Makefile", "Dockerfile"]
path_glob = ["src/**/*.c"]
content_regex = ["(?m)^SPECIAL_FORMAT="]
command = ["python3", "./tools/format_special.py", "{path}"]
timeout_ms = 30000
max_stdout_bytes = 67108864
```

`basename_glob`、`path_glob`、`path_regex`を先に評価し、path条件を通ったfileだけ先頭
`content_scan_bytes`をdecodeして`content_regex`を評価します。selectorの種類同士はAND、同じlist内は
ORです。contentだけのruleも使用できます。ruleは記述順で最初に一致した1件だけを実行します。

commandはshellを介さずargv配列として実行し、`{path}`へ絶対pathを渡します。非0終了、timeout、
空stdout、不正UTF-8、出力上限超過はfile単位のfailureです。

content探索はCLIでも切り替えられます。

```sh
python3 local_files.py convert --config local-files.toml --content-match
python3 local_files.py convert --config local-files.toml --no-content-match
```

無効時は`content_regex`を持つruleを実行しません。

## MarkItDown plugin

独自Python形式はMarkItDownの`markitdown.plugin` entry pointで追加できます。pluginは明示的に
`enable_plugins = true`とした場合だけloadします。同梱sampleは`.mydoc`をUTF-8 textへ変換します。

```sh
.venv/bin/python -m pip install -e plugins/sample-local-format
```

```toml
[extract]
enable_plugins = true
```

plugin packageは任意codeを実行できるため、信頼できるものだけを専用virtual environmentへinstallして
ください。

## Apache Tika

Tikaは自動downloadせず、利用者が用意したcommandをprocess分離・timeout付きで起動します。

```toml
[extract]
tika_command = [
  "java",
  "-jar",
  "/absolute/path/to/tika-app-3.3.1.jar",
  "--text",
  "{path}",
]
tika_timeout_ms = 30000
max_extracted_bytes = 67108864
```

Tika未設定の対象fileは`extractor_unavailable`としてfailure shardへ記録します。画像だけのPDFや
Office内画像はOCRしないため、通常の文字layerがなければ`no_text`になります。

## Document IDとpath

絶対pathはdocument IDに使いません。root相対pathをPOSIX separator、Unicode NFCへ正規化し、次の
決定的IDを作ります。

```text
lf:v1:<collection_id>:<sha256(collection_id + NUL + normalized_relative_path)>:p<part>
```

- 同じpathのfileを編集してもIDは同じです。
- renameすると別IDになります。
- 同じ内容でもpathが異なれば別documentです。
- 1,000,000 UTF-8 bytes以下へ本文分割した各partに固定幅ordinalを付けます。
- metadataには`source_path`、`source_sha256`、MIME、extractor、part番号を保存します。
- titleには255 UTF-8 bytes以内へ切ったroot相対pathを保存します。

本文は段落、改行、UTF-8境界の順に切り、part間の重複は作りません。

## Shard、manifest、checkpoint

既定shard目標は64 MiBです。1 recordだけで目標を超える場合は、そのrecordだけのshardとして出力します。
各stageの`manifest.json`はrecord一覧ではなく、次のdescriptorを保持します。

```text
schema_version, stage, target, config_fingerprint, source_manifest_sha256
total_records, total_bytes
shards[].path, shards[].record_count, shards[].file_bytes, shards[].sha256
failure_count, failure_shards[]
```

全shardは一時directoryで作り、件数、byte数、SHA-256を確定してからdirectory単位で公開します。
`prepare`、`embed`、`build`は入力manifestの順序、size、件数、checksumを再検証します。

embeddingは入力document shardごとにcheckpointを作ります。入力shard SHA、passage manifest SHA、
chunk config SHA、provider、endpoint、model、model ID、dimensions、prompt profileが全て一致する完了shard
だけを再利用します。不一致のcheckpointがある場合は、自動で混在させず明示errorにします。

## FIFO buildとatomic公開

`build`は検証済みdocument shardをmanifest順にFIFOへstreamし、既存の
`yappo_makeindex build --input FIFO`を1回だけ起動します。build側producer、checksum、process、accepted
件数、index componentのどれかが失敗した場合、最終index pathへ公開しません。成功したstage indexだけを
最終indexと同じ親directory内でatomic renameします。

NDJSON shard境界はindex segment境界ではありません。複数shardをFIFOへ渡しても、主要YAP2 componentの
256 MiB payload上限を回避する自動segment分割にはなりません。現行buildの10,000 operation単位の
segment化とsize超過時のerrorをそのまま使います。詳細と将来のsegment planner案は
[index segment sizing](../../docs/index_segment_sizing.md)を参照してください。

## Source code検索の性質

原文bodyでも通常のlexical document検索はできます。titleとbodyをNFKC casefoldとICU word boundaryで
tokenizeするため、一般的な単語やidentifierを検索できます。

ただし、次は保証しません。

- punctuationだけのoperator検索
- AST、定義・参照関係
- comment、string、実行codeの区別
- camelCaseの強制分割
- 関数境界を守ったRAG chunk

document検索は原文全文が対象ですが、passageは汎用sentence/grapheme chunkerなので関数途中で切れる
場合があります。構造が重要なrepositoryでは、外部整形ruleで関数名、型、path、本文を含む検索用textへ
変換してください。

fixtureでは`snake_case_identifier`と`camelCaseIdentifier`は完全なidentifierで検索できましたが、
`camel`だけでは`camelCaseIdentifier`へ一致しませんでした。`Namespace::QualifiedName`はqualified name
全体でも`QualifiedName`でも一致し、記号だけの`++`と`->`は一致しませんでした。この挙動を
`test_source_identifier_queries_record_actual_tokenizer_behavior`で実際のindex/searchに対して固定しています。
コード専用のtoken分解やoperator indexを保証するものではありません。

## Failureの扱い

file単位の失敗は次のように記録します。

```json
{"path":"legacy/example.key","code":"extractor_unavailable","message":"Apache Tika command is not configured","extractor":"tika"}
```

一部fileが失敗しても正常fileのdocuments shardは公開します。成功fileが1件もない、設定が不正、出力先が
既に存在する、manifest/checksumが一致しない、embeddingやbuildが失敗した場合はcommand全体を失敗させます。
