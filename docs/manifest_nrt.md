# v2 manifest と NRT publish

`manifest.json` は公開済み immutable segment のスナップショットです。検索側は一つの generation を読み続け、writer は新しい manifest を完成させてから atomic rename します。そのため検索中に未完成の segment 一覧を観測しません。

## API

- `YAP_V2_manifest_load(path, manifest)` は厳格な JSON を読み込み、format version、generation、segment ID、件数、SHA-256 hex を検証します。呼び出し側は先に `YAP_V2_manifest_init` を実行します。
- `YAP_V2_manifest_save_atomic(path, manifest)` は manifest を検証し、一時ファイルへの `fsync` 後に `rename` します。
- `YAP_V2_manifest_publish_next(path, manifest)` は `<path>.lock` を `flock(LOCK_EX)` で取得し、現在の manifest の generation に1を加えた値を採番して保存します。

## 更新手順

1. 新しい document/passage segment を一時ファイルへ生成して fsync する。
2. segment を公開名へ atomic rename する（この時点では manifest から未参照でよい）。
3. 現在の manifest を読み、既存 segment と新しい segment を含む候補を作る。
4. `YAP_V2_manifest_publish_next` を呼び出す。

publish はロック下で generation を再読込するため、複数 writer が同時に呼び出しても generation は単調増加します。manifest が存在しない場合は generation 1 から開始します。generation が `UINT64_MAX` に達した場合は更新を拒否します。

JSON parser は未定義キー、重複 segment ID、path separator を含む ID、checksum の桁数違い、末尾の余分なデータを拒否します。
