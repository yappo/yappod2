import { closeSync, existsSync, mkdirSync, openSync, readFileSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawn, spawnSync } from "node:child_process";
import { configPathFromArgs, loadSharedConfig } from "./shared-config.mjs";

const scriptsDir = dirname(fileURLToPath(import.meta.url));
const webDir = resolve(scriptsDir, "..");
const repoRoot = resolve(webDir, "../..");

function pidPath(config, name) {
  return resolve(config.daemon.runDirectory, `${name}.pid`);
}

function readPid(path) {
  if (!existsSync(path)) return null;
  const value = Number.parseInt(readFileSync(path, "utf8").trim(), 10);
  return Number.isSafeInteger(value) && value > 0 ? value : null;
}

function alive(pid) {
  try { process.kill(pid, 0); return true; } catch { return false; }
}

async function waitFor(check, attempts = 80) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (await check()) return true;
    await new Promise((resolvePromise) => setTimeout(resolvePromise, 100));
  }
  return false;
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", ...options });
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
  const pid = readPid(path);
  if (pid === null) return;
  if (alive(pid)) {
    process.kill(pid, "SIGTERM");
    const stopped = await waitFor(() => !alive(pid), 50);
    if (!stopped) {
      process.kill(pid, "SIGKILL");
      await waitFor(() => !alive(pid), 10);
    }
  }
  if (existsSync(path)) unlinkSync(path);
}

async function stop(config, quiet = false) {
  for (const name of ["web", "mock-llm", "front", "core"]) await stopPid(config, name);
  if (!quiet) console.log("yappod search stack stopped");
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
    if (existsSync(path)) throw new Error(`PID file already exists: ${path}`);
  }
  run("npm", ["run", "build"], { cwd: webDir });
  let started = false;
  try {
    run(core, ["--config", config.path]);
    if (!await waitFor(() => {
      const pid = readPid(pidPath(config, "core"));
      return pid !== null && alive(pid);
    })) throw new Error("yappod_core did not start");

    run(front, ["--config", config.path]);
    if (!await waitFor(async () => {
      try {
        const response = await fetch(`http://${config.daemon.frontHost}:${config.daemon.frontPort}/health/ready`);
        return response.ok;
      } catch { return false; }
    })) throw new Error("yappod_front did not become ready");

    if (config.mock.enabled) {
      const mockPid = startBackground(
        process.execPath, [resolve(webDir, "scripts/mock-llm.mjs"), "--config", config.path], webDir,
        resolve(config.daemon.runDirectory, "mock-llm.log"),
        resolve(config.daemon.runDirectory, "mock-llm.error"),
      );
      writeFileSync(pidPath(config, "mock-llm"), `${mockPid}\n`, { mode: 0o600 });
      if (!await waitFor(async () => {
        try { return (await fetch(`http://${config.mock.host}:${config.mock.port}/health`)).ok; }
        catch { return false; }
      })) throw new Error("mock LLM did not become ready");
    }

    const webPid = startBackground(
      process.execPath, [resolve(webDir, "server/dist/index.js"), "--config", config.path], webDir,
      resolve(config.daemon.runDirectory, "web.log"),
      resolve(config.daemon.runDirectory, "web.error"),
    );
    writeFileSync(pidPath(config, "web"), `${webPid}\n`, { mode: 0o600 });
    if (!await waitFor(async () => {
      try { return (await fetch(`http://${config.web.host}:${config.web.port}/api/health`)).ok; }
      catch { return false; }
    })) throw new Error("Web application did not become ready");
    started = true;
    console.log(`yappod search is ready: http://${config.web.host}:${config.web.port}`);
  } finally {
    if (!started) await stop(config, true);
  }
}

const [command, ...rest] = process.argv.slice(2);
const config = await loadSharedConfig(configPathFromArgs(rest));
if (command === "build") buildIndex(config);
else if (command === "start") await start(config);
else if (command === "stop") await stop(config);
else throw new Error("Usage: stack.mjs build|start|stop --config PATH");
