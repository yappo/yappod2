import { afterEach, describe, expect, it, vi } from "vitest";
import type { FastifyInstance } from "fastify";
import { buildApp } from "../src/app.js";
import type { EmbeddingClientOptions } from "../src/embedding-client.js";
import type { Citation, RetrieveResponse } from "../src/types.js";

interface CapturedRequest {
  url: string;
  init?: RequestInit;
}

const apps: FastifyInstance[] = [];

afterEach(async () => {
  await Promise.all(apps.splice(0).map((app) => app.close()));
});

function citation(overrides: Partial<Citation> = {}): Citation {
  return {
    passage_id: "passage-1",
    document_id: "doc-1",
    url: "https://ja.wikipedia.org/wiki/情報検索",
    title: "情報検索",
    text: "情報検索は必要な情報を探し出す処理である。",
    start_char: 0,
    end_char: 22,
    context_start: 0,
    context_end: 66,
    lexical_score: 1.2,
    vector_score: 0,
    fused_score: 0.016,
    ...overrides,
  };
}

function retrieval(citations: Citation[] = [citation()]): RetrieveResponse {
  return {
    api_version: 2,
    generation: 7,
    context: citations.map((item) => item.text).join("\n\n"),
    citations,
  };
}

function fakeFetch(handler: (request: CapturedRequest) => Response): typeof fetch {
  return (async (input: string | URL | Request, init?: RequestInit) => {
    const url = input instanceof Request ? input.url : String(input);
    return handler({ url, init });
  }) as typeof fetch;
}

async function ragApp(
  fetchImpl: typeof fetch,
  configured = true,
  embedding?: Omit<EmbeddingClientOptions, "fetchImpl">,
): Promise<FastifyInstance> {
  const app = await buildApp({
    baseUrl: "http://yappod.test",
    timeoutMs: 1000,
    fetchImpl,
    embedding,
    llm: configured ? {
      baseUrl: "http://llm.test/v1",
      model: "mock-model",
      effort: "low",
      authorizationToken: "llm-server-secret",
      timeoutMs: 1000,
      fetchImpl,
    } : undefined,
  });
  apps.push(app);
  return app;
}

