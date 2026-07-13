import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { App } from "./App";
import type { WebApi } from "./api";
import type { SearchResponse } from "./types";

function searchResponse(overrides: Partial<SearchResponse> = {}): SearchResponse {
  return {
    generation: 7,
    total: 2,
    results: [{
      id: "jawiki:1",
      document_id: "jawiki:1",
      title: "情報検索",
      url: "https://ja.wikipedia.org/wiki/情報検索",
      snippet: "情報検索は、情報を探し出す処理である。",
      lexical_score: 1.2,
      vector_score: 0,
      fused_score: 0.016,
    }],
    next_cursor: "v1.7.10.digest",
    ...overrides,
  };
}

function api(overrides: Partial<WebApi> = {}): WebApi {
  return {
    status: vi.fn().mockResolvedValue({ ready: true, generation: 7 }),
    search: vi.fn().mockResolvedValue(searchResponse()),
    registerDocument: vi.fn().mockResolvedValue({ generation: 8, accepted: 1, upserts: 1, deletes: 0 }),
    ...overrides,
  };
}

describe("Wikipedia search UI", () => {
  it("searches, compares results, and follows cursor pagination", async () => {
    const secondPage = searchResponse({
      results: [{
        id: "jawiki:2",
        document_id: "jawiki:2",
        title: "検索エンジン",
        url: "https://ja.wikipedia.org/wiki/検索エンジン",
        snippet: "検索エンジンは情報を検索するシステムである。",
        lexical_score: 0.8,
        vector_score: 0,
        fused_score: 0.015,
      }],
      next_cursor: null,
    });
    const mockApi = api({ search: vi.fn().mockResolvedValueOnce(searchResponse()).mockResolvedValueOnce(secondPage) });
    const user = userEvent.setup();
    render(<App api={mockApi} />);

    await user.type(screen.getByRole("searchbox", { name: "検索語句" }), "情報検索");
    await user.click(screen.getByRole("button", { name: "検索を実行" }));

    expect(await screen.findByRole("link", { name: /情報検索/ })).toHaveAttribute("href", "https://ja.wikipedia.org/wiki/情報検索");
    expect(screen.getByText("情報検索は、情報を探し出す処理である。")).toBeVisible();
    await user.click(screen.getByRole("button", { name: "続きを読み込む" }));
    expect(await screen.findByRole("link", { name: /検索エンジン/ })).toBeVisible();
    expect(mockApi.search).toHaveBeenNthCalledWith(2, "情報検索", 10, "v1.7.10.digest");
  });

  it("shows separate empty, validation, and daemon states", async () => {
    const mockApi = api({ status: vi.fn().mockResolvedValue({ ready: false }), search: vi.fn().mockResolvedValue(searchResponse({ total: 0, results: [], next_cursor: null })) });
    const user = userEvent.setup();
    render(<App api={mockApi} />);

    expect(await screen.findByText("接続できません")).toBeVisible();
    await user.click(screen.getByRole("button", { name: "検索を実行" }));
    expect(screen.getByText("検索語句を入力してください")).toBeVisible();
    await user.type(screen.getByRole("searchbox", { name: "検索語句" }), "該当なし");
    await user.click(screen.getByRole("button", { name: "検索を実行" }));
    expect(await screen.findByText("一致する記事がありません")).toBeVisible();
  });

  it("registers a document and searches it without exposing credentials", async () => {
    const mockApi = api();
    const user = userEvent.setup();
    render(<App api={mockApi} />);

    await user.click(screen.getByRole("button", { name: "文書登録" }));
    await user.type(screen.getByLabelText(/文書ID/), "manual:ux");
    await user.type(screen.getByLabelText(/title/), "操作設計");
    await user.type(screen.getByLabelText(/原典URL/), "https://example.test/ux");
    await user.type(screen.getByLabelText(/本文/), "利用者の行動を基準に画面を設計する。長い日本語本文にも対応する。");
    await user.click(screen.getByRole("button", { name: "文書を登録" }));

    expect(await screen.findByText("文書を登録しました")).toBeVisible();
    expect(mockApi.registerDocument).toHaveBeenCalledWith({
      id: "manual:ux",
      title: "操作設計",
      url: "https://example.test/ux",
      body: "利用者の行動を基準に画面を設計する。長い日本語本文にも対応する。",
    });
    await user.click(screen.getByRole("button", { name: "登録した文書を検索" }));
    await waitFor(() => expect(mockApi.search).toHaveBeenCalledWith("操作設計", 10, undefined));
  });
});
