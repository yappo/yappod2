# Yappod2 v2索引契約

> **現在の状態:** 共通header、document/passage segment、manifest、lexical component、metadata filter、snippet、embedding provider、永続vector/exact検索を実装済みです。ANN以降の未完成部分は[現代検索基盤の完成契約](modern_search_completion_contract.md)に従って実装します。

この文書は、v2索引を生成・読み込むコンポーネント間の契約です。ここではデータ形式と検証規則を定義し、segmentの生成・検索・既存v1形式との接続は別の実装仕様で扱います。

## 互換性

- `format_version` は整数 `2` 固定です。
- v1索引をv2として開いてはいけません。v1からv2への変換では、元文書から再索引します。
- tokenizer、chunking、embeddingの設定が変わる場合は、manifest generationを共有してはなりません。
- すべての永続整数はlittle-endianです。C構造体のメモリ表現、`size_t`、`time_t`、ポインタは永続化しません。
- 文字列はUTF-8、NULを含まない長さ付きバイト列です。C APIの`YAP_V2_BYTES_VIEW`は所有権を持ちません。

## 文書とパッセージ

文書の外部IDは更新・削除・検索結果の安定した結合キーです。

```text
document.id              required, <=255 bytes
document.url             optional, <=255 bytes
document.title           optional, <=255 bytes
document.body            optional, <=1 MiB
document.metadata_json   optional, <=1 MiB, canonical JSON reserved for parser task
document.updated_at      required, Unix milliseconds, >=0

passage.id               required, <=255 bytes
passage.parent_document_id required, <=255 bytes
passage.text             required, <=1 MiB
passage.ordinal          zero-based within parent document
passage.start_char       inclusive Unicode character offset
passage.end_char         exclusive Unicode character offset
```

`end_char`は`start_char`以上で、v2の契約上限`YAP_V2_MAX_CHUNK_CHARS`以下です。パッセージの重複は許容しますが、同一親文書内のordinalはsegment writer側で一意にしなければなりません。

## config.toml

設定ファイルはindex directory直下の`config.toml`です。読み込み時はTOMLとして解析し、構文エラーや制約違反を検出した場合は起動時に失敗させます。キーと制約は次のとおりです。

```toml
format_version = 2

[tokenizer]
id = "unicode_bigram_v2"

[chunking]
max_chars = 1200
overlap_chars = 200

[vector]
enabled = false
model_id = ""
dimensions = 0
metric = "cosine"

[metadata]
filterable_fields = ["author.name", "lang", "year"]
```

- `tokenizer.id`は空でない識別子です。
- `chunking.max_chars`は1以上、`overlap_chars`は`max_chars`未満です。
- `vector.enabled=false`の場合、model IDは空、dimensionsは0、metricは`disabled`相当として扱います。
- 有効なvectorはmodel ID、dimensions（1〜65536）、`cosine`/`dot`/`l2`を必須とします。
- vectorのdimension、metric、model IDは同一index内で固定し、検索時に不一致を拒否します。
- 省略可能な値のdefaultはtokenizer `unicode_nfkc_casefold_v2`、chunk max 1200、overlap 200、vector disabledです。
- 未知のtop-level key、table、table内keyは将来の綴り間違いを黙認しないため拒否します。
- fingerprintはdefault適用後の全設定を固定順のcanonical表現へ変換し、SHA-256を計算した32-byte値です。空白、コメント、key順序の違いはfingerprintへ影響しません。
- `metadata.filterable_fields`は省略可能です。dot区切りのJSON object pathを最大64件指定し、
  byte順へ正規化して重複を拒否します。この一覧はfingerprintへ含めます。

## manifest.json

manifestは公開済みsegmentのスナップショットです。writerは一時ファイルへ全量を書き、fsync後にatomic renameします。

```json
{
  "format_version": 2,
  "generation": 42,
  "config_fingerprint": "64-lowercase-hex",
  "segments": [
    {
      "id": "seg-000042",
      "documents": 1000,
      "passages": 3400,
      "tombstones": 12,
      "components": [
        {
          "name": "documents.yap2",
          "file_type": 4,
          "records": 4400,
          "file_bytes": 12345678,
          "sha256": "64-lowercase-hex"
        }
      ]
    }
  ]
}
```

