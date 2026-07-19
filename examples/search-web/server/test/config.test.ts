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
  delete process.env.YAPPOD_TEST_EMBEDDING_TOKEN;
  delete process.env.YAPPOD_TEST_LLM_TOKEN;
  delete process.env.YAPPOD_TEST_OPENAI_TOKEN;
  await Promise.all(directories.splice(0).map((directory) => rm(directory, { recursive: true })));
});

describe("web config", () => {
  it("loads the Web startup timeout independently from the yappod request timeout", async () => {
    const path = await configFile("[web]\nyappod_timeout_ms=5000\nstartup_timeout_ms=30000\n");
    await expect(loadWebConfig(path)).resolves.toMatchObject({
      web: { yappodTimeoutMs: 5000, startupTimeoutMs: 30000 },
    });
  });

  it("requires the shared config file", async () => {
    const path = join(tmpdir(), `missing-yappod-config-${process.pid}.toml`);
    await expect(loadWebConfig(path)).rejects.toThrow("cannot read shared config");
  });

  it("loads embedding settings separately from LLM settings", async () => {
    process.env.YAPPOD_TEST_EMBEDDING_TOKEN = "embedding-secret";
    const path = await configFile(`
[embedding]
provider = "lmstudio"
base_url = "http://127.0.0.1:1234/v1"
model = "embedding-model"
model_id = "embeddinggemma-300m-768-local-v1"
dimensions = 768
prompt_profile = "embeddinggemma"
authorization_token_env = "YAPPOD_TEST_EMBEDDING_TOKEN"
timeout_ms = 45000
batch_size = 8
`);
    await expect(loadWebConfig(path)).resolves.toMatchObject({
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
    await expect(loadWebConfig(defaults)).resolves.toMatchObject({
      embedding: {
        provider: "ollama",
        baseUrl: "http://localhost:11434",
        model: "embeddinggemma",
        dimensions: 768,
        profile: "plain",
        timeoutMs: 60000,
        batchSize: 16,
      },
    });
    const invalid = await configFile("[embedding]\nprovider='other'\nbase_url='http://localhost:1'\nmodel='m'\n");
    await expect(loadWebConfig(invalid)).rejects.toThrow("embedding.provider must be lmstudio, ollama, or openai");
  });

  it("loads model, effort, timeout and authorization token from the named environment variable", async () => {
    process.env.YAPPOD_TEST_LLM_TOKEN = "server-secret";
    const path = await configFile(`
[llm]
base_url = "http://127.0.0.1:1234/v1"
model = "local-model"
effort = "low"
authorization_token_env = "YAPPOD_TEST_LLM_TOKEN"
timeout_ms = 45000
`);
    await expect(loadWebConfig(path)).resolves.toMatchObject({
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
    const removedSchemaVersion = await configFile("schema_version=1\n");
    await expect(loadWebConfig(removedSchemaVersion)).rejects.toThrow("unknown key: schema_version");
    const unknown = await configFile("[llm]\nbase_url='http://localhost:1234/v1'\nmodel='m'\ntokne='x'\n");
    await expect(loadWebConfig(unknown)).rejects.toThrow("unknown key: tokne");
    const incomplete = await configFile("[llm]\nbase_url='http://localhost:1234/v1'\n");
    await expect(loadWebConfig(incomplete)).rejects.toThrow("llm.model");
  });

  it("loads OpenAI endpoints and usage logs while enforcing secure remote URLs", async () => {
    process.env.YAPPOD_TEST_OPENAI_TOKEN = "openai-secret";
    const path = await configFile(`
[embedding]
provider = "openai"
endpoint_url = "https://api.openai.com/v1/embeddings"
model = "text-embedding-3-small"
dimensions = 768
prompt_profile = "plain"
authorization_token_env = "YAPPOD_TEST_OPENAI_TOKEN"

[usage_log]
path = "logs/usage.jsonl"
`);
    await expect(loadWebConfig(path)).resolves.toMatchObject({
      embedding: {
        provider: "openai",
        endpointUrl: "https://api.openai.com/v1/embeddings",
        model: "text-embedding-3-small",
        dimensions: 768,
        authorizationToken: "openai-secret",
      },
      usageLogPath: join(path, "..", "logs/usage.jsonl"),
    });

    const insecure = await configFile("[embedding]\nprovider='openai'\nendpoint_url='http://api.openai.com/v1/embeddings'\nmodel='m'\n");
    await expect(loadWebConfig(insecure)).rejects.toThrow("must use https");
  });

  it("rejects plaintext tokens and allows only explicit private HTTP ranges", async () => {
    const plaintext = await configFile("[embedding]\nprovider='openai'\nendpoint_url='https://api.openai.com/v1/embeddings'\nmodel='m'\nauthorization_token='secret'\n");
    await expect(loadWebConfig(plaintext)).rejects.toThrow("use embedding.authorization_token_env");
    const insecureLlm = await configFile("[llm]\nbase_url='http://api.openai.com/v1'\nmodel='m'\n");
    await expect(loadWebConfig(insecureLlm)).rejects.toThrow("must use https");

    for (const endpoint of [
      "http://localhost:1/v1", "http://127.255.0.1:1/v1", "http://10.2.3.4:1/v1",
      "http://172.31.0.1:1/v1", "http://192.168.1.2:1/v1", "http://[::1]:1/v1",
      "http://[fd12::1]:1/v1", "https://api.example.com/v1",
    ]) {
      const allowed = await configFile(`[embedding]\nprovider='openai'\nendpoint_url='${endpoint}'\nmodel='m'\n`);
      await expect(loadWebConfig(allowed)).resolves.toBeDefined();
    }
    for (const endpoint of [
      "http://example.com/v1", "http://8.8.8.8/v1", "http://169.254.1.1/v1", "http://[fe80::1]/v1",
    ]) {
      const rejected = await configFile(`[embedding]\nprovider='openai'\nendpoint_url='${endpoint}'\nmodel='m'\n`);
      await expect(loadWebConfig(rejected)).rejects.toThrow("must use https");
    }
  });

  it("accepts config and development arguments", () => {
    expect(configPathFromArgs(["--config", "settings.toml"])).toEqual({
      path: join(process.cwd(), "settings.toml"), development: false,
    });
    expect(configPathFromArgs(["--development", "--config", "settings.toml"]).development).toBe(true);
    expect(() => configPathFromArgs(["--unknown"])).toThrow("Usage:");
  });
});
