# Yappod2サーバーの運用

この文書では、`yappod_core`と`yappod_front`の起動、停止、監視、更新、障害調査を説明します。設定値の一覧は[設定リファレンス](configuration.md)、HTTP APIは[`yappod_front` APIリファレンス](yappod-front-api.md)、全メトリクスは[監視とメトリクス](observability.md)を参照してください。

## coreとfrontの役割

`yappod_core`は検証済みの索引スナップショットを保持し、検索、RAG向け取得、文書更新を実行します。`yappod_front`はHTTPを受け付け、検索と更新をcoreへ転送します。`POST /v2/passages:prepare`だけはfrontのプロセス内で実行します。

外部クライアントからcoreのポートを直接公開しないでください。coreとfrontの通信は内部HTTP/1.1ですが、
認証やTLSを備えた外部公開APIではありません。frontはcoreへのクライアントとしてlibcurlを使い、検索と取得を
`QUERY`、更新を`POST`で送ります。必要に応じてfrontの手前へTLS終端とアクセス制御を配置します。

## 起動前の確認

```sh
./build/yappo_makeindex verify --config /srv/yappod/application.toml
```

次を確認します。

- `[index].directory`が意図した索引を指していることを確認します。
- `config.toml`、`manifest.yap2`、マニフェストが参照する各セグメントを実行ユーザーが読み取れることを確認します。
- `[daemon].run_directory`を実行ユーザーが作成、書き込みできることを確認します。
- `front_port`と`core_port`が別のプロセスに使われていないことを確認します。
- 外部から文書更新を許可する場合は、16〜255バイトの`write_token`を安全に配布します。アプリケーション用TOMLの読み取り権限も制限します。

## 起動

coreを先に起動し、準備完了を確認してからfrontを起動します。

```sh
./build/yappod_core --config /srv/yappod/application.toml
./build/yappod_front --config /srv/yappod/application.toml
```

この起動方法では、両コマンドは起動処理の途中で`fork`し、親プロセスが終了した後も子プロセスがバックグラウンドで動作します。索引の初期読込と待ち受けソケットの作成は`fork`前に行うため、親プロセスの終了状態0は、そこまでの処理と`fork`に成功したことを表します。子プロセス側で行うPID・ログファイルの作成、シグナル処理、ワーカースレッドの作成までは保証しません。起動直後はPID、エラーログ、ヘルスチェックを確認してください。

アプリケーション用TOMLを使う場合、`run_directory`には次のファイルを作ります。

| ファイル | 内容 |
|---|---|
| `core.pid`、`front.pid` | バックグラウンドで動作する子プロセスのIDです。通常終了時に削除します。 |
| `core.log`、`front.log` | 標準出力を追記します。 |
| `core.error`、`front.error` | 標準エラー出力を追記します。起動失敗と実行時エラーを最初に確認します。 |

`--index`を直接指定した場合は、実行時のカレントディレクトリへ同じ名前のPIDとログを作ります。運用では保存場所を明確にするため、アプリケーション用TOMLの使用を推奨します。

### フォアグラウンドで起動する

プロセスの生存監視と標準出力・標準エラーの収集を呼び出し元が担当する場合は、両コマンドへ
`--foreground`を指定します。

```sh
./build/yappod_core --foreground --config /srv/yappod/application.toml
./build/yappod_front --foreground --config /srv/yappod/application.toml
```

フォアグラウンド実行ではforkせず、PIDファイルとログファイルを作りません。起動後のプロセスが
`yappod_core`または`yappod_front`そのものであり、標準出力と標準エラーは呼び出し元から継承します。
終了には`SIGTERM`または`SIGINT`を送ります。coreを先に起動して準備し、frontを起動する順序は
バックグラウンド実行と同じです。

systemdで両プロセスを監視するunitファイルと配置手順は、
[systemd向け起動例](../examples/systemd/README.md)にあります。

## 起動確認

```sh
curl -fsS http://127.0.0.1:18400/health/live
curl -fsS http://127.0.0.1:18400/health/ready
```

`/health/live`はfrontがHTTP要求を処理できることを示します。索引やcoreの状態までは確認しません。`/health/ready`はfrontからcoreへ接続でき、coreが検証済みスナップショットを保持し、ディスク上の索引を運用可能と判断した場合に200を返します。

