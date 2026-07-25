// Clipper M6 — minimal Node 22 HTTP proxy for the Anthropic Messages API.
//
// ZERO npm dependencies: Node's built-in `http` + global `fetch`. The actual
// request shaping / upstream call lives in handler.mjs (liftable to a
// serverless function); this file is only the http glue: routing, CORS, body
// reading, and piping the streamed SSE response back to the browser verbatim.
//
// Endpoints:
//   POST /api/chat    — {messages, tools, system} -> streamed SSE from Anthropic
//   GET  /api/health  — {ok:true, hasKey:boolean}
//
// Env: ANTHROPIC_API_KEY (required for /api/chat), MODEL (default claude-opus-4-8),
//      MAX_TOKENS (default 8192), PORT (default 8787), HOST (default 127.0.0.1).
//
// BINDING: loopback ONLY by default. This process is an UNAUTHENTICATED relay to
// the developer's Anthropic key — anything that can reach it can spend the key.
// It previously called `listen(PORT)` with no host, which binds 0.0.0.0/:: and
// hands the whole LAN a free Claude endpoint (2026-07-24 audit, finding 17). Set
// HOST explicitly to opt into wider exposure; the startup banner then says so.
// There is still no auth and no rate limit, so HOST=0.0.0.0 is only ever safe on
// a network you fully trust.
//
// The API key is read from the environment, used only as the x-api-key header,
// and is NEVER logged or sent to the client. Message contents are never logged.

import { createServer } from 'node:http';
import { argv } from 'node:process';
import { realpathSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  proxyChat,
  healthPayload,
  buildMockSse,
  DEFAULT_MODEL,
  DEFAULT_MAX_TOKENS,
} from './handler.mjs';

export const DEFAULT_HOST = '127.0.0.1';
export const DEFAULT_PORT = 8787;

// Same-origin (via the Vite dev proxy) needs no CORS. For direct cross-origin
// calls from the dev app, allow the Vite dev/preview origins only. Keep simple.
const ALLOWED_ORIGINS = new Set([
  'http://localhost:5173',
  'http://localhost:4173',
]);

/**
 * The interface to bind. Loopback unless HOST says otherwise.
 * @param {NodeJS.ProcessEnv} [env]
 */
export function resolveHost(env = process.env) {
  const raw = typeof env.HOST === 'string' ? env.HOST.trim() : '';
  return raw || DEFAULT_HOST;
}

/**
 * The port to bind. Unlike `Number(env.PORT) || 8787`, an explicit PORT=0 means
 * "pick an ephemeral port" rather than silently becoming 8787.
 * @param {NodeJS.ProcessEnv} [env]
 */
export function resolvePort(env = process.env) {
  const raw = env.PORT;
  if (raw === undefined || raw === null || String(raw).trim() === '') return DEFAULT_PORT;
  const n = Number(raw);
  return Number.isInteger(n) && n >= 0 && n <= 65535 ? n : DEFAULT_PORT;
}

/** True for the addresses only this machine can reach. */
export function isLoopbackHost(host) {
  return host === '127.0.0.1' || host === 'localhost' || host === '::1' || host === '[::1]';
}

/**
 * The startup log line. Reports the address ACTUALLY bound — the old banner
 * hardcoded `http://localhost:8787` no matter where the socket really landed,
 * which is how an all-interfaces bind stayed invisible for so long.
 */
export function startupBanner({ host, port, model, maxTokens, hasKey }) {
  const shown = host === '::' ? '[::]' : host;
  let line = `Clipper proxy on http://${shown}:${port}  model=${model}  max_tokens=${maxTokens}  hasKey=${hasKey}`;
  if (!isLoopbackHost(host)) {
    line +=
      `\nWARNING: bound to ${shown} (all interfaces / non-loopback). This proxy has NO ` +
      'auth and NO rate limit — anyone who can reach this port can spend your ' +
      'Anthropic key. Unset HOST to return to loopback-only.';
  }
  return line;
}

