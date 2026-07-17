# v2 compaction・GC・crash recovery

`yappo_compact` は、現在の `manifest.json` が参照するimmutable segment群から、最新の
upsertだけを一つの新segmentへ再構築します。新しいtombstoneで削除された文書と、より新しい
upsertで置換された旧文書・旧passageは出力しません。lexical、filterable metadata、vector、
ANNを同じgenerationで再生成するため、compaction後もlexical/vector/hybrid検索とRAG retrieval
の結果を維持します。

```sh
yappo_compact --index /tmp/yappoindex-v2
```

成功時は新generation、live document/passage数、削除したsegment数、新segment IDをJSONで返します。

## 公開と復旧の順序

1. index全体のwriter lockを取得し、現manifestとchecksumを含む全componentを検証する。
2. 現manifestが参照しない孤立segmentを削除する。
3. live document/passageと対応vectorだけを新しいsegmentへ書く。
4. 各component、segment directory、`segments/`親directoryを `fsync` する。
5. generation compare-and-swapで、1 segmentだけを参照する新manifestをatomic renameする。
6. 新manifestが参照しなくなった旧segmentを削除する。

manifestのatomic renameだけが公開点です。手順5より前に停止した場合は旧generationが完全なまま残り、
完成済みだが未参照の新segmentは次回GCされます。手順5より後に停止した場合は新generationが完全に
参照され、旧segmentは次回GCされます。このため再起動時に未完成なgenerationを読む経路はありません。

テスト専用APIの `YAP_V2_compaction_set_failpoint_for_testing()` に`before_publish`または
`after_publish`を渡すと、manifest公開の直前・直後にprocessを終了します。製品コードはこのfailpointを通常設定せず、回帰試験が旧/新の
完全な一方だけを読めることと、次回実行で孤立segmentを回収できることを検証します。

## GCの安全境界

GCはupdate/compactionと共通のwriter lock下で動作します。現在読み取って検証したmanifestの
segment IDを保護し、`segments/`直下の未参照directoryだけを
削除します。symlinkや通常directory以外は追跡・削除しません。既にsnapshotがmmapした旧fileは
POSIXのunlink後もsnapshot releaseまで有効であり、新しいreaderはmanifestが参照するsegmentだけを
開きます。compactionと通常更新の同時publishはgeneration CASで競合検出し、勝者以外のsegmentを
公開しません。writer lockはGCが構築中の未参照segmentを削除する競合も防ぎます。
