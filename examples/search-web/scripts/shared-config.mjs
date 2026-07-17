import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { parse } from "smol-toml";

function object(value, name) {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    throw new Error(`${name} must be a TOML table`);
  }
  return value;
}

function text(value, fallback, name) {
  if (value === undefined) return fallback;
  if (typeof value !== "string" || !value.trim()) throw new Error(`${name} must be a non-empty string`);
  return value.trim();
}

function integer(value, fallback, minimum, maximum, name) {
  const result = value ?? fallback;
  if (!Number.isSafeInteger(result) || result < minimum || result > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return result;
}

function flag(value, fallback, name) {
  const result = value ?? fallback;
  if (typeof result !== "boolean") throw new Error(`${name} must be a boolean`);
  return result;
}

export function configPathFromArgs(args) {
  if (args.length === 2 && args[0] === "--config" && args[1]) return resolve(args[1]);
  throw new Error("Usage: stack.mjs build|start|stop --config PATH");
}

export async function loadRawConfig(path) {
  return object(parse(await readFile(path, "utf8")), "config");
}

export async function loadSharedConfig(path) {
  const root = await loadRawConfig(path);
  const configDir = dirname(path);
  const build = object(root.build ?? {}, "build");
  const index = object(root.index ?? {}, "index");
  const daemon = object(root.daemon ?? {}, "daemon");
  const web = object(root.web ?? {}, "web");
  const mock = object(root.mock ?? {}, "mock");
  const indexValue = text(index.directory, undefined, "index.directory");
  if (!indexValue) throw new Error("index.directory is required to start the Web stack");
  const inputValue = text(build.input, undefined, "build.input");
  const makeindexValue = text(build.yappo_makeindex, undefined, "build.yappo_makeindex");
  return {
    path,
    indexDirectory: resolve(configDir, indexValue),
    build: {
      input: inputValue === undefined ? undefined : resolve(configDir, inputValue),
      makeindex: makeindexValue === undefined ? undefined : resolve(configDir, makeindexValue),
    },
    daemon: {
      runDirectory: resolve(configDir, text(daemon.run_directory, "./run", "daemon.run_directory")),
      coreHost: text(daemon.core_host, "127.0.0.1", "daemon.core_host"),
      corePort: integer(daemon.core_port, 18401, 1, 65535, "daemon.core_port"),
      frontHost: text(daemon.front_host, "127.0.0.1", "daemon.front_host"),
      frontPort: integer(daemon.front_port, 18400, 1, 65535, "daemon.front_port"),
    },
    web: {
      host: text(web.host, "127.0.0.1", "web.host"),
      port: integer(web.port, 4173, 1, 65535, "web.port"),
    },
    mock: {
      enabled: flag(mock.enabled, false, "mock.enabled"),
      host: text(mock.host, "127.0.0.1", "mock.host"),
      port: integer(mock.port, 1234, 1, 65535, "mock.port"),
      model: text(mock.model, "yappod-demo-mock", "mock.model"),
      answer: text(mock.answer, "参照資料から確認できる内容です。[1]", "mock.answer"),
      embeddingDimensions: integer(mock.embedding_dimensions, 3, 1, 65536, "mock.embedding_dimensions"),
    },
  };
}
