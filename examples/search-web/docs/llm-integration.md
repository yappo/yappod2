# search-webのLLM連携

この文書では、search-webがRAG向けの参照資料から回答を生成する流れ、`[llm]`の全設定、空回答と期限超過の確認方法を説明します。特定のモデルやOpenAI互換サーバーに共通すると確認できない挙動は断定しません。

## 回答生成の流れ

1. ブラウザーがsearch-webサーバーの`POST /api/rag`へ質問と検索方式を送ります。
2. search-webサーバーは必要なら質問を埋め込みし、`yappod_front`の`QUERY /v2/retrieve`から最大20件の本文断片を取得します。
3. 参照資料が0件なら、LLMを呼ばず`no_context`として資料不足を返します。
4. `[llm]`がなければ、LLMを呼ばず`unconfigured`として参照資料だけを返します。
5. `[llm]`があれば、search-webサーバーが質問と参照資料から`messages`を作り、OpenAI互換の`chat/completions`へ送ります。
6. `choices[0].message.content`を回答として取り出し、`[1]`形式の参照番号を検証します。
7. 存在しない番号を参照した回答は画面へ出さず、取得済みの参照資料だけを残します。

LLMへ接続するのはsearch-webサーバーです。`yappod_core`と`yappod_front`はLLMを読み込まず、回答も生成しません。

## `[llm]`の設定

回答生成を使わない場合はセクション全体を省略します。空の`[llm]`は無効化ではなく設定エラーです。

```toml
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "model-identifier"
effort = "low"
max_tokens = 8192
timeout_ms = 30000
# authorization_token_env = "LLM_API_KEY"
```

| キー | 型 | 必須・デフォルト | search-webサーバーの処理 |
|---|---|---|---|
| `base_url` | 文字列 | 必須 | 末尾へ`chat/completions`を加えます。クエリーとフラグメントは指定できません。 |
| `model` | 文字列 | 必須 | リクエストの`model`へそのまま入れます。接続先が公開する実際のモデル名を指定します。 |
| `effort` | 文字列 | 任意 | 指定した場合だけ`reasoning_effort`として送ります。値の候補をsearch-webサーバーは制限しません。 |
| `max_tokens` | 整数 | デフォルトは`8192`、1〜131072 | 出力トークン上限として、毎回`max_tokens`を明示します。 |
| `timeout_ms` | 整数 | デフォルトは`30000`、1000〜600000 | search-webサーバーが1回のHTTP応答を待つ期限です。生成トークン数の上限ではありません。 |
| `authorization_token_env` | 文字列 | 任意 | 指定した名前の環境変数を起動時に読み、Bearer認証ヘッダーへ入れます。 |

`authorization_token_env`は`[A-Za-z_][A-Za-z0-9_]*`に一致する環境変数名です。未設定または空ならsearch-webサーバーは起動に失敗します。`authorization_token`へトークンを直接書く形式は明示的に拒否します。

`base_url`はHTTPSを使用します。ただしローカルホスト、ループバックアドレス、RFC 1918のプライベートIPv4、ユニークローカルIPv6に限りHTTPも許可します。これは平文通信を安全にする機能ではないため、信頼できないネットワークではHTTPSを使用してください。

## 実際に送るリクエスト

search-webサーバーは概ね次のJSONを送ります。

```json
{
  "model": "model-identifier",
  "reasoning_effort": "low",
  "max_tokens": 8192,
  "messages": [
    {
      "role": "system",
      "content": "参照資料だけに基づいて日本語で回答するための指示"
    },
    {
      "role": "user",
      "content": "質問と番号付き参照資料"
    }
  ],
  "temperature": 0
}
```

`reasoning_effort`は`effort`を省略した場合、リクエストにも含めません。`temperature`は現在常に0です。`[llm].temperature`という設定は実装されていません。

LM Studioなどの画面で別の温度や出力上限を設定していても、search-webサーバーがリクエストに含める`model`、`max_tokens`、`temperature`、任意の`reasoning_effort`がリクエスト値になります。接続先が最終的にどう扱ったかは、接続先の受信ログで確認してください。

## `model`、`model_id`、埋め込みとの違い

`[llm].model`は回答生成へ使うLLMの実名です。索引の互換性には使いません。`[vector].model_id`と`[embedding].model_id`は、検索ベクトルと索引ベクトルが同じ生成規則か確認する識別子です。LLMと埋め込みに同じモデルを使う必要はありません。

## `max_tokens`の意味

`max_tokens = 8192`は、接続先へ最大8192出力トークンを要求する設定です。必ず8192トークンを生成する指定ではありません。入力に含まれる質問と参照資料のトークン数も含みません。接続先のコンテキスト上限、モデル固有の制限、停止条件により、それより短く終わる場合があります。

現行search-webサーバーは`max_tokens`を省略せず、設定値またはデフォルトの8192を毎回送ります。接続先の画面だけを変更しても、このリクエスト値は変わりません。

## `reasoning_effort`の注意

受理する値と効果は、OpenAI互換サーバー、モデル形式、サーバーの版により異なります。search-webサーバーは文字列を検証せずそのまま送るため、接続先が対応しない値ならHTTPエラーになる可能性があります。

推論用出力と最終回答のトークン配分も接続先によって異なります。`effort`を変更した場合は、受信リクエスト、`message.content`、接続先独自の推論フィールド、利用量を実測してください。特定のモデルと接続先で検証していない値や所要時間は、一般的な仕様として扱いません。

