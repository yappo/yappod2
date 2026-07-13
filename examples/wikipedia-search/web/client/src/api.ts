import type { RegisterInput, RegisterResponse, SearchResponse, StatusResponse } from "./types";

export class ApiError extends Error {
  constructor(public readonly code: string, message: string, public readonly status: number) {
    super(message);
    this.name = "ApiError";
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, init);
  } catch {
    throw new ApiError("bff_unavailable", "Webサーバーに接続できません", 503);
  }
  let body: unknown;
  try {
    body = await response.json();
  } catch {
    throw new ApiError("invalid_response", "Webサーバーから不正な応答を受け取りました", 502);
  }
  if (!response.ok) {
    const error = body as { code?: unknown; message?: unknown };
    throw new ApiError(
      typeof error.code === "string" ? error.code : "request_failed",
      typeof error.message === "string" ? error.message : "要求を処理できませんでした",
      response.status,
    );
  }
  return body as T;
}

export interface WebApi {
  status(): Promise<StatusResponse>;
  search(query: string, limit: number, cursor?: string): Promise<SearchResponse>;
  registerDocument(input: RegisterInput): Promise<RegisterResponse>;
}

export const webApi: WebApi = {
  status: () => request<StatusResponse>("/api/status"),
  search: (query, limit, cursor) => request<SearchResponse>("/api/search", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ query, limit, ...(cursor ? { cursor } : {}) }),
  }),
  registerDocument: (input) => request<RegisterResponse>("/api/documents", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(input),
  }),
};
