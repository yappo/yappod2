# v2 Unicode tokenizer and chunker

Yappod2 の正式 tokenizer は `unicode_nfkc_cf_v1` です。index writer と query runtime は
`config.toml` に保存された同じ tokenizer ID を使い、異なる設定の manifest を拒否します。

## Normalization

入力は UTF-8 として検証し、ICU により NFKC casefold します。このため互換文字、全角/半角、
大文字/小文字を決定的に正規化できます。不正 UTF-8 は置換や byte 単位の token 化をせず拒否します。

## Boundary

- word boundary: ICU word break iterator
- sentence boundary: ICU sentence break iterator
- grapheme boundary: ICU character break iterator
- offset: 元文書に対する Unicode code point offset

tokenizer は空白や句読点だけの span を検索語にしません。index 時と query 時の normalization、
boundary、position 規則は同一です。phrase 検索は保存済み position を使います。

## Passage chunking

chunker は sentence boundary を優先し、`chunking.max_chars` 以下へ分割します。1 sentence が
上限を超える場合は grapheme boundary で分割します。隣接 passage は
`chunking.overlap_chars` の範囲で重なりますが、grapheme cluster の途中では切りません。

passage ID は document ID、ordinal、開始/終了 offset、正規化済み本文から決定的に生成します。
同じ config と入力からは同じ passage ID と offset が得られます。precomputed vector はこの
passage ordinal 順に指定します。

## Compatibility boundary

v2 index は tokenizer ID と chunking 設定を config fingerprint に含めます。設定変更時は元文書から
新しい index を build してください。旧 byte bigram tokenizer と旧 index 形式の互換 reader は
提供しません。

契約テストは NFKC casefold、CJK/Latin、emoji、結合文字、不正 UTF-8、grapheme-safe chunk、
決定的 passage ID、index/query の token 一致を検証します。
