# `yappod_front`と`yappod_core`の内部HTTP通信

`yappod_core`は索引を開き、検索、RAG向け取得、文書更新を実行します。`yappod_front`は外部からHTTPリクエストを受け、
内容を`yappod_core`へ送って処理結果を受け取ります。

frontとcoreの間ではHTTP/1.1を使用します。検索とRAG向け取得は、本文を持つ安全で冪等な検索を表す
[RFC 10008の`QUERY`メソッド](https://www.rfc-editor.org/rfc/rfc10008.html)で送ります。文書更新は状態を変更するため
`POST`、準備完了確認は`GET`です。

このHTTP通信は同一システム内のfront/core間通信を目的としています。coreはTLS、一般利用者向け認証、CORSを備えた
公開APIではありません。通常のクライアントは[`yappod_front` HTTP API](yappod-front-api.md)を利用し、
coreのポートは内部ネットワークだけから接続できるようにしてください。

## 実装

`yappod_front`はfrontからcoreへのHTTP/1.1クライアントとしてlibcurlを使用します。検索と取得では
`CURLOPT_CUSTOMREQUEST`に`QUERY`を指定し、HTTPの版を1.1へ固定します。libcurlは既存のビルド依存関係であり、
利用者がTOMLで有効化したり接続先ライブラリを選択したりする機能ではありません。

`yappod_core`は境界付きの内部HTTP parserで要求行とヘッダーを検証し、1本のTCP接続につき1要求を処理します。
応答後は接続を閉じます。HTTP/2、HTTP/3、chunked transfer coding、接続の再利用には対応しません。

## 接続先と経路

coreの待ち受け先は`[daemon].core_host`と`core_port`です。frontは同じ値を接続先として使用します。

| 処理 | メソッド | パス | 要求本文 |
|---|---|---|---|
| 検索 | `QUERY` | `/v2/search` | UTF-8 JSON |
| RAG向け取得 | `QUERY` | `/v2/retrieve` | UTF-8 JSON |
| 文書更新 | `POST` | `/v2/documents:batch` | UTF-8 JSON |
| 準備完了確認 | `GET` | `/health/ready` | なし |

`/v2/passages:prepare`、`/health/live`、`/metrics`はcoreへ転送しません。これらはfrontが処理します。

既知のパスへ異なるメソッドを送ると、coreは`405 Method Not Allowed`と`Allow`を返します。検索と取得には
`Allow: QUERY`、更新には`Allow: POST`、準備完了確認には`Allow: GET`を返します。不明なパスは404です。

## 要求

検索、取得、更新では次のヘッダーが必要です。

```http
Content-Type: application/json
Accept: application/json
Content-Length: <JSONの正確なバイト数>
Connection: close
```

`Host`もHTTP/1.1の必須ヘッダーとして検証します。`Content-Type`がない場合またはJSON以外の場合は415、
`Content-Length`がない場合、0の場合、不正な場合は400です。要求本文の上限は1 MiBです。上限超過は413となります。

要求行1行の上限は8192バイト、要求ヘッダー全体の上限は65536バイトです。重複した`Host`、`Content-Type`、
`Content-Length`、`Authorization`、`Transfer-Encoding`、HTTP/1.1以外の要求は受理しません。

検索と取得のJSONは公開APIと同じです。front/core間通信用のJSONエンベロープは追加しません。

### 検索

```http
QUERY /v2/search HTTP/1.1
Host: 127.0.0.1:18401
Content-Type: application/json
Accept: application/json
Content-Length: 73
Connection: close

{"query":"modern search","mode":"lexical","scope":"documents","limit":10}
```

### RAG向け取得

```http
QUERY /v2/retrieve HTTP/1.1
Host: 127.0.0.1:18401
Content-Type: application/json
Accept: application/json
Content-Length: 79
Connection: close

{"query":"modern search","mode":"lexical","limit":10,"max_context_bytes":16384}
```

### 文書更新と認証

`[daemon].write_token`を設定した場合、frontは公開APIでBearer認証を検証した後、同じ
`Authorization: Bearer <token>`ヘッダーをcoreへ転送します。coreも定時間比較を使って再検証します。
検証できない場合は401です。

HTTP移行前に使用していた`YTK1`バイナリエンベロープは廃止されています。認証ヘッダーは暗号化ではないため、
coreのポートへ接続できる主体からトークンを秘匿できません。coreのネットワーク到達範囲を制限してください。

## 応答

coreは通常のHTTP/1.1状態コードとJSON本文を返します。

```http
HTTP/1.1 200 OK
Server: Yappo Search Core/2.0
Content-Type: application/json; charset=utf-8
Content-Length: 58
Cache-Control: no-store
Connection: close
Accept-Query: application/json

{"generation":7,"total":1,"results":[],"next_cursor":null}
```

検索と取得の応答には、受理できるQUERY本文のメディア型を示す`Accept-Query: application/json`を付けます。
索引は更新されるため、検索、取得、エラーを含む全応答へ`Cache-Control: no-store`を付けます。
coreから受け取る応答本文のfront側上限は16 MiBです。

入力JSONのエラーコードと本文は公開APIと共通です。coreが返した正しいHTTP状態とJSONはfrontがそのまま利用者へ
転送します。接続失敗、期限超過、不正なHTTP応答、JSON以外の応答、16 MiBを超える応答は
`503 core_unavailable`へ変換します。

## 準備完了確認

frontは公開`GET /health/ready`を処理するとき、coreへ次を送ります。

```http
GET /health/ready HTTP/1.1
Host: 127.0.0.1:18401
Accept: application/json
Connection: close
```

coreは保持中のスナップショットとディスク上の運用状態を調べます。利用可能なら200、利用できなければ503を返します。
本文にはサービス名、準備完了状態、世代、セグメント数、埋め込み、コンパクション状態が含まれます。

## 処理上限と期限

frontとcoreは、それぞれ`max_inflight`と`max_inflight_bytes`の処理枠を持ちます。core側は受理したJSON本文の長さを
数えます。上限を超えると`503 overloaded`です。

`request_timeout_ms`は1〜60000ミリ秒で、デフォルトは5000ミリ秒です。frontのlibcurlクライアントは接続と要求全体の
期限にこの値を使います。coreも受理したソケットの送受信期限に同じ値を使います。自動再試行は行いません。

## 移行と互換性

旧`YAP2`フレームと内部HTTP/1.1の自動判別や両対応は行いません。旧frontと新core、新frontと旧coreは通信できないため、
次の順序で同時更新してください。

1. 旧frontを停止します。
2. 旧coreを停止します。
3. frontとcoreの両バイナリを更新します。
4. 新coreを起動します。
5. 新frontを起動します。
6. 公開`GET /health/ready`と代表的な`QUERY /v2/search`を確認します。

設定キーとデフォルトポートは変わりません。HTTP化は将来のロードバランサー構成を妨げない通信境界にしますが、
複数coreへの索引配布、世代同期、更新の一貫性、ロードバランサー設定はこの通信仕様の対象外です。
