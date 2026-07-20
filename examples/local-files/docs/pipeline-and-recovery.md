# local-filesの処理工程、マニフェスト、再開

local-filesは、文書変換、パッセージ生成、埋め込み、索引作成を別々の段階として保存します。この文書では、各成果物の形式、再利用条件、途中失敗、安全な全再生成を説明します。

## ディレクトリの分離

設定する次のディレクトリは、同じパスにも親子関係にもできません。

- `[output].directory`は抽出済み文書を保存します。
- `[prepare].directory`はパッセージを保存します。
- `[embedding].directory`はベクトル付き文書を保存します。
- `[index].directory`はYappod2索引を保存します。

段階ごとにマニフェストと命名規則が異なります。一つのディレクトリへ混ぜると、古い分割ファイルを新しいマニフェストで誤って扱う危険があるため、設定読込時に拒否します。

## 不可分な公開

各段階は、最終出力の親ディレクトリへ`.名前.tmp.*`形式の一時ディレクトリを作り、ファイルのバッファを反映して`fsync`してから、`os.replace`で最終名へ変更します。失敗した一時ディレクトリは削除します。既存の最終ディレクトリを途中まで更新しません。

`convert`、`prepare`、最終`embed`は既存出力を上書きしません。`all`が既存出力を見つけた場合は、以下の検証に成功したときだけ再利用します。

## 分割ファイルの記述子

マニフェストの`shards`と`failure_shards`は次の記述子配列です。

```json
{
  "path": "documents-000001.ndjson",
  "record_count": 1250,
  "file_bytes": 67100000,
  "sha256": "64文字の小文字16進数",
  "oversized_records": 1
}
```

`oversized_records`は1行だけで`shard_max_bytes`を超えたレコードがある場合だけ付きます。local-filesは1つのNDJSON行を途中で分割しないため、その分割ファイルだけ設定値を超えることがあります。

再利用時は、記述子の順序と`documents-000001.ndjson`などの連番、実ファイルの大きさ、レコード数、SHA-256、マニフェストの合計を照合します。

## 変換の成果物

`output.directory`には次を保存します。

```text
documents-000001.ndjson
documents-000002.ndjson
failures-000001.ndjson
manifest.json
```

文書分割ファイルの各行はYappod2の`upsert`操作です。失敗の各行は次の形式です。

```json
{"path":"relative/file.pdf","code":"unsupported_format","message":"...","extractor":"text"}
```

`extractor`が不明な失敗では省略します。正常文書が1件もなければ変換全体を失敗させ、最終出力を公開しません。失敗ファイルがあっても正常文書が1件以上あれば、失敗を含む成果物として公開します。

変換マニフェストの主なフィールドは次のとおりです。

| フィールド | 内容 |
|---|---|
| `schema_version` | local-files成果物の内部形式の版です。アプリケーションTOMLの設定ではありません。 |
| `stage` / `target` | `convert` / `documents`です。 |
| `collection_id` | 設定した文書集合IDです。 |
| `config_fingerprint` | 実行時用セクションを除く処理工程設定と、コマンドラインによる上書きを含むSHA-256です。 |
| `input_snapshot_sha256` | 対象相対パス、元ファイルSHA-256、更新時刻から作るスナップショットです。 |
| `input_file_count` | `include`と`exclude`を適用した後の入力数です。 |
| `successful_files` | 1件以上の文書へ変換できた元ファイル数です。 |
| `total_records` / `total_bytes` | 全文書分割ファイルの合計です。 |
| `shards` | 文書分割ファイルの記述子です。 |
| `failure_count` / `failure_shards` | 抽出失敗件数と失敗分割ファイルの記述子です。 |

再利用時には現在の入力を再走査し、`input_snapshot_sha256`と`input_file_count`を比較します。元ファイルの追加、削除、内容変更、更新時刻変更を検出すると再利用しません。

## パッセージ生成の成果物

`prepare.directory`には`passages-000001.ndjson`などと`manifest.json`を保存します。パッセージは`yappo_makeindex prepare`の出力で、文書ID、パッセージID、通し番号、元本文内の位置、正規化後の本文を含みます。

パッセージ準備マニフェストは`stage: "prepare"`、`target: "rag"`です。`config_fingerprint`にはlocal-files設定に加え、アプリケーションTOMLのチェックサムを含みます。`source_manifest_sha256`は変換マニフェストのSHA-256です。したがって、元文書、トークナイザー、チャンク分割、または関連設定が変わると既存パッセージを再利用しません。

## 埋め込み処理の作業ディレクトリ

埋め込みは外部APIを多数回呼ぶ可能性があるため、入力文書分割ファイル単位で再開します。最終`embedding.directory`とは別に、処理中の作業ディレクトリを作ります。

```text
<embedding directory name>.work/
  input-000001/
    checkpoint.json
    documents-000001.ndjson
  .input-000002.progress/
    progress.json
    vectors.ndjson
```

`progress.json`には入力分割ファイル、パッセージマニフェスト、チャンク設定、プロバイダー、エンドポイント、モデル、モデルID、次元数、プロンプトのプロファイルの期待値を保存します。`vectors.ndjson`はAPIから取得済みのベクトルをパッセージ順に追記するジャーナルです。

再開時はジャーナルの各行をJSONとして読み、対象パッセージと順番、ベクトル件数、次元数、有限値を検証します。不正な末尾や別のパッセージのベクトルを無視して続行しません。

