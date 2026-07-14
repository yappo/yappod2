# ローカルファイルをcanonical NDJSONへ変換する

このdirectoryは、手元のdirectoryにある文書、設定、source code、PDF、Office文書などを走査し、
yappod2が正式入力として扱うcanonical NDJSONへ変換するサンプルです。大量の文書を1個の巨大な
JSONへ詰めず、`documents-000001.ndjson`のような複数shardへ分けて出力します。

現在このdirectoryで実装済みなのは`convert` stageです。完成時には、同じmanifestを使って
passage生成、任意のembedding、複数shardから単一indexを作るFIFO buildを追加します。未実装の
commandを実装済みとして扱わないため、現時点の手順はdocuments shard生成までを説明します。

## 何ができるか

- UTF-8 text、Markdown、各種source code、TOML/YAML/INIなどは原文を検索本文として保存します。
- JSON、JSON Lines、XMLは構造を検証し、HTML/XHTMLはparseして検索しやすいtextへ変換します。
- PDF、DOCX、XLS/XLSX、PPTXはMarkItDownを使ってtextを抽出します。
- DOC/PPT、Keynote、Pages、Numbersは、利用者がApache Tikaを設定した場合だけ抽出します。
- pathやfile先頭の内容に一致した場合、利用者の整形scriptを起動し、そのUTF-8 stdoutを本文にできます。
- 1件の失敗で全体を止めず、失敗理由を分割`failures-*.ndjson`へ残します。
- 各bodyをyappod2の1 MiB制限より小さい1,000,000 bytes以下へ分割します。

OCR、archiveの再帰展開、symlink追跡、source codeのAST解析は既定では行いません。

## まず試す: yappod2 repository自身をdocumentsにする

このrepositoryをclone済みなら、入力fileを別directoryへコピーせずに試せます。専用の
`local-files-yappod2.toml`はrepository rootを走査し、`.git/**`、`build/**`、`node_modules/**`、
virtual environment、IDE設定、生成済みdependency、このexample自身の生成先を除外します。
`.git`内のobject、hook、logなどはindex対象になりません。

repository rootで次を実行してください。

```sh
python3 -m venv examples/local-files/.venv
examples/local-files/.venv/bin/python -m pip install \
  -r examples/local-files/requirements-core.txt

examples/local-files/.venv/bin/python \
  examples/local-files/local_files.py convert \
  --config examples/local-files/local-files-yappod2.toml
```

macOSを含め、virtual environmentを作る側のcommandは`python3`を使います。Python 3.11以上には
`tomllib`が標準搭載されています。Python 3.9と3.10ではTOML parserの`tomli`が必要なので、上記の
`requirements-core.txt`を先にinstallします。source code、text、JSON、HTML、XMLと外部formatterだけを
試す場合、MarkItDownを含む重いoptional依存は不要です。

成功すると、次のfileが作られます。

```text
examples/local-files/data/yappod2-documents/
├── manifest.json
├── documents-000001.ndjson
├── documents-000002.ndjson  # data量がshard上限を超えた場合
└── failures-000001.ndjson   # unsupported fileなどがあった場合
```

生成されたdocumentには、たとえば`src/yappo_makeindex.c`というroot相対pathがtitleとmetadataの
`source_path`へ入り、source codeの原文がbodyへ入ります。converterが生成するID、title、metadataには
絶対pathを保存せず、`.git`内のdataも読みません。ただし、入力fileの原文自体に絶対pathが書かれていれば、
原文を保存する形式ではその文字列もbodyに含まれます。秘密情報を含むfileは`input.exclude`へ追加してください。
まずmanifestと先頭recordを確認できます。

```sh
examples/local-files/.venv/bin/python -m json.tool \
  examples/local-files/data/yappod2-documents/manifest.json
head -n 1 examples/local-files/data/yappod2-documents/documents-000001.ndjson
```

同じ出力directoryがすでに存在する場合、converterは上書きしません。もう一度試す場合は、必要な
生成物を利用者自身で退避または削除するか、設定の`output.directory`を別名へ変更してください。
現時点ではこの手順はcanonical documents shardの生成までです。検索indexへの登録は後続taskで追加する
FIFO build stageを使用します。未実装のcommandを前提にした手順はここには記載していません。

