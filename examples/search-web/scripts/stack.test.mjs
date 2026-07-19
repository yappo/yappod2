import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { backgroundStartupError, reconcilePidFile, waitForBackgroundReady } from "./stack.mjs";
import { loadSharedConfig } from "./shared-config.mjs";

const directories = [];

afterEach(() => {
  for (const directory of directories.splice(0)) rmSync(directory, { recursive: true });
});

function temporaryDirectory() {
  const directory = mkdtempSync(join(tmpdir(), "yappod-stack-test-"));
  directories.push(directory);
  return directory;
}

describe("search Web stack startup", () => {
  it("removes invalid and dead PID files with warnings", () => {
    const directory = temporaryDirectory();
    const invalidPath = join(directory, "core.pid");
    const deadPath = join(directory, "front.pid");
    const warnings = [];
    writeFileSync(invalidPath, "not-a-pid\n");
    writeFileSync(deadPath, "12345\n");

    expect(reconcilePidFile(invalidPath, "core", { warn: (message) => warnings.push(message) }))
      .toEqual({ state: "removed-invalid" });
    expect(reconcilePidFile(deadPath, "front", {
      isAlive: () => false,
      warn: (message) => warnings.push(message),
    })).toEqual({ state: "removed-dead", pid: 12345 });
    expect(warnings[0]).toContain("Removed stale PID file for yappod_core");
    expect(warnings[0]).toContain("does not contain a positive integer PID");
    expect(warnings[1]).toContain("PID 12345 is not running");
  });

  it("keeps a PID file for the expected running service", () => {
    const directory = temporaryDirectory();
    const path = join(directory, "core.pid");
    writeFileSync(path, "12345\n");
    expect(reconcilePidFile(path, "core", {
      isAlive: () => true,
      commandForPid: () => `${resolve(process.cwd(), "../..", "build/yappod_core")} --config config.toml`,
    })).toEqual({ state: "running", pid: 12345 });
  });

  it("does not treat a reused PID as the stack service", () => {
    const directory = temporaryDirectory();
    const path = join(directory, "web.pid");
    const warnings = [];
    writeFileSync(path, "12345\n");
    expect(reconcilePidFile(path, "web", {
      isAlive: () => true,
      commandForPid: () => "/usr/bin/unrelated-process",
      warn: (message) => warnings.push(message),
    })).toEqual({ state: "removed-reused", pid: 12345 });
    expect(warnings[0]).toContain("PID 12345 belongs to another process");
  });

  it("fails safely when a running PID cannot be identified", () => {
    const directory = temporaryDirectory();
    const path = join(directory, "front.pid");
    writeFileSync(path, "12345\n");
    expect(() => reconcilePidFile(path, "front", {
      isAlive: () => true,
      commandForPid: () => null,
    })).toThrow(`Cannot verify the process referenced by yappod_front PID file: ${path}`);
  });

  it("loads a configurable startup timeout", async () => {
    const directory = temporaryDirectory();
    const path = join(directory, "config.toml");
    writeFileSync(path, "[index]\ndirectory='index'\n[web]\nstartup_timeout_ms=30000\n");
    await expect(loadSharedConfig(path)).resolves.toMatchObject({
      web: { startupTimeoutMs: 30000 },
    });
  });

  it("stops waiting when the background process exits", async () => {
    await expect(waitForBackgroundReady(() => false, 2147483647, 30000)).resolves.toBe("exited");
  });

  it("reports the error log when startup fails", () => {
    const directory = temporaryDirectory();
    const errorPath = join(directory, "web.error");
    writeFileSync(errorPath, "first line\nconfiguration failed: invalid key\n");
    const healthUrl = "http://127.0.0.1:4173/api/health";
    const error = backgroundStartupError(
      "Web application", "exited", 123, 30000, healthUrl, errorPath);
    expect(error.message).toContain("exited before becoming ready (PID 123)");
    expect(error.message).toContain(`health check: GET ${healthUrl}`);
    expect(error.message).toContain("startup timeout: web.startup_timeout_ms = 30000");
    expect(error.message).toContain(`error log: ${errorPath}`);
    expect(error.message).toContain("configuration failed: invalid key");
  });
});
