// node --test unit tests for the proxy handler. NO live Anthropic API calls —
// `fetch` is stubbed. Verifies request shaping: model + max_tokens injected
// server-side, the key header present, and a client-supplied model IGNORED.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  buildUpstreamPayload,
  proxyChat,
  healthPayload,
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
  assert.deepEqual(healthPayload('sk-ant-xyz'), { ok: true, hasKey: true });
  assert.deepEqual(healthPayload(undefined), { ok: true, hasKey: false });
  assert.deepEqual(healthPayload(''), { ok: true, hasKey: false });
  // the key value must not appear anywhere in the payload
  assert.equal(JSON.stringify(healthPayload('sk-ant-xyz')).includes('xyz'), false);
});
