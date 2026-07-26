# local-files exampleの作業規則

適用範囲は`examples/local-files/`です。Python標準ライブラリを基本とし、入力スナップショット、
manifest、checkpointを使う再開安全性を壊さないでください。CLIや設定を変更した場合はREADMEと
設定例も更新し、`python3 -m unittest discover -s examples/local-files/tests -p 'test_*.py'`で
確認します。外部APIを使う手動確認では秘密情報を設定ファイルやログへ保存しません。
