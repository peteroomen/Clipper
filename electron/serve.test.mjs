// node --test — in-app server bootstrap (electron/serve.mjs).
//
// Exercises the main-process server WITHOUT Electron: ephemeral port binding,
// static serving from a fake dist dir, SPA fallback, and the /api routes wired
// to the REAL server/handler.mjs (reused verbatim — no live Anthropic call; the
// key-present path isn't hit, only health + keyless-mock + no-key-500).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startServer, isContainedIn, CSP } from './serve.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// Reuse the exact handler the browser proxy uses.
const handler = await import(path.join(__dirname, '..', 'server', 'handler.mjs'));

function fakeDist() {
  const dir = mkdtempSync(path.join(tmpdir(), 'clipper-dist-'));
  writeFileSync(path.join(dir, 'index.html'), '<!doctype html><title>Clipper</title><h1>rig</h1>');
  mkdirSync(path.join(dir, 'assets'));
  writeFileSync(path.join(dir, 'assets', 'app.js'), 'export const x = 1;');
  return dir;
}

test('binds an ephemeral port and serves index.html at /', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    assert.ok(h.port > 0, 'ephemeral port assigned');
    const res = await fetch(h.url);
    assert.equal(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/html/);
    assert.match(await res.text(), /rig/);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('serves a real static asset with correct MIME', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    const res = await fetch(new URL('/assets/app.js', h.url));
    assert.equal(res.status, 200);
    assert.match(res.headers.get('content-type'), /javascript/);
    assert.match(await res.text(), /export const x/);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('unknown non-api path falls back to index.html (SPA)', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    const res = await fetch(new URL('/deep/link/here', h.url));
    assert.equal(res.status, 200);
    assert.match(await res.text(), /rig/);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('/api/health reports mock:true when keyless + mock', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, apiKey: null, mock: true });
  try {
    const res = await fetch(new URL('/api/health', h.url));
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.deepEqual(body, { ok: true, hasKey: false, mock: true });
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('/api/health reports hasKey:true, mock:false when a key is present', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, apiKey: 'sk-test', mock: true });
  try {
    const body = await (await fetch(new URL('/api/health', h.url))).json();
    assert.equal(body.hasKey, true);
    assert.equal(body.mock, false); // real key always wins over mock
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('/api/chat streams canned mock SSE when keyless + mock', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, apiKey: null, mock: true });
  try {
    const res = await fetch(new URL('/api/chat', h.url), {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ messages: [{ role: 'user', content: 'tighten it up' }] }),
    });
    assert.equal(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/event-stream/);
    const text = await res.text();
    assert.match(text, /\[mock\]/);
    assert.match(text, /set_param/); // the tool_use is in the canned stream
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('/api/chat returns a clear 500 when keyless AND mock off', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, apiKey: null, mock: false });
  try {
    const res = await fetch(new URL('/api/chat', h.url), {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ messages: [] }),
    });
    assert.equal(res.status, 500);
    const body = await res.json();
    assert.match(body.error, /API key/i);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('unknown /api/* route is 404 JSON (not the SPA shell)', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    const res = await fetch(new URL('/api/nope', h.url));
    assert.equal(res.status, 404);
    assert.match(res.headers.get('content-type'), /application\/json/);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

test('path traversal cannot escape distDir', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    // %2e%2e%2f == ../  — must be contained, falling back to the SPA shell.
    const res = await fetch(new URL('/%2e%2e%2f%2e%2e%2fetc%2fpasswd', h.url));
    assert.equal(res.status, 200);
    const text = await res.text();
    assert.match(text, /rig/); // index.html, NOT /etc/passwd
    assert.doesNotMatch(text, /root:/);
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

// The main window renders model-influenced text, so the HTML response — and only
// the HTML response, since a CSP header on a .js or .wasm body does nothing — has
// to carry the policy.
test('HTML responses carry a Content-Security-Policy; asset responses do not', async () => {
  const dist = fakeDist();
  const h = await startServer({ handler, distDir: dist, mock: true });
  try {
    const res = await fetch(h.url);
    const csp = res.headers.get('content-security-policy');
    assert.ok(csp, 'index.html has a CSP header');

    // The directives the app's own threat model depends on.
    assert.match(csp, /(^|;)\s*default-src 'self'/);
    assert.match(csp, /(^|;)\s*script-src [^;]*'self'/);
    assert.match(csp, /(^|;)\s*connect-src [^;]*'self'/);
    // No remote script and no inline script: a model-authored <script> must not run.
    assert.doesNotMatch(csp, /script-src[^;]*'unsafe-inline'/);
    assert.doesNotMatch(csp, /script-src[^;]*\bhttps?:/);

    // The SPA fallback is HTML too — it must not be a hole in the policy.
    const spa = await fetch(new URL('/deep/link', h.url));
    assert.equal(spa.headers.get('content-security-policy'), csp);

    // A JS asset gets no CSP (the document's policy already governs it).
    const asset = await fetch(new URL('/assets/app.js', h.url));
    assert.equal(asset.headers.get('content-security-policy'), null);
    assert.equal(CSP, csp, 'the exported policy is the one actually served');
  } finally {
    await h.close();
    rmSync(dist, { recursive: true, force: true });
  }
});

// `filePath.startsWith(distDir)` is true for any sibling directory whose name
// merely begins with distDir. Not reachable over HTTP today (posix.normalize
// absorbs every `..` before join runs), so this pins the predicate directly
// rather than dressing it up as an end-to-end exploit that does not exist.
test('containment rejects a sibling directory that merely shares the prefix', () => {
  const dist = path.join(path.sep, 'app', 'dist');

  assert.equal(isContainedIn(dist, path.join(dist, 'index.html')), true);
  assert.equal(isContainedIn(dist, path.join(dist, 'assets', 'app.js')), true);
  assert.equal(isContainedIn(dist, dist), true, 'distDir itself is contained');

  // The bug: these all pass a bare startsWith().
  assert.equal(isContainedIn(dist, path.join(path.sep, 'app', 'dist-evil', 'secret')), false);
  assert.equal(isContainedIn(dist, `${dist}-evil`), false);
  assert.equal(isContainedIn(dist, `${dist}.bak`), false);
  assert.equal(isContainedIn(dist, path.join(path.sep, 'etc', 'passwd')), false);
});

test('two servers get distinct ephemeral ports (no collision)', async () => {
  const dist = fakeDist();
  const a = await startServer({ handler, distDir: dist, mock: true });
  const b = await startServer({ handler, distDir: dist, mock: true });
  try {
    assert.notEqual(a.port, b.port);
  } finally {
    await a.close();
    await b.close();
    rmSync(dist, { recursive: true, force: true });
  }
});
