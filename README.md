# Yappod2

Yappod2 は、単一ノード向けの UTF-8 検索基盤です。immutable segment と atomic manifest を
使い、BM25F lexical 検索、HNSW vector 検索、RRF hybrid 検索、RAG 向け passage 取得、
NRT upsert/delete、compaction を提供します。正式な index 形式は v2 のみです。
Berkeley DB 形式、旧検索 protocol、旧 CLI/API との互換性はありません。旧 index は元文書から
再索引してください。

## 必要環境

- CMake 3.20 以上
- C/C++ compiler
- cmocka、ICU4C、libcurl、libevent
- CMake が取得・固定する tomlc99、yyjson、USearch の source dependency

macOS:

```sh
brew install cmake cmocka icu4c libevent curl
```

macOSではCMakeがHomebrewを検出し、ICU4C、libevent、curlのinstall prefixを依存libraryの
探索先へ追加します。Homebrewに対する操作はprefixの参照だけで、formulaのinstallや既存の
CMake cacheの修復は行いません。

Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y cmake g++ libcmocka-dev libicu-dev \
  libcurl4-openssl-dev libevent-dev
```

## Clean checkout から検索まで

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

rm -rf examples/index
./build/yappo_makeindex build \
  --config examples/config.toml \
  --input examples/documents.ndjson

./build/search \
  --config examples/config.toml \
  --mode lexical \
  --query "modern search"
./build/search \
  --config examples/config.toml \
  --mode vector \
  --vector "0,1,0"
./build/search \
  --config examples/config.toml \
  --mode hybrid \
  --query "modern search" \
  --vector "1,0,0"
```

`build` の出力先は存在していない path である必要があります。入力または index 作成に失敗した
場合、完成していない出力先は公開されません。

## Index 設定と canonical 入力

同じapplication `config.toml`をindex生成、検索、compaction、daemon起動で使用します。
`[index].directory`、tokenizer、chunking、vector、metadata、daemon設定を保持します。indexには
index構造に必要なsectionだけが`index/config.toml`として保存されます。

標準入力は 1 行 1 operation の NDJSON です。

```json
{"operation":"upsert","id":"doc-1","url":"https://example.test/1","title":"Title","body":"Body","metadata":{"language":"ja"},"vectors":[[0.1,0.2,0.3]]}
```

precomputed vector は chunk 後の passage 順に指定します。TSV を使う場合は明示的な adapter で
canonical NDJSON に変換します。

```sh
./build/yappo_makeindex prepare \
  --config config.toml \
  --input documents.tsv \
  --input-format tsv \
  --output documents.ndjson
```

入力 schema の詳細は [canonical ingest](docs/canonical_ingest.md)、index の on-disk 契約は
[v2 index contract](docs/index_v2_contract.md)を参照してください。

## 更新と compaction

最大 100 operation の batch を一つの generation として atomic に反映します。

```sh
./build/yappo_makeindex update \
  --config examples/config.toml \
  --input operations.ndjson

./build/yappo_compact --config examples/config.toml
```

同一 document ID は新しい segment が優先され、delete tombstone は古い record を隠します。
compaction は live record だけを新しい segment に書き、manifest 公開後に orphan を回収します。
詳細は [manifest/NRT](docs/manifest_nrt.md) と
[compaction/recovery](docs/compaction_recovery.md)を参照してください。

## Index の全検証

全 component の checksum と内部構造を手動で検証します。成功時は終了コード `0`、失敗時は
非 `0` なので cron や監視から実行できます。

```sh
./build/yappo_makeindex verify --config examples/config.toml
```

検索時にはこの全検証を実行しません。実施タイミングの仕様は
[index validation](doc/index-validation.md)を参照してください。

## HTTP daemon

core を先に、front を後に起動します。両 process は background 化し、`[daemon].run_directory`に
PID と log を作成します。
daemon用には`18400`–`18409`を予約し、現在はfrontが`18400`、coreが`18401`を使用します。

```sh
/path/to/yappod2/build/yappod_core --config /path/to/config.toml
/path/to/yappod2/build/yappod_front --config /path/to/config.toml
```

公開 endpoint:

```text
POST /v2/search
POST /v2/retrieve
POST /v2/documents:batch
GET  /health/live
GET  /health/ready
GET  /metrics
```

検索例:

```sh
curl -sS -H 'Content-Type: application/json' \
  --data '{"query":"modern search","vector":[1,0,0],"mode":"hybrid","scope":"documents","limit":20}' \
  http://127.0.0.1:18400/v2/search
```

RAG 用 passage 取得例:

```sh
curl -sS -H 'Content-Type: application/json' \
  --data '{"query":"grounded context","vector":[0,1,0],"mode":"hybrid","limit":10,"max_passages_per_document":2,"max_context_bytes":32768}' \
  http://127.0.0.1:18400/v2/retrieve
```

`/v2/retrieve` は passage 本文、source document metadata、元文書内 offset、context 内 offset、
各 score を返します。LLM による回答生成は行いません。HTTP schema は
[search/RAG HTTP v2](docs/search_rag_http_v2.md)を参照してください。

write endpoint を公開する環境では、共有application TOMLの`[daemon]`へ16 byte以上のtokenを設定し、
両daemonへ同じ設定fileを渡してください。

```toml
[daemon]
write_token = "replace-with-a-secret-token"
```

更新 request は `Authorization: Bearer ...` が必要になります。deadline、in-flight 上限を含む
全設定は [runtime limits and security](docs/runtime_limits_security.md)、起動・監視・障害対応は
[operations runbook](docs/operations_runbook_v2.md)を参照してください。

## Test、install、release gate

```sh
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/yappod2-install
```

CTest は全検索 mode、RAG retrieval、atomic update、compaction/crash recovery、HTTP daemon、
ANN Recall@10、hybrid nDCG@10、並行検索/更新を検証します。CI は Ubuntu/macOS build、全 CTest、
ASan/UBSan、parser fuzz、install 後の再索引と検索 smoke を実行します。

100 万 document、300 万 passage、768 dimension の性能受入れは専用 corpus と基準 hardware 上で
別途実行します。小規模 CI の結果で代替しません。手順と閾値は
[quality/performance/reliability gate](docs/quality_performance_reliability_v2.md)、製品全体の判定条件は
[modern search completion contract](docs/modern_search_completion_contract.md)を参照してください。
