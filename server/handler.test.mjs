// node --test unit tests for the proxy handler. NO live Anthropic API calls —
// `fetch` is stubbed. Verifies request shaping: model + max_tokens injected
// server-side, the key header present, and a client-supplied model IGNORED.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  buildUpstreamPayload,
  proxyChat,
  healthPayload,
  buildMockSse,
  MOCK_LABEL,
  DEFAULT_MODEL,
  DEFAULT_MAX_TOKENS,
  ANTHROPIC_URL,
} from './handler.mjs';

test('buildUpstreamPayload injects model, max_tokens, stream, thinking', () => {
  const payload = buildUpstreamPayload(
    { messages: [{ role: 'user', content: 'hi' }] },
    { model: 'claude-opus-4-8', maxTokens: 8192 }
  );
  assert.equal(payload.model, 'claude-opus-4-8');
  assert.equal(payload.max_tokens, 8192);
  assert.equal(payload.stream, true);
  assert.deepEqual(payload.thinking, { type: 'adaptive' });
  assert.deepEqual(payload.messages, [{ role: 'user', content: 'hi' }]);
});

test('buildUpstreamPayload IGNORES a client-supplied model / max_tokens / stream', () => {
  const payload = buildUpstreamPayload(
    {
      model: 'gpt-4-hacker', // must be dropped
      max_tokens: 999999, // must be dropped
      stream: false, // must be dropped
      thinking: { type: 'disabled' }, // must be dropped
      messages: [],
    },
    { model: 'claude-opus-4-8', maxTokens: 8192 }
  );
  assert.equal(payload.model, 'claude-opus-4-8');
  assert.equal(payload.max_tokens, 8192);
  assert.equal(payload.stream, true);
  assert.deepEqual(payload.thinking, { type: 'adaptive' });
});

test('buildUpstreamPayload passes through system and tools when present', () => {
  const system = [{ type: 'text', text: 'coach', cache_control: { type: 'ephemeral' } }];
  const tools = [{ name: 'set_param', description: '', input_schema: { type: 'object' } }];
  const payload = buildUpstreamPayload(
    { system, tools, messages: [] },
    { model: DEFAULT_MODEL, maxTokens: DEFAULT_MAX_TOKENS }
  );
  assert.deepEqual(payload.system, system);
  assert.deepEqual(payload.tools, tools);
});

test('buildUpstreamPayload falls back to defaults + empty messages', () => {
  const payload = buildUpstreamPayload(undefined);
  assert.equal(payload.model, DEFAULT_MODEL);
  assert.equal(payload.max_tokens, DEFAULT_MAX_TOKENS);
  assert.deepEqual(payload.messages, []);
  assert.equal('system' in payload, false);
  assert.equal('tools' in payload, false);
});

test('proxyChat sends the x-api-key + version headers and injected model', async () => {
  let captured = null;
  const fetchStub = async (url, init) => {
    captured = { url, init };
    return { ok: true, body: null, headers: new Map() };
  };
  await proxyChat({
    clientBody: { model: 'client-choice', messages: [{ role: 'user', content: 'x' }] },
    apiKey: 'sk-ant-secret',
    model: 'claude-opus-4-8',
    maxTokens: 8192,
    fetchImpl: fetchStub,
  });
  assert.equal(captured.url, ANTHROPIC_URL);
  assert.equal(captured.init.method, 'POST');
  assert.equal(captured.init.headers['x-api-key'], 'sk-ant-secret');
  assert.equal(captured.init.headers['anthropic-version'], '2023-06-01');
  const sent = JSON.parse(captured.init.body);
  assert.equal(sent.model, 'claude-opus-4-8'); // NOT "client-choice"
  assert.equal(sent.max_tokens, 8192);
  assert.equal(sent.stream, true);
});

test('healthPayload reports hasKey without leaking the key', () => {
  assert.deepEqual(healthPayload('sk-ant-xyz'), { ok: true, hasKey: true, mock: false });
  assert.deepEqual(healthPayload(undefined), { ok: true, hasKey: false, mock: false });
  assert.deepEqual(healthPayload(''), { ok: true, hasKey: false, mock: false });
  // the key value must not appear anywhere in the payload
  assert.equal(JSON.stringify(healthPayload('sk-ant-xyz')).includes('xyz'), false);
});

