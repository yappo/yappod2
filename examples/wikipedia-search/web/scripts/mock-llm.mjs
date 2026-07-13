import { createServer } from "node:http";

const host = process.env.MOCK_LLM_HOST ?? "127.0.0.1";
const port = Number.parseInt(process.env.MOCK_LLM_PORT ?? "1234", 10);
const model = process.env.MOCK_LLM_MODEL ?? "yappod-demo-mock";
const answer = process.env.MOCK_LLM_ANSWER ?? "参照資料から確認できる内容です。[1]";

if (host !== "127.0.0.1" && host !== "localhost" && host !== "::1") {
  throw new Error("mock LLM must listen on a loopback address");
}
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  throw new Error("MOCK_LLM_PORT must be a valid TCP port");
}

function send(response, status, body) {
  response.writeHead(status, { "content-type": "application/json; charset=utf-8" });
  response.end(JSON.stringify(body));
}

const server = createServer((request, response) => {
  if (request.method === "GET" && request.url === "/health") {
    return send(response, 200, { ready: true });
  }
  if (request.method !== "POST" || request.url !== "/v1/chat/completions") {
    return send(response, 404, { error: { message: "endpoint not found" } });
  }

  let body = "";
  request.setEncoding("utf8");
  request.on("data", (chunk) => {
    body += chunk;
    if (body.length > 1024 * 1024) request.destroy();
  });
  request.on("end", () => {
    let payload;
    try {
      payload = JSON.parse(body);
    } catch {
      return send(response, 400, { error: { message: "invalid JSON" } });
    }
    const userMessage = Array.isArray(payload.messages)
      ? payload.messages.find((message) => message?.role === "user")
      : undefined;
    if (payload.model !== model || typeof userMessage?.content !== "string" || !userMessage.content.includes("[1]")) {
      return send(response, 400, { error: { message: "invalid mock request" } });
    }
    return send(response, 200, {
      id: "chatcmpl-yappod-demo",
      object: "chat.completion",
      model,
      choices: [{ index: 0, finish_reason: "stop", message: { role: "assistant", content: answer } }],
    });
  });
});

function shutdown() {
  server.close(() => process.exit(0));
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
server.listen(port, host, () => console.log(`mock LLM is ready: http://${host}:${port}`));