準備完了レスポンスの`generation`が`yappo_makeindex verify`で表示した`manifest.yap2`の世代と
一致することも確認してください。応答JSONの全フィールドは
[HTTP APIの準備完了確認](yappod-front-api.md#get-healthready)で説明しています。

## 停止

専用の停止コマンドはありません。PIDファイルを読み、対象プロセスが本当に`yappod_core`または`yappod_front`か確認してから`SIGTERM`を送ります。

```sh
ps -p "$(cat /srv/yappod/run/front.pid)" -o pid=,command=
kill -TERM "$(cat /srv/yappod/run/front.pid)"
ps -p "$(cat /srv/yappod/run/core.pid)" -o pid=,command=
kill -TERM "$(cat /srv/yappod/run/core.pid)"
```

通常は新しいHTTP要求の受け付けを止めるためfrontを先に停止し、その後coreを停止します。`SIGTERM`と`SIGINT`は待ち受けソケットを閉じ、ワーカースレッドと再読み込みスレッドの終了を待ちます。正常終了時はPIDファイルを削除します。

PIDファイルが残っていても、PIDが別のプロセスへ再利用されている可能性があります。確認せずに`kill`しないでください。該当するYappod2サーバーが存在しないことを確認できた場合だけ、残ったPIDファイルを退避または削除します。

## ログの扱い

Yappod2サーバーはログファイルを追記モードで開きます。ファイル名を変更するローテーションを行う場合、プロセスは開いたままの旧ファイルへ書き続けます。現行実装にログ再オープン用のシグナルはありません。確実に切り替えるには、frontとcoreを順に停止し、ログをローテーションしてから起動します。

調査では次を同じ時刻で記録します。

- `core.error`と`front.error`の該当範囲
- アプリケーション用TOMLの秘密情報を除いた設定値
- `/health/ready`と`/metrics`の応答
- `yappo_makeindex verify`で表示した`manifest.yap2`の世代番号
- クライアントが受け取ったHTTP状態コードと`error.code`

## 同時処理とタイムアウト

frontは`[daemon].front_io_threads`本、coreは`core_io_threads`本の接続I/Oスレッドを作ります。coreは
接続I/Oとは別に、`core_search_threads`本の検索compute workerと単一writer threadを作ります。
I/Oスレッドと検索compute workerの既定値はそれぞれ16です。接続を受け付けたI/Oスレッドは検索計算を
上限付きexecutorへ渡し、完了までその接続を保持します。`[daemon]`の関連する設定は次の意味です。

| キー | 制限する対象 |
|---|---|
| `front_io_threads` | frontが作成する接続I/Oスレッド数です。 |
| `core_io_threads` | coreが作成する接続I/Oスレッド数です。 |
| `core_search_threads` | coreが作成する検索compute worker数です。 |
| `core_writer_queue_capacity` | coreのwriter処理中とは別に待機できる更新数です。 |
| `max_inflight` | 同時に受理する検索、取得、本文断片準備の件数です。 |
| `max_inflight_bytes` | 処理中の検索、取得、本文断片準備の本文合計バイト数です。 |
| `request_timeout_ms` | 検索、取得、本文断片準備に適用するソケットと内部HTTPの期限です。 |
| `ingest_max_body_bytes` | 文書更新1件の本文上限です。デフォルト64 MiB、最大256 MiBです。 |
| `ingest_timeout_ms` | 文書更新に適用するソケットと内部HTTPの期限です。デフォルト60000ミリ秒です。 |

実際に同時処理できる通常要求数は、frontとcoreのI/Oスレッド数、coreの検索compute worker数、
`max_inflight`のうち最も小さい値を超えません。
`max_inflight`または`max_inflight_bytes`を超えた処理は`503 overloaded`になります。
文書更新は検索executorと別のwriter executorを使うため、更新待ちが検索用の処理枠を占有しません。
frontは現在2件目の更新を`503 overloaded`として拒否します。coreへ直接接続した場合は処理中の1件とは
別に`core_writer_queue_capacity`件まで待機できます。要求本文1件の上限は、検索、取得、
本文断片準備では1 MiB、文書更新では`ingest_max_body_bytes`です。
coreからfrontが受け取る内部HTTP応答本文は16 MiBを上限とします。

タイムアウト値を増やす前に、coreへの接続、索引の大きさ、同時実行数、クライアント切断、ディスクI/Oを確認します。search-webの`yappod_timeout_ms`、起動待ちの`startup_timeout_ms`、LLMや埋め込みのタイムアウトは別の待ち時間です。

## 索引のオンライン更新

`yappo_makeindex update`または`POST /v2/documents:batch`が新しい世代を公開すると、coreは最大約1秒後の定期確認で読み替えます。HTTP更新の場合は更新直後にも再読み込みを試みます。更新成功レスポンスの世代と`/health/ready`または`/metrics`の世代を比較すると、検索側への反映を確認できます。

再読み込み前に開始した検索は旧世代を使い続けます。これは異常ではありません。新しい世代を読み込めない場合、coreは旧スナップショットを維持し、HTTP更新では`503 reload_failed`を返す場合があります。この応答は「更新が公開されていない」という意味ではないため、マニフェストと`verify`を確認してください。

再読み込みは、セグメントの追記だけでなく、コンパクションによる削除、置換、並べ替えにも対応します。新旧マニフェストをセグメントIDの索引で照合し、descriptorが完全に一致するセグメントの検索用ハンドルだけを再利用します。新しいセグメントだけを開き、旧世代だけに残るセグメントは切り替え成功後に閉じるため、照合回数はセグメント数に比例します。

## コンパクションの運用

コンパクションは`compaction.lock`で別のコンパクションと直列化し、対象範囲の確定時と公開時だけ更新と同じ`writer.lock`を使います。セグメント構築中も更新は実行できますが、CPU、メモリー、ディスク入出力は共有します。実行前に空き容量を確認してください。新しいセグメントを作ってから旧セグメントを回収するため、処理中は選択範囲と再構築分の両方を置ける容量が必要です。

coreは既定で30秒ごとにマニフェストを確認します。全コンポーネントファイルの合計が64 MiB未満の
小セグメントが隣接して4個以上になると、検索処理や毎秒のマニフェスト再読み込みとは別の保守スレッドで
1回コンパクションを実行します。正常な大きさのセグメントだけなら世代を増やしません。次回確認
でも小セグメントが閾値以上残っていれば、最大8個ずつ段階的に処理します。判定値は
`[daemon].auto_compact_*`で変更でき、計画保守中は`auto_compact_enabled = false`で停止できます。

```sh
./build/yappo_compact --config /srv/yappod/application.toml
```

`/metrics`の`yappod_v2_compaction_state`と`yappod_v2_compaction_generation`で状態を確認できます。`interrupted`や`failed`の場合は、索引とログを保存してから`verify`を実行します。詳細は[索引の更新と保守](index-lifecycle.md)を参照してください。

断片化の確認には、世代数ではなく`yappod_v2_manifest_segments`、
`yappod_v2_small_segment_run`、`yappod_v2_auto_compaction_needed`を使います。
`auto_compaction_needed`が1のまま次の確認間隔を複数回過ぎた場合は、`compaction_state`、coreの
エラーログ、空き容量を確認します。文書記録数には古い版が含まれるため、利用者から見える文書数と
同じ値として扱わないでください。

## 更新APIの認証

`[daemon].write_token`を設定すると、`POST /v2/documents:batch`に`Authorization: Bearer <token>`が必要です。トークンは16〜255バイトで、空白を含められません。検索、取得、本文断片準備、ヘルスチェック、メトリクスにはこのトークンを要求しません。

現行のfrontはTLSを提供しません。ループバックアドレス以外から更新APIへ接続させる場合は、TLSを提供するリバースプロキシーの背後へ配置し、メトリクスや検索APIを含む公開範囲を別途制御してください。

## バックアップと索引の切り替え

運用中の索引を別構成へ変える場合は、同じディレクトリを上書きせず、別ディレクトリへ`build`します。`verify`と代表検索を終えた後、アプリケーション用TOMLの`[index].directory`を変更し、front、coreの順に停止してcore、frontの順に起動します。

索引バックアップは更新がない時点のディレクトリ全体を取得します。ファイル単位のコピーで世代を混在させないでください。バックアップと復元の条件は[索引の更新と保守](index-lifecycle.md#バックアップと復元)で説明しています。

## 症状別の確認

| 症状 | 確認するもの | 次の操作 |
|---|---|---|
| 起動コマンドは成功したが接続できない | `.error`、PIDが存在するか、待ち受けポート | 設定とポート競合を直して再起動します。 |
| `/health/live`は200、`/health/ready`は503 | coreのPIDと`core.error`、索引、core host/port | `verify`を実行し、frontからcoreへ接続できるか確認します。 |
| 更新後も古い結果が出る | 更新応答、マニフェスト、準備完了応答の各世代番号 | 約1秒待って再確認し、不一致が続けばcoreの再読み込みエラーを調べます。 |
| `503 overloaded` | inflight件数、本文バイト数、処理時間 | 呼び出し側の並行数を下げ、必要なら実測に基づいて上限を調整します。 |
| `503 core_unavailable` | coreのPID、ポート、`request_timeout_ms`、両ログ | coreを検証し、接続先と処理期限を確認します。 |
| `401 unauthorized` | `write_token`とBearerヘッダー | 同じトークンを安全に設定します。検索APIには付ける必要がありません。 |
| コンパクションが`interrupted` | `compaction.state`のPID、ログ、マニフェスト | 変更せず保存し、`verify`後に再実行の可否を判断します。 |

## 監視で最低限見る項目

- `/health/ready`が継続して200かを確認します。
- `yappod_v2_requests_total`の5xx増加を操作別に確認します。
- `yappod_v2_request_duration_seconds`の分位を操作別に確認します。
- `yappod_v2_inflight_requests`と上限の接近を確認します。
- `yappod_v2_manifest_generation`が更新後に進むかを確認します。
- `yappod_v2_compaction_state`が`running`のまま残っていないかを確認します。
- `yappod_v2_auto_compaction_needed`が1のまま継続していないかを確認します。
- `yappod_v2_manifest_segments`と`yappod_v2_small_segment_run`が保守後に減っているか確認します。

Prometheusによる収集設定、全メトリクス、PromQL、アラート例は[監視とメトリクス](observability.md)に掲載しています。
