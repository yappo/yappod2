# Yappod2共通作業規則

このファイルはリポジトリ全体の承認、Git、PR、CIに関する規則だけを定めます。実装責務は
[`docs/architecture.md`](docs/architecture.md)、タスク票の運用は
[`docs/task-workflow.md`](docs/task-workflow.md)、対象別の規則は各ディレクトリの
`AGENTS.md`を参照してください。

## タスクの承認

1. 着手前に「今回着手する1件」と「以降の残タスク（最大10件）」を提示し、ユーザーの合意を得ます。
2. 調査で判明できる事実は先に調べ、ユーザーには製品判断や互換性判断だけを質問します。
3. 緊急依頼は現在のタスクへ割り込ませ、元のタスクは一時停止として扱います。
4. 通常は1タスクのmerge後に次候補を提示して判断を待ちます。ただし、ユーザーが複数タスクの内容と順序を明示的に一括承認し、連続実行を指示した場合は、承認済みの全タスクが終わるまで各merge後に停止せず次へ進みます。
5. 追加権限、利用者向け契約の変更、承認済み範囲を越える製品判断が必要になった場合は、連続実行中でも停止して確認します。

## ブランチ、コミット、PR

- 合意前にブランチを作りません。変更を伴うタスクは原則として
  **1タスク = 1ブランチ = 1PR**とし、ブランチ名は`codex/<topic>`にします。調査だけで変更を
  commitしない場合はブランチを作りません。
- コミットは`type: summary`形式の短い英語命令形にします。`type`は
  `fix|refactor|test|docs|chore`のいずれかです。
- PRは`gh pr create --base main --head <branch> --title "<title>" --body-file <file>`で作成し、
  `.github/pull_request_template.md`の見出しを使って、実施結果と検証結果を日本語で記録します。
- 複数PRを束ねるタスクでは追跡Issueを作り、各PRから関連付けます。

## 検証とmerge

- 変更完了時の最低限の確認は`cmake --build build -j`、
  `ctest --test-dir build --output-on-failure`、`git diff --check`です。変更領域に固有の確認は
  対象ディレクトリの`AGENTS.md`と[`docs/development.md`](docs/development.md)に従います。
- PR作成後は`gh pr checks <number>`を20秒間隔で確認します。失敗時は
  `gh run view <run-id> --log-failed`で原因を調べ、同じブランチで修正します。
- 必須ジョブがすべて成功してから
  `gh pr merge <number> --merge --delete-branch`でmergeします。
- 仕様、既定値、プロトコル、コマンドについては現在のソース、テスト、正式文書を一次資料として
  照合します。確認できなかった事項は推測せず、不明であることを明記します。
