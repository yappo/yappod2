import { useCallback, useEffect, useRef, useState, type FormEvent } from "react";
import { webApi, type WebApi } from "./api";
import { RagView } from "./RagView";
import type { RegisterInput, RegisterResponse, SearchResult, StatusResponse } from "./types";

type View = "search" | "ask" | "register";
type RequestState = "idle" | "loading" | "success" | "error";

function visibleUrl(url: string): string {
  return url.replace(/^https?:\/\//, "");
}

function score(value: number): string {
  return Number.isFinite(value) ? value.toFixed(4) : "—";
}

function DaemonStatus({ status, checking, onRetry }: {
  status: StatusResponse | null;
  checking: boolean;
  onRetry: () => void;
}) {
  const ready = status?.ready === true;
  return (
    <div className={`daemon-status ${ready ? "is-ready" : "is-down"}`} role="status">
      <span className="status-dot" aria-hidden="true" />
      <span>{checking ? "接続確認中" : ready ? "接続済み" : "接続できません"}</span>
      {!checking && !ready && <button className="text-button" type="button" onClick={onRetry}>再接続</button>}
    </div>
  );
}

function SearchResultItem({ result }: { result: SearchResult }) {
  return (
    <article className="search-result">
      <h2>
        <a href={result.url} target="_blank" rel="noreferrer">
          {result.title || "無題の文書"}<span className="external-mark" aria-hidden="true">↗</span>
        </a>
      </h2>
      {result.url && <div className="result-url">{visibleUrl(result.url)}</div>}
      <p>{result.snippet || "本文の抜粋はありません。"}</p>
      <details>
        <summary>検索情報</summary>
        <dl className="result-metadata">
          <div><dt>文書ID</dt><dd>{result.document_id}</dd></div>
          <div><dt>Lexical score</dt><dd>{score(result.lexical_score)}</dd></div>
          <div><dt>Fused score</dt><dd>{score(result.fused_score)}</dd></div>
        </dl>
      </details>
    </article>
  );
}

function SearchView({ api, initialQuery }: { api: WebApi; initialQuery: string }) {
  const [input, setInput] = useState(initialQuery);
  const [activeQuery, setActiveQuery] = useState("");
  const [results, setResults] = useState<SearchResult[]>([]);
  const [total, setTotal] = useState(0);
  const [generation, setGeneration] = useState<number | null>(null);
  const [cursor, setCursor] = useState<string | null>(null);
  const [state, setState] = useState<RequestState>("idle");
  const [error, setError] = useState("");
  const inputRef = useRef<HTMLInputElement>(null);

  const runSearch = useCallback(async (query: string, nextCursor?: string) => {
    const normalized = query.trim();
    if (!normalized) {
      setError("検索語句を入力してください");
      setState("error");
      inputRef.current?.focus();
      return;
    }
    setState("loading");
    setError("");
    try {
      const response = await api.search(normalized, 10, nextCursor);
      setResults((current) => nextCursor ? [...current, ...response.results] : response.results);
      setTotal(response.total);
      setGeneration(response.generation);
      setCursor(response.next_cursor);
      setActiveQuery(normalized);
      setState("success");
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "検索に失敗しました");
      setState("error");
    }
  }, [api]);

  useEffect(() => {
    setInput(initialQuery);
    if (initialQuery) void runSearch(initialQuery);
  }, [initialQuery, runSearch]);

  function submit(event: FormEvent) {
    event.preventDefault();
    void runSearch(input);
  }

  return (
    <main id="main-content" className="content search-page">
      <header className="page-heading">
        <p className="eyebrow">記事を探す</p>
        <h1>Wikipediaを検索</h1>
        <p>記事のtitle、出典、本文の抜粋を同じ順序で比較できます。</p>
      </header>

      <form className="search-form" onSubmit={submit} noValidate>
        <label className="sr-only" htmlFor="search-query">検索語句</label>
        <input
          ref={inputRef}
          id="search-query"
          type="search"
          value={input}
          onChange={(event) => setInput(event.target.value)}
          placeholder="例: 情報検索"
          autoComplete="off"
          aria-describedby={error ? "search-error" : undefined}
        />
        <button className="primary-button" type="submit" aria-label="検索を実行" disabled={state === "loading"}>
          {state === "loading" && results.length === 0 ? "検索中…" : "検索"}
        </button>
      </form>

      {error && (
        <div className="message error-message" id="search-error" role="alert">
          <strong>検索できませんでした</strong>
          <span>{error}</span>
          {activeQuery && <button className="text-button" type="button" onClick={() => void runSearch(activeQuery)}>同じ条件で再試行</button>}
        </div>
      )}

      {state === "idle" && (
        <div className="empty-guidance">
          <h2>検索語句を入力してください</h2>
          <p>固有名詞や、2〜3語程度の具体的な語句から始めると比較しやすくなります。</p>
        </div>
      )}

      {state !== "idle" && results.length > 0 && (
        <section aria-labelledby="results-heading">
          <div className="results-summary">
            <h2 id="results-heading">「{activeQuery}」の検索結果</h2>
            <p>{total.toLocaleString("ja-JP")}件<span aria-hidden="true"> · </span>generation {generation}</p>
          </div>
          <div className="results-list">
            {results.map((result) => <SearchResultItem key={`${result.id}:${result.document_id}`} result={result} />)}
          </div>
          {cursor && (
            <div className="load-more">
              <button className="secondary-button" type="button" disabled={state === "loading"} onClick={() => void runSearch(activeQuery, cursor)}>
                {state === "loading" ? "読み込み中…" : "続きを読み込む"}
              </button>
            </div>
          )}
        </section>
      )}

      {state === "success" && results.length === 0 && (
        <div className="empty-guidance" role="status">
          <h2>一致する記事がありません</h2>
          <p>語句を短くするか、別の表記へ変えて検索してください。</p>
        </div>
      )}
    </main>
  );
}

