import type { Citation } from "./types.js";

export interface CitationValidation {
  valid: boolean;
  references: number[];
}

export function buildRagPrompt(question: string, citations: Citation[]): string {
  const sources = citations.map((citation, index) => [
    `[${index + 1}] ${citation.title || "無題の資料"}`,
    citation.url ? `URL: ${citation.url}` : "URL: なし",
    citation.text,
  ].join("\n")).join("\n\n");
  return [
    "質問:",
    question,
    "",
    "参照資料:",
    sources,
  ].join("\n");
}

export function validateCitations(answer: string, citationCount: number): CitationValidation {
  const references: number[] = [];
  const seen = new Set<number>();
  for (const match of answer.matchAll(/\[(\d+)\]/g)) {
    const reference = Number.parseInt(match[1] ?? "", 10);
    if (!Number.isSafeInteger(reference) || reference < 1 || reference > citationCount) {
      return { valid: false, references: [] };
    }
    if (!seen.has(reference)) {
      seen.add(reference);
      references.push(reference);
    }
  }
  return { valid: references.length > 0, references };
}
