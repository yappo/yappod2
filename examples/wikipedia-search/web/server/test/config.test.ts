import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { afterEach, describe, expect, it } from "vitest";
import { configPathFromArgs, loadWebConfig } from "../src/config.js";

const directories: string[] = [];

async function configFile(source: string): Promise<string> {
  const directory = await mkdtemp(join(tmpdir(), "yappod-web-config-"));
  directories.push(directory);
  const path = join(directory, "config.toml");
  await writeFile(path, source, { mode: 0o600 });
  return path;
}

afterEach(async () => {
  await Promise.all(directories.splice(0).map((directory) => rm(directory, { recursive: true })));
});

describe("web config", () => {
  it("treats a missing config file as an unconfigured web application", async () => {
    const path = join(tmpdir(), `missing-yappod-config-${process.pid}.toml`);
    await expect(loadWebConfig(path)).resolves.toEqual({});
  });

  it("loads embedding settings separately from LLM settings", async () => {
    const path = await configFile(`
[embedding]
provider = "lmstudio"
base_url = "http://127.0.0.1:1234/v1"
model = "embedding-model"
index_model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
profile = "embeddinggemma"
authorization_token = "embedding-secret"
timeout_ms = 45000
batch_size = 8
`);
    await expect(loadWebConfig(path)).resolves.toEqual({
      embedding: {
        provider: "lmstudio",
        baseUrl: "http://127.0.0.1:1234/v1",
        model: "embedding-model",
        indexModelId: "embeddinggemma-300m-768-local-v1",
        dimensions: 768,
        profile: "embeddinggemma",
        authorizationToken: "embedding-secret",
        timeoutMs: 45000,
        batchSize: 8,
      },
    });
  });

  it("applies embedding defaults and rejects invalid embedding settings", async () => {
    const defaults = await configFile("[embedding]\nprovider='ollama'\nbase_url='http://localhost:11434'\nmodel='embeddinggemma'\n");
    await expect(loadWebConfig(defaults)).resolves.toEqual({
      embedding: {
        provider: "ollama",
        baseUrl: "http://localhost:11434",
        model: "embeddinggemma",
        dimensions: 768,
        profile: "embeddinggemma",
        timeoutMs: 60000,
        batchSize: 16,
      },
    });
    const invalid = await configFile("[embedding]\nprovider='other'\nbase_url='http://localhost:1'\nmodel='m'\n");
    await expect(loadWebConfig(invalid)).rejects.toThrow("embedding.provider must be lmstudio or ollama");
  });

  it("loads model, effort, timeout and authorization token from TOML", async () => {
    const path = await configFile(`
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "local-model"
effort = "low"
authorization_token = "server-secret"
timeout_ms = 45000
`);
    await expect(loadWebConfig(path)).resolves.toEqual({
      llm: {
        baseUrl: "http://127.0.0.1:1234/v1",
        model: "local-model",
        effort: "low",
        authorizationToken: "server-secret",
        timeoutMs: 45000,
      },
    });
  });

  it("rejects unknown keys and incomplete LLM settings", async () => {
    const unknown = await configFile("[llm]\nbase_url='http://localhost:1234/v1'\nmodel='m'\ntokne='x'\n");
    await expect(loadWebConfig(unknown)).rejects.toThrow("unknown key: tokne");
    const incomplete = await configFile("[llm]\nbase_url='http://localhost:1234/v1'\n");
    await expect(loadWebConfig(incomplete)).rejects.toThrow("llm.model");
  });

  it("accepts only an optional --config argument", () => {
    expect(configPathFromArgs(["--config", "settings.toml"])).toBe(join(process.cwd(), "settings.toml"));
    expect(() => configPathFromArgs(["--unknown"])).toThrow("Usage:");
  });
});
