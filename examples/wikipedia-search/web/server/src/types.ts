export interface SearchResult {
  id: string;
  document_id: string;
  title: string;
  url: string;
  snippet: string;
  lexical_score: number;
  vector_score: number;
  fused_score: number;
}

export interface SearchResponse {
  api_version: number;
  generation: number;
  total: number;
  results: SearchResult[];
  next_cursor: string | null;
}

export interface ReadyResponse {
  ready: boolean;
  generation?: number;
  state?: string;
  embedding?: {
    state: "disabled" | "precomputed_ready";
    model_id: string;
    dimensions: number;
  };
}

export type SearchMode = "lexical" | "vector" | "hybrid";

export interface PreparedPassage {
  id: string;
  ordinal: number;
  start_char: number;
  end_char: number;
  text: string;
}

export interface PrepareResponse {
  model_id: string;
  dimensions: number;
  passages: PreparedPassage[];
}

export interface RegisterResponse {
  generation: number;
  accepted: number;
  upserts: number;
  deletes: number;
  segment_id?: string;
}

export interface Citation {
  passage_id: string;
  document_id: string;
  url: string;
  title: string;
  text: string;
  start_char: number;
  end_char: number;
  context_start: number;
  context_end: number;
  lexical_score: number;
  vector_score: number;
  fused_score: number;
}

export interface RetrieveResponse {
  api_version: number;
  generation: number;
  context: string;
  citations: Citation[];
}

export type GenerationStatus = "answered" | "unconfigured" | "failed" | "invalid_citations" | "no_context";

export interface RagResponse extends RetrieveResponse {
  question: string;
  retrieval_mode: SearchMode;
  answer: string | null;
  referenced_citations: number[];
  generation_status: GenerationStatus;
  generation_message?: string;
}
