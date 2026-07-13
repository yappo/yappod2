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
}

export interface RegisterResponse {
  generation: number;
  accepted: number;
  upserts: number;
  deletes: number;
  segment_id?: string;
}
