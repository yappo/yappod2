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
  generation: number;
  total: number;
  results: SearchResult[];
  next_cursor: string | null;
}

export interface StatusResponse {
  ready: boolean;
  generation?: number;
  state?: string;
  code?: string;
  message?: string;
  llm_configured?: boolean;
}

export interface RegisterInput {
  id: string;
  title: string;
  url?: string;
  body: string;
}

export interface RegisterResponse {
  generation: number;
  accepted: number;
  upserts: number;
  deletes: number;
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

export type GenerationStatus = "answered" | "unconfigured" | "failed" | "invalid_citations" | "no_context";

export interface RagResponse {
  generation: number;
  question: string;
  context: string;
  citations: Citation[];
  answer: string | null;
  referenced_citations: number[];
  generation_status: GenerationStatus;
  generation_message?: string;
}
