# index segmentのsize制限と将来の分割設計

この文書は、`examples/local-files`で大量のlocal fileをNDJSON shardへ分ける際に混同しやすい、
入力shardとindex segmentの違いを整理します。ここに書く上限は現在のsource codeに基づく値です。
今回のexampleは`yappo_makeindex`、index writer、compactionを変更しません。

## 現在の上限

主要なcompile-time上限は`src/yappo_index_v2.h`で定義されています。

| 対象 | 上限 |
|---|---:|
| document ID | 255 bytes |
| URL | 8 KiB |
| document body | 1 MiB |
| metadata | 1 MiB |
| passage text | 1 MiB |
| `chunk_max_chars` | 1,048,576 chars |
| index内のsegment数 | 100,000 |
| 1 segmentのdocument数 | 1,000,000 |
| 1 segmentのpassage数 | 4,000,000 |
| 主要YAP2 componentのpayload | 256 MiB |
| YAP2 file header | 32 bytes |

「本文が1 MiBまで」と「segment componentが256 MiBまで」は別の制約です。1 MiBは1件の
chunk/bodyやmetadataに関係する入力record側の上限です。256 MiBは`documents.yap2`、
`terms.yap2`、`postings.yap2`、`positions.yap2`、`metadata.yap2`、`vectors.yap2`など、
segment directory内の主要componentごとに適用されるpayload上限です。file全体では32 bytesの
headerが加わります。

`vectors.usearch`はUSearch libraryが保存する別形式です。現在の実装には、YAP2 writerと同じ
`YAP_V2_MAX_SEGMENT_PAYLOAD_BYTES`を明示的に適用するcheckがありません。したがって
「segment directory内の全fileが一律256 MiBまで」とは表現できません。

## writerは超過を拒否するが自動分割しない

`yappo_makeindex build`はNDJSONを10,000 operationずつ読み、各batchを既存のupdate処理へ渡します。
現在のbatch境界は件数とEOFだけで決まり、生成予定componentのbyte数は見積もりません。

各writerはbufferが256 MiBを超えようとすると`YAP_V2_OUT_OF_RANGE`を返します。これは安全な
拒否であって、残りのrecordを次のsegmentへ移す処理ではありません。したがって、入力を複数の
NDJSON shardに分けても、shard境界がindex segment境界になるとは限りません。FIFOで複数shardを
連結して既存buildへ1回渡す場合も同じです。

現在の10,000件batchが十分小さければ結果として複数segmentになりますが、件数の少ない巨大本文、
token数の多いsource code、大きなmetadata、多次元vectorなどでは、1 batchのcomponentが256 MiBを
超えて失敗し得ます。今回のexampleはこの問題を解決せず、既存buildのerrorをそのまま返す設計です。

## 単純な再試行分割だけでは足りない理由

失敗したbatchを半分にして再試行する方式は、件数起因の超過には有効です。ただし次の条件が必要です。

- 1 recordだけでも上限を超える場合は、分割不能として明示的に終了する。
- retryごとに作成途中のtemporary segmentを確実に破棄する。
- 256 MiB以外のerrorを「size超過」と誤認して分割しない。
- 何度も同じ入力をtokenize・chunk・vector処理するcostを考慮する。

この停止条件がなければ、1件の書き込みが単独で失敗するcaseで再試行を続ける実装になり得ます。
また、256 MiBぎりぎりかどうかは`documents.yap2`だけでは決まりません。lexical、metadata、vectorの
いずれか一つでも上限へ達すれば、そのbatchは成立しません。

## segment数を増やす場合の副作用

自動segment分割は健全な将来候補ですが、writerだけの局所変更にはできません。

- **BM25統計:** 現在のlexical reader/searchはsegmentが保持するdocument frequencyやfield length統計を
  使用します。segmentの切り方が変わるとscore分布が変わり得ます。
- **検索fan-out:** lexical/vector queryは全segmentを巡回し、segmentごとの候補を集約します。
  segment数が増えるほどcandidate buffer、ANN呼び出し、merge costが増えます。
- **重複ID:** 現在のupdate処理は1 batch内の重複document IDを拒否します。plannerがbatchを分ける場合、
  build全体での重複・置換semanticsを維持する必要があります。
- **manifest上限:** segment数自体にも100,000件の上限があります。小さすぎるsegmentを大量に作る方式は
  この上限と運用costを早く消費します。
- **compaction:** 現在のcompactionはlive document/passageを集め、1個の出力segmentを書きます。
  buildだけが分割できても、compactionが同じdata量で再び256 MiB超過する可能性があります。

## 256 MiBを拡張する場合のリスク

constantを大きくするだけでは安全な拡張になりません。現在のreader/writerにはpayload全体やfile全体を
memoryへ確保する経路があり、lexical、metadata、vectorの構築も大きな中間bufferを持ちます。上限を
拡張するとpeak RSS、allocation failure、checksum計算時間、一時file、障害時の再処理単位が大きくなります。

さらに、古いbinaryは新しい上限のcomponentを`OUT_OF_RANGE`またはinvalid formatとして拒否します。
format versionを据え置いたまま上限だけ変える場合でも、reader互換性、manifest/checksum検証、旧binaryへ
戻す運用を明示的に評価する必要があります。`vectors.usearch`にはlibrary側のformat・memory mapping条件も
別途あります。

## 将来設計

将来の対応は、buildとcompactionが共有する独立した**segment planner**として検討するのが適切です。
plannerには少なくとも次の責務が必要です。

1. document、passage、lexical、metadata、vector componentの予測sizeを保守的に見積もる。
2. document境界でsegmentを確定し、単一recordが収まらない場合は有限回で明示errorにする。
3. 実sizeが予測を超えた場合だけ、transactionalにrollbackして有限分割する。
4. buildとcompactionの両方で同じ上限・分割規則を使う。
5. segment-local score、query fan-out、重複ID、manifest上限、公開atomicityをtestする。

これはcore index formatと検索特性に関わるため、local-files exampleとは別taskで設計・実装・互換性検証を
行います。今回のFIFO exampleが保証するのは、複数の検証済みNDJSON shardを既存buildへ順番どおりに
streamできることまでであり、index segmentのsize問題を解決するものではありません。
