# index segmentの容量制限と自動分割

indexを新規作成する経路は、`yappo_makeindex build`、update、`yappo_compact`の3つです。
どの経路でも、1個の出力segment内のYAP2 componentが256 MiBを超えないよう、共通の
segment plannerが文書境界で出力を分割します。`yappo_compact`を自動実行する機能ではなく、
利用者がcompactを実行したときの出力作成にも同じ容量制限を適用する、という意味です。

index format versionは2のままです。既存の公開済みsegmentや256 MiBの上限値は変更しません。

## 分割単位と順序

plannerが扱う1単位は次のどちらかです。

- 1件のupsert文書、その文書から生成した全passage、その全vector
- 1件のdelete tombstone

入力順に単位を追加し、次の単位を追加するといずれかのcomponent上限、文書数上限、または
passage数上限を超える場合、現在のsegmentを確定して次のsegmentを開始します。したがって
各segmentには収まる最大の入力prefixが入り、作成segment数が最小になります。文書と、その文書の
passage・vectorが別segmentへ分かれることはありません。

`build`は従来どおり最大10,000 operationずつ読みます。そのbatchの中で必要な個数のsegmentへ
分割します。`update`は最大100 operationの1リクエスト内を分割します。`compact`は収集したlive文書を
同じ規則で分割します。

## componentごとの上限

上限はYAP2 fileの32-byte headerを除いたpayloadに適用します。

| component | payload上限 | plannerが計算する内容 |
|---|---:|---|
| `documents.yap2` | 256 MiB | segment header、文書record、passage record |
| `terms.yap2` | 256 MiB | 固定12 bytes、各termの44 bytesとterm文字列 |
| `postings.yap2` | 256 MiB | 固定56 bytes、各term 20 bytes、posting 48 bytes、128 postingごとのblock 16 bytes |
| `positions.yap2` | 256 MiB | 固定12 bytes、各term 16 bytes、各token位置8 bytes |
| `metadata.yap2` | 256 MiB | 固定24 bytes、filter field名、各scalar entryの20 bytesと値 |
| `vectors.yap2` | 256 MiB | 固定40 bytes、model ID、passage ID、record、alignment、float vector |
| `tombstones.yap2` | 256 MiB | 固定12 bytes、各文書ID |

plannerの計算値とは別に、各writerにも256 MiBの確認を残しています。writerが容量超過を返した場合だけ、
該当する準備済み入力を二分して有限回再実行します。I/O、allocation、invalid format、path長など、容量以外の
エラーでは再分割しません。

`vectors.usearch`はYAP2 fileではないため、この256 MiB上限の対象外です。ANN writer自身が返す既存の
エラー処理を使用します。

## 単独文書が収まらない場合

1文書とそのpassage・vectorを同じsegmentへ置く規則は崩しません。1文書単独でもいずれかのcomponentが
上限を超える場合は`YAP_V2_SEGMENT_CAPACITY_EXCEEDED`を返します。エラーには次を含めます。

- 文書ID
- 超過したcomponent名
- 必要なpayload byte数
- 上限byte数

delete tombstone単独の超過にも同じ形式を使用し、文書IDとしてdelete対象IDを示します。

## manifestへの公開

### update

update開始時にwriter lockを取得します。分割後の新segmentをすべて作成してfsyncし、全descriptorを
manifestのローカルコピーへ追加して検証した後、そのupdateのmanifestを1回更新します。ここで
「1回更新する」とは、そのupdate処理の最後に1回だけ更新するという意味です。将来のupdateで
manifestを更新しない、という意味ではありません。

途中で失敗した場合、そのupdateで作成したsegment directoryをすべて削除し、manifestは変更しません。
process crashで未参照directoryが残った場合も検索からは見えず、既存のcompaction GCが回収します。

### compact

compactはlive文書から必要数の新segmentをすべて作成し、その全segmentだけを含むmanifestへ処理の最後に
1回切り替えます。公開後に旧segmentをGCします。公開前にcrashした場合は旧manifest、公開後にcrashした
場合は新manifestが有効です。

### build

build先全体を従来どおり一時directoryに作成します。全入力の処理に成功した後で、一時directoryを指定された
index directoryへrenameします。

## BM25統計

各split segmentは、そのsegmentに格納された文書とpassageだけから独立したlexical indexとBM25統計を
作ります。複数segment間でdocument frequencyやfield length統計を共有しません。そのため、分割境界が
変われば検索結果のscoreが変わる場合があります。分割前と分割後のscore一致は保証しません。

## 成功レスポンス

updateのHTTP APIとCLI、および`yappo_compact`は、作成segmentが1個の場合も複数の場合も
`segment_ids`配列を返します。

```json
{"segment_ids":["seg-..."]}
```

```json
{"segment_ids":["seg-...","seg-..."]}
```

単数の`segment_id`は返しません。build CLIの成功レスポンスは従来どおり`generation`と`accepted`で、
`segment_ids`は追加しません。
