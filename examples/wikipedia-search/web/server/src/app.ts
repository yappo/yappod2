import fastifyStatic from "@fastify/static";
import Fastify, { type FastifyInstance } from "fastify";
import { join } from "node:path";
import { YappodClient, YappodRequestError, type YappodClientOptions } from "./yappod-client.js";

export interface AppConfig extends Omit<YappodClientOptions, "fetchImpl"> {
  staticDir?: string;
  fetchImpl?: typeof fetch;
}

interface SearchBody {
  query: string;
  limit?: number;
  cursor?: string;
}

interface DocumentBody {
  id: string;
  title: string;
  url?: string;
  body: string;
}

const searchSchema = {
  body: {
    type: "object",
    additionalProperties: false,
    required: ["query"],
    properties: {
      query: { type: "string", minLength: 1, maxLength: 500 },
      limit: { type: "integer", minimum: 1, maximum: 100 },
      cursor: { type: "string", minLength: 1, maxLength: 512 },
    },
  },
} as const;

const documentSchema = {
  body: {
    type: "object",
    additionalProperties: false,
    required: ["id", "title", "body"],
    properties: {
      id: { type: "string", minLength: 1, maxLength: 200 },
      title: { type: "string", minLength: 1, maxLength: 500 },
      url: { type: "string", maxLength: 2048 },
      body: { type: "string", minLength: 1, maxLength: 900000 },
    },
  },
} as const;

function publicError(error: unknown): { status: number; body: { code: string; message: string } } {
  if (error instanceof YappodRequestError) {
    const message = error.status === 401
      ? "文書登録の認証に失敗しました。BFFのwrite tokenを確認してください"
      : error.message;
    return { status: error.status, body: { code: error.code, message } };
  }
  return { status: 500, body: { code: "internal_error", message: "処理中に予期しない問題が発生しました" } };
}

export async function buildApp(config: AppConfig): Promise<FastifyInstance> {
  const app = Fastify({ logger: false });
  const client = new YappodClient(config);

  app.get("/api/status", async (_request, reply) => {
    try {
      const status = await client.status();
      return reply.send({ ready: status.ready === true, generation: status.generation, state: status.state });
    } catch (error) {
      const mapped = publicError(error);
      return reply.send({ ready: false, code: mapped.body.code, message: mapped.body.message });
    }
  });

  app.post<{ Body: SearchBody }>("/api/search", { schema: searchSchema }, async (request, reply) => {
    try {
      const query = request.body.query.trim();
      if (!query) return reply.code(400).send({ code: "invalid_query", message: "検索語句を入力してください" });
      return reply.send(await client.search({
        query,
        limit: request.body.limit ?? 10,
        cursor: request.body.cursor,
      }));
    } catch (error) {
      const mapped = publicError(error);
      return reply.code(mapped.status).send(mapped.body);
    }
  });

  app.post<{ Body: DocumentBody }>("/api/documents", { schema: documentSchema }, async (request, reply) => {
    try {
      if (!request.body.id.trim() || !request.body.title.trim() || !request.body.body.trim()) {
        return reply.code(400).send({ code: "invalid_document", message: "文書ID、title、本文は必須です" });
      }
      const result = await client.registerDocument({
        id: request.body.id.trim(),
        title: request.body.title.trim(),
        url: request.body.url?.trim() || undefined,
        body: request.body.body,
      });
      return reply.send(result);
    } catch (error) {
      const mapped = publicError(error);
      return reply.code(mapped.status).send(mapped.body);
    }
  });

  if (config.staticDir) {
    await app.register(fastifyStatic, { root: config.staticDir });
    app.setNotFoundHandler((request, reply) => {
      if (request.method === "GET" && !request.url.startsWith("/api/")) return reply.sendFile("index.html");
      return reply.code(404).send({ code: "not_found", message: "endpointが見つかりません" });
    });
  }

  return app;
}

export function defaultStaticDir(): string {
  return join(import.meta.dirname, "../../../client/dist");
}
