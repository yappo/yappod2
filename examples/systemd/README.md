# systemdでYappod2サーバーを起動する

この例では、systemdが`yappod_core`と`yappod_front`のプロセス監視、再起動、ログ収集を担当します。
両コマンドは`--foreground`で起動するため、forkせず、PIDファイルと独自のログファイルを作りません。

## 前提

- Yappod2の実行ファイルを`/usr/local/bin`へインストールします。
- 専用の`yappod`ユーザーとグループを作成します。
- アプリケーション用TOMLを`/etc/yappod/application.toml`へ配置します。
- `[index].directory`は`/var/lib/yappod/index`に設定します。
- `[daemon].run_directory`は`/run/yappod`に設定します。フォアグラウンド実行ではPIDとログを作りませんが、
  アプリケーション用TOMLではこのキー自体が必須です。

ユーザーやグループの作成方法はLinuxディストリビューションごとに異なります。既存のサービス用ユーザー管理規則に
従って作成してください。次のコマンドでは、ユーザーとグループがすでに存在するものとします。

## 配置

リポジトリのルートでビルドとインストールを行います。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

設定ディレクトリを作り、トップレベルの設定例をコピーします。

```sh
sudo install -d -o root -g yappod -m 0750 /etc/yappod
sudo install -o root -g yappod -m 0640 \
  config.example.toml /etc/yappod/application.toml
sudo install -d -o yappod -g yappod -m 0750 /var/lib/yappod/index
```

`/etc/yappod/application.toml`を編集し、少なくとも次の値を配置先に合わせます。

```toml
[index]
directory = "/var/lib/yappod/index"

[daemon]
run_directory = "/run/yappod"
core_host = "127.0.0.1"
core_port = 18401
front_host = "127.0.0.1"
front_port = 18400
```

索引を別の場所へ置く場合は、unitの`ProtectSystem=strict`によってcoreからその場所へ書き込めません。
`systemctl edit yappod-core.service`で`ReadWritePaths=`を追加するか、unitの保護設定を配置方針に合わせて
変更してください。frontは索引を読み取れる必要があります。

索引を作成または復元してから、実行ユーザーで検証します。

```sh
sudo -u yappod /usr/local/bin/yappo_makeindex verify \
  --config /etc/yappod/application.toml
```

unitファイルを配置します。

```sh
sudo install -o root -g root -m 0644 \
  examples/systemd/yappod-core.service /etc/systemd/system/yappod-core.service
sudo install -o root -g root -m 0644 \
  examples/systemd/yappod-front.service /etc/systemd/system/yappod-front.service
sudo systemd-analyze verify \
  /etc/systemd/system/yappod-core.service \
  /etc/systemd/system/yappod-front.service
sudo systemctl daemon-reload
```

unitは`StateDirectory=yappod`で`/var/lib/yappod`を、`RuntimeDirectory=yappod`で`/run/yappod`を
coreの実行ユーザー向けに用意します。`ProtectSystem=strict`と`ProtectHome=true`を設定しているため、
アプリケーション用TOMLと索引を上記以外へ置く場合は、読み書き権限とsystemdのファイルシステム保護を
両方確認してください。

## 起動と確認

core、frontの順で有効化して起動します。

```sh
sudo systemctl enable --now yappod-core.service yappod-front.service
systemctl status yappod-core.service yappod-front.service
curl -fsS http://127.0.0.1:18400/health/live
curl -fsS http://127.0.0.1:18400/health/ready
```

`yappod-front.service`の`Wants=`と`After=`はcoreの起動開始を依頼し、実行順序をcoreの後にしますが、
coreの準備完了までは待ちません。運用可能かどうかは`/health/ready`で確認してください。coreが一時的に
停止してもfrontは生存し、準備完了確認で503を返します。

## ログ

標準出力と標準エラーはsystemd journalへ送ります。追跡する場合は次を実行します。

```sh
sudo journalctl -u yappod-core.service -u yappod-front.service -f
```

## 停止と再起動

新しい要求の受け付けを先に止めるため、停止はfront、coreの順で行います。

```sh
sudo systemctl stop yappod-front.service yappod-core.service
```

設定や索引の配置を変更した後は、core、frontの順で再起動し、準備完了を確認します。

```sh
sudo systemctl restart yappod-core.service
sudo systemctl restart yappod-front.service
curl -fsS http://127.0.0.1:18400/health/ready
```

unitの`Restart=on-failure`は異常終了したプロセスを再起動します。管理者が`systemctl stop`を実行した
場合や、`SIGTERM`によって正常終了した場合は再起動しません。

systemdの各設定の正式な意味は、systemdプロジェクトの
[`systemd.service`](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)、
[`systemd.exec`](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)、
[`systemd.unit`](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html)を参照してください。
