// node --test unit tests for the pure history-trimming function. Run via the
// root `npm run test:history` (Node 22 native TS type-stripping). Excluded from
// the web tsc build (see web/tsconfig.json) and from the Playwright testDir, so
// it never interferes with those suites.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  trimHistory,
  MAX_MESSAGES,
  KEEP_FULL,
  TRIMMED_MARKER,
  type HistoryMessage,
} from './history.ts';

// ── builders ────────────────────────────────────────────────────────────────
const userText = (t: string): HistoryMessage => ({
  role: 'user',
  content: [{ type: 'text', text: t }],
});
const assistantText = (t: string): HistoryMessage => ({
  role: 'assistant',
  content: [{ type: 'text', text: t }],
});
// An assistant turn with a leading thinking block + a tool_use.
const assistantThinkTool = (id: string): HistoryMessage => ({
  role: 'assistant',
  content: [
    { type: 'thinking', thinking: 'hmm', signature: 'sig-xyz' },
    { type: 'text', text: 'let me set that' },
    { type: 'tool_use', id, name: 'set_param', input: { unit: 'pedal', param: 'dist', value: 0.4 } },
  ],
});
const userToolResult = (id: string, payload: string): HistoryMessage => ({
  role: 'user',
  content: [{ type: 'tool_result', tool_use_id: id, content: payload }],
});

// One completed exchange = user, assistant(think+tool_use), user(tool_result), assistant(text).
function exchange(n: number): HistoryMessage[] {
  return [
    userText(`msg ${n}`),
    assistantThinkTool(`toolu_${n}`),
    userToolResult(`toolu_${n}`, `{"applied":${n}}`),
    assistantText(`done ${n}`),
  ];
}

const isThinking = (b: unknown) =>
  !!b && ((b as { type?: string }).type === 'thinking' || (b as { type?: string }).type === 'redacted_thinking');
const contentOf = (m: HistoryMessage) => m.content as Array<Record<string, unknown>>;

test('exports the documented default constants', () => {
  assert.equal(MAX_MESSAGES, 20);
  assert.equal(KEEP_FULL, 5);
  assert.equal(typeof TRIMMED_MARKER, 'string');
});

test('caps history to MAX_MESSAGES', () => {
  // 10 exchanges = 40 messages, all clean-user-started.
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 10; i++) history.push(...exchange(i));
  const out = trimHistory(history);
  assert.ok(out.length <= MAX_MESSAGES, `length ${out.length} <= ${MAX_MESSAGES}`);
});

test('the window ALWAYS starts on a genuine user turn', () => {
  // Force a cap boundary that would otherwise land mid-exchange.
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 10; i++) history.push(...exchange(i));
  const out = trimHistory(history);
  assert.equal(out[0].role, 'user');
  // ...and it is NOT an orphaned tool_result carrier.
  assert.ok(!contentOf(out[0]).some((b) => b.type === 'tool_result'));
});

test('never starts on a leading assistant / orphan tool_result even when capped there', () => {
  // Head is an orphaned tool_result + assistant, then a real exchange.
  const history: HistoryMessage[] = [
    userToolResult('orphan', 'x'), // orphan (no preceding tool_use)
    assistantText('stray'),
    ...exchange(1),
  ];
  const out = trimHistory(history);
  assert.equal(out[0].role, 'user');
  assert.ok(!contentOf(out[0]).some((b) => b.type === 'tool_result'));
  // The orphan tool_result is gone.
  assert.equal(out[0].content[0] && (out[0].content[0] as Record<string, unknown>).type, 'text');
});

test('tool_use/tool_result pairing is preserved in trimmed (old) turns', () => {
  // Enough exchanges that exchange 0 is OLD (before the protected window).
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 8; i++) history.push(...exchange(i));
  const out = trimHistory(history);

  // Collect surviving tool_use ids and tool_result ids.
  const toolUseIds = new Set<string>();
  const toolResultIds = new Set<string>();
  for (const m of out) {
    for (const b of contentOf(m)) {
      if (b.type === 'tool_use') toolUseIds.add(b.id as string);
      if (b.type === 'tool_result') toolResultIds.add(b.tool_use_id as string);
    }
  }
  // Every surviving tool_use has a matching tool_result and vice-versa.
  for (const id of toolUseIds) assert.ok(toolResultIds.has(id), `tool_use ${id} keeps its result`);
  for (const id of toolResultIds) assert.ok(toolUseIds.has(id), `tool_result ${id} keeps its use`);
});

test('OLD tool_result payloads become the marker; recent ones stay full', () => {
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 8; i++) history.push(...exchange(i));
  const out = trimHistory(history);

  const results = out.flatMap((m) => contentOf(m).filter((b) => b.type === 'tool_result'));
  const trimmed = results.filter((b) => b.content === TRIMMED_MARKER);
  const full = results.filter((b) => b.content !== TRIMMED_MARKER);
  assert.ok(trimmed.length > 0, 'some old tool_results were trimmed to the marker');
  assert.ok(full.length > 0, 'the most recent tool_results kept their full payload');
});

test('thinking is stripped from OLD assistant turns only, never the current turn', () => {
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 8; i++) history.push(...exchange(i));
  // Simulate an ACTIVE tool loop: the last exchange is still in flight —
  // user text, then assistant(think+tool_use) awaiting a result.
  history.push(userText('in flight'), assistantThinkTool('toolu_live'));
  const out = trimHistory(history);

  // The current turn (the trailing in-flight assistant) keeps its thinking + signature.
  const live = out[out.length - 1];
  assert.equal(live.role, 'assistant');
  const liveThinking = contentOf(live).find(isThinking) as Record<string, unknown> | undefined;
  assert.ok(liveThinking, 'in-flight thinking block preserved');
  assert.equal(liveThinking!.signature, 'sig-xyz');

  // At least one OLD assistant turn had its thinking removed.
  const anyOldStripped = out
    .slice(0, out.length - 2)
    .some((m) => m.role === 'assistant' && !contentOf(m).some(isThinking));
  assert.ok(anyOldStripped, 'an old assistant turn was stripped of thinking');
});

test('is pure — does not mutate its input', () => {
  const history: HistoryMessage[] = [];
  for (let i = 0; i < 8; i++) history.push(...exchange(i));
  const snapshot = JSON.stringify(history);
  trimHistory(history);
  assert.equal(JSON.stringify(history), snapshot, 'input history unchanged');
});

test('short histories pass through untouched', () => {
  const history = [userText('hi'), assistantText('hey')];
  const out = trimHistory(history);
  assert.equal(out.length, 2);
  assert.equal(out[0], history[0]); // same reference — untouched
  assert.equal(out[1], history[1]);
});

test('empty / malformed input yields an empty history', () => {
  assert.deepEqual(trimHistory([]), []);
  assert.deepEqual(trimHistory([assistantText('orphan assistant')]), []);
});
