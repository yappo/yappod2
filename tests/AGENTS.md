# テストの作業規則

適用範囲は`tests/`です。テストの役割と実装責務の対応は
[`docs/architecture.md`](../docs/architecture.md)を参照してください。

- `common`から`server`までは`src/`と同じ責務へ置き、CLIやインストール後の動作は
  `acceptance`へ置きます。共通fixtureは`support`、品質評価は`quality`、fuzzerは`fuzz`へ置きます。
- CTest helperでは検証対象の内部ライブラリを`LIBRARIES`で指定します。本番Cソースを
  `EXTRA_SOURCES`で再コンパイルせず、`EXTRA_SOURCES`はテスト専用fixtureだけに使います。
- 利用者向け動作を維持する変更では既存CTest名と期待結果を維持します。
- `source_dependency_rules`を回避する例外を追加せず、責務境界が不適切なら実装側の配置または
  依存方向を直します。
- 完了時は`cmake --build build -j`と
  `ctest --test-dir build --output-on-failure`で全CTestを実行します。
