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
//      MAX_TOKENS (default 8192), PORT (default 8787).
//
// The API key is read from the environment, used only as the x-api-key header,
// and is NEVER logged or sent to the client. Message contents are never logged.

import { createServer } from 'node:http';
import {
  proxyChat,
  healthPayload,
  DEFAULT_MODEL,
  DEFAULT_MAX_TOKENS,
} from './handler.mjs';

const PORT = Number(process.env.PORT) || 8787;
const MODEL = process.env.MODEL || DEFAULT_MODEL;
const MAX_TOKENS = Number(process.env.MAX_TOKENS) || DEFAULT_MAX_TOKENS;

// Same-origin (via the Vite dev proxy) needs no CORS. For direct cross-origin
// calls from the dev app, allow the Vite dev/preview origins only. Keep simple.
const ALLOWED_ORIGINS = new Set([
  'http://localhost:5173',
  'http://localhost:4173',
]);

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
    if (total > limitBytes) throw new Error('request body too large');
    chunks.push(chunk);
  }
  if (total === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

const server = createServer(async (req, res) => {
  applyCors(req, res);

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  const url = new URL(req.url, `http://localhost:${PORT}`);

  if (req.method === 'GET' && url.pathname === '/api/health') {
    sendJson(res, 200, healthPayload(process.env.ANTHROPIC_API_KEY));
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/chat') {
    const apiKey = process.env.ANTHROPIC_API_KEY;
    if (!apiKey) {
      // Clear, actionable 500 — no key configured. Never leak anything.
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
      sendJson(res, 400, { error: 'Invalid JSON request body.' });
      return;
    }

    let upstream;
    try {
      upstream = await proxyChat({ clientBody, apiKey, model: MODEL, maxTokens: MAX_TOKENS });
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

server.listen(PORT, () => {
  const hasKey = Boolean(process.env.ANTHROPIC_API_KEY);
  // Log only non-sensitive startup facts (never the key or its value).
  console.log(
    `Clipper proxy on http://localhost:${PORT}  model=${MODEL}  max_tokens=${MAX_TOKENS}  hasKey=${hasKey}`
  );
});
