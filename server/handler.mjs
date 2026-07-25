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

// Health payload — advertises whether a key is configured WITHOUT leaking it,
// and whether the keyless dev MOCK mode is active (so the UI can show a friendly
// "[mock]" notice instead of the scary "no key" one).
export function healthPayload(apiKey, mock = false) {
  return { ok: true, hasKey: Boolean(apiKey), mock: Boolean(mock) && !apiKey };
}

// Mock provider (post-M6). Pattern ported in spirit from Riff's
// lib/ai/mock-provider.ts, adapted to Clipper's raw SSE: when the server runs
// WITHOUT a key and MOCK=1, `/api/chat` streams these canned events instead of a
// 500, so the whole chat UX (streamed text + the tool-use loop + the applied
// chips) works end-to-end for local dev, with no Anthropic key or spend.
//
// Deterministic two-step script, keyed off the request's own messages:
//   - First request (no tool_result yet): a short "[mock]" line + ONE set_param
//     tool_use (pedal dist -> 0.40) so the loop runs and a knob visibly moves.
//   - Follow-up request (carries the tool_result): a text-only "[mock]" wrap-up.
export const MOCK_LABEL = '[mock]';

// Does the request already contain a tool_result? (i.e. we're on the loop's
// second leg, after the client executed our tool_use.)
function historyHasToolResult(clientBody) {
  const messages =
    clientBody && Array.isArray(clientBody.messages) ? clientBody.messages : [];
  return messages.some(
    (m) =>
      m &&
      Array.isArray(m.content) &&
      m.content.some((b) => b && b.type === 'tool_result')
  );
}

// Serialize a list of Anthropic stream events as an SSE body (same wire shape
// the real proxy pipes through, so the client parser is exercised identically).
function toSse(events) {
  return events.map((e) => `event: ${e.type}\ndata: ${JSON.stringify(e)}`).join('\n\n') + '\n\n';
}

// Build the canned SSE body for one mock turn from the request's messages.
export function buildMockSse(clientBody) {
  if (historyHasToolResult(clientBody)) {
    // Second leg: plain end_turn wrap-up.
    return toSse([
      { type: 'message_start', message: { id: 'msg_mock_2', type: 'message', role: 'assistant', content: [] } },
      { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
      {
        type: 'content_block_delta',
        index: 0,
        delta: {
          type: 'text_delta',
          text: `${MOCK_LABEL} Done — dist is at 40 now. That eases the fizz so palm mutes sit tighter.`,
        },
      },
      { type: 'content_block_stop', index: 0 },
      { type: 'message_delta', delta: { stop_reason: 'end_turn' }, usage: { output_tokens: 12 } },
      { type: 'message_stop' },
    ]);
  }

  // First leg: a short line + a set_param tool_use (pedal dist -> 0.40).
  return toSse([
    { type: 'message_start', message: { id: 'msg_mock_1', type: 'message', role: 'assistant', content: [] } },
    { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
    {
      type: 'content_block_delta',
      index: 0,
      delta: { type: 'text_delta', text: `${MOCK_LABEL} Tightening things up — easing the distortion back a touch.` },
    },
    { type: 'content_block_stop', index: 0 },
    { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_mock_1', name: 'set_param', input: {} } },
    {
      type: 'content_block_delta',
      index: 1,
      delta: { type: 'input_json_delta', partial_json: '{"unit":"pedal","param":"dist","value":0.4}' },
    },
    { type: 'content_block_stop', index: 1 },
    { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 18 } },
    { type: 'message_stop' },
  ]);
}
