import { buildApp, defaultStaticDir } from "./app.js";
import { configPathFromArgs, loadWebConfig } from "./config.js";

const port = Number.parseInt(process.env.PORT ?? "4173", 10);
const host = process.env.HOST ?? "127.0.0.1";
const config = await loadWebConfig(configPathFromArgs(process.argv.slice(2)));
const app = await buildApp({
  baseUrl: process.env.YAPPOD_URL ?? "http://127.0.0.1:18400",
  writeToken: process.env.YAPPOD_WRITE_TOKEN,
  timeoutMs: Number.parseInt(process.env.YAPPOD_TIMEOUT_MS ?? "5000", 10),
  staticDir: process.env.NODE_ENV === "development" ? undefined : defaultStaticDir(),
  llm: config.llm,
  embedding: config.embedding,
  usageLogPath: config.usageLogPath,
});

try {
  await app.listen({ host, port });
  console.log(`Wikipedia search web is ready: http://${host}:${port}`);
} catch (error) {
  console.error(error instanceof Error ? (error.stack ?? error.message) : error);
  process.exit(1);
}
