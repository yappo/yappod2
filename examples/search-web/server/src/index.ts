import { buildApp, defaultStaticDir } from "./app.js";
import { configPathFromArgs, loadWebConfig } from "./config.js";
import { formatServerError } from "./user-error.js";

async function main(): Promise<void> {
  const args = configPathFromArgs(process.argv.slice(2));
  const config = await loadWebConfig(args.path);
  const app = await buildApp({
    baseUrl: `http://${config.daemon.frontHost}:${config.daemon.frontPort}`,
    writeToken: config.daemon.writeToken,
    timeoutMs: config.web.yappodTimeoutMs,
    staticDir: args.development ? undefined : defaultStaticDir(),
    llm: config.llm,
    embedding: config.embedding,
    usageLogPath: config.usageLogPath,
  });
  await app.listen({ host: config.web.host, port: config.web.port });
  console.log(`yappod search web is ready: http://${config.web.host}:${config.web.port}`);
}

try {
  await main();
} catch (error) {
  if (process.env.YAPPOD_EXAMPLE_DEBUG === "1" && error instanceof Error) {
    console.error(error.stack ?? error.message);
  } else {
    const args = process.argv.slice(2);
    const configIndex = args.indexOf("--config");
    console.error(formatServerError(
      error,
      configIndex >= 0 ? args[configIndex + 1] : undefined,
      true,
    ));
  }
  process.exitCode = 1;
}
