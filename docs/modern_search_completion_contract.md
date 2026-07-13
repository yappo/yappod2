# 現代検索基盤の完成契約

## この文書の目的

この文書は、Yappod2を全文検索、ベクトル検索、hybrid検索、RAG向けpassage取得を備えた
単一ノード検索基盤へ置き換えた実装範囲と、releaseごとに再検証する最終受入条件を定義します。

## 現在地

| 機能 | 正式な製品経路 | 検証 |
|---|---|---|
| Lexical ranking | v2 postings、複数segment、BM25F、block-max WAND | `lexical_v2_*`、`v2_search_quality` |
| Document/passage | canonical ingest、initial build、snapshot、update | `v2_cli_acceptance`、`update_v2` |
| Manifest | checksum、atomic publish/reload、compaction、recovery | `manifest_v2_nrt`、`update_v2` |
| Search | CLI と HTTP の lexical/vector/hybrid orchestrator | `v2_cli_acceptance`、`http_v2_*` |
| RAG | passage ranking、source metadata、citation/context response | `retrieve_v2`、`http_v2_runtime` |
| Vector | 永続vector、exact、HNSW ANN、filter | `vector_v2_persistence`、`ann_v2` |
| Hybrid | lexical/vector候補、RRF、document/passage集約 | `query_v2`、`v2_search_quality` |
| Tokenizer | ICU NFKC casefold、boundary、chunker、config fingerprint | `unicode_tokenizer`、`canonical_ingest` |
| Operations | resource limit、write auth、health/metrics、crash/load試験 | `runtime_policy_v2`、`observability_v2`、daemon tests |

## 完成する製品の境界

- v1/Berkeley DB indexを正式経路から廃止し、v2 immutable segmentへ完全置換する。
- lexical、vector、hybrid検索をCLIとHTTP APIの双方から利用可能にする。
- RAG用途では、回答生成ではなく引用可能なpassage取得までを提供する。
- embeddingは事前計算vectorとベンダー中立なHTTP providerを提供する。
- 文書のupsert/deleteをCLIとHTTPで受け、generation単位で検索へ反映する。
- 単一ノードで100万document、最大300万passage級を対象とする。
- v1 indexからの変換は行わず、元文書から再索引する。

次は今回の完成条件に含めません。後から追加する場合は新しい製品拡張として扱います。

- LLMによる回答生成
- 分散sharding、replication、automatic failover
- facet、autocomplete、typo tolerance
- v1 index/APIとの後方互換

## 目標となる公開インターフェース

### Index layout

```text
<index_dir>/
  config.toml
  manifest.json
  segments/<segment-id>/
    documents.yap2
    terms.yap2
    postings.yap2
    positions.yap2
    metadata.yap2
    vectors.yap2
    vectors.usearch
```

manifestは各ファイルのsize、SHA-256、record件数、config fingerprintを保持します。
manifestから参照されるファイルの破損や設定不一致はfail-closedで拒否します。

### Search and ingest API

```text
POST /v2/search
POST /v2/retrieve
POST /v2/documents:batch
GET  /health/live
GET  /health/ready
GET  /metrics
```

- `/v2/search`は`lexical`、`vector`、`hybrid`と`documents`、`passages`を選択可能にする。
- `/v2/retrieve`はpassage本文、文書ID、URL、title、文字offset、lexical/vector/fused
  scoreを返す。
- `/v2/documents:batch`は最大100件のupsert/deleteをatomicに公開する。
- filterは設定済みmetadata fieldに対する`eq`、`in`、範囲、`exists`と論理演算を扱う。
- limitは最大100、cursorはmanifest generationとqueryへ結び付ける。

### CLI

```text
yappo_makeindex prepare --config config.toml --input documents.ndjson --output passages.ndjson
yappo_makeindex build --config config.toml --input documents.ndjson --index <dir>
yappo_makeindex update --input operations.ndjson --index <dir>
search --index <dir> --mode lexical|vector|hybrid --query <text>
yappo_compact --index <dir>
```

canonical入力はNDJSONとし、現行TSVは明示指定された入力adapterとしてだけ残します。

## 実装記録

次の24項目を1 branch、1 PRとして番号順に実装しました。名称は履歴上の番号だけでなく、
変更の意味を表す正本です。

1. **完成契約の訂正**
   - 本文書を正本とし、履歴上の番号だけでは意味が分からない表記を機能名へ変更する。
   - 既存部品と未接続箇所を明示する。
2. **ビルド・依存基盤**
   - ICU4C、libcurl、libevent、yyjson、tomlc99、USearchを再現可能なCMake buildへ導入する。
3. **v2 config loader**
   - `config.toml`のschema、default、未知key拒否、config fingerprintを実装する。
