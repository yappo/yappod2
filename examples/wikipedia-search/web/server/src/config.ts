import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { parse } from "smol-toml";
import type { EmbeddingClientOptions, EmbeddingProfile, EmbeddingProvider } from "./embedding-client.js";

export interface LlmServerConfig {
  baseUrl: string;
  model: string;
  effort?: string;
  authorizationToken?: string;
  timeoutMs: number;
}

export interface WebConfig {
  llm?: LlmServerConfig;
  embedding?: Omit<EmbeddingClientOptions, "fetchImpl">;
}

function table(value: unknown, name: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${name} must be a TOML table`);
  }
  return value as Record<string, unknown>;
}

function onlyKeys(value: Record<string, unknown>, allowed: string[], name: string): void {
  const unknown = Object.keys(value).find((key) => !allowed.includes(key));
  if (unknown) throw new Error(`${name} contains unknown key: ${unknown}`);
}

function requiredString(value: unknown, name: string): string {
  if (typeof value !== "string" || !value.trim()) throw new Error(`${name} must be a non-empty string`);
  return value.trim();
}

function optionalString(value: unknown, name: string): string | undefined {
  if (value === undefined) return undefined;
  return requiredString(value, name);
}

function url(value: unknown, name: string): string {
  const result = requiredString(value, name);
  const parsed = new URL(result);
  if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
    throw new Error(`${name} must use http or https`);
  }
  return result;
}

function integer(value: unknown, defaultValue: number, minimum: number, maximum: number, name: string): number {
  const result = value ?? defaultValue;
  if (typeof result !== "number" || !Number.isSafeInteger(result) || result < minimum || result > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return result;
}

function llmConfig(value: unknown): LlmServerConfig {
  const llm = table(value, "llm");
  onlyKeys(llm, ["base_url", "model", "effort", "authorization_token", "timeout_ms"], "llm");
  return {
    baseUrl: url(llm.base_url, "llm.base_url"),
    model: requiredString(llm.model, "llm.model"),
    effort: optionalString(llm.effort, "llm.effort"),
    authorizationToken: optionalString(llm.authorization_token, "llm.authorization_token"),
    timeoutMs: integer(llm.timeout_ms, 30000, 1000, 600000, "llm.timeout_ms"),
  };
}

function embeddingConfig(value: unknown): Omit<EmbeddingClientOptions, "fetchImpl"> {
  const embedding = table(value, "embedding");
  onlyKeys(embedding, [
    "provider", "base_url", "model", "index_model_id", "dimensions", "profile",
    "authorization_token", "timeout_ms", "batch_size",
  ], "embedding");
  const provider = requiredString(embedding.provider, "embedding.provider");
  if (provider !== "lmstudio" && provider !== "ollama") {
    throw new Error("embedding.provider must be lmstudio or ollama");
  }
  const profile = optionalString(embedding.profile, "embedding.profile") ?? "embeddinggemma";
  if (profile !== "plain" && profile !== "embeddinggemma") {
    throw new Error("embedding.profile must be plain or embeddinggemma");
  }
  return {
    provider: provider as EmbeddingProvider,
    baseUrl: url(embedding.base_url, "embedding.base_url"),
    model: requiredString(embedding.model, "embedding.model"),
    indexModelId: optionalString(embedding.index_model_id, "embedding.index_model_id"),
    dimensions: integer(embedding.dimensions, 768, 1, 65536, "embedding.dimensions"),
    profile: profile as EmbeddingProfile,
    authorizationToken: optionalString(embedding.authorization_token, "embedding.authorization_token"),
    timeoutMs: integer(embedding.timeout_ms, 60000, 1000, 600000, "embedding.timeout_ms"),
    batchSize: integer(embedding.batch_size, 16, 1, 1024, "embedding.batch_size"),
  };
}

export function defaultConfigPath(): string {
  return resolve(process.cwd(), "config.toml");
}

export function configPathFromArgs(args: string[]): string {
  if (args.length === 0) return defaultConfigPath();
  if (args.length === 2 && args[0] === "--config" && args[1]) return resolve(args[1]);
  throw new Error("Usage: node server/dist/index.js [--config PATH]");
}

export async function loadWebConfig(path = defaultConfigPath()): Promise<WebConfig> {
  let source: string;
  try {
    source = await readFile(path, "utf8");
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return {};
    throw new Error(`cannot read web config: ${path}`, { cause: error });
  }
  let root: Record<string, unknown>;
  try {
    root = table(parse(source), "config");
  } catch (error) {
    throw new Error(`invalid web config: ${path}: ${error instanceof Error ? error.message : String(error)}`);
  }
  onlyKeys(root, ["llm", "embedding"], "config");
  return {
    ...(root.llm === undefined ? {} : { llm: llmConfig(root.llm) }),
    ...(root.embedding === undefined ? {} : { embedding: embeddingConfig(root.embedding) }),
  };
}
