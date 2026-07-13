import { useRef, useState, type FormEvent, type ReactNode } from "react";
import type { WebApi } from "./api";
import type { Citation, RagResponse } from "./types";

type RequestState = "idle" | "loading" | "success" | "error";

function visibleUrl(url: string): string {
  return url.replace(/^https?:\/\//, "");
}

function AnswerText({ answer }: { answer: string }) {
  const parts = answer.split(/(\[\d+\])/g);
  return (
    <p className="answer-text">
      {parts.map((part, index): ReactNode => {
        const match = /^\[(\d+)\]$/.exec(part);
        if (!match) return part;
        const number = Number.parseInt(match[1] ?? "", 10);
        return <a key={`${part}:${index}`} className="citation-reference" href={`#citation-${number}`} aria-label={`参照資料${number}へ`}>{part}</a>;
      })}
    </p>
  );
}

function CitationItem({ citation, number, referenced }: {
  citation: Citation;
  number: number;
  referenced: boolean;
}) {
  return (
    <article className="citation-item" id={`citation-${number}`}>
      <div className="citation-number" aria-hidden="true">{number}</div>
      <div className="citation-body">
        <div className="citation-heading">
          <h3>
            {citation.url
              ? <a href={citation.url} target="_blank" rel="noreferrer">{citation.title || "無題の資料"}<span className="external-mark" aria-hidden="true">↗</span></a>
              : citation.title || "無題の資料"}
          </h3>
          {referenced && <span className="source-use">回答で参照</span>}
        </div>
        {citation.url && <div className="result-url">{visibleUrl(citation.url)}</div>}
        <p>{citation.text}</p>
        <details>
          <summary>資料情報</summary>
          <dl className="citation-metadata">
            <div><dt>Passage ID</dt><dd>{citation.passage_id}</dd></div>
            <div><dt>本文位置</dt><dd>{citation.start_char}–{citation.end_char}</dd></div>
            <div><dt>Lexical score</dt><dd>{citation.lexical_score.toFixed(4)}</dd></div>
          </dl>
        </details>
      </div>
    </article>
  );
}

export function RagView({ api }: { api: WebApi }) {
  const [question, setQuestion] = useState("");
  const [state, setState] = useState<RequestState>("idle");
  const [error, setError] = useState("");
  const [result, setResult] = useState<RagResponse | null>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);

  async function submit(event: FormEvent) {
    event.preventDefault();
    const normalized = question.trim();
    if (!normalized) {
      setError("質問を入力してください");
      setState("error");
      inputRef.current?.focus();
      return;
    }
    setState("loading");
    setError("");
    setResult(null);
    try {
      setResult(await api.ask(normalized));
      setState("success");
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "参照資料を取得できませんでした");
      setState("error");
    }
  }

  const referenced = new Set(result?.referenced_citations ?? []);

  return (
    <main id="main-content" className="content rag-page">
      <header className="page-heading">
        <p className="eyebrow">根拠を確認する</p>
        <h1>資料に基づいて質問</h1>
        <p>Wikipediaから関連箇所を取得し、回答と参照資料を同じ画面で確認します。</p>
      </header>

      <form className="question-form" onSubmit={submit} noValidate>
        <label htmlFor="rag-question">質問</label>
        <textarea
          ref={inputRef}
          id="rag-question"
          rows={3}
          value={question}
          onChange={(event) => setQuestion(event.target.value)}
          placeholder="例: 日本の情報検索技術はどのように発展しましたか？"
          aria-describedby={error ? "rag-error" : "rag-help"}
        />
        <div className="question-actions">
          <p id="rag-help">回答中の[1]は、下の参照資料番号に対応します。</p>
          <button className="primary-button" type="submit" disabled={state === "loading"}>
            {state === "loading" ? "資料を確認中…" : "資料を調べる"}
          </button>
        </div>
      </form>

      {error && (
        <div className="message error-message" id="rag-error" role="alert">
          <strong>参照資料を取得できませんでした</strong>
          <span>{error}</span>
        </div>
      )}

      {state === "idle" && (
        <div className="empty-guidance">
          <h2>確認したいことを質問してください</h2>
          <p>回答生成が未設定でも、検索された本文と出典は確認できます。</p>
        </div>
      )}

      {result && (
        <div className="rag-result">
          <section className="answer-section" aria-labelledby="answer-heading">
            <div className="section-heading">
              <p className="section-kicker">質問</p>
              <p className="question-text">{result.question}</p>
              <h2 id="answer-heading">根拠に基づく回答</h2>
            </div>
            {result.generation_status === "answered" && result.answer
              ? <AnswerText answer={result.answer} />
              : result.generation_status === "no_context"
                ? <div className="message neutral-message" role="status"><strong>回答できる資料がありません</strong><span>{result.generation_message}</span></div>
                : <div className="message warning-message" role="status"><strong>回答は表示していません</strong><span>{result.generation_message}</span></div>}
          </section>

          {result.context && (
            <details className="context-details">
              <summary>取得したcontextを確認</summary>
              <pre>{result.context}</pre>
            </details>
          )}

          <section className="citations-section" aria-labelledby="citations-heading">
            <div className="citations-title">
              <h2 id="citations-heading">参照資料</h2>
              <p>{result.citations.length}件<span aria-hidden="true"> · </span>generation {result.generation}</p>
            </div>
            {result.citations.length > 0
              ? <div className="citation-list">{result.citations.map((citation, index) => (
                  <CitationItem
                    key={citation.passage_id}
                    citation={citation}
                    number={index + 1}
                    referenced={referenced.has(index + 1)}
                  />
                ))}</div>
              : <p className="no-citations">参照資料は取得されませんでした。</p>}
          </section>
        </div>
      )}
    </main>
  );
}
