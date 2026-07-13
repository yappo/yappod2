import fastifyStatic from "@fastify/static";
import Fastify, { type FastifyInstance } from "fastify";
import { join } from "node:path";
import { EmbeddingClient, EmbeddingRequestError, type EmbeddingClientOptions } from "./embedding-client.js";
import { OpenAICompatibleClient, type LlmClientOptions } from "./llm-client.js";
import { validateCitations } from "./rag.js";
import type { ReadyResponse, SearchMode } from "./types.js";
import { YappodClient, YappodRequestError, type YappodClientOptions } from "./yappod-client.js";

export interface AppConfig extends Omit<YappodClientOptions, "fetchImpl"> {
  staticDir?: string;
  fetchImpl?: typeof fetch;
  llm?: LlmClientOptions;
  embedding?: EmbeddingClientOptions;
}

interface SearchBody {
  query: string;
  limit?: number;
  cursor?: string;
  mode?: SearchMode;
}

interface DocumentBody {
  id: string;
  title: string;
  url?: string;
  body: string;
}

interface RagBody {
  question: string;
  mode?: SearchMode;
}

const searchModes = ["lexical", "vector", "hybrid"] as const;

const searchSchema = {
  body: {
    type: "object",
    additionalProperties: false,
    required: ["query"],
    properties: {
      query: { type: "string", minLength: 1, maxLength: 500 },
      limit: { type: "integer", minimum: 1, maximum: 100 },
      cursor: { type: "string", minLength: 1, maxLength: 512 },
      mode: { type: "string", enum: searchModes },
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

const ragSchema = {
  body: {
    type: "object",
    additionalProperties: false,
    required: ["question"],
    properties: {
      question: { type: "string", minLength: 1, maxLength: 1000 },
      mode: { type: "string", enum: searchModes },
    },
  },
} as const;

function publicError(error: unknown): { status: number; body: { code: string; message: string } } {
  if (error instanceof EmbeddingRequestError) {
    return { status: error.status, body: { code: error.code, message: error.message } };
  }
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
  const llm = config.llm ? new OpenAICompatibleClient(config.llm) : null;
  const embedding = config.embedding ? new EmbeddingClient({
    ...config.embedding,
    fetchImpl: config.embedding.fetchImpl ?? config.fetchImpl,
  }) : null;

  function vectorReady(status: ReadyResponse): boolean {
    return embedding !== null && status.embedding?.state === "precomputed_ready" &&
      status.embedding.dimensions === embedding.options.dimensions &&
      (!embedding.options.indexModelId || status.embedding.model_id === embedding.options.indexModelId);
  }

  async function queryVector(text: string, mode: SearchMode): Promise<number[] | undefined> {
    if (mode === "lexical") return undefined;
    if (embedding === null) {
      throw new EmbeddingRequestError("embedding_unconfigured", "意味検索は未設定です。キーワード検索を選択してください");
    }
    const status = await client.status();
    if (status.embedding?.state !== "precomputed_ready") {
      throw new EmbeddingRequestError("index_vector_disabled", "接続中のindexはvector検索に対応していません", 409);
    }
    if (status.embedding.dimensions !== embedding.options.dimensions) {
      throw new EmbeddingRequestError(
        "embedding_dimension_mismatch",
        `indexは${status.embedding.dimensions}次元、embedding serverは${embedding.options.dimensions}次元に設定されています`,
        409,
      );
    }
    if (embedding.options.indexModelId && status.embedding.model_id !== embedding.options.indexModelId) {
      throw new EmbeddingRequestError(
        "embedding_model_mismatch",
        `indexのmodel ID (${status.embedding.model_id}) とBFF設定 (${embedding.options.indexModelId}) が一致しません`,
        409,
      );
    }
    return (await embedding.embedQueries([text]))[0];
  }

  app.get("/api/status", async (_request, reply) => {
    try {
      const status = await client.status();
      const availableModes: SearchMode[] = ["lexical"];
      if (vectorReady(status)) availableModes.push("vector", "hybrid");
      return reply.send({
        ready: status.ready === true,
        generation: status.generation,
        state: status.state,
        llm_configured: llm !== null,
        embedding_configured: embedding !== null,
        index_embedding: status.embedding,
        available_modes: availableModes,
      });
    } catch (error) {
      const mapped = publicError(error);
      return reply.send({ ready: false, code: mapped.body.code, message: mapped.body.message });
    }
  });

  app.post<{ Body: SearchBody }>("/api/search", { schema: searchSchema }, async (request, reply) => {
    try {
      const query = request.body.query.trim();
      if (!query) return reply.code(400).send({ code: "invalid_query", message: "検索語句を入力してください" });
      const mode = request.body.mode ?? "lexical";
      const vector = await queryVector(query, mode);
      return reply.send(await client.search({
        query,
        mode,
        vector,
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
      const input = {
        id: request.body.id.trim(),
        title: request.body.title.trim(),
        url: request.body.url?.trim() || undefined,
        body: request.body.body,
      };
      const status = await client.status();
      let vectors: number[][] | undefined;
      if (status.embedding?.state === "precomputed_ready") {
        if (embedding === null) {
          throw new EmbeddingRequestError("embedding_unconfigured", "vector indexへの登録にはembedding設定が必要です");
        }
        if (status.embedding.dimensions !== embedding.options.dimensions) {
          throw new EmbeddingRequestError("embedding_dimension_mismatch", "indexとembedding serverの次元数が一致しません", 409);
        }
        if (embedding.options.indexModelId && status.embedding.model_id !== embedding.options.indexModelId) {
          throw new EmbeddingRequestError("embedding_model_mismatch", "indexとBFFのembedding model IDが一致しません", 409);
        }
        const prepared = await client.prepareDocument(input.id, input.body);
        if (prepared.dimensions !== embedding.options.dimensions || prepared.passages.length === 0) {
          throw new EmbeddingRequestError("invalid_prepare_response", "登録文書のpassageを準備できませんでした", 502);
        }
        vectors = await embedding.embedDocuments(prepared.passages.map((passage) => ({
          text: passage.text,
          title: input.title,
        })));
      }
      const result = await client.registerDocument({ ...input, vectors });
      return reply.send(result);
    } catch (error) {
      const mapped = publicError(error);
      return reply.code(mapped.status).send(mapped.body);
    }
  });

  app.post<{ Body: RagBody }>("/api/rag", { schema: ragSchema }, async (request, reply) => {
    const question = request.body.question.trim();
    if (!question) return reply.code(400).send({ code: "invalid_question", message: "質問を入力してください" });
    let retrieval;
    const mode = request.body.mode ?? "lexical";
    try {
      const vector = await queryVector(question, mode);
      retrieval = await client.retrieve(question, mode, vector);
    } catch (error) {
      const mapped = publicError(error);
      return reply.code(mapped.status).send(mapped.body);
    }
    const base = {
      ...retrieval,
      question,
      retrieval_mode: mode,
      answer: null,
      referenced_citations: [] as number[],
    };
    if (retrieval.citations.length === 0 || !retrieval.context.trim()) {
      return reply.send({
        ...base,
        generation_status: "no_context",
        generation_message: "回答に使える参照資料が見つかりませんでした",
      });
    }
    if (llm === null) {
      return reply.send({
        ...base,
        generation_status: "unconfigured",
        generation_message: "回答生成は未設定です。取得した参照資料を確認できます",
      });
    }
    try {
      const answer = await llm.answer(question, retrieval.citations);
      const validation = validateCitations(answer, retrieval.citations.length);
      if (!validation.valid) {
        return reply.send({
          ...base,
          generation_status: "invalid_citations",
          generation_message: "生成された回答の参照番号を検証できなかったため、回答を表示していません",
        });
      }
      return reply.send({
        ...base,
        answer,
        referenced_citations: validation.references,
        generation_status: "answered",
      });
    } catch {
      return reply.send({
        ...base,
        generation_status: "failed",
        generation_message: "回答生成に失敗しました。取得した参照資料はそのまま確認できます",
      });
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
  return join(import.meta.dirname, "../../client/dist");
}
