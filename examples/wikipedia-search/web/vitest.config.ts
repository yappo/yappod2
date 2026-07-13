import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    environment: "jsdom",
    setupFiles: ["./client/src/test-setup.ts"],
    include: ["client/src/**/*.test.tsx", "server/test/**/*.test.ts"],
    restoreMocks: true,
  },
});
