import { readFile } from "node:fs/promises";
import { isIP } from "node:net";
import { dirname, resolve } from "node:path";
import { parse } from "smol-toml";
import type { EmbeddingClientOptions, EmbeddingProfile, EmbeddingProvider } from "./embedding-client.js";

export interface LlmServerConfig {
  baseUrl: string;
  model: string;
  effort?: string;
  authorizationToken?: string;
  timeoutMs: number;
}

export interface DaemonConfig {
  runDirectory: string;
  coreHost: string;
  corePort: number;
  frontHost: string;
  frontPort: number;
  maxInflight: number;
  maxInflightBytes: number;
  requestTimeoutMs: number;
  writeToken?: string;
}

export interface WebServerConfig {
  host: string;
  port: number;
  yappodTimeoutMs: number;
}

export interface MockConfig {
  enabled: boolean;
  host: string;
  port: number;
  model: string;
  answer: string;
  embeddingDimensions: number;
}

export interface WebConfig {
  daemon: DaemonConfig;
  web: WebServerConfig;
  indexDirectory?: string;
  llm?: LlmServerConfig;
  embedding?: Omit<EmbeddingClientOptions, "fetchImpl">;
  usageLogPath?: string;
  mock: MockConfig;
}

function table(value: unknown, name: string): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${name} must be a TOML table`);
  }
  return value as Record<string, unknown>;
}

function optionalTable(value: unknown, name: string): Record<string, unknown> {
  return value === undefined ? {} : table(value, name);
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

function boolean(value: unknown, defaultValue: boolean, name: string): boolean {
  const result = value ?? defaultValue;
  if (typeof result !== "boolean") throw new Error(`${name} must be a boolean`);
  return result;
}

function httpHostAllowed(hostname: string): boolean {
  const host = hostname.replace(/^\[|\]$/g, "").replace(/\.$/, "").toLowerCase();
  if (host === "localhost") return true;
  if (isIP(host) === 4) {
    const parts = host.split(".").map(Number);
    return parts[0] === 127 || parts[0] === 10 ||
      (parts[0] === 172 && (parts[1] ?? 0) >= 16 && (parts[1] ?? 0) <= 31) ||
      (parts[0] === 192 && parts[1] === 168);
  }
  if (isIP(host) === 6) {
    if (host === "::1") return true;
    const first = Number.parseInt(host.split(":")[0] || "0", 16);
    return (first & 0xfe00) === 0xfc00;
  }
  return false;
}

function url(value: unknown, name: string, base = false): string {
  const result = requiredString(value, name);
  const parsed = new URL(result);
  if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
    throw new Error(`${name} must use http or https`);
  }
  if (parsed.protocol === "http:" && !httpHostAllowed(parsed.hostname)) {
    throw new Error(`${name} must use https outside localhost, loopback, or private networks`);
  }
  if (base && (parsed.search || parsed.hash)) {
    throw new Error(`${name} must not contain a query or fragment`);
  }
  return base ? result.replace(/\/$/, "") : result;
}

function integer(value: unknown, defaultValue: number, minimum: number, maximum: number, name: string): number {
  const result = value ?? defaultValue;
  if (typeof result !== "number" || !Number.isSafeInteger(result) || result < minimum || result > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return result;
}

function authorizationToken(value: Record<string, unknown>, name: string): string | undefined {
  if (value.authorization_token !== undefined) {
    throw new Error(`${name}.authorization_token is not supported; use ${name}.authorization_token_env`);
  }
  const environmentName = optionalString(value.authorization_token_env, `${name}.authorization_token_env`);
  if (!environmentName) return undefined;
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(environmentName)) {
    throw new Error(`${name}.authorization_token_env must be an environment variable name`);
  }
  const token = process.env[environmentName]?.trim();
  if (!token) throw new Error(`environment variable ${environmentName} is not set or empty`);
  return token;
}

function llmConfig(value: unknown): LlmServerConfig {
  const llm = table(value, "llm");
  onlyKeys(llm, ["base_url", "model", "effort", "authorization_token", "authorization_token_env", "timeout_ms"], "llm");
  return {
    baseUrl: url(llm.base_url, "llm.base_url", true),
    model: requiredString(llm.model, "llm.model"),
    effort: optionalString(llm.effort, "llm.effort"),
    authorizationToken: authorizationToken(llm, "llm"),
    timeoutMs: integer(llm.timeout_ms, 30000, 1000, 600000, "llm.timeout_ms"),
  };
}

function embeddingConfig(value: unknown): Omit<EmbeddingClientOptions, "fetchImpl"> {
  const embedding = table(value, "embedding");
  onlyKeys(embedding, [
    "directory", "provider", "base_url", "endpoint_url", "model", "model_id", "dimensions",
    "prompt_profile", "authorization_token", "authorization_token_env", "timeout_ms", "batch_size",
  ], "embedding");
  const provider = requiredString(embedding.provider, "embedding.provider");
  if (provider !== "lmstudio" && provider !== "ollama" && provider !== "openai") {
    throw new Error("embedding.provider must be lmstudio, ollama, or openai");
  }
  if ((embedding.base_url === undefined) === (embedding.endpoint_url === undefined)) {
    throw new Error("embedding must specify exactly one of base_url or endpoint_url");
  }
  const profile = optionalString(embedding.prompt_profile, "embedding.prompt_profile") ?? "plain";
  if (profile !== "plain" && profile !== "embeddinggemma") {
    throw new Error("embedding.prompt_profile must be plain or embeddinggemma");
  }
  return {
    provider: provider as EmbeddingProvider,
    ...(embedding.base_url === undefined ? {} : { baseUrl: url(embedding.base_url, "embedding.base_url", true) }),
    ...(embedding.endpoint_url === undefined ? {} : { endpointUrl: url(embedding.endpoint_url, "embedding.endpoint_url") }),
    model: requiredString(embedding.model, "embedding.model"),
    indexModelId: optionalString(embedding.model_id, "embedding.model_id"),
    dimensions: integer(embedding.dimensions, 768, 1, 65536, "embedding.dimensions"),
    profile: profile as EmbeddingProfile,
    authorizationToken: authorizationToken(embedding, "embedding"),
    timeoutMs: integer(embedding.timeout_ms, 60000, 1000, 600000, "embedding.timeout_ms"),
    batchSize: integer(embedding.batch_size, 16, 1, 1024, "embedding.batch_size"),
  };
}

function daemonConfig(value: unknown, configDir: string): DaemonConfig {
  const daemon = optionalTable(value, "daemon");
  onlyKeys(daemon, [
    "run_directory", "core_host", "core_port", "front_host", "front_port", "max_inflight",
    "max_inflight_bytes", "request_timeout_ms", "write_token",
  ], "daemon");
  const writeToken = optionalString(daemon.write_token, "daemon.write_token");
  if (writeToken && (writeToken.length < 16 || writeToken.length > 255 || /[\u0000-\u0020\u007f]/.test(writeToken))) {
    throw new Error("daemon.write_token must contain 16 to 255 non-whitespace bytes");
  }
  return {
    runDirectory: resolve(configDir, optionalString(daemon.run_directory, "daemon.run_directory") ?? "./run"),
    coreHost: optionalString(daemon.core_host, "daemon.core_host") ?? "127.0.0.1",
    corePort: integer(daemon.core_port, 18401, 1, 65535, "daemon.core_port"),
    frontHost: optionalString(daemon.front_host, "daemon.front_host") ?? "127.0.0.1",
    frontPort: integer(daemon.front_port, 18400, 1, 65535, "daemon.front_port"),
    maxInflight: integer(daemon.max_inflight, 4, 1, 1024, "daemon.max_inflight"),
    maxInflightBytes: integer(daemon.max_inflight_bytes, 4194304, 1, 1073741824, "daemon.max_inflight_bytes"),
    requestTimeoutMs: integer(daemon.request_timeout_ms, 5000, 1, 60000, "daemon.request_timeout_ms"),
    writeToken,
  };
}

function webServerConfig(value: unknown): WebServerConfig {
  const web = optionalTable(value, "web");
  onlyKeys(web, ["host", "port", "yappod_timeout_ms"], "web");
  return {
    host: optionalString(web.host, "web.host") ?? "127.0.0.1",
    port: integer(web.port, 4173, 1, 65535, "web.port"),
    yappodTimeoutMs: integer(web.yappod_timeout_ms, 5000, 1, 600000, "web.yappod_timeout_ms"),
  };
}

function mockConfig(value: unknown): MockConfig {
  const mock = optionalTable(value, "mock");
  onlyKeys(mock, ["enabled", "host", "port", "model", "answer", "embedding_dimensions"], "mock");
  return {
    enabled: boolean(mock.enabled, false, "mock.enabled"),
    host: optionalString(mock.host, "mock.host") ?? "127.0.0.1",
    port: integer(mock.port, 1234, 1, 65535, "mock.port"),
    model: optionalString(mock.model, "mock.model") ?? "yappod-demo-mock",
    answer: optionalString(mock.answer, "mock.answer") ?? "参照資料から確認できる内容です。[1]",
    embeddingDimensions: integer(mock.embedding_dimensions, 3, 1, 65536, "mock.embedding_dimensions"),
  };
}

export function defaultConfigPath(): string {
  return resolve(process.cwd(), "config.toml");
}

export function configPathFromArgs(args: string[]): { path: string; development: boolean } {
  let path: string | undefined;
  let development = false;
  for (let index = 0; index < args.length; index += 1) {
    if (args[index] === "--development") {
      development = true;
    } else if (args[index] === "--config" && args[index + 1]) {
      path = resolve(args[index + 1] as string);
      index += 1;
    } else {
      throw new Error("Usage: node server/dist/index.js --config PATH [--development]");
    }
  }
  return { path: path ?? defaultConfigPath(), development };
}

export async function loadWebConfig(path = defaultConfigPath()): Promise<WebConfig> {
  let source: string;
  try {
    source = await readFile(path, "utf8");
  } catch (error) {
    throw new Error(`cannot read shared config: ${path}`, { cause: error });
  }
  let root: Record<string, unknown>;
  try {
    root = table(parse(source), "config");
  } catch (error) {
    throw new Error(`invalid shared config: ${path}: ${error instanceof Error ? error.message : String(error)}`);
  }
  onlyKeys(root, [
    "format_version", "collection_id", "index", "tokenizer", "chunking",
    "vector", "metadata", "input", "output", "prepare", "embedding", "usage_log",
    "build", "extract", "formatters", "daemon", "web", "llm", "mock",
  ], "config");
  const configDir = dirname(path);
  const usageLog = optionalTable(root.usage_log, "usage_log");
  onlyKeys(usageLog, ["path"], "usage_log");
  const usageLogPath = usageLog.path === undefined
    ? undefined
    : resolve(configDir, requiredString(usageLog.path, "usage_log.path"));
  const index = optionalTable(root.index, "index");
  onlyKeys(index, ["directory"], "index");
  const indexDirectory = index.directory === undefined
    ? undefined
    : resolve(configDir, requiredString(index.directory, "index.directory"));
  return {
    daemon: daemonConfig(root.daemon, configDir),
    web: webServerConfig(root.web),
    indexDirectory,
    ...(root.llm === undefined ? {} : { llm: llmConfig(root.llm) }),
    ...(root.embedding === undefined ? {} : { embedding: embeddingConfig(root.embedding) }),
    ...(usageLogPath === undefined ? {} : { usageLogPath }),
    mock: mockConfig(root.mock),
  };
}
