import type { ReadyResponse, RegisterResponse, RetrieveResponse, SearchResponse } from "./types.js";

export interface SearchInput {
  query: string;
  limit: number;
  cursor?: string;
}

export interface DocumentInput {
  id: string;
  title: string;
  url?: string;
  body: string;
}

export interface YappodClientOptions {
  baseUrl: string;
  writeToken?: string;
  timeoutMs: number;
  fetchImpl?: typeof fetch;
}

export class YappodRequestError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    message: string,
  ) {
    super(message);
    this.name = "YappodRequestError";
  }
}

function errorMessage(body: unknown, fallback: string): string {
  if (typeof body !== "object" || body === null) return fallback;
  const candidate = (body as { message?: unknown }).message;
  return typeof candidate === "string" && candidate.length > 0 ? candidate : fallback;
}

export class YappodClient {
  private readonly fetchImpl: typeof fetch;

  constructor(private readonly options: YappodClientOptions) {
    this.fetchImpl = options.fetchImpl ?? globalThis.fetch;
  }

  private async request<T>(path: string, init?: RequestInit): Promise<T> {
    let response: Response;
    try {
      response = await this.fetchImpl(new URL(path, this.options.baseUrl), {
        ...init,
        signal: AbortSignal.timeout(this.options.timeoutMs),
      });
    } catch {
      throw new YappodRequestError(503, "daemon_unavailable", "yappodに接続できません");
    }

    let body: unknown;
    try {
      body = await response.json();
    } catch {
      throw new YappodRequestError(502, "invalid_daemon_response", "yappodから不正な応答を受け取りました");
    }
    if (!response.ok) {
      const code = typeof body === "object" && body !== null &&
        typeof (body as { code?: unknown }).code === "string"
        ? (body as { code: string }).code
        : "daemon_error";
      throw new YappodRequestError(response.status, code, errorMessage(body, "yappodが要求を処理できませんでした"));
    }
    return body as T;
  }

  status(): Promise<ReadyResponse> {
    return this.request<ReadyResponse>("/health/ready");
  }

  search(input: SearchInput): Promise<SearchResponse> {
    return this.request<SearchResponse>("/v2/search", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        query: input.query,
        mode: "lexical",
        scope: "documents",
        limit: input.limit,
        ...(input.cursor ? { cursor: input.cursor } : {}),
      }),
    });
  }

  retrieve(question: string): Promise<RetrieveResponse> {
    return this.request<RetrieveResponse>("/v2/retrieve", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        query: question,
        mode: "lexical",
        limit: 8,
        max_passages_per_document: 2,
        max_context_bytes: 16384,
      }),
    });
  }

  registerDocument(input: DocumentInput): Promise<RegisterResponse> {
    const headers: Record<string, string> = { "content-type": "application/json" };
    if (this.options.writeToken) headers.authorization = `Bearer ${this.options.writeToken}`;
    return this.request<RegisterResponse>("/v2/documents:batch", {
      method: "POST",
      headers,
      body: JSON.stringify({
        operations: [{
          operation: "upsert",
          id: input.id,
          title: input.title,
          ...(input.url ? { url: input.url } : {}),
          body: input.body,
          metadata: { language: "ja", source: "manual" },
        }],
      }),
    });
  }
}
