import { test, expect, type Route } from '@playwright/test';

// M6 assistant tests. The Anthropic proxy is MOCKED with page.route — canned SSE
// bytes stand in for a real streamed response, so NO live API call is made. We
// verify: (a) streamed text renders in the glass chat; (b) a canned tool_use
// (set_param dist 0.55) visibly moves the knob, updates rig state, and produces
// a follow-up request carrying a tool_result block; (c) proxy-down and (d) a 500
// (no key) surface a clear in-chat notice.

// Build a Server-Sent-Events body from a list of event objects.
function sse(events: Array<Record<string, unknown>>): string {
  return (
    events
      .map((e) => `event: ${e.type}\ndata: ${JSON.stringify(e)}`)
      .join('\n\n') + '\n\n'
  );
}

const TEXT_ONLY = sse([
  { type: 'message_start', message: { id: 'msg_1', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Pushed dist to 60 for a thicker rhythm bed.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'message_delta', delta: { stop_reason: 'end_turn' }, usage: { output_tokens: 9 } },
  { type: 'message_stop' },
]);

// First turn: a short text block + a tool_use asking to set pedal dist to 0.55.
const TOOL_TURN = sse([
  { type: 'message_start', message: { id: 'msg_2', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Tightening it up.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_1', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"pedal","param":"dist","value":0.55}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

// Follow-up turn after the tool result: a plain end_turn confirmation.
const FOLLOWUP = sse([
  { type: 'message_start', message: { id: 'msg_3', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Done — dist is at 55 now.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'message_delta', delta: { stop_reason: 'end_turn' }, usage: { output_tokens: 7 } },
  { type: 'message_stop' },
]);

async function mockHealthOk(route: Route) {
  await route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({ ok: true, hasKey: true }),
  });
}

test('assistant: streamed text renders in the glass chat', async ({ page }) => {
  const errors: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(String(e)));

  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', (route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: TEXT_ONLY })
  );

  await page.goto('/');
  await expect(page.getByTestId('chat')).toBeVisible();

  await page.getByTestId('chat-input').fill('give me a warm rhythm tone');
  await page.getByTestId('chat-send').click();

  await expect(page.getByText('Pushed dist to 60 for a thicker rhythm bed.')).toBeVisible();
  expect(errors, `console errors: ${errors.join(' | ')}`).toEqual([]);
});

test('assistant: a tool_use moves the knob, updates rig state, and sends a tool_result follow-up', async ({
  page,
}) => {
  let call = 0;
  let secondBody: { messages?: Array<{ role: string; content: unknown }> } | null = null;

  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: TOOL_TURN });
    } else {
      secondBody = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Default pedal distortion is 0.7 -> readout 70.
  await expect(page.getByTestId('knob-distortion-value')).toHaveText('70');

  await page.getByTestId('chat-input').fill('tighter please');
  await page.getByTestId('chat-send').click();

  // The applied tool call renders as a chip in the flow ("Dist 70 → 55").
  await expect(page.getByTestId('tool-chips')).toContainText('Dist 70 → 55');
  // The knob readout moved to 55...
  await expect(page.getByTestId('knob-distortion-value')).toHaveText('55');
  // ...and the live rig state reflects it.
  const dist = await page.evaluate(
    () => (window as unknown as { __CLIPPER_TEST__: { getRig: () => any } }).__CLIPPER_TEST__.getRig().pedal.params.distortion
  );
  expect(dist).toBe(0.55);
  // The model's follow-up confirmation rendered.
  await expect(page.getByText('Done — dist is at 55 now.')).toBeVisible();

  // Two requests were made; the SECOND carried a tool_result for toolu_1.
  expect(call).toBe(2);
  const msgs = secondBody!.messages ?? [];
  const hasToolResult = msgs.some(
    (m) =>
      Array.isArray(m.content) &&
      (m.content as Array<Record<string, unknown>>).some(
        (c) => c.type === 'tool_result' && c.tool_use_id === 'toolu_1'
      )
  );
  expect(hasToolResult).toBe(true);
});

// M6.1: the coach can calibrate the INPUT TRIM (set_param unit:"input").
const TRIM_TURN = sse([
  { type: 'message_start', message: { id: 'msg_t', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Your input is weak — lifting the trim.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_t', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"input","param":"trim","value":0.7}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

test('assistant: set_param unit:"input" moves the trim knob and updates the rig', async ({
  page,
}) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: TRIM_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Default trim is unity (knob 1/3 -> readout 33).
  await expect(page.getByTestId('knob-input-trim-value')).toHaveText('33');

  await page.getByTestId('chat-input').fill('the pedal has no balls even cranked');
  await page.getByTestId('chat-send').click();

  // The applied tool call renders a chip and the trim knob moves to 70.
  await expect(page.getByTestId('tool-chips')).toContainText('Trim 33 → 70');
  await expect(page.getByTestId('knob-input-trim-value')).toHaveText('70');
  const trim = await page.evaluate(
    () => (window as unknown as { __CLIPPER_TEST__: { getRig: () => any } }).__CLIPPER_TEST__.getRig().input.trim
  );
  expect(trim).toBe(0.7);
});

test('assistant: proxy-down shows a clear in-chat error notice', async ({ page }) => {
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', (route) => route.abort());

  await page.goto('/');
  await page.getByTestId('chat-input').fill('anything');
  await page.getByTestId('chat-send').click();

  const notice = page.getByTestId('chat-notice');
  await expect(notice).toBeVisible();
  await expect(notice).toContainText('npm run server');
});

test('assistant: a 500 (no key) surfaces the server error message', async ({ page }) => {
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', (route) =>
    route.fulfill({
      status: 500,
      contentType: 'application/json',
      body: JSON.stringify({ error: 'ANTHROPIC_API_KEY is not set on the server. Export it and restart.' }),
    })
  );

  await page.goto('/');
  await page.getByTestId('chat-input').fill('hello');
  await page.getByTestId('chat-send').click();

  await expect(page.getByTestId('chat-notice')).toContainText('ANTHROPIC_API_KEY is not set');
});

test('assistant: a 429 surfaces the rate-limit copy (error classification)', async ({ page }) => {
  await page.route('**/api/health', mockHealthOk);
  // Upstream rate limit, passed through by the proxy as a 429. The raw upstream
  // message is intentionally NOT shown — the client maps 429 to friendly copy.
  await page.route('**/api/chat', (route) =>
    route.fulfill({
      status: 429,
      contentType: 'application/json',
      body: JSON.stringify({ error: 'rate_limit_error: too many requests' }),
    })
  );

  await page.goto('/');
  await page.getByTestId('chat-input').fill('give me a lead tone');
  await page.getByTestId('chat-send').click();

  const notice = page.getByTestId('chat-notice');
  await expect(notice).toBeVisible();
  await expect(notice).toContainText('rate-limiting');
  await expect(notice).toContainText('try again');
  // The raw upstream error text must NOT leak into the UI.
  await expect(notice).not.toContainText('rate_limit_error');
});
