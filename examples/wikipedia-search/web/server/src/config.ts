import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { parse } from "smol-toml";

export interface LlmServerConfig {
  baseUrl: string;
  model: string;
  effort?: string;
  authorizationToken?: string;
  timeoutMs: number;
}

export interface WebConfig {
  llm?: LlmServerConfig;
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

function llmConfig(value: unknown): LlmServerConfig {
  const llm = table(value, "llm");
  onlyKeys(llm, ["base_url", "model", "effort", "authorization_token", "timeout_ms"], "llm");
  const baseUrl = requiredString(llm.base_url, "llm.base_url");
  const url = new URL(baseUrl);
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw new Error("llm.base_url must use http or https");
  }
  const timeout = llm.timeout_ms ?? 30000;
  if (typeof timeout !== "number" || !Number.isSafeInteger(timeout) || timeout < 1000 || timeout > 600000) {
    throw new Error("llm.timeout_ms must be an integer from 1000 to 600000");
  }
  return {
    baseUrl,
    model: requiredString(llm.model, "llm.model"),
    effort: optionalString(llm.effort, "llm.effort"),
    authorizationToken: optionalString(llm.authorization_token, "llm.authorization_token"),
    timeoutMs: timeout,
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
  onlyKeys(root, ["llm"], "config");
  return root.llm === undefined ? {} : { llm: llmConfig(root.llm) };
}
