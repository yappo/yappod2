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

## PR運用ルール（必須）
- 基本は **1タスク = 1ブランチ = 1PR**。ブランチ名は `codex/<topic>` を使います。
- PRは「1つの挙動変更 + 関連テスト」に絞り、レビューが 10〜20 分で終わる粒度を目安にします。
- 目安として、変更ファイルは 3〜8、差分は 300 行前後までに抑えます（超える場合は分割）。
- `build` と `ctest` が通った時点で早めにPRを作成し、変更を抱え込まない運用にします。
- リファクタと仕様変更は同じPRに混ぜません。必要なら「基盤PR → 挙動PR」の順で分けます。

## Configuration & Data Notes
- 入力フォーマットは 1 行 1 ドキュメントのタブ区切り: `URL\tCOMMAND\tTITLE\tBODY_SIZE\tBODY`。
- インデックス用ディレクトリには `pos/` サブディレクトリが必要です。
