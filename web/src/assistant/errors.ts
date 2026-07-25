// Chat error classification (post-M6). Ported in spirit from Riff's
// lib/ai/chat-errors.ts, adapted to Clipper's hand-rolled client: instead of a
// Vercel-SDK provider error, we classify a failed /api/chat by HTTP status (or
// a network failure), and map each category to distinct, actionable copy shown
// in the chat notice. Keeps "something went wrong" out of the UI.
//
// Categories:
//   proxy_unreachable — fetch itself failed (server not running)
//   missing_key       — proxy 500 (no/invalid ANTHROPIC_API_KEY); the proxy's
//                        own body carries the exact export command, preferred
//   rate_limited      — upstream 429
//   overloaded        — upstream 5xx (529/503/502 — Anthropic briefly down)
//   unknown           — anything else
//
// `refusal` is handled separately in the tool-use loop (stop_reason === refusal)
// and is intentionally NOT part of this HTTP classifier.

export type ChatErrorCode =
  | 'proxy_unreachable'
  | 'missing_key'
  | 'rate_limited'
  | 'overloaded'
  | 'unknown';

export const CHAT_ERROR_COPY: Record<ChatErrorCode, string> = {
  proxy_unreachable:
    "Couldn't reach the assistant server. Start it with `npm run server` " +
    '(and make sure ANTHROPIC_API_KEY is exported).',
  missing_key:
    'The assistant server has no valid API key. Export it and restart: ' +
    '`export ANTHROPIC_API_KEY=sk-ant-... && npm run server`.',
  rate_limited:
    'The AI is rate-limiting requests right now. Wait a few seconds and try again.',
  overloaded: 'The AI is briefly overloaded. Try again in a moment.',
  unknown:
    'Something went wrong talking to the assistant. Check the server logs (the ' +
    'terminal running `npm run server`) and try again.',
};

// Map an HTTP status from /api/chat to a category. 500 is the proxy's own
// no-key error (checked before the generic 5xx bucket); 401/403 mean the key was
// rejected upstream — both point the user at the key. 429 is rate limiting;
// other 5xx (502/503/529) are upstream overload.
export function classifyStatus(status: number): ChatErrorCode {
  if (status === 429) return 'rate_limited';
  if (status === 401 || status === 403 || status === 500) return 'missing_key';
  if (status >= 500) return 'overloaded';
  return 'unknown';
}
