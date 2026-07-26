# search-web exampleの作業規則

適用範囲は`examples/search-web/`です。Node.js 22と`package-lock.json`を基準にし、ブラウザー、
search-webサーバー、Yappod2、埋め込みAPI、LLM APIの境界を混同しないでください。変更完了時は
このディレクトリで`npm run typecheck`、`npm test`、`npm run build`、
`npm run test:e2e`を実行します。外部APIへ接続しないテスト構成を使い、トークンをTOML、
fixture、ログへ保存しません。