入力分割ファイルを完了すると、一時グループを`input-000001`へ公開し、次の`checkpoint.json`を保存します。

| フィールド | 内容 |
|---|---|
| `input_shard_sha256` | 元の文書分割ファイルです。 |
| `passage_manifest_sha256` | パッセージ全体の版です。 |
| `chunk_config_sha256` | アプリケーションTOMLのチェックサムです。 |
| `provider`、`base_url`、`endpoint_url` | 接続方法です。 |
| `model`、`model_id`、`dimensions`、`prompt_profile` | モデル、互換性識別子、次元数、プロンプトの構成というベクトル生成条件です。 |
| `document_count`、`passage_count` | この入力グループで処理した件数です。 |
| `output_shards` | ベクトル付き文書分割ファイルの記述子です。 |

既存入力グループの期待値、チェックサム、件数が1つでも異なる場合は再利用を中止します。完成済みグループと途中ジャーナルを無条件に連結しません。

## 埋め込みの最終成果物

全グループを検証してから、`embedding.directory`へ`documents-*.ndjson`とマニフェストを不可分に公開します。各文書操作の`vectors`は、生成したパッセージと同じ順序です。

マニフェストは`stage: "embed"`、`target: "hybrid"`で、通常の分割ファイル情報に加えて`model_id`、`dimensions`、`passage_count`を持ちます。`source_manifest_sha256`はパッセージマニフェストを指します。最終公開後は作業ディレクトリを削除します。

## 索引作成と`local-files-build.json`

索引作成は、語彙検索またはRAG向けの対象では変換文書分割ファイル、複合検索では埋め込み済み文書分割ファイルを使います。名前付きFIFOへ全分割ファイルを順に流し、`yappo_makeindex build`を1回だけ実行します。

索引作成後に次を確認します。

- `config.toml`の形式、トークナイザー、文書分割、ベクトル、メタデータが要求と一致します。
- `manifest.json`の世代が索引作成結果と一致します。
- セグメントの文書合計が受理件数と一致します。
- 全コンポーネントの実サイズとSHA-256がマニフェストに一致します。
- 語彙コンポーネントとメタデータコンポーネントが存在します。
- 複合検索では`vectors.yap2`と`vectors.usearch`も存在します。

成功すると索引直下へ次の`local-files-build.json`を保存します。

```json
{
  "schema_version": 1,
  "stage": "build",
  "target": "hybrid",
  "generation": 2,
  "accepted": 1250,
  "source_manifest_sha256": "...",
  "application_config_sha256": "..."
}
```

`all`が既存索引を再利用するには、この状態、元マニフェスト、アプリケーションTOML、索引コンポーネントのすべてが一致する必要があります。Yappod2本体だけで作った有効な索引でも、このlocal-files用の状態ファイルがなければ`all`の再開対象にはしません。

## 失敗後に再実行できる場合

- 変換またはパッセージ生成が最終公開前に失敗した場合は、原因を直して同じコマンドを再実行できます。一時ディレクトリは処理が片付けます。
- 埋め込みのAPI通信が途中で失敗した場合は、入力と設定を変えずに`embed`を再実行すると、検証済みジャーナルとチェックポイントから続行します。
- 索引作成が失敗した場合、未完成の一時成果物を片付けたうえで再実行できます。既存の最終`index.directory`がある場合は、内容を確認せず上書きしません。

入力または設定を変更した後、古いチェックポイントだけを残して再開しようとすると、意図的に`configuration mismatch`で失敗します。

## 全再生成が必要な場合

次の場合は、段階の一部だけを削除すると依存関係が崩れるため、全成果物を別の場所へ退避して再生成します。

- 入力スナップショットが既存の変換マニフェストと一致しません。
- トークナイザー、チャンク分割、ベクトル、モデルID、次元数を変更しました。
- マニフェスト、分割ファイル、チェックポイント、ジャーナルのチェックサムまたは件数が一致しません。
- 古い版や別設定で作った成果物が同じデータディレクトリに混在しています。
- どの元入力から作ったか確認できません。

設定例の既定出力をすべて再生成でき、必要な調査資料を保存済みであることを確認した場合だけ、リポジトリのルートで次を実行します。

```sh
rm -rf examples/data/local-files
```

このコマンドは文書、失敗、パッセージ、埋め込み、作業ディレクトリ、チェックポイント、ジャーナル、索引、PID、ログを含む`examples/data/local-files`全体を削除します。`input.root`が別の場所なら元ファイルは削除しませんが、利用者が同ディレクトリへ独自ファイルを置いていないことを必ず確認してください。

削除後は、同じ設定で`all`を最初から実行します。削除範囲を限定する必要がある場合は、マニフェストの依存関係を理解して個別に判断し、READMEの一律手順としては扱いません。

## 障害調査で保存するもの

- 使用したアプリケーション用TOMLです。秘密情報は環境変数にあるため含めません。
- 各段階の`manifest.json`と`local-files-build.json`です。
- 該当分割ファイルの記述子と実際のSHA-256です。
- 埋め込み処理用の`progress.json`、`checkpoint.json`、ジャーナル末尾です。
- 標準エラー出力と`failures-*.ndjson`です。
- 索引の`manifest.json`と`yappo_makeindex verify`結果です。

未知の例外だけを追加調査する場合は`YAPPOD_EXAMPLE_DEBUG=1`を付けて再実行します。通常の設定不一致を解決する最初の手段ではありません。
