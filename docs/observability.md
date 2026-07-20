# 監視とメトリクス

`yappod_front`は、稼働確認、検索可能性の確認、Prometheusテキスト形式のメトリクスをHTTPで提供します。この文書では、各エンドポイントと全メトリクスの意味、収集例、障害時の読み方を説明します。

## 監視エンドポイント

| エンドポイント | 正常時 | 確認する範囲 | 主な用途 |
|---|---:|---|---|
| `GET /health/live` | `200` | frontプロセスがHTTPを受けられること | プロセスが生存しているかの確認 |
| `GET /health/ready` | `200`、利用不能時`503` | frontが索引を読め、coreのヘルスチェック用フレームが成功すること | リクエストを受ける前の準備完了確認 |
| `GET /metrics` | `200` | front内のカウンター、処理時間、処理中件数、索引状態 | Prometheusによる収集と傾向監視 |

これらのGETリクエストへ`Content-Length`を付けると`400 invalid_request`になります。認証はありません。外部へ公開する場合は、ネットワークまたはリバースプロキシーでアクセスを制限してください。

### 生存確認

```sh
curl -fsS http://127.0.0.1:18400/health/live
```

```json
{
  "status": "live",
  "service": "yappod_front"
}
```

この応答は索引とcoreを確認しません。`200`でも検索できるとは限りません。

### 準備完了確認

```sh
curl -fsS http://127.0.0.1:18400/health/ready
```

```json
{
  "status": "ready",
  "service": "yappod_front",
  "ready": true,
  "generation": 3,
  "segments": 2,
  "embedding": {
    "state": "disabled",
    "model_id": "",
    "dimensions": 0
  },
  "compaction": {
    "state": "idle",
    "generation": 0,
    "updated_at_unix": 0
  }
}
```

| フィールド | 意味 |
|---|---|
| `status` | `ready`または`not_ready`です。 |
| `service` | frontでは`yappod_front`です。内部ヘルスチェックのレスポンスでは`yappod_core`になります。 |
| `ready` | 検索を受けられるかを表す真偽値です。 |
| `generation` | frontがディスク上のマニフェストから確認した世代です。 |
| `segments` | マニフェストが参照するセグメント数です。 |
| `embedding.state` | ベクトル対応索引なら`precomputed_ready`、語彙索引なら`disabled`です。外部埋め込みサーバーの稼働状態ではありません。 |
| `embedding.model_id` | 索引`config.toml`に保存されたベクトルモデルの識別子です。 |
| `embedding.dimensions` | 索引に保存されたベクトルの次元数です。 |
| `compaction.state` | `idle`、`running`、`succeeded`、`failed`、`interrupted`、`unknown`のいずれかです。 |
| `compaction.generation` | `compaction.state`が指す世代です。 |
| `compaction.updated_at_unix` | 状態ファイルを更新したUnix秒です。 |

準備完了確認では、frontが指定された索引を解析できることに加え、coreへ接続して内部ヘルスチェックのリクエストが`200`を返すことを確認します。ただし、frontとcoreが同じ索引ディレクトリを設定しているかをパス文字列で比較するわけではありません。

## Prometheusへの登録

`GET /metrics`のContent-Typeは次のとおりです。

```text
text/plain; version=0.0.4; charset=utf-8
```

Prometheusの最小設定例です。

```yaml
scrape_configs:
  - job_name: yappod_front
    scrape_interval: 15s
    static_configs:
      - targets: ["127.0.0.1:18400"]
```

frontを複数起動する場合は、各frontを別の収集対象として登録します。メトリクスはfrontプロセスのメモリー内にあり、再起動するとカウンターとヒストグラムは0へ戻ります。

手動で確認する場合は次を実行します。

```sh
curl -fsS http://127.0.0.1:18400/metrics
```

## リクエスト数

### `yappod_v2_requests_total`

| 項目 | 値 |
|---|---|
| Prometheusの型 | カウンター |
| ラベル | `operation`、`status_class` |
| `operation` | `search`、`retrieve`、`ingest` |
| `status_class` | `2xx`、`4xx`、`5xx` |

frontが処理を完了したv2 APIリクエスト数です。エンドポイントとの対応は次のとおりです。

| HTTPエンドポイント | `operation` |
|---|---|
| `POST /v2/search` | `search` |
| `POST /v2/retrieve` | `retrieve` |
| `POST /v2/passages:prepare` | `retrieve` |
| `POST /v2/documents:batch` | `ingest` |

ステータスクラスは最終的にfrontが返した状態から決めます。200〜299は`2xx`、400〜499は`4xx`、それ以外は`5xx`へ数えます。このため、現行実装では3xxも`5xx`側へ入りますが、Yappod2の対象APIは通常3xxを返しません。

`/health/*`、`/metrics`、未知のパス、HTTPメソッド違反はこのカウンターへ入りません。HTTPのリクエスト行やヘッダーの解析段階で返したエラーも、対象操作を確定する前なので入りません。

PromQL例です。

```promql
sum by (operation) (rate(yappod_v2_requests_total{status_class="5xx"}[5m]))
```

```promql
sum by (operation, status_class) (increase(yappod_v2_requests_total[1h]))
```

## リクエスト処理時間のヒストグラム

### `yappod_v2_request_duration_seconds`

| 時系列 | Prometheus上の役割 |
|---|---|
| `yappod_v2_request_duration_seconds_bucket{operation,le}` | 累積バケット |
| `yappod_v2_request_duration_seconds_sum{operation}` | 観測時間の合計秒数 |
| `yappod_v2_request_duration_seconds_count{operation}` | 観測件数 |

