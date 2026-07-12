# 運用・リリース手順

## 起動前チェック

1. index directory と `pos/` の所有者・権限を確認する。
2. v2 index を利用する場合は `manifest.json` を読み、format version、generation、segment checksum が期待値と一致することを確認する。
3. tokenizer/chunking/vector の設定を同じ generation の writer と reader で共有する。設定変更時は再索引して generation を分ける。
4. core を起動してから front を起動する。front は全 core への接続が確立してからリクエストを受け付ける。

## Probe と監視

- `GET /healthz` は検索を実行しない liveness probe で、front process が HTTP を受け付けられると `200` を返す。
- 検索クライアントは `/v2/search` の `total`、`next_cursor`、レスポンス時間、HTTP 400/5xx を記録する。
- 検索結果の generation はログ・トレース側で保持し、異なる generation の passage 引用を同じ回答に混ぜない。
- 4xx はクライアント入力、5xx/接続エラーは core・index・リソース枯渇の順に調査する。

## 更新と rollback

1. 新しい immutable segment を一時名へ書き、checksum と読込検証を完了する。
2. segment を公開名へ atomic rename する。
3. `YAP_V2_manifest_publish_next` で generation を採番し、候補 segment 一覧を atomic publish する。
4. `/healthz` と JSON 検索の小さな smoke query を確認する。

問題がある場合は、直前の manifest をそのまま再 publish して rollback する。公開済み segment を上書きせず、不要 segment の削除は参照が切れた後に世代遅延を置いて行う。

## リリース gate

ローカルまたは CI で次を全て成功させる。

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
ctest --test-dir build -R search_quality --output-on-failure
cmake --install build --prefix /tmp/yappod-release-smoke
```

CI では Ubuntu、macOS、clang sanitizer、install smoke の全 job を必須とする。性能評価は `tests/quality` の小規模 baseline を回帰検出に使い、本番リリース判定では同じ hardware・dataset・repeat 数の大規模負荷試験を別途保存する。

## 失敗時の証跡

- commit、manifest generation、segment checksum、設定ファイルの hash
- query、limit/cursor、HTTP status、latency、返却件数
- core/front のログ、sanitizer log、CTest の `LastTest.log`

これらを同じリリース ID で保存すると、検索品質の変化と index 更新の影響を追跡できます。
