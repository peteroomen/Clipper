// Clipper M6 — Anthropic proxy handler.
//
// This module is the whole proxy, minus the Node http glue in index.mjs. It is
// deliberately structured so the request-shaping core (`buildUpstreamPayload`)
// and the upstream call (`proxyChat`) are pure/injectable — they take a `fetch`
// and never reach for globals — so the same code could be lifted into a
// serverless function (Vercel/Cloudflare) by swapping index.mjs for a platform
// entry point. Zero npm dependencies: Node's built-in http (in index.mjs) and
// the global `fetch`.
//
// Security: the API key lives ONLY here / in the environment. It is never sent
// to the client, never logged, and never echoed. Message contents are never
// logged either.

export const DEFAULT_MODEL = 'claude-opus-4-8';
export const DEFAULT_MAX_TOKENS = 8192;
export const ANTHROPIC_URL = 'https://api.anthropic.com/v1/messages';
export const ANTHROPIC_VERSION = '2023-06-01';

// Build the upstream Anthropic request body from the client's {messages, tools,
// system}. model / max_tokens / stream / thinking are injected SERVER-SIDE — a
// client-supplied `model` (or max_tokens, stream, thinking) is intentionally
// dropped, so the client can never choose the model or the thinking policy.
//
// Thinking: `{type: "adaptive"}` is set explicitly — on Opus 4.8 omitting it
// runs WITHOUT thinking. budget_tokens / temperature / top_p / top_k are never
// sent (they 400 on Opus 4.8). The client is responsible for putting
// cache_control on the last stable system block.
export function buildUpstreamPayload(clientBody, { model, maxTokens } = {}) {
  const body = clientBody && typeof clientBody === 'object' ? clientBody : {};
  const payload = {
    model: model || DEFAULT_MODEL, // server-injected; client's model ignored
    max_tokens: maxTokens || DEFAULT_MAX_TOKENS, // server-injected
    stream: true, // streaming ALWAYS
    thinking: { type: 'adaptive' }, // explicit — Opus 4.8 needs it
    messages: Array.isArray(body.messages) ? body.messages : [],
  };
  if (body.system !== undefined) payload.system = body.system;
  if (Array.isArray(body.tools)) payload.tools = body.tools;
  return payload;
}

// Call the Anthropic Messages API with the shaped payload. `fetchImpl` is
// injected so tests can stub it (and so a serverless runtime can supply its own
// fetch). Returns the upstream Response (its body is an SSE stream).
export async function proxyChat({ clientBody, apiKey, model, maxTokens, fetchImpl = fetch }) {
  const payload = buildUpstreamPayload(clientBody, { model, maxTokens });
  return fetchImpl(ANTHROPIC_URL, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'x-api-key': apiKey,
      'anthropic-version': ANTHROPIC_VERSION,
    },
    body: JSON.stringify(payload),
  });
}

// Health payload — advertises whether a key is configured WITHOUT leaking it.
export function healthPayload(apiKey) {
  return { ok: true, hasKey: Boolean(apiKey) };
}
