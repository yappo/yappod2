import { appendFile, mkdir } from "node:fs/promises";
import { dirname } from "node:path";

export interface UsageLogEvent {
  service: "embedding" | "llm";
  operation: string;
  provider: string;
  model: string;
  usage: unknown;
}

export type UsageLogger = (event: UsageLogEvent) => Promise<void>;

export function createUsageLogger(path: string): UsageLogger {
  return async (event) => {
    const record = {
      timestamp: new Date().toISOString(),
      source: "search-web",
      service: event.service,
      operation: event.operation,
      provider: event.provider,
      model: event.model,
      usage: typeof event.usage === "object" && event.usage !== null && !Array.isArray(event.usage)
        ? event.usage
        : null,
    };
    try {
      await mkdir(dirname(path), { recursive: true });
      await appendFile(path, `${JSON.stringify(record)}\n`, { encoding: "utf8", flag: "a" });
    } catch (error) {
      console.error(`search-web: warning: cannot append usage log ${path}: ${error instanceof Error ? error.message : String(error)}`);
    }
  };
}
