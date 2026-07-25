// Clipper desktop — in-process HTTP server for the Electron shell.
//
// This is the same proxy the browser build talks to (`server/index.mjs`), with
// two differences that the desktop shell needs:
//
//   1. It ALSO serves the built web app (`web/dist`) as statics, so the
//      BrowserWindow can load http://localhost:<port>/ and the app's relative
//      `/api/*` fetches "just work" — identical behavior to `npm run server`
//      fronted by a static host, no file:// hacks.
//   2. It binds an EPHEMERAL localhost port (port 0) so multiple instances never
//      collide, and it takes its config (api key, mock, model, distDir, and the
//      handler module itself) by injection instead of reading process.env — so
//      the same function is trivially unit-testable and so the packaged app can
//      point it at paths inside the .app bundle.
//
// The request-shaping / upstream-call / mock-SSE logic is NOT duplicated here:
// it is reused verbatim from `server/handler.mjs` (passed in as `handler`). The
// only thing duplicated from `server/index.mjs` is the unavoidable http glue
// (body reading, JSON replies, streaming the SSE through) — a few dozen lines.

import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
};

// Content-Security-Policy for the app document. The main window renders
// model-influenced text, so it gets the same treatment key-prompt.html already
// had. Served as a HEADER (not a <meta> in web/index.html) so it applies to the
// desktop shell without also applying under `vite dev`, where it would fight HMR.
//
// Each relaxation is here because the built app measurably needs it:
//   'wasm-unsafe-eval' — the engine is `WebAssembly.instantiate()` over a binary
//        embedded in web/public/generated/clipper.js (Emscripten SINGLE_FILE).
//        Chromium refuses ALL Wasm compilation once a script-src directive exists
//        unless this (or the far broader 'unsafe-eval') is present, so without it
//        the CSP silently kills every sound the app makes.
//   style-src 'unsafe-inline' — React `style={{…}}` props are inline styles.
//   img-src data: — the inline data-URI favicon in web/index.html.
//   font-src data: — the single @font-face in web/src/styles/tokens.css ships its
//        woff2 as a base64 data: URI, which `default-src 'self'` rejects.
// Deliberately absent: any remote origin, and any inline/eval'd SCRIPT. base-uri
// has no default-src fallback, so it is pinned explicitly.
export const CSP = [
  "default-src 'self'",
  "script-src 'self' 'wasm-unsafe-eval'",
  "style-src 'self' 'unsafe-inline'",
  "connect-src 'self'",
  "img-src 'self' data:",
  "font-src 'self' data:",
  "object-src 'none'",
  "base-uri 'none'",
  "frame-ancestors 'none'",
].join('; ');

/**
 * Is `filePath` inside `dir` (or `dir` itself)?
 *
 * A bare `filePath.startsWith(dir)` is true for any SIBLING whose name merely
 * begins with `dir` — `/app/dist-evil/secret` "passes" a `/app/dist` check. Not
 * reachable over HTTP today (path.posix.normalize absorbs every `..` before the
 * join), but the predicate was wrong regardless of reachability.
 */
export function isContainedIn(dir, filePath) {
  if (filePath === dir) return true;
  return filePath.startsWith(dir.endsWith(path.sep) ? dir : dir + path.sep);
}

function sendJson(res, status, obj) {
  const bytes = Buffer.from(JSON.stringify(obj));
  res.writeHead(status, {
    'content-type': 'application/json',
    'content-length': bytes.length,
  });
  res.end(bytes);
}

