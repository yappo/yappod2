import { buildApp, defaultStaticDir } from "./app.js";

const port = Number.parseInt(process.env.PORT ?? "4173", 10);
const host = process.env.HOST ?? "127.0.0.1";
const llmBaseUrl = process.env.LLM_BASE_URL;
const llmModel = process.env.LLM_MODEL;
if ((llmBaseUrl && !llmModel) || (!llmBaseUrl && llmModel)) {
  throw new Error("LLM_BASE_URL and LLM_MODEL must be configured together");
}
const embeddingProvider = process.env.EMBEDDING_PROVIDER;
const embeddingBaseUrl = process.env.EMBEDDING_BASE_URL;
const embeddingModel = process.env.EMBEDDING_MODEL;
const embeddingProfile = process.env.EMBEDDING_PROFILE ?? "embeddinggemma";
if (embeddingProvider && embeddingProvider !== "lmstudio" && embeddingProvider !== "ollama") {
  throw new Error("EMBEDDING_PROVIDER must be lmstudio or ollama");
}
if (embeddingProfile !== "plain" && embeddingProfile !== "embeddinggemma") {
  throw new Error("EMBEDDING_PROFILE must be plain or embeddinggemma");
}
if ([embeddingProvider, embeddingBaseUrl, embeddingModel].some(Boolean) &&
    ![embeddingProvider, embeddingBaseUrl, embeddingModel].every(Boolean)) {
  throw new Error("EMBEDDING_PROVIDER, EMBEDDING_BASE_URL and EMBEDDING_MODEL must be configured together");
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
  embedding: embeddingProvider && embeddingBaseUrl && embeddingModel ? {
    provider: embeddingProvider as "lmstudio" | "ollama",
    baseUrl: embeddingBaseUrl,
    model: embeddingModel,
    indexModelId: process.env.EMBEDDING_INDEX_MODEL_ID,
    dimensions: Number.parseInt(process.env.EMBEDDING_DIMENSIONS ?? "768", 10),
    profile: embeddingProfile,
    timeoutMs: Number.parseInt(process.env.EMBEDDING_TIMEOUT_MS ?? "60000", 10),
    batchSize: Number.parseInt(process.env.EMBEDDING_BATCH_SIZE ?? "16", 10),
    apiKey: process.env.EMBEDDING_API_KEY,
  } : undefined,
});

try {
  await app.listen({ host, port });
  console.log(`Wikipedia search web is ready: http://${host}:${port}`);
} catch (error) {
  console.error(error instanceof Error ? (error.stack ?? error.message) : error);
  process.exit(1);
}
