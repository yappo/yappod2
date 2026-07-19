function errorDetails(error: unknown): { reason: string; lower: string } {
  if (!(error instanceof Error)) {
    const reason = String(error);
    return { reason, lower: reason.toLowerCase() };
  }
  let reason = error.message || error.name;
  const cause = error.cause;
  if (cause instanceof Error && cause.message && !reason.includes(cause.message)) {
    reason += `: ${cause.message}`;
  }
  return { reason, lower: reason.toLowerCase() };
}

export function formatServerError(
  error: unknown,
  configPath?: string,
  unexpected = false,
): string {
  const { reason, lower } = errorDetails(error);
  const actions: string[] = [];
  if (lower.includes("cannot read shared config") || lower.includes("enoent") || lower.includes("no such file")) {
    actions.push(`Check that the config file exists and is readable: ${configPath ?? "the --config path"}`);
    actions.push("Compare it with examples/search-web/config.example.toml.");
  } else if (["invalid shared config", "must be", "unknown key", "is required", "must specify"]
    .some((token) => lower.includes(token))) {
    actions.push(`Correct the named setting in ${configPath ?? "the --config file"}.`);
    actions.push("Compare the affected section with examples/search-web/config.example.toml.");
  } else if (lower.includes("environment variable")) {
    actions.push("Set the environment variable named by authorization_token_env in the current shell.");
    actions.push("Keep only the variable name, not the secret token, in the TOML file.");
  } else if (lower.includes("eaddrinuse") || lower.includes("address already in use")) {
    actions.push(`Change web.port in ${configPath ?? "the config"}, or stop the process using that port.`);
    actions.push("If the complete example stack is running, use its stop command before restarting it.");
  } else if (lower.includes("eacces") || lower.includes("permission denied")) {
    actions.push("Check web.host, web.port, and the current user's permission to listen on that address.");
  }
  actions.push("Run `node examples/search-web/server/dist/index.js --config PATH` with the intended config file.");
  const lines = [
    "search-web-server: error: server startup failed",
    `Reason: ${reason}`,
  ];
  if (configPath) lines.push(`Config: ${configPath}`);
  lines.push("How to fix:");
  actions.forEach((action, index) => lines.push(`  ${index + 1}. ${action}`));
  if (unexpected) {
    lines.push("  Debug: re-run with YAPPOD_EXAMPLE_DEBUG=1 to include the JavaScript stack trace.");
  }
  return lines.join("\n");
}
