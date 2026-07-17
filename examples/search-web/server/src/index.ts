import { buildApp, defaultStaticDir } from "./app.js";
import { configPathFromArgs, loadWebConfig } from "./config.js";

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

try {
  await app.listen({ host: config.web.host, port: config.web.port });
  console.log(`yappod search web is ready: http://${config.web.host}:${config.web.port}`);
} catch (error) {
  console.error(error instanceof Error ? (error.stack ?? error.message) : error);
  process.exit(1);
}
