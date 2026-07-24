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
    () => (window as unknown as { __CLIPPER_TEST__: { getRig: () => any } }).__CLIPPER_TEST__.getRig().pedals[0].params.distortion
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

// M6.3: the coach can turn on the JC chorus (set_switch name:'chorus') and shape
// it (set_param param:'speed'). A first turn emits both tool calls; a follow-up
// confirms. Verifies the rig state and the tool chips.
const CHORUS_TURN = sse([
  { type: 'message_start', message: { id: 'msg_c', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Adding the JC bloom.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_c1', name: 'set_switch', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"name":"chorus","on":true}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'content_block_start', index: 2, content_block: { type: 'tool_use', id: 'toolu_c2', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 2, delta: { type: 'input_json_delta', partial_json: '{"unit":"amp","param":"speed","value":0.35}' } },
  { type: 'content_block_stop', index: 2 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 14 } },
  { type: 'message_stop' },
]);

test('assistant: set_switch chorus + set_param speed engage the JC chorus', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: CHORUS_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await expect(page.getByTestId('chat')).toBeVisible();

  await page.getByTestId('chat-input').fill('give me that lush JC chorus, slowish');
  await page.getByTestId('chat-send').click();

  // Chips for both applied tool calls render.
  await expect(page.getByTestId('tool-chips')).toContainText('Chorus on');
  await expect(page.getByTestId('tool-chips')).toContainText('Speed');
  // The mode 3-way switched to Chorus (mode 1) and speed reached the rig.
  await expect(page.getByTestId('chorus-mode-chorus')).toHaveAttribute('aria-checked', 'true');
  const state = await page.evaluate(() => {
    const p = (window as any).__CLIPPER_TEST__.getRig().amp.params;
    return { mode: p.chorusMode, speed: p.speed };
  });
  expect(state.mode).toBe(1);
  expect(state.speed).toBe(0.35);
});

// M6.7: the coach can dial in the spring reverb (set_param unit:'amp'
// param:'reverb'). A canned tool_use sets the mix to 0.25 for an ambient-but-clear
// wash; verify the rig state and the chip.
const REVERB_TURN = sse([
  { type: 'message_start', message: { id: 'msg_rv', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'A touch of the JC spring.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_rv', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"amp","param":"reverb","value":0.25}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 11 } },
  { type: 'message_stop' },
]);

test('assistant: set_param reverb dials the spring reverb mix', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: REVERB_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await expect(page.getByTestId('chat')).toBeVisible();

  await page.getByTestId('chat-input').fill('add a little ambience for this ballad');
  await page.getByTestId('chat-send').click();

  // The chip renders and the reverb knob reached the rig.
  await expect(page.getByTestId('tool-chips')).toContainText('Reverb');
  await expect(page.getByRole('slider', { name: 'Reverb' })).toHaveAttribute('aria-valuenow', '25');
  const reverb = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.params.reverb
  );
  expect(reverb).toBe(0.25);
});

