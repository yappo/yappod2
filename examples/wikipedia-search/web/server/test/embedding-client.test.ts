import { describe, expect, it, vi } from "vitest";
import { EmbeddingClient, EmbeddingRequestError } from "../src/embedding-client.js";

function fakeFetch(response: Response, inspect?: (url: string, init?: RequestInit) => void): typeof fetch {
  return vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
    inspect?.(input instanceof Request ? input.url : String(input), init);
    return response;
  }) as typeof fetch;
}

describe("EmbeddingClient", () => {
  it("calls LM Studio's OpenAI-compatible endpoint and restores index order", async () => {
    const fetchImpl = fakeFetch(Response.json({ data: [
      { index: 1, embedding: [0, 1] },
      { index: 0, embedding: [1, 0] },
    ] }), (url, init) => {
      expect(url).toBe("http://127.0.0.1:1234/v1/embeddings");
      expect(JSON.parse(String(init?.body))).toEqual({ model: "embeddinggemma", input: ["一", "二"] });
    });
    const client = new EmbeddingClient({
      provider: "lmstudio",
      baseUrl: "http://127.0.0.1:1234/v1",
      model: "embeddinggemma",
      dimensions: 2,
      timeoutMs: 1000,
      fetchImpl,
    });
    await expect(client.embed(["一", "二"])).resolves.toEqual([[1, 0], [0, 1]]);
  });

  it("calls Ollama in bounded batches", async () => {
    const fetchImpl = vi.fn(async (_input: string | URL | Request, init?: RequestInit) => {
      const input = JSON.parse(String(init?.body)).input as string[];
      return Response.json({ embeddings: input.map((_, index) => [index, input.length]) });
    }) as typeof fetch;
    const client = new EmbeddingClient({
      provider: "ollama",
      baseUrl: "http://127.0.0.1:11434",
      model: "embeddinggemma",
      dimensions: 2,
      timeoutMs: 1000,
      batchSize: 2,
      fetchImpl,
    });
    await expect(client.embed(["一", "二", "三"])).resolves.toEqual([[0, 2], [1, 2], [0, 1]]);
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    expect(String(fetchImpl.mock.calls[0]?.[0])).toBe("http://127.0.0.1:11434/api/embed");
  });

  it("applies EmbeddingGemma's retrieval prompts to queries and documents", async () => {
    const bodies: unknown[] = [];
    const fetchImpl = vi.fn(async (_input: string | URL | Request, init?: RequestInit) => {
      const body = JSON.parse(String(init?.body));
      bodies.push(body);
      return Response.json({ embeddings: body.input.map(() => [1, 0]) });
    }) as typeof fetch;
    const client = new EmbeddingClient({
      provider: "ollama",
      baseUrl: "http://127.0.0.1:11434",
      model: "embeddinggemma",
      dimensions: 2,
      timeoutMs: 1000,
      profile: "embeddinggemma",
      fetchImpl,
    });
    await client.embedQueries(["日本の首都"]);
    await client.embedDocuments([{ title: "東京都", text: "東京都は日本の首都である。" }]);
    expect(bodies).toEqual([
      { model: "embeddinggemma", input: ["task: search result | query: 日本の首都"] },
      { model: "embeddinggemma", input: ["title: 東京都 | text: 東京都は日本の首都である。"] },
    ]);
  });

  it("rejects wrong dimensions without exposing an API key", async () => {
    const key = "embedding-server-secret";
    const client = new EmbeddingClient({
      provider: "lmstudio",
      baseUrl: "http://embedding.test/v1",
      model: "embeddinggemma",
      dimensions: 3,
      timeoutMs: 1000,
      authorizationToken: key,
      fetchImpl: fakeFetch(Response.json({ data: [{ index: 0, embedding: [1, 2] }] })),
    });
    try {
      await client.embed(["本文"]);
      throw new Error("expected embed to fail");
    } catch (error) {
      expect(error).toBeInstanceOf(EmbeddingRequestError);
      expect((error as EmbeddingRequestError).code).toBe("embedding_dimension_mismatch");
      expect(String(error)).not.toContain(key);
    }
  });
});