4. **Unicode tokenizerとchunker**
   - NFKC casefold、word/sentence/grapheme境界、決定的passage IDを実装する。
5. **Canonical ingest model**
   - NDJSON upsert/delete、metadata canonicalization、TSV adapter、`prepare`を実装する。
6. **Segmentとmanifestの完成**
   - 複数component、checksum、tombstone、atomic publishを実装する。
7. **Lexical index writer**
   - terms、postings、positions、field length、block-maxを生成する。
8. **Lexical segment reader**
   - memory-map、境界検証、term/posting/position iteratorを実装する。
9. **v2 lexical ranking**
   - BM25F、AND/OR、phrase、block-max WAND top-kを実装する。
10. **Metadata filterとsnippet**
    - filterable field index、filter AST、UTF-8安全なsnippetを実装する。
11. **Embedding provider**
    - HTTP batch providerとprecomputed vector readerを実装する。
12. **永続vectorとexact検索**
    - passage vector fileを既存exact backendへ接続する。
13. **HNSW ANN**
    - USearch build/save/viewとsegment横断候補取得を実装する。
14. **検索snapshotとlatest-wins**
    - generation snapshot、atomic reload、重複IDとtombstoneの解決を実装する。
15. **Hybrid query orchestration**
    - lexical/vector候補、filter、RRF、document/passage集約を接続する。
16. **RAG retrieval**
    - passage ranking、citation、per-document/context上限を実装する。
17. **Core内部protocol**
    - length-prefixed frameでsearch、retrieve、ingest、healthを扱う。
18. **検索・RAG HTTP API**
    - `/v2/search`と`/v2/retrieve`をv2 runtimeへ接続する。
19. **CLI・HTTP更新とNRT publish**
    - batch writer、generation publish、reader refreshを接続する。
20. **Compaction・GC・crash recovery**
    - live-only rewrite、orphan cleanup、publish failpointを実装する。
21. **並行性・資源制限・security**
    - writer lock、deadline、queue/memory limit、write tokenを実装する。
22. **Health・metrics・運用**
    - readiness、generation、latency、embedding/compaction状態、runbookを実装する。
23. **品質・性能・信頼性検証**
    - lexical/vector/hybrid評価、ANN recall、負荷、sanitizer、fuzz、更新並行試験を追加する。
24. **Legacy除去とrelease acceptance**
    - Berkeley DB、v1 reader/writer、旧protocol/API、不要wrapperを削除し、全受入試験を通す。

## 全体の受入条件

次をすべて満たした場合に限り、現代検索基盤を完成扱いにします。

1. Clean checkoutからREADMEの手順だけでbuild、再索引、起動できる。
2. 同一corpusをlexical、vector、hybridで検索できる。
3. RAG retrievalが引用可能なpassage本文とsource metadataを返す。
4. upsert/deleteが成功応答後1秒以内に新generationへ反映される。
5. restart、compaction、更新中crash後も旧または新generationの完全な一方だけを読める。
6. ANN Recall@10がexact ground truthに対して0.95以上である。
7. hybrid nDCG@10がlexical/vectorの良い方から0.01を超えて悪化しない。
8. 8 core、32 GiB、NVMe、100万document、300万passage、768 dimensionの基準環境で、
   concurrency 16、top 20のP95がlexical 100 ms以下、hybrid 200 ms以下である。
9. daemon RSSが24 GiB以下である。
10. macOS/Ubuntu CI、ASan/UBSan、install smoke、全CTestが成功する。
11. 正式文書に、完成に必要な未接続機能や「後で実装する」項目が残っていない。
12. `main`へ24項目すべてのPRがmergeされ、working treeがcleanである。

機能・品質の小規模受入れは CTest と CI で自動検証します。条件8と9の基準性能だけは、
専用の100万document corpusと指定hardwareで `v2_load_probe --assert-reference` を実行し、
release artifactを保存しなければ「確認済み」にできません。基準環境を使わないCI smokeで
代替してはいけません。

## 開発フロー

- 開発baseとPR baseは`main`とする。
- タスク合意後に`codex/<topic>`を作成する。
- commitは`fix|refactor|test|docs|chore: imperative summary`形式にする。
- 各PRで`cmake --build build -j`と`ctest --test-dir build --output-on-failure`を実行する。
- `gh pr create --base main --head <branch> --title <title> --body-file <file>`でPRを作る。
- 必須CIを20秒間隔で確認し、すべて成功してからmergeする。
- merge後は`main`へ戻り、`git pull --ff-only origin main`で反映を確認する。
- 次項目へ自動遷移せず、次の着手候補と残項目を提示する。
