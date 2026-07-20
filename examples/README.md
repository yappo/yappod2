# サンプルの選び方

`examples/`には、Yappod2のコマンドを小さな固定データで試す設定と、実際のデータを取り込む3つのサンプルがあります。この文書では入力、成果物、必要な外部サービスを比較します。

## まず検索コマンドを試す

外部サービスやPython、Node.jsを使わず、語句による検索だけを確認する場合は、リポジトリのルートで次を実行します。

```sh
cmake --build build -j
./build/yappo_makeindex build \
  --config examples/config.lexical.toml \
  --input examples/documents.lexical.ndjson
./build/search \
  --config examples/config.lexical.toml \
  --mode lexical \
  --query search
```

`config.lexical.toml`は語彙検索だけを使う設定で、`examples/index-lexical`へ索引を作ります。出力先がすでにある場合は上書きしないため、別の出力先を使うときは設定の`index.directory`を変更してください。`config.toml`と`documents.ndjson`は3次元の固定ベクトルを含み、ベクトル検索と複合検索のコマンド確認に使います。これらの小さなデータは機能確認用であり、検索品質や性能の評価用ではありません。

## 3つのサンプル

| サンプル | 入力 | 主な処理 | 成果物 | 外部サービス |
|---|---|---|---|---|
| [local-files](local-files/README.md) | 手元のディレクトリにあるテキスト、ソース、PDF、Office文書など | 収集、本文抽出、整形、分割、埋め込み、索引作成 | 分割NDJSON、マニフェスト、再開位置、埋め込み、索引 | ベクトルを使う場合は埋め込みAPI、形式によってTikaや拡張処理が必要です。 |
| [Wikipedia](wikipedia-search/README.md) | Wikimedia Action APIまたは日本語Wikipediaダンプ | 取得、チェックサム確認、WikiExtractor出力の変換、埋め込み、索引作成 | 正式なNDJSON、パッセージ、埋め込み付きNDJSON、索引 | データ取得にはネットワーク、ベクトルを使う場合は埋め込みAPIが必要です。 |
| [search-web](search-web/README.md) | 作成済みのYappod2索引 | core、front、BFF、Web UIの起動、検索、RAG向け取得、文書登録 | PID、ログ、Web用索引作成成果物、任意のAPI利用量ログ | ベクトル検索には埋め込みAPI、回答生成にはLLMが必要です。 |

### local-filesを選ぶ場合

自分の文書を検索したい場合に使います。入力元のファイルは変更せず、設定した出力ディレクトリへ再生成可能な成果物を作ります。抽出失敗をファイル単位で記録し、大きな入力を複数のNDJSONへ分け、途中から再開するための情報も保存します。

### Wikipediaを選ぶ場合

データ取得から検索までの流れを公開データで確認したい場合に使います。少量の記事はWikimedia Action API、大量の記事はダンプを利用します。Wikipedia固有なのはデータの取得と変換であり、Yappod2の索引設定、検索API、Web UIは他のデータと共通です。

### search-webを選ぶ場合

すでに有効な索引があり、ブラウザーで検索、質問、文書登録を行いたい場合に使います。local-filesとWikipediaのアプリケーション用TOMLをそのまま渡せます。search-webは索引作成プログラムではありませんが、設定に`[build]`があれば、索引が存在しないときだけ作成できます。

## 共通する必要環境

最初にリポジトリのルートでCプログラムをビルドします。

```sh
cmake -S . -B build
cmake --build build -j
```

- local-filesとWikipediaはPython 3.9以上を使用します。
- search-webはNode.js 22以上とnpmを使用します。
- PDFやOffice文書の抽出、Wikipediaダンプの変換、埋め込み、LLMには各READMEに記載した追加環境が必要です。
- 外部APIの認証情報はTOMLへ直接書かず、`authorization_token_env`で指定した環境変数から読みます。

## パスと実行場所

各アプリケーション用TOML内の相対パスは、そのTOMLが置かれたディレクトリを基準に解決します。READMEのコマンドは、`cd`を明記した箇所以外はリポジトリのルートから実行します。別のカレントディレクトリから実行してもTOML内の相対パスの意味は変わりませんが、コマンドラインへ渡す相対パスはシェルのカレントディレクトリ基準です。

## 成果物を混ぜない

文書NDJSON、パッセージ、埋め込み、索引は、同じ入力と設定から生成した組み合わせを使います。別の実行結果や異なる`model_id`、`dimensions`の成果物を一つのディレクトリへ混ぜないでください。語彙索引のアプリケーション用TOMLへ`[vector].enabled = true`を追加しても、既存索引はベクトル対応になりません。別ディレクトリへ作り直します。

エラーの共通形式、PID、ログ、安全な再生成は[サンプルの問題解決](troubleshooting.md)を参照してください。アプリケーション用TOMLの全キーは[設定リファレンス](../docs/configuration.md)にあります。
