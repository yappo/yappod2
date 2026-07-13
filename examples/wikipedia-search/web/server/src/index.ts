import { buildApp, defaultStaticDir } from "./app.js";

const port = Number.parseInt(process.env.PORT ?? "4173", 10);
const host = process.env.HOST ?? "127.0.0.1";
const llmBaseUrl = process.env.LLM_BASE_URL;
const llmModel = process.env.LLM_MODEL;
if ((llmBaseUrl && !llmModel) || (!llmBaseUrl && llmModel)) {
  throw new Error("LLM_BASE_URL and LLM_MODEL must be configured together");
}
const app = await buildApp({
  baseUrl: process.env.YAPPOD_URL ?? "http://127.0.0.1:10080",
  writeToken: process.env.YAPPOD_WRITE_TOKEN,
  timeoutMs: Number.parseInt(process.env.YAPPOD_TIMEOUT_MS ?? "5000", 10),
  staticDir: process.env.NODE_ENV === "development" ? undefined : defaultStaticDir(),
  llm: llmBaseUrl && llmModel ? {
    baseUrl: llmBaseUrl,
    model: llmModel,
    apiKey: process.env.LLM_API_KEY,
    timeoutMs: Number.parseInt(process.env.LLM_TIMEOUT_MS ?? "30000", 10),
  } : undefined,
});

try {
  await app.listen({ host, port });
  console.log(`Wikipedia search web is ready: http://${host}:${port}`);
} catch (error) {
  app.log.error(error);
  process.exitCode = 1;
}
