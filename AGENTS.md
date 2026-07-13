# Repository Guidelines

## Project Structure & Module Organization
- `src/` にコアの C ソースとヘッダーがあります（`yappod_core`, `yappod_front`, `search`, `yappo_makeindex` など）。
- `build/` はビルド成果物置き場です（再生成可能）。
- `examples/` に v2 config と canonical NDJSON の実行可能なサンプルがあります。
- ルートの `README`/`INSTALL`/`NEWS` に使い方や変更履歴があります。

## Build, Test, and Development Commands
- `cmake -S . -B build` でビルド設定を生成します。
- `cmake --build build -j` でビルドします。
- `cmake --install build` でインストールします。
- `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/yappod` でインストール先を指定できます。

README の例:
- インデックス作成: `yappo_makeindex build --config examples/config.toml --input examples/documents.ndjson --index /tmp/yappoindex`
- 検索: `search --index /tmp/yappoindex --mode lexical --query キーワード`
- デーモン起動: `yappod_core --index /tmp/yappoindex` と `yappod_front --index /tmp/yappoindex --core-host 127.0.0.1`

## Coding Style & Naming Conventions
- 言語: C（`src/*.c` では K&R 風の中括弧と 2 スペースインデントが見られます）。
- 関数名は `YAP_` プレフィックス（例: `YAP_Index_get_domainindex`）。
- ファイル名は `yappo_*.c` / `yappo_*.h` でモジュールを分けています。
- 宣言は対応する `*.h` にまとめてください。

## Testing Guidelines
- テスト実行は `ctest --test-dir build --output-on-failure` を基準手順とします。
- 必要に応じて `examples/` の小規模データでインデックス作成 → 全検索 mode の動作確認も行ってください。

## Commit & Pull Request Guidelines
- コミットは短く動詞ベースの英語にします。
- PR には概要、検証手順（コマンドと入力例）、互換性や挙動変更のメモを含めてください。

## タスク開発フロー（必須）
1. 優先順位は「AIが必要タスクを整理し順番に処理」が基本です。**ユーザから緊急依頼が来た場合は割り込みを最優先**し、元タスクは一時停止として扱います。
2. タスク開始前に、必ず「今回着手タスク1件」と「以降の残タスク（最大10件）」を提示し、ユーザ合意後に着手します。
   - 今後必要になるタスクを全て先読みで洗い出して、全て提示してください。タスク完了後に後出しで追加タスクを提示しないよう努力してください。
3. 合意前にブランチは作りません。
   - git commit が必要なタスクの場合は、着手確定後に **1タスク = 1ブランチ = 1PR**（`codex/<topic>`）で進めます（例: `git checkout -b codex/fix-mergepos-missing-d`）。
   - タスクの内容が調査のみでコードやファイルの変更や git commit が不要なタスクだと判断したら、ブランチ作成は不要です
4. タスクの内容を実装し、実装後のローカル確認は最低限以下を実行します。
   - `cmake --build build -j`
   - `ctest --test-dir build --output-on-failure`
   - 必要に応じて不具合再現/修正確認コマンド（CLI/HTTPなど）を実行して正常に動作することを確認する
5. コミットメッセージは `type: summary` 形式（英語・命令形）を必須とします。`type` は `fix|refactor|test|docs|chore`。例: `fix: validate required -d option in mergepos`
6. PRは `gh` コマンドで作成し、本文は次のテンプレートを必須とします。
   - `gh pr create --base main --head <branch> --title \"<title>\" --body-file <file>`
   - `## 背景`
   - `## 変更内容`
   - `## 検証手順`
   - `## 影響範囲`
7. PR作成後は GitHub Actions の CI を確認し、**必須ジョブが全て成功してから** merge します。失敗していたら gh コマンドで CI のログを確認し原因を調査しコミットを再度行います (`gh run view <run-id> --log-failed`)
   - `gh pr checks <pr-number>`
     - for loop で sleep 20 をしながらポーリングで確認すること
   - `gh pr merge <pr-number> --merge --delete-branch`
8. タスク完了後は次タスクへ自動遷移せず、再度「次タスク候補 + 残タスク（最大10件）」を提示してユーザ判断を待ちます。

### PR本文テンプレート（コピペ用）
```md
## 背景
- 

## 変更内容
- 

## 検証手順
- `cmake --build build -j`
- `ctest --test-dir build --output-on-failure`
- 

## 影響範囲
- 

```

## Configuration & Data Notes
- 正式入力は 1 行 1 operation の canonical NDJSON です。TSV は `prepare --input-format tsv` adapter で変換します。
- index の正式 layout は `config.toml`、`manifest.json`、`segments/<segment-id>/` です。
