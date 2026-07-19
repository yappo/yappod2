import { describe, expect, it } from "vitest";
import { formatServerError } from "../src/user-error.js";

describe("server startup errors", () => {
  it("explains how to recover from a missing config", () => {
    const path = "/tmp/missing-config.toml";
    const message = formatServerError(
      new Error(`cannot read shared config: ${path}`, {
        cause: new Error("ENOENT: no such file or directory"),
      }),
      path,
    );

    expect(message).toContain("search-web-server: error: server startup failed");
    expect(message).toContain("Reason: cannot read shared config");
    expect(message).toContain(`Config: ${path}`);
    expect(message).toContain("How to fix:");
    expect(message).toContain("examples/search-web/config.example.toml");
  });

  it("points port conflicts to web.port", () => {
    const message = formatServerError(
      new Error("listen EADDRINUSE: address already in use 127.0.0.1:4173"),
      "/tmp/config.toml",
    );
    expect(message).toContain("Change web.port");
    expect(message).toContain("stop the process using that port");
  });
});
