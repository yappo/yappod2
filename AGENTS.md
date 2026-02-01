# Repository Guidelines

## Project Structure & Module Organization
- `src/` にコアの C ソースとヘッダーがあります（`yappod_core`, `yappod_front`, `search`, `yappo_makeindex` など）。
- `build/` はビルド成果物置き場です（再生成可能）。
- `sample.gz` はインデックス入力のサンプルです。
- ルートの `README`/`INSTALL`/`NEWS` に使い方や変更履歴があります。

## Build, Test, and Development Commands
- `./configure` で環境に合わせた Makefile を生成します。
- `make` でビルドします。
- `make install` はデフォルトで `/usr/local/yappod` にインストールします。
- `./configure --prefix=$HOME/yappod` でホーム配下にインストールできます。

README の例:
- インデックス作成: `yappo_makeindex -f sample.gz -d /tmp/yappoindex`
- 検索: `search -l /tmp/yappoindex キーワード`
- デーモン起動: `yappod_core -l /tmp/yappoindex` と `yappod_front -l /tmp/yappoindex -s localhost`

## Coding Style & Naming Conventions
- 言語: C（`src/*.c` では K&R 風の中括弧と 2 スペースインデントが見られます）。
- 関数名は `YAP_` プレフィックス（例: `YAP_Index_get_domainindex`）。
- ファイル名は `yappo_*.c` / `yappo_*.h` でモジュールを分けています。
- 宣言は対応する `*.h` にまとめてください。

## Testing Guidelines
- 自動テスト基盤はありません。`sample.gz` などの小規模データでインデックス作成 → 検索を通して動作確認してください。

## Commit & Pull Request Guidelines
- 既存のコミットは短く動詞ベースの英語（例: “support modern berkeley db”）。同じスタイルを推奨します。
- PR には概要、検証手順（コマンドと入力例）、互換性や挙動変更のメモを含めてください。

## Configuration & Data Notes
- 入力フォーマットは 1 行 1 ドキュメントのタブ区切り: `URL\tCOMMAND\tTITLE\tBODY_SIZE\tBODY`。
- インデックス用ディレクトリには `pos/` サブディレクトリが必要です。