function RegisterView({ api, onSearch }: { api: WebApi; onSearch: (query: string) => void }) {
  const [form, setForm] = useState<RegisterInput>({ id: "", title: "", url: "", body: "" });
  const [state, setState] = useState<RequestState>("idle");
  const [error, setError] = useState("");
  const [result, setResult] = useState<RegisterResponse | null>(null);

  function update(field: keyof RegisterInput, value: string) {
    setForm((current) => ({ ...current, [field]: value }));
  }

  async function submit(event: FormEvent) {
    event.preventDefault();
    const normalized = { ...form, id: form.id.trim(), title: form.title.trim(), url: form.url?.trim() };
    if (!normalized.id || !normalized.title || !normalized.body.trim()) {
      setError("文書ID、title、本文は必須です");
      setState("error");
      return;
    }
    setState("loading");
    setError("");
    setResult(null);
    try {
      const response = await api.registerDocument(normalized);
      setResult(response);
      setState("success");
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "文書登録に失敗しました");
      setState("error");
    }
  }

  return (
    <main id="main-content" className="content register-page">
      <header className="page-heading">
        <p className="eyebrow">indexを更新する</p>
        <h1>文書を登録</h1>
        <p>ローカルindexへ文書を1件追加します。接続情報とwrite tokenはBFFからブラウザへ公開されません。</p>
      </header>

      <form className="document-form" onSubmit={submit} noValidate>
        <div className="field-row">
          <label htmlFor="document-id">文書ID <span>必須</span></label>
          <input id="document-id" value={form.id} onChange={(event) => update("id", event.target.value)} placeholder="manual:example" />
          <p className="field-help">既存IDを指定すると、その文書を新しい内容で更新します。</p>
        </div>
        <div className="field-row">
          <label htmlFor="document-title">title <span>必須</span></label>
          <input id="document-title" value={form.title} onChange={(event) => update("title", event.target.value)} />
        </div>
        <div className="field-row">
          <label htmlFor="document-url">原典URL</label>
          <input id="document-url" type="url" value={form.url} onChange={(event) => update("url", event.target.value)} placeholder="https://example.com/article" />
        </div>
        <div className="field-row">
          <label htmlFor="document-body">本文 <span>必須</span></label>
          <textarea id="document-body" rows={12} value={form.body} onChange={(event) => update("body", event.target.value)} />
          <p className="field-help">HTMLではなく、検索対象にするplain textを入力してください。</p>
        </div>
        {error && <div className="message error-message" role="alert"><strong>登録できませんでした</strong><span>{error}</span></div>}
        <div className="form-actions">
          <button className="primary-button" type="submit" disabled={state === "loading"}>
            {state === "loading" ? "登録中…" : "文書を登録"}
          </button>
        </div>
      </form>

      {result && (
        <div className="message success-message" role="status">
          <strong>文書を登録しました</strong>
          <span>index generation {result.generation}へ反映されています。</span>
          <button className="secondary-button" type="button" onClick={() => onSearch(form.title)}>登録した文書を検索</button>
        </div>
      )}
    </main>
  );
}

export function App({ api = webApi }: { api?: WebApi }) {
  const [view, setView] = useState<View>("search");
  const [searchSeed, setSearchSeed] = useState("");
  const [status, setStatus] = useState<StatusResponse | null>(null);
  const [checking, setChecking] = useState(true);

  const refreshStatus = useCallback(async () => {
    setChecking(true);
    try {
      setStatus(await api.status());
    } catch (caught) {
      setStatus({ ready: false, message: caught instanceof Error ? caught.message : "接続を確認できません" });
    } finally {
      setChecking(false);
    }
  }, [api]);

  useEffect(() => { void refreshStatus(); }, [refreshStatus]);

  function searchRegisteredDocument(query: string) {
    setSearchSeed(query);
    setView("search");
    void refreshStatus();
  }

  return (
    <>
      <a className="skip-link" href="#main-content">本文へ移動</a>
      <header className="site-header">
        <div className="header-inner">
          <button className="brand" type="button" onClick={() => setView("search")} aria-label="検索画面へ">
            <span className="brand-mark" aria-hidden="true">索</span>
            <span>日本語 Wikipedia 索引</span>
          </button>
          <nav aria-label="主要な画面">
            <button type="button" aria-current={view === "search" ? "page" : undefined} onClick={() => setView("search")}>検索</button>
            <button type="button" aria-current={view === "ask" ? "page" : undefined} onClick={() => setView("ask")}>質問</button>
            <button type="button" aria-current={view === "register" ? "page" : undefined} onClick={() => setView("register")}>文書登録</button>
          </nav>
          <DaemonStatus status={status} checking={checking} onRetry={() => void refreshStatus()} />
        </div>
      </header>
      {view === "search" && <SearchView api={api} initialQuery={searchSeed} />}
      {view === "ask" && <RagView api={api} />}
      {view === "register" && <RegisterView api={api} onSearch={searchRegisteredDocument} />}
      <footer className="site-footer">
        <span>yappod Wikipedia search example</span>
        <span>原典のライセンスと帰属は各記事で確認してください。</span>
      </footer>
    </>
  );
}