## 必要環境

- 基本のconvertはPython 3.9以上（executable名は`python3`で構いません）
- Python 3.9と3.10では`requirements-core.txt`の`tomli`
- PDFやOffice文書を扱う場合はPython 3.10以上と`requirements.txt`のPython依存package
- Keynote、Pages、Numbers、旧Office形式を扱う場合だけJavaとApache Tika 3.3.1

text、JSON、HTML、XMLと外部formatterだけなら、MarkItDownなしでもconvertできます。すべての
組み込み形式を使う場合は、このdirectory専用のvirtual environmentを作ります。

```sh
cd examples/local-files
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-core.txt
```

PDFやOffice文書も変換する場合だけ、full requirementsを追加でinstallします。

```sh
.venv/bin/python -m pip install -r requirements.txt
```

Tikaは自動downloadしません。Apache公式から取得してchecksumを確認し、後述の
`extract.tika_command`へ絶対pathを設定します。

## 最短の実行手順

1. `input/`を作り、検索したいfileを置きます。
2. `local-files.toml`の`collection_id`、`input.root`、`output.directory`を確認します。
3. `convert`を実行します。

```sh
mkdir -p input
cp /path/to/searchable/file.md input/

.venv/bin/python local_files.py convert \
  --config local-files.toml
```

成功すると、stdoutへ機械可読なsummary JSONを1行出力します。

```json
{"collection_id":"my-files","failure_count":0,"shards":[{"file_bytes":1234,"path":"documents-000001.ndjson","record_count":1,"sha256":"..."}],"stage":"convert","successful_files":1,"target":"documents","total_bytes":1234,"total_records":1}
```

出力directoryは次の形になります。

```text
data/documents/
├── documents-000001.ndjson
├── failures-000001.ndjson   # 失敗がある場合だけ作成
└── manifest.json
```

既存の出力directoryは上書きしません。全shardとmanifestを一時directoryへ書いて検証し、最後に
directory単位でrenameするため、途中失敗した出力が完成品として見えることはありません。

## 設定file

すべての相対pathは`local-files.toml`があるdirectoryを基準に解決します。

### collectionと入出力

```toml
schema_version = 1
collection_id = "my-files"

[input]
root = "./input"
include = ["*", "**/*"]
exclude = [".git/**", ".venv/**", "data/**"]
follow_symlinks = false

[output]
directory = "./data/documents"
shard_max_bytes = 67108864
body_max_bytes = 1000000
```

`collection_id`は1〜32文字の英数字、`.`、`_`、`-`で指定します。異なるrootで同じ相対pathが
衝突しないためのnamespaceです。include/excludeは正規化したroot相対pathに適用します。

## Document IDとpath

絶対pathをdocument IDにはしません。絶対pathは長くなりやすく、rootを移動すると変わり、手元の
directory情報も露出するためです。IDは次の値から決定的に作ります。

```text
lf:v1:<collection_id>:<sha256(collection_id + NUL + normalized_relative_path)>:p<part>
```

- 同じpathのfileを編集してもIDは変わりません。
- fileをrenameするとIDは変わります。
- 同じ内容でもpathが異なれば別documentです。
- 絶対pathは外部scriptへ渡すときだけ使い、documentやmetadataへ保存しません。
- 完全な相対path、元fileのSHA-256、MIME、extractor、part番号をmetadataへ保存します。
- titleには255 UTF-8 bytes以内へ切った相対pathを使います。

## 外部整形script

外部scriptは、独自形式を追加するだけでなく、source codeをTree-sitterやctagsで構造化したtextへ
変換する用途にも使えます。commandはshellを介さずargvとして実行され、`{path}`へ入力fileの
絶対pathを1個だけ渡します。exit code 0のUTF-8 stdout全体が検索本文になります。

### file名だけで選ぶ

