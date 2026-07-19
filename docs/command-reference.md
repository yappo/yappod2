# コマンドリファレンス

この文書のコマンドは、特記がない限りリポジトリのルートディレクトリで実行します。成功時は終了状態
`0`、失敗時は0以外を返します。

## `yappo_makeindex prepare`

```text
yappo_makeindex prepare --config CONFIG --input INPUT --output OUTPUT [--input-format ndjson|tsv]
```

入力を正規化し、パッセージのNDJSONを作成します。`--input-format`の既定値は`ndjson`です。TSVは移行用の
入力形式としてだけ利用できます。出力パスは未作成である必要があります。入力はファイル、出力は指定したNDJSONです。

## `yappo_makeindex build`

```text
yappo_makeindex build --config CONFIG --input documents.ndjson
```

`[index].directory`へ新しい索引を作成します。`--index`は受理しません。出力先が存在する場合や入力全体を
検証できない場合は公開しません。成功時はgenerationと受理件数をJSONで出力します。

## `yappo_makeindex update`

```text
yappo_makeindex update --config CONFIG --input operations.ndjson
```

既存索引へ最大100操作の一括更新を公開します。成功時はgeneration、`upsert`と`delete`の件数、セグメントIDを
JSONで出力します。1操作でも不正な場合は一括更新全体を拒否します。

## `yappo_makeindex verify`

```text
yappo_makeindex verify --config CONFIG
```

manifestから参照される全コンポーネントの大きさ、チェックサム、ヘッダー、内部構造を検証します。

## `search`

```text
search (--config CONFIG | --index INDEX_DIR) --mode lexical|vector|hybrid
       [--query TEXT] [--vector N,N,...] [--scope documents|passages] [--limit 1..100]
```

`--config`と`--index`はどちらか一方だけを指定します。`lexical`には`--query`、`vector`には`--vector`、
`hybrid`には両方が必要です。`--scope`の既定値は`documents`、`--limit`の既定値は10です。成功時は
`/v2/search`と同じJSONレスポンスを1行で標準出力へ書きます。

## `yappo_compact`

```text
yappo_compact (--config CONFIG | --index INDEX_DIR)
```

現在有効なレコードだけを新しいセグメントへまとめ、参照されなくなったセグメントを回収します。成功時は新しい
generation、文書数、パッセージ数、削除したセグメント数をJSONで出力します。

## `yappod_core`

```text
yappod_core --config CONFIG
yappod_core --index INDEX_DIR [--port PORT]
```

`--config`は`--index`や`--port`と同時に指定できません。アプリケーション設定を使う場合はcoreのポート、
実行用ディレクトリ、処理期限を`[daemon]`から読みます。起動前に索引を検証し、デーモン化した後はPID、標準出力、
標準エラー出力を`run_directory`の`core.pid`、`core.log`、`core.error`へ保存します。

## `yappod_front`

```text
yappod_front --config CONFIG
yappod_front --index INDEX_DIR --core-host HOST [--port PORT] [--core-port PORT]
```

`--config`は個別のデーモン用オプションと同時に指定できません。frontは公開HTTP APIを受け、coreへ内部フレームで
転送します。通常はcoreを先に起動してください。デーモン化した後は`front.pid`、`front.log`、`front.error`を
`run_directory`へ保存します。索引を開けない場合やポートを待ち受けられない場合は起動に失敗します。
