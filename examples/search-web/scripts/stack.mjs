import { closeSync, existsSync, mkdirSync, openSync, readFileSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawn, spawnSync } from "node:child_process";
import { configPathFromArgs, loadSharedConfig } from "./shared-config.mjs";

const scriptsDir = dirname(fileURLToPath(import.meta.url));
const webDir = resolve(scriptsDir, "..");
const repoRoot = resolve(webDir, "../..");
const pollIntervalMs = 100;

function pidPath(config, name) {
  return resolve(config.daemon.runDirectory, `${name}.pid`);
}

const serviceNames = {
  core: "yappod_core",
  front: "yappod_front",
  web: "Web application",
  "mock-llm": "mock LLM",
};

const serviceCommandMarkers = {
  core: resolve(repoRoot, "build/yappod_core"),
  front: resolve(repoRoot, "build/yappod_front"),
  web: resolve(webDir, "server/dist/index.js"),
  "mock-llm": resolve(webDir, "scripts/mock-llm.mjs"),
};

function info(message) {
  console.log(`[info] ${message}`);
}

function warn(message) {
  console.warn(`[warn] ${message}`);
}

function readPidRecord(path) {
  if (!existsSync(path)) return { state: "missing" };
  let source;
  try {
    source = readFileSync(path, "utf8");
  } catch (error) {
    throw new Error(`Cannot read PID file: ${path}: ${error.message}`);
  }
  if (!/^[1-9][0-9]*\s*$/.test(source)) return { state: "invalid" };
  const pid = Number(source.trim());
  return Number.isSafeInteger(pid) ? { state: "valid", pid } : { state: "invalid" };
}

function alive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === "EPERM";
  }
}

function processCommand(pid) {
  const result = spawnSync("ps", ["-p", String(pid), "-o", "command="], { encoding: "utf8" });
  if (result.error || result.status !== 0 || !result.stdout.trim()) return null;
  return result.stdout.trim();
}

function removeStalePid(path, service, reason, warnMessage) {
  try {
    unlinkSync(path);
  } catch (error) {
    throw new Error(`Cannot remove stale PID file for ${service}: ${path}: ${error.message}`);
  }
  warnMessage(`Removed stale PID file for ${service}: ${path} (${reason})`);
}

export function reconcilePidFile(path, name, dependencies = {}) {
  const service = serviceNames[name];
  const marker = serviceCommandMarkers[name];
  if (!service || !marker) throw new Error(`Unknown stack service: ${name}`);
  const isAlive = dependencies.isAlive ?? alive;
  const commandForPid = dependencies.commandForPid ?? processCommand;
  const warnMessage = dependencies.warn ?? warn;
  const record = readPidRecord(path);
  if (record.state === "missing") return record;
  if (record.state === "invalid") {
    removeStalePid(path, service, "file does not contain a positive integer PID", warnMessage);
    return { state: "removed-invalid" };
  }
  if (!isAlive(record.pid)) {
    removeStalePid(path, service, `PID ${record.pid} is not running`, warnMessage);
    return { state: "removed-dead", pid: record.pid };
  }
  const command = commandForPid(record.pid);
  if (command === null) {
    throw new Error(
      `Cannot verify the process referenced by ${service} PID file: ${path} (PID ${record.pid})`);
  }
  if (!command.includes(marker)) {
    removeStalePid(path, service, `PID ${record.pid} belongs to another process`, warnMessage);
    return { state: "removed-reused", pid: record.pid };
  }
  return { state: "running", pid: record.pid };
}

function runningPid(path) {
  const record = readPidRecord(path);
  return record.state === "valid" && alive(record.pid) ? record.pid : null;
}

async function waitFor(check, attempts = 80) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (await check()) return true;
    await new Promise((resolvePromise) => setTimeout(resolvePromise, pollIntervalMs));
  }
  return false;
}

export async function waitForBackgroundReady(check, pid, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (true) {
    if (!alive(pid)) return "exited";
    const remainingMs = deadline - Date.now();
    if (remainingMs <= 0) return "timeout";
    if (await check(remainingMs)) return "ready";
    await new Promise((resolvePromise) =>
      setTimeout(resolvePromise, Math.min(pollIntervalMs, Math.max(0, deadline - Date.now()))));
  }
}

