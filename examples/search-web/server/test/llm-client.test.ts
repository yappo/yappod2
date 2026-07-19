import { describe, expect, it, vi } from "vitest";
import { OpenAICompatibleClient } from "../src/llm-client.js";

describe("OpenAI-compatible LLM client", () => {
  it("records model and returned usage after a successful completion", async () => {
    const usageLog = vi.fn(async () => undefined);
    const fetchImpl = vi.fn(async () => Response.json({
      choices: [{ message: { content: "回答です。[1]" } }],
      usage: { prompt_tokens: 10, completion_tokens: 4, total_tokens: 14 },
    })) as typeof fetch;
    const client = new OpenAICompatibleClient({
      baseUrl: "https://api.openai.com/v1",
      model: "answer-model",
      maxTokens: 8192,
      timeoutMs: 1000,
      usageLog,
      fetchImpl,
    });
    await expect(client.answer("質問", [{
      passage_id: "p1", document_id: "d1", url: "https://example.test", title: "資料",
      text: "本文", start_char: 0, end_char: 2, context_start: 0, context_end: 2,
      lexical_score: 1, vector_score: 0, fused_score: 1,
    }])).resolves.toBe("回答です。[1]");
    expect(usageLog).toHaveBeenCalledWith(expect.objectContaining({
      service: "llm",
      model: "answer-model",
      usage: { prompt_tokens: 10, completion_tokens: 4, total_tokens: 14 },
    }));
    const request = JSON.parse(String(fetchImpl.mock.calls[0]?.[1]?.body));
    expect(request.max_tokens).toBe(8192);
  });
});
