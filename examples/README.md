# サンプル一覧

Yappod2には、目的の異なる3つのサンプルがあります。コマンドだけを確認する場合は、ルートの
`examples/config*.toml`と小さなNDJSONを利用してください。

| サンプル | 適した用途 | 主な成果物 |
|---|---|---|
| [local-files](local-files/README.md) | 手元の文書、PDF、Office文書、ソースコードを検索します。 | 文書とパッセージの分割ファイル、ベクトル、索引です。 |
| [Wikipedia](wikipedia-search/README.md) | 日本語Wikipediaの記事で検索とRAGを試します。 | 正式な入力NDJSON、索引、Web UIです。 |
| [search-web](search-web/README.md) | 作成済みの任意の索引をブラウザーから利用します。 | BFF、Web UI、PID、ログです。 |

local-filesとWikipediaにはPython 3.9以上、search-webにはNode.js 22以上とnpmが必要です。どのサンプルでも、先に
リポジトリのルートで`cmake --build build -j`を実行してください。PDFやOffice文書の抽出、Wikipediaのダンプ変換、
embeddingには、各READMEに記載した追加のプログラムまたはサービスが必要です。

手元のファイルを入力にする場合はlocal-files、日本語Wikipediaの取得から始める場合はWikipedia、すでに作成した索引を
画面から利用する場合はsearch-webを選びます。local-filesとWikipediaの設定は、そのままsearch-webの起動にも使えます。

外部のembedding APIを使わずに最初の検索を試す場合は、ルートREADMEの語彙検索から始めてください。
サンプルで問題が発生した場合は[問題解決](troubleshooting.md)を参照してください。
