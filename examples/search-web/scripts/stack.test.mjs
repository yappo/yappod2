import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { backgroundStartupError, waitForBackgroundReady } from "./stack.mjs";
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