async function readJsonBody(req, limitBytes = 5_000_000) {
  const chunks = [];
  let total = 0;
  for await (const chunk of req) {
    total += chunk.length;
    if (total > limitBytes) throw new Error('request body too large');
    chunks.push(chunk);
  }
  if (total === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

// Resolve a request path to a real file inside `distDir`, guarding against
// path traversal. Missing paths fall back to index.html (SPA behavior).
async function resolveStatic(distDir, pathname) {
  const decoded = decodeURIComponent(pathname);
  const normalized = path.posix.normalize(decoded).replace(/^(\.\.(\/|\\|$))+/, '');
  let filePath = path.join(distDir, normalized);
  if (!isContainedIn(distDir, filePath)) filePath = path.join(distDir, 'index.html');
  try {
    const s = await stat(filePath);
    if (s.isDirectory()) filePath = path.join(filePath, 'index.html');
    else return filePath;
    await stat(filePath); // ensure the dir's index.html exists
    return filePath;
  } catch {
    // Unknown path -> serve the SPA shell so client routing can take over.
    return path.join(distDir, 'index.html');
  }
}

function serveStatic(res, filePath) {
  const ext = path.extname(filePath).toLowerCase();
  const type = MIME[ext] || 'application/octet-stream';
  const headers = { 'content-type': type, 'cache-control': 'no-cache' };
  // Only the document carries the policy — it already governs every subresource
  // the document pulls in, and a CSP header on a .js or .wasm body does nothing.
  if (ext === '.html') headers['content-security-policy'] = CSP;
  res.writeHead(200, headers);
  const stream = createReadStream(filePath);
  stream.on('error', () => {
    if (!res.headersSent) res.writeHead(404);
    res.end();
  });
  stream.pipe(res);
}

/**
 * Start the in-process Clipper server.
 *
 * @param {object} opts
 * @param {object} opts.handler   The `server/handler.mjs` module exports (reused verbatim).
 * @param {string} opts.distDir   Absolute path to the built web app (web/dist).
 * @param {string|null} [opts.apiKey]  Anthropic key, or null/undefined for keyless.
 * @param {boolean} [opts.mock]   When true AND no key, /api/chat streams canned SSE.
 * @param {string}  [opts.model]  Model override (defaults to handler.DEFAULT_MODEL).
 * @param {number}  [opts.maxTokens] Max tokens (defaults to handler.DEFAULT_MAX_TOKENS).
 * @param {string}  [opts.host]   Bind host (default 127.0.0.1).
 * @param {number}  [opts.port]   Bind port (default 0 = ephemeral).
 * @returns {Promise<{server: import('node:http').Server, port: number, url: string, close: () => Promise<void>}>}
 */
export function startServer(opts) {
  const {
    handler,
    distDir,
    apiKey = null,
    mock = false,
    host = '127.0.0.1',
    port = 0,
  } = opts || {};
  if (!handler) throw new Error('startServer: `handler` (server/handler.mjs exports) is required');
  if (!distDir) throw new Error('startServer: `distDir` is required');

  const model = opts.model || handler.DEFAULT_MODEL;
  const maxTokens = opts.maxTokens || handler.DEFAULT_MAX_TOKENS;

  const server = createServer(async (req, res) => {
    let url;
    try {
      url = new URL(req.url, `http://${host}`);
    } catch {
      sendJson(res, 400, { error: 'Bad request URL.' });
      return;
    }

    // --- /api/health ---------------------------------------------------------
    if (req.method === 'GET' && url.pathname === '/api/health') {
      sendJson(res, 200, handler.healthPayload(apiKey, mock));
      return;
    }

    // --- /api/chat -----------------------------------------------------------
    if (req.method === 'POST' && url.pathname === '/api/chat') {
      if (!apiKey && !mock) {
        sendJson(res, 500, {
          error:
            'No Anthropic API key configured. Set ANTHROPIC_API_KEY, add a key in ' +
            'the app, or relaunch with MOCK=1 for the keyless demo.',
        });
        return;
      }

      let clientBody;
      try {
        clientBody = await readJsonBody(req);
      } catch {
        sendJson(res, 400, { error: 'Invalid JSON request body.' });
        return;
      }

      // Keyless MOCK: stream the canned SSE (reused from handler.mjs).
      if (!apiKey) {
        const body = Buffer.from(handler.buildMockSse(clientBody));
        res.writeHead(200, {
          'content-type': 'text/event-stream',
          'cache-control': 'no-cache, no-transform',
          connection: 'keep-alive',
        });
        res.end(body);
        return;
      }

      let upstream;
      try {
        upstream = await handler.proxyChat({ clientBody, apiKey, model, maxTokens });
      } catch {
        sendJson(res, 502, { error: 'Failed to reach the Anthropic API.' });
        return;
      }

      if (!upstream.ok) {
        const text = await upstream.text().catch(() => '');
        let message = `Anthropic API error (${upstream.status}).`;
        try {
          const parsed = JSON.parse(text);
          if (parsed?.error?.message) message = parsed.error.message;
        } catch {
          /* non-JSON upstream error — keep the generic message */
        }
        sendJson(res, upstream.status, { error: message });
        return;
      }

      res.writeHead(200, {
        'content-type': upstream.headers.get('content-type') || 'text/event-stream',
        'cache-control': 'no-cache, no-transform',
        connection: 'keep-alive',
      });
      try {
        for await (const chunk of upstream.body) res.write(chunk);
      } catch {
        /* client disconnected — nothing to log */
      } finally {
        res.end();
      }
      return;
    }

    if (url.pathname.startsWith('/api/')) {
      sendJson(res, 404, { error: 'Not found.' });
      return;
    }

    // --- statics (the built web app) -----------------------------------------
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      sendJson(res, 405, { error: 'Method not allowed.' });
      return;
    }
    const filePath = await resolveStatic(distDir, url.pathname);
    serveStatic(res, filePath);
  });

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.removeListener('error', reject);
      const addr = server.address();
      const boundPort = typeof addr === 'object' && addr ? addr.port : port;
      resolve({
        server,
        port: boundPort,
        url: `http://${host}:${boundPort}/`,
        close: () =>
          new Promise((res2) => server.close(() => res2())),
      });
    });
  });
}
