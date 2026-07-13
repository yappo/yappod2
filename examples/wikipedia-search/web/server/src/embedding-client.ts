export type EmbeddingProvider = "lmstudio" | "ollama";
export type EmbeddingProfile = "plain" | "embeddinggemma";

export interface EmbeddingClientOptions {
  provider: EmbeddingProvider;
  baseUrl: string;
  model: string;
  indexModelId?: string;
  dimensions: number;
  timeoutMs: number;
  batchSize?: number;
  authorizationToken?: string;
  profile?: EmbeddingProfile;
  fetchImpl?: typeof fetch;
}

export class EmbeddingRequestError extends Error {
  constructor(
    public readonly code: string,
    message: string,
    public readonly status = 503,
  ) {
    super(message);
    this.name = "EmbeddingRequestError";
  }
}

function endpoint(options: EmbeddingClientOptions): URL {
  const base = options.baseUrl.endsWith("/") ? options.baseUrl : `${options.baseUrl}/`;
  return new URL(options.provider === "lmstudio" ? "embeddings" : "api/embed", base);
}

function vector(value: unknown, dimensions: number): number[] {
  if (!Array.isArray(value) || value.length !== dimensions ||
      value.some((item) => typeof item !== "number" || !Number.isFinite(item))) {
    throw new EmbeddingRequestError(
      "embedding_dimension_mismatch",
      `embeddingが設定された${dimensions}次元と一致しません`,
      502,
    );
  }
  return value as number[];
}

function lmStudioVectors(body: unknown, count: number, dimensions: number): number[][] {
  if (typeof body !== "object" || body === null || !Array.isArray((body as { data?: unknown }).data)) {
    throw new EmbeddingRequestError("invalid_embedding_response", "LM Studioから不正な応答を受け取りました", 502);
  }
  const data = (body as { data: unknown[] }).data;
  if (data.length !== count) {
    throw new EmbeddingRequestError("invalid_embedding_response", "LM Studioのembedding件数が入力と一致しません", 502);
  }
  const output: Array<number[] | undefined> = new Array(count);
  for (const item of data) {
    if (typeof item !== "object" || item === null) {
      throw new EmbeddingRequestError("invalid_embedding_response", "LM Studioのembedding項目が不正です", 502);
    }
    const candidate = item as { index?: unknown; embedding?: unknown };
    if (!Number.isInteger(candidate.index) || (candidate.index as number) < 0 ||
        (candidate.index as number) >= count || output[candidate.index as number] !== undefined) {
      throw new EmbeddingRequestError("invalid_embedding_response", "LM Studioのembedding順序を検証できません", 502);
    }
    output[candidate.index as number] = vector(candidate.embedding, dimensions);
  }
  if (output.some((item) => item === undefined)) {
    throw new EmbeddingRequestError("invalid_embedding_response", "LM Studioのembeddingに欠落があります", 502);
  }
  return output as number[][];
}

function ollamaVectors(body: unknown, count: number, dimensions: number): number[][] {
  if (typeof body !== "object" || body === null || !Array.isArray((body as { embeddings?: unknown }).embeddings)) {
    throw new EmbeddingRequestError("invalid_embedding_response", "Ollamaから不正な応答を受け取りました", 502);
  }
  const embeddings = (body as { embeddings: unknown[] }).embeddings;
  if (embeddings.length !== count) {
    throw new EmbeddingRequestError("invalid_embedding_response", "Ollamaのembedding件数が入力と一致しません", 502);
  }
  return embeddings.map((item) => vector(item, dimensions));
}

export class EmbeddingClient {
  private readonly fetchImpl: typeof fetch;
  private readonly batchSize: number;

  constructor(readonly options: EmbeddingClientOptions) {
    this.fetchImpl = options.fetchImpl ?? globalThis.fetch;
    this.batchSize = options.batchSize ?? 16;
    if (!Number.isInteger(options.dimensions) || options.dimensions < 1 ||
        !Number.isInteger(this.batchSize) || this.batchSize < 1 || this.batchSize > 1024) {
      throw new Error("embedding dimensions and batch size must be positive integers");
    }
  }

  async embed(inputs: string[]): Promise<number[][]> {
    if (inputs.length === 0 || inputs.some((input) => !input.trim())) {
      throw new EmbeddingRequestError("invalid_embedding_input", "embedding対象の本文が空です", 400);
    }
    const output: number[][] = [];
    for (let offset = 0; offset < inputs.length; offset += this.batchSize) {
      const batch = inputs.slice(offset, offset + this.batchSize);
      output.push(...await this.embedBatch(batch));
    }
    return output;
  }

  embedQueries(inputs: string[]): Promise<number[][]> {
    return this.embed(this.options.profile === "embeddinggemma"
      ? inputs.map((input) => `task: search result | query: ${input}`)
      : inputs);
  }

  embedDocuments(inputs: Array<{ text: string; title?: string }>): Promise<number[][]> {
    return this.embed(this.options.profile === "embeddinggemma"
      ? inputs.map((input) => `title: ${input.title?.trim() || "none"} | text: ${input.text}`)
      : inputs.map((input) => input.text));
  }

  private async embedBatch(inputs: string[]): Promise<number[][]> {
    let response: Response;
    const headers: Record<string, string> = { "content-type": "application/json" };
    if (this.options.authorizationToken) headers.authorization = `Bearer ${this.options.authorizationToken}`;
    try {
      response = await this.fetchImpl(endpoint(this.options), {
        method: "POST",
        headers,
        body: JSON.stringify({ model: this.options.model, input: inputs }),
        signal: AbortSignal.timeout(this.options.timeoutMs),
      });
    } catch {
      throw new EmbeddingRequestError("embedding_unavailable", "embedding serverに接続できません");
    }
    if (!response.ok) {
      throw new EmbeddingRequestError(
        "embedding_failed",
        `${this.options.provider === "lmstudio" ? "LM Studio" : "Ollama"}がembeddingを生成できませんでした`,
        response.status >= 400 && response.status < 500 ? 502 : 503,
      );
    }
    let body: unknown;
    try {
      body = await response.json();
    } catch {
      throw new EmbeddingRequestError("invalid_embedding_response", "embedding serverからJSON以外の応答を受け取りました", 502);
    }
    return this.options.provider === "lmstudio"
      ? lmStudioVectors(body, inputs.length, this.options.dimensions)
      : ollamaVectors(body, inputs.length, this.options.dimensions);
  }
}
