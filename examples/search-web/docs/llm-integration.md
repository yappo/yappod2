# search-webのLLM連携

## 構成

回答生成サービスへ接続するのは`yappod_core`や`yappod_front`ではなく、Node.js/Fastifyで実装されたBFFです。
BFFは`/v2/retrieve`から参照資料を取得し、質問と参照資料をOpenAI互換の`/v1/chat/completions`へ送ります。

```text
ブラウザー → search-web BFF → yappod_front → yappod_core
                         ↘ Chat Completions互換LLM
```

## 設定

```toml
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "model-identifier"
effort = "low"
max_tokens = 8192
timeout_ms = 30000
# authorization_token_env = "LLM_API_KEY"
```

| キー | 条件 | 説明 |
|---|---|---|
| `base_url` | 必須 | `chat/completions`を加える基準URLです。プライベートネットワーク以外のHTTP接続は拒否します。 |
| `model` | 必須 | 接続先が公開するモデル名です。 |
| `effort` | 任意 | `reasoning_effort`として送ります。接続先が受理する値を指定します。 |
| `max_tokens` | 1〜131072、既定値8192 | 出力トークン数の上限として毎回送ります。 |
| `timeout_ms` | 1000〜600000、既定値30000 | BFFが1回の生成を待つ時間です。 |
| `authorization_token_env` | 任意 | Bearerトークンを読む環境変数名です。 |

現在のBFFは`temperature: 0`を必ずリクエストへ含めます。LM Studioなどの画面で別の値を指定しても、BFFからの
リクエストでは0が使われます。`[llm].temperature`は実装されていません。

## 接続先の画面設定との関係

BFFがリクエストへ含める`model`、`max_tokens`、`temperature`は、接続先の画面で選んだ既定値より優先されます。
`effort`を設定した場合は`reasoning_effort`も明示して送ります。省略した場合は、この項目をリクエストへ含めません。
接続先が受理する`reasoning_effort`の値は製品やモデルにより異なるため、接続先の受信ログとレスポンスを確認してください。

`timeout_ms`の既定値は30000ミリ秒です。モデルの読み込み直後や長い参照資料を渡す場合は、回答生成が30秒を超える
可能性があります。これはBFF側の待ち時間であり、接続先の生成上限ではありません。実際の生成時間を測ったうえで、
必要な場合だけ運用設定を長くしてください。既定値そのものは30秒のままです。

## 回答として採用するフィールド

BFFが画面へ表示するのは`choices[0].message.content`です。文字列が存在しない場合や空白だけの場合は失敗として
扱います。`reasoning_content`は回答本文として採用しません。`finish_reason`だけを成功判定には使いません。

モデルや互換サーバーによってフィールドの扱いが異なる可能性があります。この文書では特定モデルの一般仕様を断定せず、
実際のレスポンスを確認する方法を説明します。

## 空の回答を調べる

1. LM Studioなどのサーバー側で受信リクエストを開き、`max_tokens`、`reasoning_effort`、`temperature`を確認します。
2. レスポンスの`content`、`reasoning_content`、`finish_reason`を別々に確認します。
3. `usage.prompt_tokens`、`completion_tokens`と実際の生成時間を確認します。
4. 生成時間が`llm.timeout_ms`を超えていないか確認します。
5. BFFの`.error`と利用量のログを確認します。

通信失敗、期限超過、HTTPエラー、不正なJSON、空の`content`は利用者画面で似た失敗表示になるため、BFFとLLMの
両方のログを確認してください。

## curlで接続先を確認する

トークンが不要なローカルサーバーの例です。モデル名とURLは実際の設定に合わせます。

```sh
curl -sS http://127.0.0.1:1234/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model-identifier","max_tokens":128,"messages":[{"role":"user","content":"日本語で短く回答してください。"}]}'
```

外部APIを使う場合はトークンをコマンド履歴へ直接残さず、環境変数からヘッダーへ渡してください。

## `src`、`dist`、再起動

TypeScriptの実装は`server/src`、実行されるJavaScriptはビルド後の`server/dist`にあります。`src`を編集しただけでは
起動中のプロセスへ反映されません。

```sh
cd examples/search-web
npm run typecheck
npm test
npm run build
```

ビルド後、対象の設定で起動したBFFを再起動します。別の複製にある`dist`を起動していないか、PIDとコマンドラインも確認します。
