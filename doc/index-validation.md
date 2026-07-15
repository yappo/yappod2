# Index 検証の実施タイミング

必要な処理は次の3つです。検索時の全検証は不要です。

| タイミング | 必須 | 実行する処理 | コスト |
|---|---:|---|---|
| `yappod_core` 起動時 | 必須 | config/manifestを確認し、全segmentを検証して検索用readerを一度だけ開く | index全体に比例。起動時の1回だけ |
| document追加時 | 必須 | 入力と新しく作ったsegmentだけを検証し、manifestをatomic publishしてreaderへ追加する | 追加データ量に比例。既存segmentは再検証しない |
| 検索時 | なし | 保持済みreaderを参照し、query tokenize、posting/ANN検索、filter/ranking/snippet生成だけを行う | queryと候補件数に比例。index全体のchecksum計算はしない |
| cron・ユーザー操作 | 任意 | `yappo_makeindex verify`で全componentのchecksumと内部構造を検証する | index全体に比例。検索処理とは別に実行 |

coreはmanifest generationを1秒間隔で確認します。HTTP追加はpublish直後に、外部の
`yappo_makeindex update`は次の確認時に、追加segmentだけを読んでsnapshotへ反映します。

## 何から守るか

- 起動時検証: 壊れたファイル、configとmanifestの不一致、読めない内部構造を検索受付前に拒否する。
- 追加時検証: 書込途中または壊れた新segmentをmanifestへ公開しない。
- atomic manifest publish: 検索が未完成のgenerationを見ることを防ぐ。
- 手動全検証: 稼働後のdisk corruptionや管理者によるindex改変を検出する。

検索requestごとの全checksum再計算は、immutable segmentと起動時検証で得られる安全性を重複して
確認するだけなので実行しません。

## 手動・定期実行

```sh
./build/yappo_makeindex verify --index /path/to/index
```

成功時は検証したgenerationとsegment数を標準出力へ出して終了コード`0`を返します。失敗時は
標準エラーへ理由を出して非`0`を返します。cronや監視は非`0`をアラート条件にできます。

```cron
0 3 * * * /opt/yappod/bin/yappo_makeindex verify --index /srv/yappod/index || /usr/local/bin/alert-yappod
```

全検証はCPUとstorage帯域を使うため、検索負荷が低い時間帯に実行してください。検証中もcoreは
保持済みsnapshotで検索を継続します。

## Wikipedia 10kでの参考実測

2026-07-16に同一hostの1.7GB、4 segmentのindexで測定した参考値です。storage cacheやhardwareで
変わるため保証値ではありません。

- `verify`の全検証: 17.25秒
- 起動済みcoreへのlexical検索3回: 5.16–9.49ミリ秒
