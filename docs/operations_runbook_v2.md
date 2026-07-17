# v2 daemon 運用runbook

このrunbookは単一ノードの `yappod_core` + `yappod_front` と、v2 immutable indexを
対象にします。分散sharding、replication、自動failoverは対象外です。

## 起動と停止

coreを先に起動し、frontを後から起動します。両processへ同じapplication TOMLを渡します。

```toml
# /srv/yappod/application.toml
[daemon]
max_inflight = 16
max_inflight_bytes = 16777216
request_timeout_ms = 5000
write_token = "replace-with-at-least-16-bytes"
```

```sh
yappod_core --config /srv/yappod/config.toml
yappod_front --config /srv/yappod/config.toml
```

停止時はfrontをロードバランサから外し、`/health/ready`への新規probeを止めてからfront、coreの
順にSIGTERMを送ります。daemonは新規acceptを止め、pid fileを削除して終了します。

## Health endpoint

| Endpoint | 成功条件 | 用途 |
|---|---|---|
| `GET /health/live` | front processがHTTP応答可能 | process再起動判定 |
| `GET /health/ready` | frontが全coreへhealth frameを送受信でき、起動時に読込済みのsnapshotが利用可能 | traffic投入判定 |

ready responseにはmanifest `generation`、segment数、embedding状態、compaction状態を含みます。
probeごとに全component checksumを再計算しません。全検証は
`yappo_makeindex verify --config /srv/yappod/config.toml`を監視またはcronから実行します。
index破損やcore不通時もlivenessは`200`のまま、readinessだけが`503`になります。ロードバランサは
`/health/ready`、process supervisorは`/health/live`を使用してください。

vector無効時のembedding状態は`disabled`、vector有効時は`precomputed_ready`です。現在のonline
query APIはcallerがvectorを渡すため、daemonが外部embedding providerへ暗黙に通信することは
ありません。

## Prometheus metrics

`GET /metrics`はPrometheus text exposition formatを返します。label値は実装内の固定集合で、
query、document ID、token、model IDなどの高cardinality情報を含みません。

主要metric:

- `yappod_v2_requests_total{operation,status_class}`: search/retrieve/ingestの完了件数
- `yappod_v2_request_duration_seconds`: front受付からcore応答までの固定bucket histogram
- `yappod_v2_ready`: 直近scrape時のreadiness
- `yappod_v2_manifest_generation`: 検証済みmanifest generation
- `yappod_v2_inflight_requests` / `yappod_v2_inflight_request_bytes`: 現在のadmission使用量
- `yappod_v2_inflight_request_limit` / `yappod_v2_inflight_byte_limit`: 起動時limit
- `yappod_v2_embedding_configured`: vector/embedding構成の有無
- `yappod_v2_compaction_state{state}` / `yappod_v2_compaction_generation`: compactionの状態と最終世代

latencyはHTTPのend-to-end時間です。socket deadlineで応答を打ち切ったrequestは5xxとして記録されます。
実行中CPU queryを強制停止する時間ではありません。

推奨する初期alert:

- `yappod_v2_ready == 0` が1分継続
- 5分間の5xx率が1%超
- in-flight countまたはbytesがlimitの80%超で5分継続
- `compaction_state` が`failed`または`interrupted`
- generationが予定した更新/compaction後も増えない

latency閾値は[品質・性能・信頼性ゲート](quality_performance_reliability_v2.md)の基準 corpus と
hardware で採取した release benchmark に従って固定します。

## 更新とcompaction

更新成功responseのgenerationを保存し、次の`/health/ready`または検索responseで同じか新しいgenerationを
確認します。compactionはwriter lockでupdateと直列化されます。

```sh
yappo_compact --config /srv/yappod/config.toml
curl -fsS http://127.0.0.1:18400/health/ready
curl -fsS http://127.0.0.1:18400/metrics | grep yappod_v2_compaction
```

`compaction.state`はatomicに更新されます。正常終了は`succeeded`、通常失敗は`failed`、process crash後に
残ったrunning PIDが存在しなければprobeは`interrupted`と報告します。compaction失敗だけでは現在の
manifestを無効にしないため、readinessはindex snapshotが完全なら`200`を維持します。

## 障害切り分け

1. `/health/live`が失敗: front process、listen port、OS resourceを確認する。
2. live成功・ready失敗: core process/port、core error log、`config.toml`、`manifest.json`、component checksumを確認する。
3. `overloaded` 503: in-flight count/bytesとrequest latencyを確認し、slow queryまたはlimit設定を調査する。
4. `core_unavailable` 503: front-core socket deadline、core crash、index検証失敗を確認する。
5. compaction `failed/interrupted`: manifest generationが検索可能なことを確認してから再実行する。次回実行がorphanをGCする。
6. embedding `disabled`: vector modeを要求せず、必要なら元文書からvector有効configで再索引する。

証跡としてrelease commit、config hash、manifest generation、HTTP status、latency、front/core log、
compaction state、metrics snapshotを同じincident IDへ保存してください。