function logTail(path, maximumLines = 80) {
  if (!existsSync(path)) return "";
  return readFileSync(path, "utf8").trimEnd().split("\n").slice(-maximumLines).join("\n");
}

export function backgroundStartupError(service, status, pid, timeoutMs, healthUrl, errorPath) {
  const reason = status === "exited"
    ? `${service} exited before becoming ready (PID ${pid})`
    : `${service} did not become ready within ${timeoutMs} ms (PID ${pid})`;
  const output = logTail(errorPath);
  return new Error(`${reason}\nhealth check: GET ${healthUrl}` +
    `\nstartup timeout: web.startup_timeout_ms = ${timeoutMs}` +
    `\nerror log: ${errorPath}${
    output ? `\n--- ${service} error output ---\n${output}` : "\n(error log is missing or empty)"
  }`);
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", ...options });
  if (result.error) throw new Error(`cannot run ${command}: ${result.error.message}`);
  if (result.status !== 0) throw new Error(`${command} failed with status ${result.status ?? "unknown"}`);
}

function startBackground(command, args, cwd, stdoutPath, stderrPath) {
  const stdout = openSync(stdoutPath, "a");
  const stderr = openSync(stderrPath, "a");
  const child = spawn(command, args, { cwd, detached: true, stdio: ["ignore", stdout, stderr] });
  closeSync(stdout);
  closeSync(stderr);
  child.unref();
  return child.pid;
}

function buildIndex(config) {
  if (existsSync(config.indexDirectory)) {
    throw new Error(`index path already exists: ${config.indexDirectory}`);
  }
  if (!config.build.input || !config.build.makeindex) {
    throw new Error("build.input and build.yappo_makeindex are required to create an index");
  }
  for (const [name, path] of [
    ["build.input", config.build.input],
    ["build.yappo_makeindex", config.build.makeindex],
  ]) {
    if (!existsSync(path)) throw new Error(`${name} not found: ${path}`);
  }
  run(config.build.makeindex, [
    "build", "--config", config.path,
    "--input", config.build.input,
  ]);
}

async function stopPid(config, name) {
  const path = pidPath(config, name);
  const service = serviceNames[name];
  const record = reconcilePidFile(path, name);
  if (record.state !== "running") return false;
  info(`Stopping ${service} (PID ${record.pid})`);
  process.kill(record.pid, "SIGTERM");
  const stopped = await waitFor(() => !alive(record.pid), 50);
  if (!stopped) {
    warn(`${service} did not stop after SIGTERM; sending SIGKILL (PID ${record.pid})`);
    process.kill(record.pid, "SIGKILL");
    if (!await waitFor(() => !alive(record.pid), 10)) {
      throw new Error(`${service} is still running after SIGKILL (PID ${record.pid})`);
    }
  }
  if (existsSync(path)) unlinkSync(path);
  info(`Stopped ${service} (PID ${record.pid})`);
  return true;
}

async function stop(config, quiet = false) {
  for (const name of ["web", "mock-llm", "front", "core"]) await stopPid(config, name);
  if (!quiet) info("yappod search stack stop completed");
}

