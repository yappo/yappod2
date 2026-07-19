# 運用

## 起動

coreを先に、frontを後に起動します。アプリケーション用TOMLを使うと、索引、接続先、PID、ログ、処理上限を共有できます。

```sh
./build/yappod_core --config /srv/yappod/application.toml
./build/yappod_front --config /srv/yappod/application.toml
```

`run_directory`には`core.pid`、`front.pid`、`core.log`、`front.log`、対応する`.error`を保存します。

## 起動準備の確認と監視

- `/health/live`はfrontプロセスが動作していることを確認します。
- `/health/ready`はcoreへの接続と検証済みスナップショットを確認します。
- `/metrics`はリクエスト数、処理時間、generation、処理中の量、embeddingとコンパクションの状態を返します。

監視では準備状態、5xx、処理時間、generation、frontとcore両方のログを同じ時刻で確認してください。

## 資源制限

`max_inflight`は同時リクエスト数、`max_inflight_bytes`は受理中の本文バイト数の合計、`request_timeout_ms`は
frontとcoreの間の処理期限です。上限を超えたリクエストは処理前に拒否します。

## 更新認証

`daemon.write_token`を設定すると`/v2/documents:batch`にBearerトークンが必要です。16〜255バイトの空白を含まない値を
coreとfrontで共有します。検索、ヘルスチェック、メトリクスにはこのトークンを要求しません。

## 障害の確認順

1. PIDが実際のプロセスを指しているか確認します。
2. `.error`と`.log`の末尾を確認します。
3. `/health/live`と`/health/ready`を別々に確認します。
4. アプリケーション用TOMLの索引パス、ポート、タイムアウトを確認します。
5. `yappo_makeindex verify`で公開スナップショットを検証します。

調査時は設定ファイルのハッシュ値、manifestのgeneration、コンポーネントのチェックサム、HTTP状態コード、処理時間、
両デーモンのログを保存します。
