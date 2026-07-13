# RAG passage retrieval

v2 の documents segment には文書と passage（chunk）が同じスナップショットで保存されます。`yappo_rag.h` は segment の `storage` をコピーせずに passage を引用するための read-only API です。

## API

- `YAP_RAG_find_passage(segment, passage_id, &view)` は passage ID を完全一致で検索します。戻り値の view は segment が解放されるまで有効です。
- `YAP_RAG_list_passages(segment, document_id, out, capacity, &count)` は親文書の passage を `ordinal` 昇順で返します。同一親文書の ordinal は segment writer が一意性を検証します。
- `out == NULL, capacity == 0` で必要件数だけを取得できます。capacity が足りない場合は `YAP_RAG_BUFFER_TOO_SMALL` と必要件数を返します。

本文・ID・文字範囲は `YAP_V2_PASSAGE_VIEW` の borrowed view です。RAG のコンテキスト組み立て側は、同じ segment snapshot の寿命を保ったまま本文を prompt にコピーしてください。更新中の別 generation と混在させず、manifest の generation 単位で引用を管理します。

```c
const YAP_V2_PASSAGE_VIEW *passages[8];
size_t count;
YAP_RAG_list_passages(&segment, document_id, passages, 8, &count);
```

この低レベル API 自体は順位付けを行いません。query/retrieve 層が passage candidate、hybrid ranking、vector 検索、source metadata、context byte offset と接続し、`POST /v2/retrieve` で引用可能な context と citation を返します。
