# 運用・リリース手順

> v2 manifest、lexical/vector/hybrid検索、RAG retrieval、更新、compaction、runtime制限、
> health/metricsは製品経路へ接続済みです。具体的なprobe、metric、alert、障害対応は
> [v2 daemon運用runbook](operations_runbook_v2.md)に従います。最終完成判定は
> [現代検索基盤の完成契約](modern_search_completion_contract.md)に従います。

## 起動前チェック

1. index directory、`manifest.json`、`segments/` の所有者・権限を確認する。
2. v2 index を利用する場合は `manifest.json` を読み、format version、generation、segment checksum が期待値と一致することを確認する。
3. tokenizer/chunking/vector の設定を同じ generation の writer と reader で共有する。設定変更時は再索引して generation を分ける。
4. core を起動してから front を起動する。front は全 core への接続が確立してからリクエストを受け付ける。

## Probe と監視

- `GET /health/live` はfront processのliveness、`GET /health/ready`は全core接続と検証済みindex snapshotのreadinessを返す。
- `GET /metrics` のrequest件数、latency、generation、in-flight量、embedding/compaction状態を収集する。
- 検索クライアントは `/v2/search` の `total`、`next_cursor`、レスポンス時間、HTTP 400/5xx を記録する。
- 検索結果の generation はログ・トレース側で保持し、異なる generation の passage 引用を同じ回答に混ぜない。
- 4xx はクライアント入力、5xx/接続エラーは core・index・リソース枯渇の順に調査する。

## 更新と rollback

1. 新しい immutable segment を一時名へ書き、checksum と読込検証を完了する。
2. segment を公開名へ atomic rename する。
3. `YAP_V2_manifest_publish_next` で generation を採番し、候補 segment 一覧を atomic publish する。
4. `/health/ready` と JSON 検索の小さな smoke queryを確認し、期待generation以上であることを検証する。

問題がある場合は、直前の manifest をそのまま再 publish して rollback する。公開済み segment を上書きせず、不要 segment の削除は参照が切れた後に世代遅延を置いて行う。

## リリース gate

ローカルまたは CI で次を全て成功させる。

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'search_quality|ann_v2|v2_daemon_reliability' --output-on-failure
cmake --install build --prefix /tmp/yappod-release-smoke
```

CI では Ubuntu、macOS、clang sanitizer、libFuzzer、install smoke の全 job を必須とする。
品質・性能・信頼性の具体的なgateとreference benchmarkは
[v2 品質・性能・信頼性ゲート](quality_performance_reliability_v2.md)に従う。CIの小規模
baselineは回帰検出専用であり、本番リリース判定では同じhardware・dataset・request数の
`v2_load_probe --assert-reference`結果を別途保存する。

## 失敗時の証跡

- commit、manifest generation、segment checksum、設定ファイルの hash
- query、limit/cursor、HTTP status、latency、返却件数
- core/front のログ、sanitizer log、CTest の `LastTest.log`

これらを同じリリース ID で保存すると、検索品質の変化と index 更新の影響を追跡できます。
