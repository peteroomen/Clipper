import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { execSync } from 'node:child_process';

// Build stamp shown in the app footer so a stale build is visible at a glance.
function gitStamp(): string {
  try {
    return execSync('git rev-parse --short HEAD', { encoding: 'utf8' }).trim();
  } catch {
    return 'dev';
  }
}

// Lean config. No special headers needed: the WASM is embedded (SINGLE_FILE)
// so we avoid SharedArrayBuffer / COOP-COEP entirely.
export default defineConfig({
  plugins: [react()],
  define: {
    __BUILD_STAMP__: JSON.stringify(gitStamp()),
  },
  server: {
    port: 5173,
    // Same-origin API: the web app calls /api/chat and /api/health, which Vite
    // forwards to the M6 proxy server (server/index.mjs) on :8787. Keeps the
    // Anthropic key server-side and avoids any CORS in dev.
    proxy: {
      '/api': {
        target: 'http://localhost:8787',
        changeOrigin: true,
      },
    },
  },
  preview: {
    port: 4173,
  },
});
