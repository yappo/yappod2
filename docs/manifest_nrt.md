# v2 manifest と NRT publish

> **実装状態:** canonical batch writer、CLI/HTTP ingest、generation publish、reader refresh、
> live-only compaction、orphan GC、crash recovery、deadline、writer lock、write token を
> 正式な v2 経路へ接続しています。

`manifest.json` は公開済み immutable segment のスナップショットです。検索側は一つの generation を読み続け、writer は新しい manifest を完成させてから atomic rename します。そのため検索中に未完成の segment 一覧を観測しません。

manifestの`segments`は古い順から新しい順です。検索snapshotは末尾からIDを解決し、新しいdocumentが古い同一IDを置換し、新しいtombstoneが古いdocumentを不可視にします。同一segmentにupsert documentとtombstoneがある場合はdocumentを優先し、tombstoneはそれ以前のsegmentだけに適用します。

## API

- `YAP_V2_manifest_load(path, manifest)` は厳格な JSON を読み込み、format version、generation、segment ID、件数、SHA-256 hex を検証します。呼び出し側は先に `YAP_V2_manifest_init` を実行します。
- `YAP_V2_manifest_save_atomic(path, manifest)` は manifest を検証し、一時ファイルへの `fsync` 後に `rename` します。
- `YAP_V2_manifest_publish_next(path, manifest)` は `<path>.lock` を `flock(LOCK_EX)` で取得し、現在の manifest の generation に1を加えた値を採番して保存します。
- `YAP_V2_manifest_publish_if_generation(path, expected, manifest)` はlock下で現在generationが
  `expected`と同じ場合だけ`expected + 1`を公開します。batch writerはsegment構築中に別writerが
  先行した場合のlost updateを防ぐため、このcompare-and-swap APIを使います。
- `YAP_V2_snapshot_manager_reload`は新manifestを読み、descriptorが一致する既存segmentを再利用し、追加segmentだけを検証・読込してからcurrent pointerを交換します。同一generationはno-op、generation逆行は拒否し、検証失敗時は旧snapshotを維持します。queryはacquire/releaseでgenerationを固定し、reload後も参照中の旧snapshotは解放されません。

## 更新手順

1. 新しい document/passage segment を一時ファイルへ生成し、component、segment、`segments/`親directoryをfsyncする。
2. segment を公開名へ atomic rename する（この時点では manifest から未参照でよい）。
3. 新しいsegmentだけのsize、SHA-256、header、内部構造を検証する。
4. 現在の manifest を読み、既存 segment と新しい segment を含む候補を作る。
5. 読み込んだgenerationをexpectedとして`YAP_V2_manifest_publish_if_generation`を呼び出す。

CAS publishはlock下でgenerationを再読込します。競合したwriterは`YAP_V2_CONFLICT`となり、
候補segmentをmanifestへ部分公開しません。generationが`UINT64_MAX`に達した場合も更新を拒否します。

component headerのgenerationは、そのimmutable segmentが作成されたgenerationです。manifest readerは
`1 <= component generation <= manifest generation`、file size、SHA-256、file type、payload sizeを
合わせて検証します。このため既存segmentを書き換えずに新delta segmentだけを追加できます。

JSON parser は未定義キー、重複 segment ID、path separator を含む ID、checksum の桁数違い、末尾の余分なデータを拒否します。

compactionの公開・復旧・GC手順は [v2 compaction・GC・crash recovery](compaction_recovery.md)
を参照してください。
