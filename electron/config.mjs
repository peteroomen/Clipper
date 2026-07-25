// Clipper desktop — API key resolution + persistence.
//
// Resolution order (first hit wins):
//   1. ANTHROPIC_API_KEY environment variable.
//   2. A JSON config file at app.getPath('userData')/config.json — shape:
//        { "apiKey": "sk-ant-..." }
//   3. Neither -> null (the shell then prompts the user in-app and calls
//      saveApiKey to persist to the config file).
//
// SECURITY / v1 tradeoff: the key is stored in PLAIN TEXT in config.json inside
// the app's per-user data directory. It is NOT stored in the macOS Keychain.
// This is documented in docs/DEVELOPMENT.md as a known v1 limitation; the file
// lives in the user's own account-scoped userData dir (mode 0600 on write).

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';

/**
 * Resolve the Anthropic API key.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]  Defaults to process.env.
 * @param {string} [opts.configPath]      Path to config.json (userData/config.json).
 * @returns {{ key: string|null, source: 'env'|'config'|null }}
 */
export function resolveApiKey(opts = {}) {
  const env = opts.env || process.env;
  const fromEnv = typeof env.ANTHROPIC_API_KEY === 'string' ? env.ANTHROPIC_API_KEY.trim() : '';
  if (fromEnv) return { key: fromEnv, source: 'env' };

  if (opts.configPath) {
    try {
      const cfg = JSON.parse(readFileSync(opts.configPath, 'utf8'));
      const k = cfg && typeof cfg.apiKey === 'string' ? cfg.apiKey.trim() : '';
      if (k) return { key: k, source: 'config' };
    } catch {
      /* missing or invalid config.json -> fall through */
    }
  }

  return { key: null, source: null };
}

/**
 * Persist the API key to config.json (plain text, mode 0600). Creates the
 * parent directory if needed.
 *
 * @param {object} opts
 * @param {string} opts.configPath  Absolute path to config.json.
 * @param {string} opts.apiKey      The key to store.
 */
export function saveApiKey(opts) {
  const { configPath, apiKey } = opts;
  if (!configPath) throw new Error('saveApiKey: configPath is required');
  const key = typeof apiKey === 'string' ? apiKey.trim() : '';
  if (!key) throw new Error('saveApiKey: apiKey is empty');
  mkdirSync(path.dirname(configPath), { recursive: true });
  writeFileSync(configPath, JSON.stringify({ apiKey: key }, null, 2), { mode: 0o600 });
}