// M6.4: the coach can address a specific pedal INSTANCE by index. With two RATs
// in the chain, a set_param carrying pedal:1 moves the SECOND pedal's knob only.
const PEDAL1_TURN = sse([
  { type: 'message_start', message: { id: 'msg_p', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Backing off the second RAT.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_p', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"pedal","pedal":1,"param":"dist","value":0.3}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

test('assistant: set_param addresses a pedal instance by index', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: PEDAL1_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Add a second RAT so index 1 exists.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-rat').click();
  await expect(page.getByTestId('board-unit-1')).toBeVisible();

  await page.getByTestId('chat-input').fill('the second dirt box is too much');
  await page.getByTestId('chat-send').click();

  // The chip is tagged with the pedal position, and pedal #2's dist knob moved.
  await expect(page.getByTestId('tool-chips')).toContainText('Dist #2');
  await expect(
    page.getByTestId('board-unit-1').getByRole('slider', { name: 'Distortion' })
  ).toHaveAttribute('aria-valuenow', '30');
  // Pedal #1 (index 0) is unchanged at its default 70.
  await expect(
    page.getByTestId('board-unit-0').getByRole('slider', { name: 'Distortion' })
  ).toHaveAttribute('aria-valuenow', '70');
  const dists = await page.evaluate(() =>
    (window as any).__CLIPPER_TEST__.getRig().pedals.map((p: any) => p.params.distortion)
  );
  expect(dists).toEqual([0.7, 0.3]);
});

// M8: the coach can set an SD-1's DRIVE. The SD-1 is added via the gear tray
// (index 1); a canned set_param carrying param:'drive' on pedal 1 moves its Drive
// knob only (the 'drive' alias resolves to the shared distortion slot).
const SD1_DRIVE_TURN = sse([
  { type: 'message_start', message: { id: 'msg_s', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Backing the SD-1 down to a clean boost.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_sd', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"pedal","pedal":1,"param":"drive","value":0.35}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

test('assistant: set_param drive dials an SD-1 instance', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: SD1_DRIVE_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Add an SD-1 via the gear tray so index 1 exists (default SD-1 drive = 50).
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-sd1').click();
  await expect(page.getByTestId('board-unit-1')).toBeVisible();
  await expect(
    page.getByTestId('board-unit-1').getByRole('slider', { name: 'Drive' })
  ).toHaveAttribute('aria-valuenow', '50');

  await page.getByTestId('chat-input').fill('make the SD-1 a clean boost');
  await page.getByTestId('chat-send').click();

  // The chip renders and the SD-1's Drive knob moved to 35.
  await expect(page.getByTestId('tool-chips')).toContainText('Drive');
  await expect(
    page.getByTestId('board-unit-1').getByRole('slider', { name: 'Drive' })
  ).toHaveAttribute('aria-valuenow', '35');
  // Rig state: the second pedal is an sd1 whose drive (distortion slot) is 0.35.
  const pedals = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals);
  expect(pedals.length).toBe(2);
  expect(pedals[1].type).toBe('sd1');
  expect(pedals[1].params.distortion).toBe(0.35);
});

// v1.1: the coach can ADD a TS808 "Screamer" (add_pedal type:'ts'). A canned
// tool_use appends the green box; verify the chip, that it renders on the board
// with its green 'slim' face, and that the rig round-trips a 'ts' pedal with its
// default knobs (drive 0.5 / tone 0.5 / level 0.75).
const ADD_TS_TURN = sse([
  { type: 'message_start', message: { id: 'msg_ts', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Dropping a Screamer in front for a clean mid boost.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_ts', name: 'add_pedal', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"ts"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

test('assistant: add_pedal adds a TS Screamer (round-trip)', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: ADD_TS_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Board starts with one RAT (index 0).
  const before = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals.length);
  expect(before).toBe(1);

  await page.getByTestId('chat-input').fill('add a green screamer boost');
  await page.getByTestId('chat-send').click();

  // The chip renders and a second board unit appears with the green slim face.
  await expect(page.getByTestId('tool-chips')).toContainText('Screamer');
  await expect(page.getByTestId('board-unit-1')).toBeVisible();
  const face = await page
    .getByTestId('board-unit-1')
    .getByTestId('pedal')
    .getAttribute('data-face');
  expect(face).toBe('slim');

  // Rig state round-trips: a second pedal of type 'ts' with the TS defaults.
  const pedals = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals);
  expect(pedals.length).toBe(2);
  expect(pedals[1].type).toBe('ts');
  expect(pedals[1].params).toEqual({ distortion: 0.5, filter: 0.5, level: 0.75 });
});


// v1.1 item 4: the coach can ADD a Muff "Pi" fuzz (add_pedal type:'muff'). A canned
// tool_use appends the violet big box; verify the chip, that it renders on the board
// with its 'wide' (triangle-knob) face, and that the rig round-trips a 'muff' pedal
// with its default knobs (sustain 0.6 / tone 0.5 / volume 0.6).
const ADD_MUFF_TURN = sse([
  { type: 'message_start', message: { id: 'msg_mf', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Dropping a Pi fuzz in for a wall of sustain.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_mf', name: 'add_pedal', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"muff"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 12 } },
  { type: 'message_stop' },
]);

test('assistant: add_pedal adds a Muff Pi fuzz (round-trip)', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: ADD_MUFF_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await page.getByTestId('chat-input').fill('give me a huge fuzz wall');
  await page.getByTestId('chat-send').click();

  // The chip renders and a second board unit appears with the violet 'wide' face.
  await expect(page.getByTestId('tool-chips')).toContainText('Pi Fuzz');
  await expect(page.getByTestId('board-unit-1')).toBeVisible();
  const muff = page.getByTestId('board-unit-1').getByTestId('pedal');
  await expect(muff).toHaveAttribute('data-pedal-type', 'muff');
  await expect(muff).toHaveAttribute('data-face', 'wide');
  // The three triangle knobs are present (Sustain / Volume / Tone).
  await expect(muff.getByTestId('knob-sustain')).toBeVisible();
  await expect(muff.getByTestId('knob-volume')).toBeVisible();
  await expect(muff.getByTestId('knob-tone')).toBeVisible();

  // Rig state round-trips: a second pedal of type 'muff' with the Muff defaults.
  const pedals = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals);
  expect(pedals.length).toBe(2);
  expect(pedals[1].type).toBe('muff');
  expect(pedals[1].params).toEqual({ distortion: 0.6, filter: 0.5, level: 0.6 });
});

// v1.1: the coach can ADD a phaser (add_pedal type:'phaser'). A canned tool_use
// appends one; verify the chip, the rig state, and that its one-knob face renders.
const ADD_PHASER_TURN = sse([
  { type: 'message_start', message: { id: 'msg_ph', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Adding a phaser for that swirl.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_ph', name: 'add_pedal', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"phaser"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 10 } },
  { type: 'message_stop' },
]);

test('assistant: add_pedal drops a phaser into the chain', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: ADD_PHASER_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await page.getByTestId('chat-input').fill('give me some swirl');
  await page.getByTestId('chat-send').click();

  // The chip confirms the add, and a phaser face renders as the second unit.
  await expect(page.getByTestId('tool-chips')).toContainText('Phaser');
  const phaser = page.getByTestId('board-unit-1').getByTestId('pedal');
  await expect(phaser).toHaveAttribute('data-pedal-type', 'phaser');
  await expect(phaser.getByTestId('knob-speed')).toBeVisible();
  const types = await page.evaluate(() =>
    (window as any).__CLIPPER_TEST__.getRig().pedals.map((p: any) => p.type)
  );
  expect(types).toEqual(['rat', 'phaser']);
});

// M9.4: the coach can swap the AMP head (set_amp type:'jcm800'). A canned tool_use
// switches to the JCM800; verify the chip, the rig state, and that the amp FACE
// changed to the Eight Hundred wordmark.
const SET_AMP_TURN = sse([
  { type: 'message_start', message: { id: 'msg_a', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Swapping in the JCM800 for real amp crunch.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_a', name: 'set_amp', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"jcm800"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 11 } },
  { type: 'message_stop' },
]);

test('assistant: set_amp switches the amp head to the JCM800', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: SET_AMP_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  // Starts on the Clean 120 face.
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  await page.getByTestId('chat-input').fill('give me a cranked Marshall crunch');
  await page.getByTestId('chat-send').click();

  // The chip renders and the rig's amp voice is now the JCM800.
  await expect(page.getByTestId('tool-chips')).toContainText('Amp JCM800');
  const type = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.type
  );
  expect(type).toBe('jcm800');
  // The amp FACE swapped to the Eight Hundred wordmark (JCM face).
  await expect(page.getByTestId('amp-name')).toContainText('Eight Hundred');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'jcm800');
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

const SET_AMP_TWIN_TURN = sse([
  { type: 'message_start', message: { id: 'msg_tw', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Loading the Twin for glassy cleans and headroom.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_tw', name: 'set_amp', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"twin"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 11 } },
  { type: 'message_stop' },
]);

test('assistant: set_amp switches the amp head to the Twin', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: SET_AMP_TWIN_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  await page.getByTestId('chat-input').fill('give me pristine Fender cleans');
  await page.getByTestId('chat-send').click();

  await expect(page.getByTestId('tool-chips')).toContainText('Amp Twin Sixty-Five');
  const type = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.type
  );
  expect(type).toBe('twin');
  // The amp FACE swapped to the Twin Sixty-Five wordmark.
  await expect(page.getByTestId('amp-name')).toContainText('Twin Sixty-Five');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'twin');
});

// v1.1: the coach can swap to the AC30 "top boost" chime combo (set_amp type:'ac30').
const SET_AMP_AC30_TURN = sse([
  { type: 'message_start', message: { id: 'msg_ac', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Loading the Thirty for chime and jangle.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'toolu_ac', name: 'set_amp', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"ac30"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 11 } },
  { type: 'message_stop' },
]);

test('assistant: set_amp switches the amp head to the AC30 (Thirty)', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    if (call === 1) {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: SET_AMP_AC30_TURN });
    } else {
      await route.fulfill({ status: 200, contentType: 'text/event-stream', body: FOLLOWUP });
    }
  });

  await page.goto('/');
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  await page.getByTestId('chat-input').fill('give me jangly chime tones');
  await page.getByTestId('chat-send').click();

  await expect(page.getByTestId('tool-chips')).toContainText('Amp Thirty');
  const type = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.type
  );
  expect(type).toBe('ac30');
  // The amp FACE swapped to the Thirty wordmark.
  await expect(page.getByTestId('amp-name')).toContainText('Thirty');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'ac30');
});