test('healthPayload advertises mock ONLY when there is no key', () => {
  // mock=true is only reported when a key is absent (real key always wins).
  assert.deepEqual(healthPayload(undefined, true), { ok: true, hasKey: false, mock: true });
  assert.deepEqual(healthPayload('', true), { ok: true, hasKey: false, mock: true });
  assert.deepEqual(healthPayload('sk-ant-xyz', true), { ok: true, hasKey: true, mock: false });
  assert.deepEqual(healthPayload(undefined, false), { ok: true, hasKey: false, mock: false });
});

test('proxyChat passes upstream error status codes through faithfully', async () => {
  // The handler returns the upstream Response as-is; index.mjs echoes its status.
  // Verify a 429 / 529 upstream is surfaced verbatim (not swallowed to 200/500).
  for (const status of [429, 529, 503, 401]) {
    const fetchStub = async () => ({ ok: false, status, text: async () => '{"error":{"message":"x"}}' });
    const resp = await proxyChat({
      clientBody: { messages: [] },
      apiKey: 'sk-ant-x',
      model: DEFAULT_MODEL,
      maxTokens: DEFAULT_MAX_TOKENS,
      fetchImpl: fetchStub,
    });
    assert.equal(resp.ok, false);
    assert.equal(resp.status, status);
  }
});

// ── MOCK mode ────────────────────────────────────────────────────────────────
// Parse an SSE body into an ordered list of event objects (mirrors the client).
function parseSse(body) {
  const events = [];
  for (const raw of body.split('\n\n')) {
    for (const line of raw.split('\n')) {
      if (line.startsWith('data:')) {
        const payload = line.slice(5).trim();
        if (payload && payload !== '[DONE]') events.push(JSON.parse(payload));
      }
    }
  }
  return events;
}

test('buildMockSse first leg: labeled text + one set_param tool_use, stop_reason tool_use', () => {
  const body = buildMockSse({ messages: [{ role: 'user', content: [{ type: 'text', text: 'tighter' }] }] });
  const events = parseSse(body);

  const textDelta = events.find((e) => e.type === 'content_block_delta' && e.delta?.type === 'text_delta');
  assert.ok(textDelta.delta.text.includes(MOCK_LABEL), 'text is labeled [mock]');

  const toolStart = events.find(
    (e) => e.type === 'content_block_start' && e.content_block?.type === 'tool_use'
  );
  assert.ok(toolStart, 'a tool_use block is emitted');
  assert.equal(toolStart.content_block.name, 'set_param');

  const jsonDelta = events.find((e) => e.type === 'content_block_delta' && e.delta?.type === 'input_json_delta');
  const input = JSON.parse(jsonDelta.delta.partial_json);
  assert.deepEqual(input, { unit: 'pedal', param: 'dist', value: 0.4 });

  const msgDelta = events.find((e) => e.type === 'message_delta');
  assert.equal(msgDelta.delta.stop_reason, 'tool_use');
});

test('buildMockSse second leg: text-only wrap-up when a tool_result is present', () => {
  const body = buildMockSse({
    messages: [
      { role: 'user', content: [{ type: 'text', text: 'tighter' }] },
      { role: 'assistant', content: [{ type: 'tool_use', id: 'toolu_mock_1', name: 'set_param', input: {} }] },
      { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'toolu_mock_1', content: '{}' }] },
    ],
  });
  const events = parseSse(body);

  // No tool_use this leg — pure end_turn text.
  assert.ok(!events.some((e) => e.type === 'content_block_start' && e.content_block?.type === 'tool_use'));
  const textDelta = events.find((e) => e.type === 'content_block_delta' && e.delta?.type === 'text_delta');
  assert.ok(textDelta.delta.text.includes(MOCK_LABEL));
  const msgDelta = events.find((e) => e.type === 'message_delta');
  assert.equal(msgDelta.delta.stop_reason, 'end_turn');
});

test('buildMockSse is deterministic for the same request', () => {
  const req = { messages: [{ role: 'user', content: [{ type: 'text', text: 'x' }] }] };
  assert.equal(buildMockSse(req), buildMockSse(req));
});
