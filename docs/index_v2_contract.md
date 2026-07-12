# Yappod2 v2索引契約

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
```

- `tokenizer.id`は空でない識別子です。
- `chunking.max_chars`は1以上、`overlap_chars`は`max_chars`未満です。
- `vector.enabled=false`の場合、model IDは空、dimensionsは0、metricは`disabled`相当として扱います。
- 有効なvectorはmodel ID、dimensions（1〜65536）、`cosine`/`dot`/`l2`を必須とします。
- vectorのdimension、metric、model IDは同一index内で固定し、検索時に不一致を拒否します。

## manifest.json

manifestは公開済みsegmentのスナップショットです。writerは一時ファイルへ全量を書き、fsync後にatomic renameします。

```json
{
  "format_version": 2,
  "generation": 42,
  "segments": [
    {
      "id": "seg-000042",
      "documents": 1000,
      "passages": 3400,
      "file_bytes": 12345678,
      "checksum": "sha256-hex"
    }
  ]
}
```

- `generation`は0ではない単調増加値です。初期manifestのgenerationは1です。
- segment IDは`[A-Za-z0-9._-]`だけを使い、path separatorを含めません。
- segment IDはmanifest内で一意です。
- `documents`、`passages`、`file_bytes`は符号なし整数です。
- checksumはsegment一式を対象とするSHA-256のhex表現を予定します。C契約では32-byte値として保持し、segmentの生成・読み込み時に計算と照合を行います。
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

file typeはterms=1、postings=2、positions=3、documents=4、metadata=5、vectors=6です。headerの予約領域はなく、未知のfile typeやversionは読み込み時に拒否します。

## C APIの所有権

- `YAP_V2_*_VIEW`の入力バイト列は呼び出し側が所有し、validate関数はコピーしません。
- `YAP_V2_MANIFEST`の`segments`は`manifest_add_segment`がreallocで所有します。
- `manifest_free`はsegmentsを解放し、初期状態へ戻します。
- encode/decodeは固定長bufferのみを扱い、I/Oやファイルdescriptorを所有しません。
