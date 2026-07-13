# v2 search quality and load tooling

This directory contains deterministic v2 quality, ANN recall, daemon reliability, and load tools.
Committed corpora are deliberately small regression guards; they are not substitutes for the
release reference benchmark.

CTest registers:

- `v2_search_quality`: lexical/vector/hybrid nDCG@10 and Recall@10
- `ann_v2`: HNSW Recall@10 against exact ground truth
- `v2_daemon_reliability`: concurrent search/update, P95, RSS, and latest-generation visibility
- `search_quality_metrics`: metric implementation contracts

Run the gates locally:

```sh
cmake --build build -j
ctest --test-dir build \
  -R '^(v2_search_quality|ann_v2|v2_daemon_reliability|search_quality_metrics)$' \
  --output-on-failure
```

`v2_load_probe` runs against an externally started daemon and is intentionally not a CTest smoke.
The authoritative 1M-document procedure, hardware profile, thresholds, and evidence requirements
are documented in
[`docs/quality_performance_reliability_v2.md`](../../docs/quality_performance_reliability_v2.md).