- `generation`は0ではない単調増加値です。初期manifestのgenerationは1です。
- segment IDは`[A-Za-z0-9._-]`だけを使い、path separatorを含めません。
- segment IDはmanifest内で一意です。
- `documents`、`passages`、`tombstones`、各componentの`records`と`file_bytes`は符号なし整数です。
- component名はpath separatorを含まないsegment directory直下の名前で、file typeはsegment内で一意です。
- `sha256`はcomponent file全体（headerを含む）のSHA-256です。JSONではlowercase hex、C契約では32-byte値として保持します。
- readerはcomponentごとにsize、SHA-256、header generation、file type、payload sizeを検証し、1件でも不一致ならsnapshot全体を拒否します。
- config fingerprintはdefault適用済みconfigのfingerprintと一致しなければならず、異なるconfigによる次generation publishも拒否します。
- tombstone componentはdocument IDの集合です。後続のsnapshot解決で古いsegmentの同一document IDを不可視にします。
- 空segment一覧は初期empty indexとして許容します。

## segment file header

すべてのsegmentファイルは32-byte headerを持ちます。

| byte | size | field |
|---:|---:|---|
| 0 | 4 | ASCII `YAP2` |
| 4 | 2 | format version (`uint16`, LE) |
| 6 | 2 | header bytes (`uint16`, LE, 32) |
| 8 | 4 | file type (`uint32`, LE) |
| 12 | 8 | manifest generation (`uint64`, LE, non-zero) |
| 20 | 8 | payload bytes (`uint64`, LE) |
| 28 | 4 | payload CRC32C (`uint32`, LE) |

file typeはterms=1、postings=2、positions=3、documents=4、metadata=5、vectors=6、tombstones=7です。headerの予約領域はなく、未知のfile typeやversionは読み込み時に拒否します。

## documents segment payload

`documents` file type（file type=4）は、header直後に次のlittle-endian payloadを持ちます。payload全体の上限は256 MiBで、headerの`payload_crc32c`はこのpayloadに対するCRC32Cです。

```text
uint32 payload_version       = 1
uint32 segment_id_bytes      (1..255)
byte   segment_id            (segment ID文字列)
uint64 document_count
uint64 passage_count
repeat document_count:
  uint32 record_type         = 1
  uint32 record_bytes
  uint32 id_bytes + byte[id_bytes]
  uint32 url_bytes + byte[url_bytes]
  uint32 title_bytes + byte[title_bytes]
  uint32 body_bytes + byte[body_bytes]
  uint32 metadata_json_bytes + byte[metadata_json_bytes]
  uint64 updated_at_unix_ms
repeat passage_count:
  uint32 record_type         = 2
  uint32 record_bytes
  uint32 id_bytes + byte[id_bytes]
  uint32 parent_document_id_bytes + byte[parent_document_id_bytes]
  uint32 text_bytes + byte[text_bytes]
  uint32 ordinal
  uint32 start_char
  uint32 end_char
```

document recordは先に、passage recordは後に並びます。writerは文書ID・passage ID・同一親文書内のordinalを一意にし、passageの親文書が同じsegmentに存在することを検証します。readerはレコード長、件数、各フィールドの上限、親文書参照、CRC32C、SHA-256をすべて検証してから公開します。

## lexical component payload

`terms.yap2`、`postings.yap2`、`positions.yap2` は同じ tokenizer 出力から一括生成します。
payload version は1で、termは正規化済みUTF-8 byte順、postingはobject type、object ordinal順、
positionはfield、token ordinal順に決定的に並びます。

- termsはterm bytes、document frequency、対応するpostings/positions payloadのoffsetとbytesを保持します。
- postingsはdocument/passageの種別とordinal、title/body/passage別のterm frequencyとfield length、position範囲を保持します。
- postings payload headerはdocument数、passage数、posting総数に加え、title/body/passageの
  全token数を保持し、BM25Fの平均field lengthをsegment単位で決定可能にします。
