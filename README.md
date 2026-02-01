# Yappod2

このパッケージは[Yappod](https://github.com/yappo/yappod)を今現在の環境でも動作させることを目的としたプロジェクトです。

# 以下は Yappod の readme(いつか書き直す)

一部のスコアリングに関するルーチンは省かれています。

本当に適当なドキュメントですが嘘は書いてないです。

また、プログラム自体にも様々なおかしなところが有ります。

-------------------------------------------------------------------------------
インデックスの作り方

1.インデックスを作りたい文書を集めます。

2.文書をインデクサが読み込める型式にコンバートします。

フォーマットは一行につき、1ドキュメント入り、各項目はタブで区切られます。
漢字コードは予めEUCに変換しておいて下さい。
項目は左のレコードから順に下記の通りになります。

URL\tCOMMAND\tTITLE\tBODY_SIZE\tBODY

URL       = 文書のURL
COMMAND   = インデクサヘの指示
TITLE     = 文書のタイトル
BODY_SIZE = 文書のサイズ
BODY      = 文書

COMMANDにはADDとDELETEが入ります。
ADDは文書をインデックスに追加する。
DELETEはURLをインデックスから削除します。
BODYには改行やタブを含めてはいけません。

sample.gz というサンプルを置いたので参考にしてください。

3.インデックス格納用のディレクトリを作成します。
  好きな場所にインデックス格納場所用のディレクトリを作成して下さい。
  例えば /tmp/yappoindex とか
  作成したディレクトリの中には、位置情報ファイルを格納するためのディレクトリposも作成してください。
  ※pos が無い場合、yappo_makeindex はエラーで終了します。

  $ mkdir /tmp/yappoindex
  $ mkdir /tmp/yappoindex/pos

4.インデックスコマンドを走らせる。
  $ yappo_makeindex -f (2で作ったファイル) -d (3で作ったディレクトリ)
  上記の様なコマンドでインデックスが作成されます。
  今回の例だと $ yappo_makeindex -f sample.gz -d /tmp/yappoindex などです。
  
-------------------------------------------------------------------------------
ビルド（CMake / macOS向け）

前提:
- Command Line Tools
- Homebrew で berkeley-db / zlib / cmake を導入済み

設定とビルド:
  $ cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBDB_INCLUDE_DIR="$(brew --prefix berkeley-db)/include" \
      -DBDB_LIB="$(brew --prefix berkeley-db)/lib/libdb.dylib"
  $ cmake --build build -j

インストール（デフォルト先へ）:
  $ cmake --install build

インストール先を指定する場合:
  $ cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$HOME/yappod" \
      -DBDB_INCLUDE_DIR="$(brew --prefix berkeley-db)/include" \
      -DBDB_LIB="$(brew --prefix berkeley-db)/lib/libdb.dylib"
  $ cmake --build build -j
  $ cmake --install build

Ninja でビルドする場合:
  - 上記の CMake 設定コマンドに `-G Ninja` を追加してください。
    例: `cmake -S . -B build -G Ninja ...`
  - ビルドは同じく `cmake --build build`
  - 事前に `brew install ninja` が必要です。

生成物:
  build/search
  build/yappo_makeindex
  build/yappo_margepos
  build/yappod_core
  build/yappod_front

備考:
- インデックス用ディレクトリに pos/ が無い場合、yappo_makeindex はエラー終了します。
- Homebrew を使わない場合は BDB_INCLUDE_DIR / BDB_LIB を手動指定してください。


-------------------------------------------------------------------------------
検索のしかた

コマンドラインで検索が出来ます。
出力は適当です。

$ search -l (インデックス格納ディレクトリ) (キーワード)

例/ $ search -l /tmp/yappoindex キーワード

-------------------------------------------------------------------------------
検索デーモンをあげる

$ yappod_core -l /tmp/yappoindex
$ yappod_front -l /tmp/yappoindex -s localhost

検索デーモンにクエリーを投げる方法は

http://localhost:10080/yappo/100000/AND/0-10?キーワード

これで100000バイト未満の文書の中から「キーワード」をAND検索で1件目から10件までの検索結果を取得出来ます。

結果のフォーマットは下記の通り
==========RESULT============
キーワードに適合した文書数
結果行数

URL\tタイトル\t文書サイズ\tインデックスの作り方の2のファイルを作った時間\tスコア
============================
-------------------------------------------------------------------------------

大体の使用方法はこんな感じです。

追加説明などは http://tech.yappo.ne.jp/ にて公開されるかもしれません。

ちなみに現在は各種効率をあげた1.1系の開発を行なっています。
また、将来的にはまったく別の実装を作成する予定も有ります。
