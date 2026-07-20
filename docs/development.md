# 開発と品質確認

この文書はYappod2本体を変更する開発者向けです。利用者向けの導入は[INSTALL](../INSTALL)、コマンドの利用方法は[コマンドリファレンス](command-reference.md)を参照してください。

## ソースの構成

| ディレクトリ | 内容 |
|---|---|
| `src/` | Cによる索引、検索、HTTP、デーモン、コマンドの実装です。公開対象を含む宣言は対応するヘッダーにあります。 |
| `tests/cmocka/` | C API、保存形式、CLI、HTTP、異常系の単体・結合テストです。 |
| `tests/quality/` | 検索品質、ANN再現率、デーモン信頼性、負荷計測のテスト資料と実行ファイルです。 |
| `examples/` | 実行可能な設定、入力生成、Web UIです。 |
| `cmake/` | 外部依存関係とCMake補助処理です。 |

## 必要な依存関係

CMake 3.20以降、C99コンパイラー、POSIXスレッド、ICU4C、libcurl、libevent、cmocka、Python 3を使用します。tomlc99、yyjson、USearchは`cmake/Dependencies.cmake`で取得するコミットを固定しています。初回のCMake設定時には、未取得の依存関係を取得できるネットワーク接続が必要になる場合があります。

macOSでHomebrewを使う例は[INSTALL](../INSTALL)に掲載しています。依存関係を更新する場合は、固定コミット、ライセンス、ビルド対象、保存形式や近似最近傍探索結果への影響を確認します。

## 通常のビルドと全テスト

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`YAPPOD_WARNINGS_AS_ERRORS`は既定で有効です。警告を一時的に無視する前提の変更は追加しないでください。CTestの登録内容だけを確認する場合は次を実行します。

```sh
ctest --test-dir build -N
```

## CMakeの主な選択肢

| 選択肢 | 既定値 | 用途 |
|---|---|---|
| `YAPPOD_WARNINGS_AS_ERRORS` | `ON` | コンパイラー警告をエラーにします。 |
| `YAPPOD_TESTS_DAEMON` | `ON` | ポート、fork、signalを使うデーモン試験を構築します。 |
| `YAPPOD_TESTS_NODAEMON` | `ON` | デーモンを起動しない単体・結合試験を構築します。 |
| `YAPPOD_BUILD_FUZZERS` | `OFF` | ClangのlibFuzzer用対象を構築します。 |

デーモンを実行できない制限環境では`-DYAPPOD_TESTS_DAEMON=OFF`を使えますが、通常の変更確認では両方を有効にしてください。

## テスト群

現在のCTestは次の領域を確認します。

- `config_v2`と`application_config`は索引用TOMLとアプリケーション用TOMLの型、範囲、整合条件を確認します。
- `canonical_ingest`、`update_v2`、`segment_planner_v2`はNDJSON、更新の不可分性、セグメント上限を確認します。
- `lexical_v2_*`、`bm25_score`、`vector_search`、`ann_v2`、`hybrid_rerank`は各検索方式を確認します。
- `manifest_v2_nrt`、`snapshot_v2`、`index_v2_contracts`は公開、再読み込み、保存形式の契約を確認します。
- `core_protocol_v2`、`http_v2_runtime`、`http_v2_daemon`は内部フレームとHTTP APIを確認します。
- `runtime_policy_v2`、`observability_v2`、`v2_daemon_reliability`は処理上限、メトリクス、並行更新を確認します。
- `v2_search_quality`と`search_quality_metrics`は品質指標の回帰を確認します。
- `wikipedia_example_converter`はWikipedia exampleの変換処理を確認します。

特定領域だけを再実行する場合も、変更完了時には全CTestを実行します。

```sh
ctest --test-dir build -R '^(core_protocol_v2|http_v2_runtime|http_v2_daemon)$' --output-on-failure
ctest --test-dir build -R '^(ann_v2|hybrid_rerank|v2_search_quality)$' --output-on-failure
```

## exampleのテスト

Python exampleはそれぞれのディレクトリから標準ライブラリの`unittest`で確認できます。

```sh
python3 -m unittest discover -s examples/local-files/tests -p 'test_*.py'
python3 -m unittest discover -s examples/wikipedia-search/tests -p 'test_*.py'
```

search-webはリポジトリのルートから次を実行します。

```sh
cd examples/search-web
npm ci
npm run typecheck
npm test
npm run build
npm run test:e2e
```

`npm ci`は依存関係を更新する処理ではなく、`package-lock.json`どおりに再現します。外部LLMや埋め込みAPIへ接続しないテスト構成を使用してください。

## サニタイザー

通常のビルドと別のディレクトリを作り、ASanとUBSanを有効にします。コンパイラーに応じて指定方法が異なるため、CIのworkflowと実際のコンパイラーオプションを揃えてください。サニタイザーで失敗した入力は最小化し、通常ビルドでも検出できる回帰テストを可能な限り追加します。

## ファジング

ClangとlibFuzzerを使える環境では次の対象を構築できます。

- `core_protocol_v2_fuzz`はfront/core間フレームの解析を対象にします。
- `ingest_v2_fuzz`は正式なNDJSON入力の解析を対象にします。

```sh
cmake -S . -B build-fuzz \
  -DCMAKE_C_COMPILER=clang \
  -DYAPPOD_BUILD_FUZZERS=ON
cmake --build build-fuzz -j
```

ファザーの実行時間、入力データ集合、検出した入力の保存方針は実行環境で明示します。秘密情報や利用者データを入力データ集合へ入れないでください。

## 小さな索引による受け入れ確認

保存形式、索引作成、検索を変更した場合は、一時ディレクトリへ索引を作って全検索方式を確認します。既存の利用者索引をテスト対象にしないでください。

```sh
tmpdir="$(mktemp -d)"
sed "s|directory = \"./index-lexical\"|directory = \"$tmpdir/index\"|" \
  examples/config.lexical.toml > "$tmpdir/application.toml"
./build/yappo_makeindex build \
  --config "$tmpdir/application.toml" \
  --input examples/documents.lexical.ndjson
./build/yappo_makeindex verify --config "$tmpdir/application.toml"
./build/search --config "$tmpdir/application.toml" --mode lexical --query search
```

この例では一時ディレクトリを自動削除していません。出力を確認した後、作成した`$tmpdir`だけを削除してください。

## 文書変更の確認

文書だけを変更した場合も、記載したコマンド、設定キー、既定値、範囲、HTTP状態、JSONフィールドをソースとテストへ照合します。

```sh
git diff --check
```

Markdownの相対リンク、削除した文書への参照、廃止済みの`schema_version`、平文の外部APIトークンがないことも確認します。日本語文書は、見出しだけでなく表と注記を含め、自然なですます調になっているか目視で校正します。

## 品質試験と性能測定

リポジトリ内の小規模テストは、結果の再現性と回帰検出を目的とします。実運用規模の性能を保証するものではありません。大規模な基準試験では、ハードウェア、OS、コンパイラー、データ集合、検索文、同時実行数、事前実行、索引設定を固定し、P50/P95/P99、RSS、ANN Recall@10、nDCG@10を条件と一緒に保存します。具体的な対象は[品質テスト](../tests/quality/README.md)を参照してください。
