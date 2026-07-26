# C実装の作業規則

適用範囲は`src/`です。全体の依存方向と契約境界は
[`docs/architecture.md`](../docs/architecture.md)を一次資料とします。

- C99、2スペースインデント、既存のK&R風の中括弧、`YAP_`関数名規約に合わせます。宣言は対応する
  ヘッダーへ置きます。
- `common`、`config`、`storage`、`components`、`query`／`indexing`、`server`、`app`の順に
  上位責務を構成します。逆方向のincludeやリンクを追加せず、共有する型や処理は適切な下位責務へ
  移します。
- 内部includeは`"query/yappo_query_v2.h"`のように責務付きパスで記述します。
- CLI、設定、canonical NDJSON、HTTP、front/core通信、メトリクス、v2索引形式を変更する場合は、
  対応する正式文書と受け入れテストも同じタスクで更新します。
- 対応する責務の`tests/`へ回帰テストを置き、完了時は
  [`docs/development.md`](../docs/development.md)の通常ビルドと全CTestを実行します。