```toml
[formatters]
enabled = true
content_match_enabled = false
content_scan_bytes = 1048576

[[formatters.rules]]
name = "dockerfile"
basename_glob = ["Dockerfile"]
command = ["python3", "./tools/format_dockerfile.py", "{path}"]
timeout_ms = 30000
max_stdout_bytes = 67108864
```

### pathを絞ってから内容も調べる

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
```

評価順は`basename_glob`、`path_glob`、`path_regex`が先で、これらを通過したfileだけ先頭
`content_scan_bytes`を読みます。selectorの種類同士はAND、同じlist内はORです。ruleは記述順で、
最初に一致した1件だけを実行します。

設定を変えず一時的にcontent探索を切り替えることもできます。

```sh
.venv/bin/python local_files.py convert --config local-files.toml --content-match
.venv/bin/python local_files.py convert --config local-files.toml --no-content-match
```

content探索を無効にすると、`content_regex`を持つruleは一致しません。path条件だけのruleは通常どおり
動作します。外部scriptのtimeout、非0終了、空stdout、不正UTF-8、出力上限超過はfile単位の失敗です。

## MarkItDown plugin

MarkItDownの公式plugin interfaceも利用できます。pluginは既定で無効です。同梱sampleは`.mydoc`を
UTF-8 textへ変換します。

```sh
.venv/bin/python -m pip install -e plugins/sample-local-format
```

次に設定を変更します。

```toml
[extract]
enable_plugins = true
```

plugin packageは任意codeを実行できるため、信頼できるものだけを専用virtual environmentへinstallして
ください。

## Apache Tikaを使う

Keynote、Pages、Numbers、DOC、PPTを扱う場合だけ設定します。

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

Tikaが未設定の対象fileは黙って無視せず、`extractor_unavailable`としてfailure shardへ記録します。
画像だけのPDFやOffice内画像はOCRしないため、通常の文字layerがなければ`no_text`になります。

## Source code検索の性質

原文をbodyへ保存しても、通常のlexical document検索はできます。yappod2はNFKC casefoldとICU word
boundaryでtitle/bodyをtokenizeするため、一般的な単語やidentifierをindexします。

一方、これはコード専用indexではありません。

- punctuationだけのoperator検索
- AST、定義・参照関係
- comment、string、実行codeの区別
- camelCaseの強制分割
- 関数境界を守ったRAG chunk

これらは保証しません。document検索は原文全文が対象ですが、RAG/vector用passageは汎用chunkerなので
関数途中で切れる場合があります。構造が重要なら、外部整形ruleで関数名、型、path、本文を含む
Markdownなどへ変換してください。

## Failureの扱い

壊れたfile、暗号化file、unsupported形式、timeout、空抽出などは次のように記録します。

```json
{"path":"legacy/example.key","code":"extractor_unavailable","message":"Apache Tika command is not configured","extractor":"tika"}
```

一部fileが失敗しても、正常fileのdocuments shardは公開します。成功fileが1件もない場合、設定不正、
出力directoryが既に存在する場合はcommand全体を失敗させます。

## Shardとindex segmentは別物

64 MiBのNDJSON shardは中間fileを扱いやすくするための単位で、indexのsegment境界ではありません。
現行`yappo_makeindex build`は入力streamを10,000 operation単位でsegment化します。各主要YAP2
componentには256 MiB payload上限があり、超過を拒否しますが自動でbatchを再分割しません。

この制限と、自動segment分割を導入した場合の検索・compactionへの影響は
[index segment sizing](../../docs/index_segment_sizing.md)を参照してください。

## 完成予定のpipeline

後続タスクで、同じmanifestを使う次のstageを追加します。

```text
convert -> documents shards
        -> lexical: documents shardsをFIFO build
        -> rag: prepareでpassage shardsを保存してFIFO build
        -> hybrid: prepare -> embed -> vectors付きdocuments shards -> FIFO build
```

`lexical`は外部passage fileとembeddingを作りません。ただし現行indexerはvector無効時にも内部passageを
生成します。index内部のpassageまで完全に無くすにはcore変更が必要であり、このサンプルの対象外です。
