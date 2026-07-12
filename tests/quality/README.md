# Search quality baseline

This directory contains the deterministic smoke corpus used to detect search-quality regressions.
It is deliberately small and is not a substitute for the product-level judged corpus planned for
the modern search release.

## Data format

All files are UTF-8 TSV. Blank lines and lines beginning with `#` are ignored.

- `queries.tsv`: `query_id<TAB>query_text`; query IDs must be unique.
- `qrels.tsv`: `query_id<TAB>document_id<TAB>relevance`; relevance is an integer from 0 to 3 and
  follows the graded-qrels convention used by nDCG.
- `baseline.tsv`: `metric<TAB>min|max<TAB>threshold`; quality metrics use minimum thresholds and
  latency uses a maximum smoke-test threshold.

Document IDs are the URLs emitted by the current `search` CLI. Every query must have at least one
judgment with relevance greater than zero.

## Metrics

`search_quality` builds a fresh index, executes each query repeatedly, and reports:

- mean nDCG@10 using gain `2^relevance - 1`;
- mean MRR@10 where any relevance greater than zero is relevant;
- mean Recall@10 over judged relevant documents;
- nearest-rank P95 wall-clock latency across all query executions.

The CLI also verifies that repeated executions return the same document order. The committed P95
threshold is intentionally a broad smoke guard because wall-clock performance varies by host; full
release performance is measured on the documented reference hardware.

## Run locally

```sh
cmake --build build -j
./build/search_quality \
  --build-dir ./build \
  --input ./tests/fixtures/index.txt \
  --queries ./tests/quality/queries.tsv \
  --qrels ./tests/quality/qrels.tsv \
  --baseline ./tests/quality/baseline.tsv \
  --repeat 5
```

The same check is registered with CTest as `search_quality_baseline`.
