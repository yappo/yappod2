import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { createUsageLogger } from "../src/usage-log.js";

const directories: string[] = [];

afterEach(async () => {
  vi.restoreAllMocks();
  await Promise.all(directories.splice(0).map((directory) => rm(directory, { recursive: true })));
});

describe("usage log", () => {
  it("appends one JSON line per response including model and nullable usage", async () => {
    const directory = await mkdtemp(join(tmpdir(), "yappod-usage-log-"));
    directories.push(directory);
    const path = join(directory, "nested", "usage.jsonl");
    const log = createUsageLogger(path);
    await log({ service: "embedding", operation: "query_embed", provider: "openai", model: "text-embedding-3-small", usage: { total_tokens: 4 } });
    await log({ service: "llm", operation: "rag_answer", provider: "openai-compatible", model: "answer-model", usage: undefined });
    const records = (await readFile(path, "utf8")).trim().split("\n").map((line) => JSON.parse(line));
    expect(records).toHaveLength(2);
    expect(records[0]).toMatchObject({ source: "search-web", model: "text-embedding-3-small", usage: { total_tokens: 4 } });
    expect(records[1]).toMatchObject({ model: "answer-model", usage: null });
  });

  it("warns and continues when append fails", async () => {
    const directory = await mkdtemp(join(tmpdir(), "yappod-usage-log-failure-"));
    directories.push(directory);
    const warning = vi.spyOn(console, "error").mockImplementation(() => undefined);
    await expect(createUsageLogger(directory)({
      service: "embedding", operation: "query_embed", provider: "openai",
      model: "text-embedding-3-small", usage: undefined,
    })).resolves.toBeUndefined();
    expect(warning).toHaveBeenCalledWith(expect.stringContaining("warning: cannot append usage log"));
  });
});
