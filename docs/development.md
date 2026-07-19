# 開発と品質確認

## 依存ライブラリ

ICU4C、libcurl、libevent、cmockaはシステムのパッケージを利用します。tomlc99、yyjson、USearchは
`cmake/Dependencies.cmake`でコミットを固定します。依存関係の更新時はライセンスとセキュリティ情報を確認し、
新しいビルドディレクトリでのビルドから
全テストを実行してください。

## 通常の確認

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CTestには入力、索引、全検索方式、HTTP、更新、コンパクション、異常終了からの復旧、ANNの再現率、検索品質、並行処理の
テストが含まれます。

## サニタイザーとファジング

CIではASanとUBSanを適用したテストと、coreのフレーム読み出し、正式な入力の解析に対するファジングを実行します。
異常終了を起こす新しい入力を追加する場合は、秘密情報を含まない最小化済みの入力をテスト資料と通常の回帰テストへ追加してください。

## 品質と負荷

CIに含める小さなテスト資料は回帰検出用です。100万文書、300万パッセージ、768次元の基準試験を代替しません。
基準試験ではハードウェア、データ集合、検索文の集合を固定し、P95、RSS、ANN Recall@10、複合検索のnDCG@10と
実行条件を保存します。具体的なテスト用実行ファイルは[品質テスト](../tests/quality/README.md)を参照してください。
