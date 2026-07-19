# 検索品質と負荷の確認

このディレクトリには、検索品質、ANNの再現率、デーモンの信頼性、負荷を決定的に確認する道具があります。
リポジトリへ含める小さなテスト資料は回帰検出用であり、リリース時の大規模な基準試験を代替しません。

CTestには次が登録されています。

- `v2_search_quality`は各検索方式のnDCG@10とRecall@10を確認します。
- `ann_v2`は全件走査を正解としてHNSWのRecall@10を確認します。
- `v2_daemon_reliability`は検索と更新の並行実行、P95、RSS、最新generationの可視性を確認します。
- `search_quality_metrics`は品質指標の計算規則を確認します。

```sh
cmake --build build -j
ctest --test-dir build \
  -R '^(v2_search_quality|ann_v2|v2_daemon_reliability|search_quality_metrics)$' \
  --output-on-failure
```

`v2_load_probe`は外部で起動したデーモンへリクエストを送り、通常のCTestには含めません。大規模試験では
ハードウェア、データ集合、検索文、事前実行、同時実行数を固定し、結果と実行条件を一緒に保存してください。概要は
[開発と品質確認](../../docs/development.md)を参照してください。