function applyCors(req, res) {
  const origin = req.headers.origin;
  if (origin && ALLOWED_ORIGINS.has(origin)) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
    res.setHeader('Access-Control-Allow-Methods', 'POST, GET, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'content-type');
  }
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
    if (total > limitBytes) {
      // Destroy the request, don't just stop reading it: replying 400 while the
      // client keeps uploading into a socket nobody drains leaves the connection
      // (and its buffers) alive for as long as the sender wants.
      req.destroy();
      throw new Error('request body too large');
    }
    chunks.push(chunk);
  }
  if (total === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

/**
 * Build the proxy's http.Server WITHOUT listening. Config is read from `env`
 * (defaulting to process.env) so this is constructible in a test.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {import('node:http').Server}
 */
export function createProxyServer(opts = {}) {
  const env = opts.env || process.env;
  const model = env.MODEL || DEFAULT_MODEL;
  const maxTokens = Number(env.MAX_TOKENS) || DEFAULT_MAX_TOKENS;
  // Keyless dev MOCK mode: only active when there is NO key AND MOCK=1. With a key
  // present it is ignored (real Anthropic always wins).
  const mock = env.MOCK === '1';

  return createServer(async (req, res) => {
    applyCors(req, res);

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }

    let url;
    try {
      url = new URL(req.url, 'http://localhost');
    } catch {
      sendJson(res, 400, { error: 'Bad request URL.' });
      return;
    }

    if (req.method === 'GET' && url.pathname === '/api/health') {
      sendJson(res, 200, healthPayload(env.ANTHROPIC_API_KEY, mock));
      return;
    }

    if (req.method === 'POST' && url.pathname === '/api/chat') {
      const apiKey = env.ANTHROPIC_API_KEY;
      if (!apiKey && !mock) {
        // Clear, actionable 500 — no key configured and mock disabled. This is the
        // DEFAULT keyless behavior. Never leak anything.
        sendJson(res, 500, {
          error:
            'ANTHROPIC_API_KEY is not set on the server. Export it and restart: ' +
            'export ANTHROPIC_API_KEY=sk-ant-... && npm run server',
        });
        return;
      }

      let clientBody;
      try {
        clientBody = await readJsonBody(req);
      } catch {
        // The socket may already be destroyed (body-limit overflow); writing to a
        // dead response throws, and there is nobody left to tell anyway.
        if (!res.writableEnded && !req.destroyed) {
          sendJson(res, 400, { error: 'Invalid JSON request body.' });
        }
        return;
      }

      // Keyless MOCK mode: stream a canned SSE response instead of calling
      // Anthropic. Deterministic, clearly labeled "[mock]" in the text.
      if (!apiKey) {
        const body = Buffer.from(buildMockSse(clientBody));
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
        upstream = await proxyChat({ clientBody, apiKey, model, maxTokens });
      } catch {
        // Network / DNS failure reaching Anthropic. Do not include the error
        // object (could theoretically carry request detail); keep it generic.
        sendJson(res, 502, { error: 'Failed to reach the Anthropic API.' });
        return;
      }

      if (!upstream.ok) {
        // Pass the upstream error status through, but read the body ourselves so
        // we return a compact, non-streaming JSON the client can display. The key
        // is never part of an error body.
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

      // Happy path: pipe the SSE stream through verbatim.
      res.writeHead(200, {
        'content-type': upstream.headers.get('content-type') || 'text/event-stream',
        'cache-control': 'no-cache, no-transform',
        connection: 'keep-alive',
      });
      try {
        for await (const chunk of upstream.body) {
          res.write(chunk);
        }
      } catch {
        /* client disconnected or stream aborted — nothing to log */
      } finally {
        res.end();
      }
      return;
    }

    sendJson(res, 404, { error: 'Not found.' });
  });
}

/**
 * Create AND bind the proxy.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {Promise<{server: import('node:http').Server, host: string, port: number, url: string, close: () => Promise<void>}>}
 */
export function startProxy(opts = {}) {
  const env = opts.env || process.env;
  const host = resolveHost(env);
  const port = resolvePort(env);
  const server = createProxyServer({ env });

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.removeListener('error', reject);
      const addr = server.address();
      const boundPort = typeof addr === 'object' && addr ? addr.port : port;
      resolve({
        server,
        host,
        port: boundPort,
        url: `http://${host === '::' ? '[::]' : host}:${boundPort}/`,
        close: () => new Promise((done) => server.close(() => done())),
      });
    });
  });
}

// Only self-start when this file IS the process entry point, so `node --test`
// can import the exports above without a stray listener appearing.
function isEntryPoint() {
  const entry = argv[1];
  if (!entry) return false;
  try {
    return realpathSync(entry) === realpathSync(fileURLToPath(import.meta.url));
  } catch {
    return false;
  }
}

if (isEntryPoint()) {
  const env = process.env;
  startProxy({ env })
    .then(({ host, port }) => {
      // Log only non-sensitive startup facts (never the key or its value).
      console.log(
        startupBanner({
          host,
          port,
          model: env.MODEL || DEFAULT_MODEL,
          maxTokens: Number(env.MAX_TOKENS) || DEFAULT_MAX_TOKENS,
          hasKey: Boolean(env.ANTHROPIC_API_KEY),
        })
      );
    })
    .catch((err) => {
      // A bad HOST must fail loudly — never silently fall back to every interface.
      console.error(`Clipper proxy failed to bind ${resolveHost(env)}:${resolvePort(env)} — ${err?.message || err}`);
      process.exitCode = 1;
    });
}