async function start(config) {
  const core = resolve(repoRoot, "build/yappod_core");
  const front = resolve(repoRoot, "build/yappod_front");
  if (!existsSync(resolve(config.indexDirectory, "manifest.json"))) {
    if (existsSync(config.indexDirectory)) {
      throw new Error(`index path exists but is not a valid index: ${config.indexDirectory}`);
    }
    buildIndex(config);
  }
  if (!existsSync(core) || !existsSync(front)) {
    throw new Error("yappod binaries not found; run cmake --build build -j at the repository root");
  }
  if (!existsSync(resolve(webDir, "node_modules"))) {
    throw new Error(`Web dependencies not found; run npm install in ${webDir}`);
  }
  mkdirSync(config.daemon.runDirectory, { recursive: true, mode: 0o700 });
  for (const name of ["core", "front", "web", "mock-llm"]) {
    const path = pidPath(config, name);
    const record = reconcilePidFile(path, name);
    if (record.state === "running") {
      throw new Error(
        `${serviceNames[name]} is already running (PID ${record.pid}; PID file: ${path})`);
    }
  }
  run("npm", ["run", "build"], { cwd: webDir });
  let started = false;
  try {
    info("Starting yappod_core");
    run(core, ["--config", config.path]);
    if (!await waitFor(() => {
      return runningPid(pidPath(config, "core")) !== null;
    })) throw new Error("yappod_core did not start");
    info(`Started yappod_core (PID ${runningPid(pidPath(config, "core"))})`);

    info("Starting yappod_front");
    run(front, ["--config", config.path]);
    if (!await waitFor(async () => {
      try {
        const response = await fetch(`http://${config.daemon.frontHost}:${config.daemon.frontPort}/health/ready`);
        return response.ok;
      } catch { return false; }
    })) throw new Error("yappod_front did not become ready");
    info(`yappod_front is ready (PID ${runningPid(pidPath(config, "front"))})`);

    if (config.mock.enabled) {
      info("Starting mock LLM");
      const mockErrorPath = resolve(config.daemon.runDirectory, "mock-llm.error");
      const mockPid = startBackground(
        process.execPath, [resolve(webDir, "scripts/mock-llm.mjs"), "--config", config.path], webDir,
        resolve(config.daemon.runDirectory, "mock-llm.log"),
        mockErrorPath,
      );
      writeFileSync(pidPath(config, "mock-llm"), `${mockPid}\n`, { mode: 0o600 });
      const mockHealthUrl = `http://${config.mock.host}:${config.mock.port}/health`;
      const mockStatus = await waitForBackgroundReady(async (remainingMs) => {
        try {
          return (await fetch(mockHealthUrl, {
            signal: AbortSignal.timeout(Math.min(1000, remainingMs)),
          })).ok;
        } catch { return false; }
      }, mockPid, config.web.startupTimeoutMs);
      if (mockStatus !== "ready") {
        throw backgroundStartupError(
          "mock LLM", mockStatus, mockPid, config.web.startupTimeoutMs, mockHealthUrl, mockErrorPath);
      }
      info(`mock LLM is ready (PID ${mockPid})`);
    }

    info("Starting Web application");
    const webErrorPath = resolve(config.daemon.runDirectory, "web.error");
    const webPid = startBackground(
      process.execPath, [resolve(webDir, "server/dist/index.js"), "--config", config.path], webDir,
      resolve(config.daemon.runDirectory, "web.log"),
      webErrorPath,
    );
    writeFileSync(pidPath(config, "web"), `${webPid}\n`, { mode: 0o600 });
    const webHealthUrl = `http://${config.web.host}:${config.web.port}/api/health`;
    const webStatus = await waitForBackgroundReady(async (remainingMs) => {
      try {
        return (await fetch(webHealthUrl, {
          signal: AbortSignal.timeout(Math.min(1000, remainingMs)),
        })).ok;
      }
      catch { return false; }
    }, webPid, config.web.startupTimeoutMs);
    if (webStatus !== "ready") {
      throw backgroundStartupError(
        "Web application", webStatus, webPid, config.web.startupTimeoutMs, webHealthUrl, webErrorPath);
    }
    started = true;
    info(`yappod search is ready: http://${config.web.host}:${config.web.port}`);
  } finally {
    if (!started) await stop(config, true);
  }
}

function commandLine(configPath, command) {
  const quotedPath = configPath.includes(" ") ? JSON.stringify(configPath) : configPath;
  return `node examples/search-web/scripts/stack.mjs ${command} --config ${quotedPath}`;
}

