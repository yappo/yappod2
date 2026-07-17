import assert from "node:assert/strict";
import { configPathFromArgs, loadRawConfig, loadSharedConfig } from "../scripts/shared-config.mjs";

const configPath = configPathFromArgs(process.argv.slice(2));
const config = await loadSharedConfig(configPath);
const rawConfig = await loadRawConfig(configPath);
const webUrl = `http://${config.web.host}:${config.web.port}`;
const yappodUrl = `http://${config.daemon.frontHost}:${config.daemon.frontPort}`;
const writeToken = rawConfig.daemon?.write_token;
if (typeof writeToken !== "string") throw new Error("daemon.write_token is required");

async function jsonRequest(baseUrl, path, body) {
  const response = await fetch(new URL(path, baseUrl), body === undefined ? undefined : {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const payload = await response.json();
  assert.equal(response.ok, true, `${path} failed: ${response.status} ${JSON.stringify(payload)}`);
  assert.equal(JSON.stringify(payload).includes(writeToken), false, `${path} exposed the write token`);
  return payload;
}

const home = await fetch(webUrl);
assert.equal(home.ok, true);
assert.match(await home.text(), /<div id="root"><\/div>/);

const status = await jsonRequest(webUrl, "/api/status");
assert.equal(status.ready, true);
assert.equal(status.llm_configured, true);
assert.equal(status.embedding_configured, true);
assert.deepEqual(status.available_modes, ["lexical", "vector", "hybrid"]);

const initial = await jsonRequest(webUrl, "/api/search", { query: "検索技術", limit: 5 });
assert.equal(initial.results.some((result) => result.title === "検索技術"), true);
const localFile = await jsonRequest(webUrl, "/api/search", { query: "localfilemarker", limit: 5 });
assert.equal(localFile.results.some((result) => result.title === "src/yappo_makeindex.c" && result.url === ""), true);
for (const mode of ["vector", "hybrid"]) {
  const semantic = await jsonRequest(webUrl, "/api/search", { query: "情報を探す技術", mode, limit: 5 });
  assert.equal(semantic.results.length > 0, true);
}

const documentId = "stage5-integration-document";
const uniqueTerm = "統合動作確認語";
const registered = await jsonRequest(webUrl, "/api/documents", {
  id: documentId,
  title: "統合動作確認文書",
  url: "https://example.test/stage5-integration",
  body: `${uniqueTerm}は登録後の再検索と引用取得を確認するための語句です。`,
});
assert.equal(registered.accepted, 1);
assert.equal(registered.upserts, 1);

let updated;
let updateError;
for (let attempt = 0; attempt < 20; attempt += 1) {
  try {
    updated = await jsonRequest(webUrl, "/api/search", { query: uniqueTerm, limit: 5 });
    updateError = undefined;
    if (updated.results.some((result) => result.document_id === documentId)) break;
  } catch (error) {
    updateError = error;
  }
  await new Promise((resolve) => setTimeout(resolve, 100));
}
if (!updated && updateError) {
  const direct = await fetch(new URL("/v2/search", yappodUrl), {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ query: uniqueTerm, mode: "lexical", scope: "documents", limit: 5 }),
  });
  console.error(`direct search after registration: ${direct.status} ${await direct.text()}`);
  throw updateError;
}
assert.equal(updated.results.some((result) => result.document_id === documentId), true);

const retrieved = await jsonRequest(yappodUrl, "/v2/retrieve", {
  query: uniqueTerm,
  mode: "lexical",
  limit: 5,
  max_passages_per_document: 2,
  max_context_bytes: 16384,
});
assert.equal(retrieved.citations.some((citation) => citation.document_id === documentId), true);
assert.match(retrieved.context, new RegExp(uniqueTerm));

const rag = await jsonRequest(webUrl, "/api/rag", { question: `${uniqueTerm}とは何ですか？` });
assert.equal(rag.generation_status, "answered");
assert.equal(rag.answer, "参照資料から確認できる内容です。[1]");
assert.deepEqual(rag.referenced_citations, [1]);
assert.equal(rag.citations.some((citation) => citation.document_id === documentId), true);

const vectorRag = await jsonRequest(webUrl, "/api/rag", {
  question: "情報を探す方法は何ですか？",
  mode: "vector",
});
assert.equal(vectorRag.retrieval_mode, "vector");
assert.equal(vectorRag.generation_status, "answered");

console.log("search Web API E2E passed");
