# wikipedia-search exampleの作業規則

適用範囲は`examples/wikipedia-search/`です。Python標準ライブラリによる変換、原子的な出力公開、
チェックサム検証を維持してください。CLIや設定を変更した場合はREADMEと設定例も更新し、
`python3 -m unittest discover -s examples/wikipedia-search/tests -p 'test_*.py'`で確認します。
ネットワークを使う確認とローカルfixtureによる確認は結果を分けて記録します。
