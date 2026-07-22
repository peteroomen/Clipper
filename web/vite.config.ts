import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Lean config. No special headers needed: the WASM is embedded (SINGLE_FILE)
// so we avoid SharedArrayBuffer / COOP-COEP entirely.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
  },
  preview: {
    port: 4173,
  },
});
