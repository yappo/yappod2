import { afterEach, describe, expect, it } from "vitest";
import type { FastifyInstance } from "fastify";
import { buildApp } from "../src/app.js";

interface CapturedRequest {
  url: string;
  init?: RequestInit;
}

const apps: FastifyInstance[] = [];

afterEach(async () => {
  await Promise.all(apps.splice(0).map((app) => app.close()));
});

function fakeFetch(handler: (request: CapturedRequest) => Response): typeof fetch {
  return (async (input: string | URL | Request, init?: RequestInit) => {
    const url = input instanceof Request ? input.url : String(input);
    return handler({ url, init });
  }) as typeof fetch;
}

async function appWith(fetchImpl: typeof fetch, writeToken?: string): Promise<FastifyInstance> {
  const app = await buildApp({
    baseUrl: "http://127.0.0.1:10080",
    writeToken,
    timeoutMs: 1000,
    fetchImpl,
  });
  apps.push(app);
  return app;
}

describe("Wikipedia search BFF", () => {
  it("normalizes daemon readiness without exposing connection settings", async () => {
    const app = await appWith(fakeFetch(({ url }) => {
      expect(url).toBe("http://127.0.0.1:10080/health/ready");
      return Response.json({ ready: true, generation: 4, state: "precomputed_ready" });
    }), "server-only-secret");

    const response = await app.inject({ method: "GET", url: "/api/status" });
    expect(response.statusCode).toBe(200);
    expect(response.json()).toEqual({ ready: true, generation: 4, state: "precomputed_ready", llm_configured: false });
    expect(response.body).not.toContain("server-only-secret");
    expect(response.body).not.toContain("10080");
  });

  it("forwards lexical search and its opaque cursor", async () => {
    const requests: CapturedRequest[] = [];
    const app = await appWith(fakeFetch((request) => {
      requests.push(request);
      return Response.json({ generation: 8, total: 1, results: [], next_cursor: null });
    }));

    const response = await app.inject({
      method: "POST",
      url: "/api/search",
      payload: { query: "情報検索", limit: 10, cursor: "v1.8.10.digest" },
    });

    expect(response.statusCode).toBe(200);
    expect(requests[0]?.url).toBe("http://127.0.0.1:10080/v2/search");
    expect(JSON.parse(String(requests[0]?.init?.body))).toEqual({
      query: "情報検索",
      mode: "lexical",
      scope: "documents",
      limit: 10,
      cursor: "v1.8.10.digest",
    });
  });

  it("keeps the write token in the BFF and builds one canonical upsert", async () => {
    const requests: CapturedRequest[] = [];
    const app = await appWith(fakeFetch((request) => {
      requests.push(request);
      return Response.json({ generation: 9, accepted: 1, upserts: 1, deletes: 0 });
    }), "0123456789abcdef-secret");

    const response = await app.inject({
      method: "POST",
      url: "/api/documents",
      payload: { id: "manual:1", title: "登録文書", url: "https://example.test/1", body: "検索できる本文" },
    });

    expect(response.statusCode).toBe(200);
    expect(new Headers(requests[0]?.init?.headers).get("authorization")).toBe("Bearer 0123456789abcdef-secret");
    expect(JSON.parse(String(requests[0]?.init?.body))).toEqual({ operations: [{
      operation: "upsert",
      id: "manual:1",
      title: "登録文書",
      url: "https://example.test/1",
      body: "検索できる本文",
      metadata: { language: "ja", source: "manual" },
    }] });
    expect(response.body).not.toContain("0123456789abcdef-secret");
  });

  it("separates invalid input, daemon outage, and write authorization errors", async () => {
    let calls = 0;
    const invalidApp = await appWith(fakeFetch(() => { calls += 1; return Response.json({}); }));
    const invalid = await invalidApp.inject({ method: "POST", url: "/api/search", payload: { query: "   " } });
    expect(invalid.statusCode).toBe(400);
    expect(calls).toBe(0);

    const outageApp = await appWith((async () => { throw new Error("offline"); }) as typeof fetch);
    const outage = await outageApp.inject({ method: "POST", url: "/api/search", payload: { query: "検索" } });
    expect(outage.statusCode).toBe(503);
    expect(outage.json().code).toBe("daemon_unavailable");

    const unauthorizedApp = await appWith(fakeFetch(() => Response.json(
      { code: "unauthorized", message: "raw upstream message" },
      { status: 401 },
    )));
    const unauthorized = await unauthorizedApp.inject({
      method: "POST",
      url: "/api/documents",
      payload: { id: "manual:1", title: "文書", body: "本文" },
    });
    expect(unauthorized.statusCode).toBe(401);
    expect(unauthorized.json().message).toContain("write token");
  });
});
