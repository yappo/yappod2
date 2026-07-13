# Core protocol v2

`yappod_front`と`yappod_core`のv2通信は、hostの整数表現やC struct配置に依存しない
length-prefixed binary frameを使用します。すべての多byte整数はnetwork byte orderです。

| offset | bytes | field |
|---:|---:|---|
| 0 | 4 | magic `YAP2` |
| 4 | 2 | protocol version (`1`) |
| 6 | 2 | message type |
| 8 | 2 | flags（version 1では`0`） |
| 10 | 2 | reserved（`0`） |
| 12 | 8 | request ID（`0`は禁止） |
| 20 | 4 | payload byte length |
| 24 | N | payload |

message typeはsearch、retrieve、ingest、healthそれぞれのrequest/responseと共通error responseを
区別します。requestとresponseは同じrequest IDを保持します。payloadは上位runtimeが解釈する
UTF-8 JSONで、frame層はopaque bytesとして転送します。

readerは固定長headerを完全に読んでから上限を検証し、許可された長さだけを確保します。
絶対上限は16 MiBで、各接続はそれ以下の上限を指定します。未知version/type、非zero flags/reserved、request ID 0、上限超過、
途中で切れたframeはfail-closedで拒否します。memory decoderはheaderまたはpayloadが分割された
場合に`YAP_V2_CORE_FRAME_NEED_MORE`を返すため、nonblocking transport側で追加入力を待てます。

`YAP_V2_CORE_FRAME`は最初のdecode/read前に`YAP_V2_core_frame_init`で初期化します。
decode/readが成功したpayloadはframeが所有し、再利用時または終了時に`YAP_V2_core_frame_free`で
解放します。失敗時は既存frameを変更しません。encode/writeは呼び出し元のpayloadを借用するだけで、
所有権を取得しません。
