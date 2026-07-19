# 索引作成

## 入力形式

正式な入力はUTF-8のNDJSONです。1行には`upsert`または`delete`を1操作だけ書きます。
不正なJSON、未知のキー、型違反、空のIDが1行でもあれば入力全体を拒否します。

```json
{"operation":"upsert","id":"doc-1","url":"https://example.test/","title":"題名","body":"本文です。","metadata":{"lang":"ja"},"updated_at_unix_ms":1735689600000}
{"operation":"delete","id":"doc-2"}
```

`upsert`では`id`と`body`が必須です。`metadata`はオブジェクトに限り、キーの順序を正規化したJSONとして保存します。
ベクトル対応索引へ書く場合は、チャンク分割後のパッセージ順に二次元配列`vectors`を指定します。件数、`dimensions`、
有限値をすべて検証します。

## 文字列の処理

正式なトークン分割器はICUを使ってUTF-8を検証し、NFKC Casefoldで正規化します。単語境界で検索トークンを作り、
文の境界を優先してパッセージを作ります。長い文は書記素クラスタの境界で分割するため、結合文字や絵文字の
途中では切りません。位置は元文書に対するUnicodeコードポイント単位です。

`chunking.max_chars`と`overlap_chars`はパッセージの大きさと重なりを決めます。パッセージIDは文書ID、`ordinal`、
位置、正規化済み本文から決定的に作られます。同じ設定と入力からは同じIDが得られます。

## `prepare`

```sh
./build/yappo_makeindex prepare \
  --config application.toml \
  --input documents.ndjson \
  --output passages.ndjson
```

TSVを変換する場合だけ`--input-format tsv`を明示します。TSVは正式入力ではなく、移行用の入力形式です。

## embeddingの生成

索引へ保存するベクトルは、事前にNDJSONへ含めます。CライブラリにはOpenAI互換HTTPによる一括生成と、IDで結合する
事前計算済みベクトルの読み出し機能があります。サンプルのPythonとTypeScriptの変換プログラムはLM Studio、Ollama、OpenAI互換エンドポイントを
利用できます。

HTTPレスポンスでは`data[].index`、件数、`dimensions`、有限値を検証します。事前計算済みの形式では重複ID、
不足ID、未知のキー、壊れたJSONを拒否します。認証トークンはBearerヘッダーだけに設定し、JSONやエラーへ含めません。

## 索引の作成

`yappo_makeindex build`は入力全体を検証し、未作成のディレクトリへ索引を作ります。文書境界を保ちながらセグメントへ
分割し、すべてのコンポーネントとmanifestを完成させてから公開します。途中で失敗した未完成索引は公開しません。
