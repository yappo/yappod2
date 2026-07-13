# v2 品質・性能・信頼性ゲート

この文書は、v2 lexical/vector/hybrid検索の品質、ANN recall、daemon負荷、更新並行性、
sanitizer、fuzzを同じ手順で再検証するための正本です。CIの小規模smokeと、完成契約の
基準環境で行うrelease benchmarkは目的も閾値も異なります。小規模smokeの成功を、
100万documentでの性能受入れ結果として扱ってはいけません。

## CIで常時実行するゲート

通常のCTestには次が含まれます。

- `v2_search_quality`: 3つのjudged queryをlexical/vector/hybridの同一corpusへ実行し、
  mode別nDCG@10とRecall@10を算出する。hybrid nDCG@10がlexical/vectorの良い方から
  0.01を超えて悪化したら失敗する。
- `ann_v2`: 512件、16 dimensionの決定的vectorと40 queryを使い、exact top 10を
  ground truthとしてHNSW Recall@10を算出する。0.95未満なら失敗する。
- `v2_daemon_reliability`: 実際のfront/coreに16並行で192検索を送り、nearest-rank
  P95と両daemonのRSSを測る。その後、検索と8 generationの更新を同時に実行し、
  全検索が200のまま、最終generationとlatest documentを取得できることを確認する。
  CI smokeの上限はP95 2000 ms、RSS 1 GiBであり、環境異常と大きな回帰の検出用である。
- `v2_cli_acceptance`: clean な出力先への初回 build、全検索 mode、失敗時の atomic cleanup、
  旧 CLI の拒否を実 executable で検証する。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -R '^(ann_v2|v2_search_quality|v2_daemon_reliability)$' \
  --output-on-failure
ctest --test-dir build --output-on-failure
```

`v2_daemon_reliability`はloopback portを使うため、network sandbox内ではなく、ローカル
TCP listen/connectを許可した環境で実行します。

## ASan/UBSanとlibFuzzer

CIのsanitizer jobは全CTestにASan/UBSanを適用し、さらにcore frame decoderとcanonical
ingest JSON parserをlibFuzzerで各2000回実行します。ローカルで同じ構成を再現できます。
macOSのXcode付属Apple ClangにはlibFuzzer runtimeが含まれない配布形態があるため、その場合は
Homebrew LLVMなどlibFuzzer runtimeを含むClangを`CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`へ
指定します。runtimeがないcompilerではfuzz executableのlinkが失敗するため、fuzz成功として
扱いません。

```sh
cmake -S . -B build-sanitizer \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DYAPPOD_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitizer -j
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-sanitizer --output-on-failure
./build-sanitizer/core_protocol_v2_fuzz \
  -runs=2000 -max_len=4096 tests/fuzz/corpus/core_protocol
./build-sanitizer/ingest_v2_fuzz \
  -runs=2000 -max_len=16384 tests/fuzz/corpus/ingest
```

新しいcrash inputが見つかった場合は、個人情報や秘密を含まない最小化済みinputだけを
対応する`tests/fuzz/corpus/`へ追加し、通常のunit testにも回帰条件を固定します。

## release reference benchmark

完成契約の性能受入れは、8 core、32 GiB RAM、NVMe、100万document、300万passage、
768 dimensionの再索引済みindexで実施します。query setはreleaseごとに固定し、lexicalと
hybridのrequest JSONを用意します。どちらも`limit: 20`、hybridの`vector`は768要素で
なければ`--assert-reference`が拒否します。

daemonは`YAPPOD_V2_MAX_INFLIGHT=16`で起動し、warm-up queryを実行してreadinessを確認します。
次のprobeはmodeごとに同数のrequestを16並行で送り、P95とfront/core合計RSSを一つのJSONへ
出力します。

```sh
./build/v2_load_probe \
  --port 18400 \
  --lexical-request benchmark/lexical.json \
  --hybrid-request benchmark/hybrid.json \
  --requests 10000 \
  --concurrency 16 \
  --front-pid "$(cat run/front.pid)" \
  --core-pid "$(cat run/core.pid)" \
  --documents 1000000 \
  --passages 3000000 \
  --dimensions 768 \
  --assert-reference | tee benchmark/result.json
```

`--assert-reference`は次をすべて満たした場合だけ終了status 0を返します。

- lexical P95が100 ms以下
- hybrid P95が200 ms以下
- front/core合計RSSが24 GiB以下
- concurrency、corpus件数、dimension、request limitが完成契約と一致する

## 保存する証跡

release判定には`result.json`だけでなく、次を同じartifactへ保存します。

- git commit、OS/kernel、CPU core数、RAM、storage種別
- `config.toml`、manifest generation、manifest SHA-256、document/passage件数
- query setのSHA-256、warm-up回数、計測request数
- front/core起動引数と環境変数、daemon log、`/metrics`の計測前後snapshot
- 全CTest、sanitizer、fuzzerのlog

基準hardwareまたは基準corpusが用意できずreference benchmarkを実行していない場合、性能受入れは
「未確認」と記録します。CI smokeの数値で代替してはいけません。