バケット境界は秒単位で次の9個です。

```text
0.005, 0.010, 0.025, 0.050, 0.100, 0.200, 0.500, 1.000, +Inf
```

計測範囲はfrontで対象APIの処理を開始してからレスポンス送信処理を呼ぶまでです。検索、RAG向け取得、文書更新ではcoreとの往復を含みます。`passages:prepare`ではfront内の処理を含みます。クライアントがレスポンス本文全体を読み終えるまでの時間や、search-webサーバー、埋め込み、LLMの時間は含みません。

95パーセンタイルの例です。

```promql
histogram_quantile(
  0.95,
  sum by (operation, le) (rate(yappod_v2_request_duration_seconds_bucket[5m]))
)
```

平均時間の例です。

```promql
sum by (operation) (rate(yappod_v2_request_duration_seconds_sum[5m]))
/
sum by (operation) (rate(yappod_v2_request_duration_seconds_count[5m]))
```

## 状態と処理中リクエストのゲージ

### `yappod_v2_ready`

frontから索引とcoreを利用できる場合は`1`、それ以外は`0`です。`/health/ready`の`ready`と同じ判定を使います。

### `yappod_v2_manifest_generation`

frontがディスク上のマニフェストから読んだ現在の世代です。更新後に増えない場合は、更新対象とfrontの`index.directory`が同じか、マニフェスト公開が成功したかを確認します。

### `yappod_v2_inflight_requests`

frontが現在処理中として受理した検索、RAG向け取得、本文断片生成、文書更新の件数です。認証失敗と上限超過で拒否したリクエストは受理しないため含みません。

### `yappod_v2_inflight_request_bytes`

受理済みリクエストの`Content-Length`合計です。レスポンスの大きさ、索引のメモリーマッピング、front/core間の通信ヘッダーは含みません。

### `yappod_v2_inflight_request_limit`

`daemon.max_inflight`の適用値です。

### `yappod_v2_inflight_byte_limit`

`daemon.max_inflight_bytes`の適用値です。

使用率の例です。

```promql
yappod_v2_inflight_requests / yappod_v2_inflight_request_limit
```

```promql
yappod_v2_inflight_request_bytes / yappod_v2_inflight_byte_limit
```

### `yappod_v2_embedding_configured`

索引`config.toml`でベクトルが有効なら`1`、無効なら`0`です。名前に`embedding`を含みますが、外部の埋め込みAPIへ接続確認は行いません。search-webの`[embedding]`が正しいか、サーバーが稼働しているかはこの値から判断できません。

### `yappod_v2_compaction_state{state="..."}`

現在読み取ったコンパクション状態のラベルを一つだけ出し、値は常に`1`です。取り得るラベルは次のとおりです。

| ラベル | 意味 |
|---|---|
| `idle` | `compaction.state`がありません。 |
| `running` | 状態ファイルのPIDが存在し、コンパクション中です。 |
| `succeeded` | 最後のコンパクションが成功しました。 |
| `failed` | 最後のコンパクションが失敗しました。 |
| `interrupted` | 状態はrunningですが、そのPIDが存在しません。 |
| `unknown` | 状態ファイルを読めないか、形式が不正です。 |

全状態を0または1で同時に出すワンホット形式の時系列ではありません。状態が変わると、以前のラベルを持つ時系列は次回の収集結果に現れなくなります。

### `yappod_v2_compaction_generation`

`compaction.state`に記録された世代です。現在のマニフェスト世代とは別です。

## 負荷制限を調べる

`503`が増え、`core_unavailable`ではなく`overloaded`が返る場合は、次を確認します。

1. `yappod_v2_inflight_requests`が`yappod_v2_inflight_request_limit`へ張り付いていないか確認します。
2. `yappod_v2_inflight_request_bytes`がバイト数上限へ張り付いていないか確認します。
3. `request_duration_seconds`で、どの操作の処理が長いか確認します。
4. core/frontのエラーログと、同じ時刻のリクエストタイムアウトを確認します。
5. 上限を増やす前に、CPU、メモリー、I/Oと、遅いリクエストの原因を確認します。

上限を増やすと、同時に保持するリクエスト本文、候補配列、レスポンスのメモリー量も増えます。

## アラートの組み立て例

環境の通信量とSLOに合わせて時間と閾値を調整してください。

準備完了状態が5分間失われた例です。

```promql
max_over_time(yappod_v2_ready[5m]) == 0
```

5xx比率が5分間で5%を超えた例です。

```promql
sum(rate(yappod_v2_requests_total{status_class="5xx"}[5m]))
/
sum(rate(yappod_v2_requests_total[5m])) > 0.05
```

コンパクションが中断した例です。

```promql
yappod_v2_compaction_state{state="interrupted"} == 1
```

カウンターが0件の時間帯は比率の分母が0になります。実運用の規則では通信条件も加えてください。

## 現在提供していない情報

現行の`/metrics`には次の情報がありません。

- 結果件数、ヒット率、検索文、検索方式または`scope`別の件数
- セグメントごとの文書数、本文断片数、ファイルサイズ
- プロセスのCPU、RSS、開いているファイル記述子、スレッド数
- core単体のPrometheusエンドポイント
- search-webサーバー、埋め込みAPI、LLM APIの処理時間とエラー
- `write_token`認証失敗だけを分離したカウンター
- 再読み込みの成功・失敗回数

OSとプロセスの資源はnode exporterやprocess exporterなど別の監視手段を使用してください。search-webと外部モデルの監視は、それぞれのプロセスログとサービス側のメトリクスを組み合わせます。
