# Canonical ingest model

v2の正式入力はUTF-8 NDJSONです。1行は次のどちらかで、未知key、型違反、空のID、
不正JSONは行番号にかかわらず入力全体をfail-closedで拒否します。

```json
{"operation":"upsert","id":"doc-1","url":"https://example.test/","title":"Title","body":"Body","metadata":{"lang":"ja"},"updated_at_unix_ms":1735689600000}
{"operation":"delete","id":"doc-1"}
```

- `upsert`では`id`と`body`が必須です。`url`、`title`、`metadata`、
  `updated_at_unix_ms`は省略可能です。vectorが有効なindexへ直接publishする`update`では、
  chunk後のpassage ordinal順に`vectors`を指定します。
- `delete`は`operation`と`id`だけを許可します。
- `vectors`は二次元number配列です。外側の件数はchunkerが生成したpassage数、各内側配列は
  configのdimensionと厳密に一致し、全値がfiniteでなければbatch全体を拒否します。
  document vectorを複数passageへ複製しません。vector無効時の`vectors`指定も拒否します。
- metadataはobjectに限定し、object keyをUTF-8 byte順に再帰的に並べたcompact JSONへ
  canonicalizeします。arrayの順序は保持します。
- 旧TSVは`--input-format tsv`を明示した場合だけadapterとして受理します。TSVのURLを
  canonical document IDとして使用します。

`prepare`はconfigのchunk設定を読み、upsertを正規化済みpassage NDJSONへ展開し、deleteを
引き継ぎます。

```sh
yappo_makeindex prepare --config config.toml --input documents.ndjson --output passages.ndjson
yappo_makeindex prepare --config config.toml --input legacy.tsv --input-format tsv --output passages.ndjson
```

出力passageは`document_id`、決定的`passage_id`、ordinal、Unicode code-point offset、
正規化済みtext、canonical metadataを持ちます。これは後続のv2 segment writerへ渡す
中間形式であり、legacy Berkeley DB indexerへは入力しません。

更新例:

```json
{"operation":"upsert","id":"doc-1","body":"Body","vectors":[[0.1,0.2]]}
{"operation":"delete","id":"doc-2"}
```

```sh
yappo_makeindex update --config /srv/yappod/config.toml --input operations.ndjson
```