function actionList(error, command, configPath) {
  const reason = error instanceof Error ? error.message : String(error);
  const lower = reason.toLowerCase();
  const actions = [];
  if (lower.includes("enoent") || lower.includes("no such file") || lower.includes("cannot read")) {
    actions.push(`Check that the --config file and every reported path exist and are readable: ${configPath}`);
    actions.push("Compare the configuration with examples/search-web/config.example.toml.");
  } else if (["must be", "must use", "must not", "is required", "unknown key", "invalid shared config", "must contain"]
    .some((token) => lower.includes(token))) {
    actions.push(`Correct the named setting in ${configPath}.`);
    actions.push("Compare the affected section with examples/search-web/config.example.toml.");
  } else if (lower.includes("index path already exists")) {
    actions.push(`To use the existing index, run \`${commandLine(configPath, "start")}\` instead of build.`);
    actions.push("To rebuild, set index.directory to an unused path or preserve and remove the old generated index first.");
  } else if (lower.includes("not a valid index")) {
    actions.push(`Check that ${configPath} points index.directory to a directory containing manifest.json.`);
    actions.push("Set index.directory to an unused path to build a new index; do not mix files from different builds.");
  } else if (lower.includes("yappod binaries not found") || lower.includes("cannot run")) {
    actions.push("Run `cmake --build build -j` at the repository root.");
    actions.push(`Check command paths in ${configPath}, then retry.`);
  } else if (lower.includes("dependencies not found")) {
    actions.push("Run `npm install` in examples/search-web.");
    actions.push("Re-run the same stack command after installation succeeds.");
  } else if (lower.includes("already running")) {
    actions.push(`Stop the existing stack with \`${commandLine(configPath, "stop")}\`.`);
    actions.push("If it belongs to another configuration, change the daemon and web ports before retrying.");
  } else if (lower.includes("pid file") || lower.includes("sigkill")) {
    actions.push("Inspect the reported PID and process command before changing the PID file.");
    actions.push(`After confirming process ownership, retry \`${commandLine(configPath, command || "start")}\`.`);
  } else if (["did not start", "did not become ready", "exited before", "eaddrinuse", "address already in use"]
    .some((token) => lower.includes(token))) {
    actions.push("Read the error log path shown above; its final lines contain the startup failure.");
    actions.push(`Check daemon.core_port, daemon.front_port, web.port, and startup_timeout_ms in ${configPath}.`);
  } else if (lower.includes("failed with status")) {
    actions.push("Read the preceding command output; it contains the failing build or service diagnostic.");
    actions.push("Fix that command first, then re-run the same stack command.");
  } else if (["permission denied", "eacces", "read-only"].some((token) => lower.includes(token))) {
    actions.push("Check the reported path's parent directory permissions and the configured listen ports.");
  }
  actions.push("Run `node examples/search-web/scripts/stack.mjs build|start|stop --config PATH` to review the command form.");
  return actions;
}

export function formatStackError(error, { command, configPath, unexpected = false } = {}) {
  const reason = error instanceof Error ? (error.message || error.name) : String(error);
  const operation = ["build", "start", "stop"].includes(command) ? command : "run";
  const lines = [
    `search-web: error: cannot ${operation} the example stack`,
    `Reason: ${reason}`,
  ];
  if (configPath) lines.push(`Config: ${configPath}`);
  lines.push("How to fix:");
  actionList(error, command, configPath ?? "the path passed to --config")
    .forEach((action, index) => lines.push(`  ${index + 1}. ${action}`));
  if (unexpected) {
    lines.push("  Debug: re-run with YAPPOD_EXAMPLE_DEBUG=1 to include the JavaScript stack trace.");
  }
  return lines.join("\n");
}

async function main() {
  const [command, ...rest] = process.argv.slice(2);
  const config = await loadSharedConfig(configPathFromArgs(rest));
  if (command === "build") buildIndex(config);
  else if (command === "start") await start(config);
  else if (command === "stop") await stop(config);
  else throw new Error("Usage: stack.mjs build|start|stop --config PATH");
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    await main();
  } catch (error) {
    if (process.env.YAPPOD_EXAMPLE_DEBUG === "1" && error instanceof Error) {
      console.error(error.stack ?? error.message);
    } else {
      const args = process.argv.slice(2);
      const configIndex = args.indexOf("--config");
      const configPath = configIndex >= 0 && args[configIndex + 1]
        ? resolve(args[configIndex + 1])
        : undefined;
      console.error(formatStackError(error, {
        command: args[0],
        configPath,
        unexpected: true,
      }));
    }
    process.exitCode = 1;
  }
}