## 回答として採用するフィールド

search-webサーバーが採用するのは`choices[0].message.content`だけです。次のいずれかなら失敗です。

- `choices[0]`または`message`がありません。
- `content`が文字列ではありません。
- `content`が空文字列または空白だけです。
- HTTP応答が2xxではありません。
- 応答本文をJSONとして解析できません。
- `timeout_ms`内に応答を取得できません。

`reasoning_content`など接続先独自の推論フィールドは回答に採用しません。`finish_reason: "stop"`だけでも成功とは判断しません。`content`が空なら、終了理由にかかわらず回答本文を取得できなかったものとして扱います。

## 回答生成状態

search-webサーバーの`POST /api/rag`は参照資料を保持したまま、次の状態を返します。

| `generation_status` | 意味 |
|---|---|
| `no_context` | 検索結果に回答へ使える本文断片がありません。LLMは呼びません。 |
| `unconfigured` | `[llm]`がありません。参照資料だけを返します。 |
| `answered` | `content`を取得し、参照番号も検証できました。 |
| `invalid_citations` | 回答が存在しない資料番号を参照したため、回答を表示しません。 |
| `failed` | 通信、HTTP、JSON、空の`content`、期限超過などで回答を利用できません。 |

現行UIでは複数のLLM失敗を`failed`へまとめます。画面だけでは原因を区別できないため、search-webサーバーと接続先のログを確認します。

## 最小の接続確認

まずsearch-webを介さず、接続先が応答するか確認します。次は認証不要なローカルサーバーの例です。

```sh
curl -sS http://127.0.0.1:1234/v1/chat/completions \
  -H 'Content-Type: application/json' \
  --data @- <<'JSON'
{
  "model": "model-identifier",
  "max_tokens": 128,
  "messages": [
    {
      "role": "user",
      "content": "日本語で一文だけ回答してください。"
    }
  ],
  "temperature": 0
}
JSON
```

外部APIではトークンをTOMLやコマンド履歴へ直接書かないでください。たとえば環境変数を利用します。

```sh
curl -sS https://llm.example.test/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $LLM_API_KEY" \
  --data @- <<'JSON'
{
  "model": "model-identifier",
  "max_tokens": 128,
  "messages": [
    {
      "role": "user",
      "content": "日本語で一文だけ回答してください。"
    }
  ],
  "temperature": 0
}
JSON
```

shell historyやプロセス一覧へ秘密情報が残る可能性も考慮し、実運用では利用するシェルと秘密管理の手順に従ってください。

## 空回答の切り分け

次の順で確認します。

1. LM Studioなどの接続先とsearch-webサーバーの両方が起動していることを確認します。
2. search-webサーバーが実際に読み込んだTOMLと、起動しているリポジトリの複製をPIDとコマンドラインで確認します。
3. 接続先の受信ログで`model`、`max_tokens`、`temperature: 0`、任意の`reasoning_effort`を確認します。
4. HTTP状態、応答JSON、`choices[0].message.content`を確認します。
5. 接続先独自の`reasoning_content`などへだけ出力されていないか確認します。
6. `finish_reason`、`usage.prompt_tokens`、`usage.completion_tokens`を別々に記録します。
7. リクエスト開始から応答または切断までの実時間と`llm.timeout_ms`を比較します。
8. 同じ`messages`を小さな`max_tokens`で直接送るなど、最小リクエストとRAGリクエストの差を確認します。

`completion_tokens: 0`は出力トークンが記録されていないことを示しますが、その理由を単独で特定する情報ではありません。サーバーの生成ログ、クライアント切断、モデル状態も確認してください。

## 30秒の期限と初回生成

デフォルトの`timeout_ms`は30000ミリ秒です。モデルの読み込み直後や長いコンテキストでは、それを超える可能性があります。実測した生成時間が30秒を超え、接続先がその後に正常な`content`を返すことを確認できた場合は、たとえば次のように運用値を長くします。

```toml
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "model-identifier"
max_tokens = 8192
timeout_ms = 120000
```

`120000`は設定可能な例であり、実装のデフォルトではありません。期限を長くするだけでは空の`content`や不正な`reasoning_effort`は直りません。

## 利用量ログ

`[usage_log].path`を設定すると、search-webサーバーはLLM応答の`usage`を、サービス名、操作、プロバイダー、モデル、時刻とともにJSONLへ追記します。接続先が`usage`を返さない場合も、接続自体の成功を保証する値にはなりません。トークンやリクエスト本文はこの利用量ログへ書きません。

## `src`、`dist`、ビルド、再起動

TypeScriptの編集元は`server/src`、配布用に実行するJavaScriptは`server/dist`です。`start.sh`は毎回`npm run build`を実行してから`server/dist/index.js`を起動します。手動で`npm start`する場合は、先に索引作成が必要です。

```sh
cd examples/search-web
npm run typecheck
npm test
npm run build
```

起動済みプロセスは、ファイルを編集しただけでは入れ替わりません。編集後に同じ設定で停止してから起動します。`server/src`だけを直して古い`dist`を実行していないか、逆に`dist`だけを手編集して次のビルドで消えていないかを確認してください。複数の複製がある場合は、PIDのコマンドラインが編集対象の絶対パスを指すことも確認します。
