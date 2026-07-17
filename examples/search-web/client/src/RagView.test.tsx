import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import type { WebApi } from "./api";
import { RagView } from "./RagView";
import type { RagResponse } from "./types";

function response(overrides: Partial<RagResponse> = {}): RagResponse {
  return {
    generation: 7,
    question: "情報検索とは？",
    retrieval_mode: "lexical",
    context: "情報検索は必要な情報を探し出す処理である。",
    citations: [{
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
    }],
    answer: "情報検索は必要な情報を探す処理です。[1]",
    referenced_citations: [1],
    generation_status: "answered",
    ...overrides,
  };
}

function api(result: RagResponse): WebApi {
  return {
    status: vi.fn().mockResolvedValue({ ready: true }),
    search: vi.fn(),
    registerDocument: vi.fn(),
    ask: vi.fn().mockResolvedValue(result),
  };
}

describe("RAG document UI", () => {
  it("shows a grounded answer and links [1] to its source", async () => {
    const mockApi = api(response());
    const user = userEvent.setup();
    render(<RagView api={mockApi} llmConfigured />);

    expect(screen.getByText("LLM server設定済み")).toBeVisible();

    await user.type(screen.getByLabelText("質問"), "情報検索とは？");
    await user.click(screen.getByRole("button", { name: "資料を調べる" }));

    expect(await screen.findByText("情報検索は必要な情報を探す処理です。", { exact: false })).toBeVisible();
    expect(screen.getByRole("link", { name: "参照資料1へ" })).toHaveAttribute("href", "#citation-1");
    expect(screen.getByRole("link", { name: /情報検索/ })).toHaveAttribute("href", "https://ja.wikipedia.org/wiki/情報検索");
    expect(screen.getByText("回答で参照")).toBeVisible();
    expect(mockApi.ask).toHaveBeenCalledWith("情報検索とは？", "lexical");
  });

  it("keeps retrieved sources visible when generation is unconfigured", async () => {
    const mockApi = api(response({
      answer: null,
      referenced_citations: [],
      generation_status: "unconfigured",
      generation_message: "回答生成は未設定です。取得した参照資料を確認できます",
    }));
    const user = userEvent.setup();
    render(<RagView api={mockApi} />);

    expect(screen.getByText("未設定（参照資料のみ表示）")).toBeVisible();

    await user.type(screen.getByLabelText("質問"), "情報検索とは？");
    await user.click(screen.getByRole("button", { name: "資料を調べる" }));

    expect(await screen.findByText("回答生成は未設定です。取得した参照資料を確認できます")).toBeVisible();
    expect(within(screen.getByRole("article")).getByText("情報検索は必要な情報を探し出す処理である。")).toBeVisible();
    expect(screen.getByRole("heading", { name: "参照資料" })).toBeVisible();
  });
});