describe("citation-grounded RAG BFF", () => {
  it("retrieves passages, calls an OpenAI-compatible endpoint, and validates references", async () => {
    const requests: CapturedRequest[] = [];
    const fetchImpl = fakeFetch((request) => {
      requests.push(request);
      if (request.url === "http://yappod.test/v2/retrieve") return Response.json(retrieval());
      if (request.url === "http://llm.test/v1/chat/completions") {
        return Response.json({ choices: [{ message: { role: "assistant", content: "情報検索は情報を探す処理です。[1]" } }] });
      }
      return Response.json({}, { status: 404 });
    });
    const app = await ragApp(fetchImpl);

    const response = await app.inject({ method: "POST", url: "/api/rag", payload: { question: "情報検索とは？" } });

    expect(response.statusCode).toBe(200);
    expect(response.json()).toMatchObject({
      question: "情報検索とは？",
      answer: "情報検索は情報を探す処理です。[1]",
      referenced_citations: [1],
      generation_status: "answered",
    });
    const retrieveBody = JSON.parse(String(requests[0]?.init?.body));
    expect(retrieveBody).toEqual({
      query: "情報検索とは？",
      mode: "lexical",
      limit: 20,
      max_passages_per_document: 2,
      max_context_bytes: 32768,
    });
    const llmBody = JSON.parse(String(requests[1]?.init?.body));
    expect(llmBody.model).toBe("mock-model");
    expect(llmBody.reasoning_effort).toBe("low");
    expect(llmBody.messages[1].content).toContain("[1] 情報検索");
    expect(llmBody.messages[1].content).toContain("情報検索とは？");
    expect(new Headers(requests[1]?.init?.headers).get("authorization")).toBe("Bearer llm-server-secret");
    expect(response.body).not.toContain("llm-server-secret");
  });

  it("returns retrieval context when the LLM is unconfigured", async () => {
    const fetchImpl = vi.fn(fakeFetch(() => Response.json(retrieval())));
    const app = await ragApp(fetchImpl, false);
    const response = await app.inject({ method: "POST", url: "/api/rag", payload: { question: "情報検索とは？" } });

    expect(response.statusCode).toBe(200);
    expect(response.json()).toMatchObject({ answer: null, generation_status: "unconfigured" });
    expect(response.json().citations).toHaveLength(1);
    expect(fetchImpl).toHaveBeenCalledTimes(1);
  });

  it("uses the same vector mode for retrieval before generation", async () => {
    const requests: CapturedRequest[] = [];
    const fetchImpl = fakeFetch((request) => {
      requests.push(request);
      if (request.url.endsWith("/health/ready")) {
        return Response.json({ ready: true, embedding: { state: "precomputed_ready", model_id: "embeddinggemma", dimensions: 2 } });
      }
      if (request.url === "http://embedding.test/v1/embeddings") {
        return Response.json({ data: [{ index: 0, embedding: [0.4, 0.6] }] });
      }
      if (request.url.endsWith("/v2/retrieve")) return Response.json(retrieval());
      if (request.url.endsWith("/chat/completions")) {
        return Response.json({ choices: [{ message: { content: "意味で取得した回答です。[1]" } }] });
      }
      return Response.json({}, { status: 404 });
    });
    const app = await ragApp(fetchImpl, true, {
      provider: "lmstudio",
      baseUrl: "http://embedding.test/v1",
      model: "embeddinggemma",
      dimensions: 2,
      timeoutMs: 1000,
    });
    const response = await app.inject({
      method: "POST",
      url: "/api/rag",
      payload: { question: "関連する概念は？", mode: "vector" },
    });
    expect(response.statusCode).toBe(200);
    expect(response.json()).toMatchObject({ retrieval_mode: "vector", generation_status: "answered" });
    const retrieve = requests.find((request) => request.url.endsWith("/v2/retrieve"));
    expect(JSON.parse(String(retrieve?.init?.body))).toMatchObject({
      query: "関連する概念は？",
      mode: "vector",
      vector: [0.4, 0.6],
    });
  });

  it("rejects out-of-range and missing citations without losing sources", async () => {
    for (const answer of ["根拠のない回答です。[2]", "参照番号のない回答です。"]) {
      const fetchImpl = fakeFetch(({ url }) => url.includes("retrieve")
        ? Response.json(retrieval())
        : Response.json({ choices: [{ message: { content: answer } }] }));
      const app = await ragApp(fetchImpl);
      const response = await app.inject({ method: "POST", url: "/api/rag", payload: { question: "質問" } });
      expect(response.statusCode).toBe(200);
      expect(response.json()).toMatchObject({ answer: null, referenced_citations: [], generation_status: "invalid_citations" });
      expect(response.json().citations).toHaveLength(1);
      await app.close();
      apps.splice(apps.indexOf(app), 1);
    }
  });

  it("keeps citations when generation fails and skips generation without context", async () => {
    const failureApp = await ragApp(fakeFetch(({ url }) => url.includes("retrieve")
      ? Response.json(retrieval())
      : Response.json({ code: "failed" }, { status: 500 })));
    const failed = await failureApp.inject({ method: "POST", url: "/api/rag", payload: { question: "質問" } });
    expect(failed.json()).toMatchObject({ answer: null, generation_status: "failed" });
    expect(failed.json().citations).toHaveLength(1);

    const noContextFetch = vi.fn(fakeFetch(() => Response.json(retrieval([]))));
    const noContextApp = await ragApp(noContextFetch);
    const empty = await noContextApp.inject({ method: "POST", url: "/api/rag", payload: { question: "質問" } });
    expect(empty.json()).toMatchObject({ answer: null, generation_status: "no_context" });
    expect(noContextFetch).toHaveBeenCalledTimes(1);
  });

  it("rejects an empty question before retrieval", async () => {
    const fetchImpl = vi.fn(fakeFetch(() => Response.json(retrieval())));
    const app = await ragApp(fetchImpl);
    const response = await app.inject({ method: "POST", url: "/api/rag", payload: { question: "   " } });
    expect(response.statusCode).toBe(400);
    expect(fetchImpl).not.toHaveBeenCalled();
  });
});
