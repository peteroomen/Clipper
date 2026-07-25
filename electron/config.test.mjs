// node --test — API key resolution order (env > config.json > null).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync, readFileSync, statSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { resolveApiKey, saveApiKey } from './config.mjs';

function tmp() {
  return mkdtempSync(path.join(tmpdir(), 'clipper-cfg-'));
}

test('env var wins even when a config file also has a key', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'config.json');
  writeFileSync(configPath, JSON.stringify({ apiKey: 'sk-from-file' }));
  try {
    const r = resolveApiKey({ env: { ANTHROPIC_API_KEY: '  sk-from-env  ' }, configPath });
    assert.equal(r.key, 'sk-from-env'); // trimmed
    assert.equal(r.source, 'env');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('falls back to config.json when env is unset/blank', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'config.json');
  writeFileSync(configPath, JSON.stringify({ apiKey: 'sk-from-file' }));
  try {
    const r = resolveApiKey({ env: { ANTHROPIC_API_KEY: '   ' }, configPath });
    assert.equal(r.key, 'sk-from-file');
    assert.equal(r.source, 'config');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('returns null when neither env nor config provide a key', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'config.json'); // does not exist
  try {
    const r = resolveApiKey({ env: {}, configPath });
    assert.equal(r.key, null);
    assert.equal(r.source, null);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('invalid JSON config is ignored (treated as no key)', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'config.json');
  writeFileSync(configPath, '{ not json');
  try {
    const r = resolveApiKey({ env: {}, configPath });
    assert.equal(r.key, null);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('saveApiKey persists a readable, resolvable key', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'nested', 'config.json'); // parent created
  try {
    saveApiKey({ configPath, apiKey: '  sk-saved  ' });
    const parsed = JSON.parse(readFileSync(configPath, 'utf8'));
    assert.equal(parsed.apiKey, 'sk-saved');
    const r = resolveApiKey({ env: {}, configPath });
    assert.equal(r.key, 'sk-saved');
    assert.equal(r.source, 'config');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// The key is stored in plain text, so the file mode is the ONLY thing keeping it
// out of other accounts on the machine. `writeFileSync(..., { mode })` applies
// the mode on CREATION only — an already-existing config.json keeps whatever
// permissions it had, so a rewrite silently publishes the key.
test('saveApiKey leaves the key 0600 even when config.json already existed at 0644', (t) => {
  if (process.platform === 'win32') return t.skip('POSIX file modes only');
  const dir = tmp();
  const configPath = path.join(dir, 'config.json');
  try {
    // A pre-existing, world-readable config — exactly what an earlier version of
    // the app (or a hand-edited file) leaves behind.
    writeFileSync(configPath, JSON.stringify({ apiKey: 'sk-old' }));
    chmodSync(configPath, 0o644);
    assert.equal(statSync(configPath).mode & 0o777, 0o644, 'precondition: file is world-readable');

    saveApiKey({ configPath, apiKey: 'sk-new' });

    assert.equal(statSync(configPath).mode & 0o777, 0o600);
    assert.equal(resolveApiKey({ env: {}, configPath }).key, 'sk-new');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('saveApiKey creates a fresh config.json at 0600 regardless of umask', (t) => {
  if (process.platform === 'win32') return t.skip('POSIX file modes only');
  const dir = tmp();
  const configPath = path.join(dir, 'nested', 'config.json');
  try {
    saveApiKey({ configPath, apiKey: 'sk-fresh' });
    assert.equal(statSync(configPath).mode & 0o777, 0o600);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('saveApiKey rejects an empty key', () => {
  const dir = tmp();
  const configPath = path.join(dir, 'config.json');
  try {
    assert.throws(() => saveApiKey({ configPath, apiKey: '   ' }));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
