# v2 JSON 検索 API

`yappod_front` は既存のテキスト形式 API と並行して、機械利用向けの `GET /v2/search` を提供します。既存 URL の互換性は維持されます。

## リクエスト

クエリパラメータは次の通りです。

- `dict`（必須）: 辞書名
- `op`（必須）: `AND` または `OR`
- `q`（必須）: 検索語。複数語は既存 API と同じく `%26` で区切ります
- `max_size`（任意）: 文書サイズ上限。既定値は `INT_MAX`
- `limit`（任意）: 1〜100 件。既定値は 20
- `cursor`（任意）: 前ページの `next_cursor`

検索語とパラメータ値の `%HH` エスケープを使用してください。不正なエスケープ、未知のパラメータ、重複パラメータは `400 Bad Request` になります。

例:

```text
GET /v2/search?dict=yappo&op=AND&q=OpenAI2025&limit=20 HTTP/1.1
Host: localhost
```

## レスポンス

`Content-Type: application/json; charset=utf-8` で、次の形の JSON を返します。

```json
{
  "api_version": 2,
  "total": 1,
  "limit": 20,
  "next_cursor": null,
  "results": [
    {
      "url": "https://example.test/doc1",
      "title": "Example",
      "size": 1234,
      "lastmod": 1700000000,
      "score": 1.25
    }
  ]
}
```

結果は既存のスコア降順（同点時は file index 昇順）で固定され、`next_cursor` がある場合は同じ検索条件にその値を付けて次ページを取得します。cursor は不透明な値として扱い、手作業で変更しないでください。ページングは一つの検索結果スナップショット内で有効です。

URL、タイトルは JSON の文字列規則に従ってエスケープされます。検索結果がない場合も `total: 0` と空の `results` を返します。