- 128 postingごとのblock metadataは先頭posting ordinal、件数、最大term frequency、最小field lengthを保持します。
- positionsはfield IDとfield内のzero-based token ordinalを保持します。
- readerはterm順序、object順序、offset、件数、block範囲を検証してからiteratorを公開します。
- lexical readerは3 componentをread-only `mmap`し、同一generation、header、CRC32C、payload
  全体の連続性、TFとpositionの整合、block-max再計算値をopen時に検証します。
- `YAP_V2_LEXICAL_SEGMENT`はinit後にopenし、利用中はmapped fileを変更・削除せず、最後に
  closeします。term/posting/positionのviewとiteratorはsegment closeまでだけ有効です。

## v2 lexical ranking

- queryもindexと同じUnicode tokenizerで正規化し、OR、AND、連続token phraseを扱います。
- scoreはfield lengthで正規化したtitle/body/passageのweighted TFを合成し、IDFを一度だけ
  適用するBM25Fです。既定boostはtitle=2、body=1、passage=1です。
- OR top-kは現在blockの最大TF・最小field lengthから安全な上限を計算するblock-max WANDで
  評価対象をskipします。ANDはobject keyのposting intersection、phraseは同一field内の連続
  positionを要求します。
- score降順、同点はobject type、object ordinal昇順の決定的順序です。

## metadata filterとsnippet

`metadata.yap2`（file type=5）はpayload version、設定済みfield一覧、document数、entry数に続き、
field ordinal、document ordinal、型、値を持つentryを格納します。型はnull、boolean、number、stringです。
scalar配列は複数entryとして扱い、objectとobjectを含む配列はindex化しません。readerはheader、generation、
CRC32C、設定済みfield一覧、ordinal、型、payload境界を検証します。

filter ASTはJSON objectで、`eq`、`in`、`range`（`gt`/`gte`/`lt`/`lte`）、`exists`、
`and`、`or`、`not`を扱います。未知のoperator、key、設定外field、型違反、空の論理配列、
深さ32超、1024 node超はcompile時に拒否します。lexical searchは候補をtop-kへ追加する前にfilter callbackを評価します。

snippetはICUのgrapheme境界でwindowを選び、UTF-8 byte列を途中で切りません。指定termと一致した
grapheme範囲だけを呼び出し側指定の開始・終了markerで囲みます。

## vectors componentとexact検索

`vectors.yap2`（file type=6）はpassage配列と同じordinal順でvectorを保持します。payloadは次の
little-endian形式です。floatはIEEE 754 binary32のbit patternとして格納し、vector開始offsetを
4-byte境界へ揃えます。

```text
uint32 payload_version = 1
uint32 metric
uint32 dimensions
uint32 model_id_bytes
uint64 passage_count
uint64 id_blob_bytes
uint64 vector_offset
byte   model_id[model_id_bytes]
repeat passage_count:
  uint64 id_offset
  uint32 id_bytes
  uint32 reserved = 0
byte   id_blob[id_blob_bytes]
byte   zero_padding[0..3]
float32 vectors[passage_count][dimensions]
```

writerはpassage IDの一意性、embedding件数・dimension、finite値を検証してatomic publishします。
readerはread-only `mmap`し、header、generation、CRC32C、model、metric、dimension、ID tableの
連続性と一意性、zero padding、payload終端、finite値をすべて検証します。configとの不一致は
corruptionではなく`YAP_V2_CONFLICT`として拒否します。open後の`YAP_VECTOR_ENTRY`はmapped IDと
aligned vectorを借用し、既存flat exact backendがcosine/dot/L2 top-kを実行します。

## C APIの所有権

- `YAP_V2_*_VIEW`の入力バイト列は呼び出し側が所有し、validate関数はコピーしません。
- `YAP_V2_SEGMENT`は`YAP_V2_segment_init`で初期化してからreadし、`YAP_V2_segment_free`で解放します。read後のdocument/passage viewはsegmentの`storage`を借用します。
- `YAP_V2_segment_write`はdescriptorを出力し、`YAP_V2_segment_read`はdescriptorがNULLでなければ同じメタデータを出力します。
- `YAP_V2_MANIFEST`の`segments`は`manifest_add_segment`がreallocで所有します。
- `manifest_free`はsegmentsを解放し、初期状態へ戻します。
- encode/decodeは固定長bufferのみを扱い、I/Oやファイルdescriptorを所有しません。
