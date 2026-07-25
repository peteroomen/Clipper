import { test, expect, type Route } from '@playwright/test';

// Undo ring + cancellable assistant turn (docs §39, ADR 011) — the 2026-07-24
// audit's UI/UX findings 2 and 3.
//
// Every assertion here is a PLAYER-OBSERVABLE property, not an identity:
//   - "swap then undo" asserts the DIALLED-IN NUMBERS are back on the original
//     pedal type and instance id — the thing that used to be unrecoverable
//   - "two tool calls, one undo" asserts the PRE-TURN rig, and that the ring
//     holds exactly ONE entry (a half-applied turn would show two)
//   - "cancel" asserts the rig is byte-identical to before AND that focus did
//     not fall to <body> (the `disabled` attribute's side effect)
//   - the coalescing and memory tests report MEASURED numbers and assert bounds
//
// Nothing here asserts that undo(record(x)) === x, which would be a tautology
// over the ring's own bookkeeping.

interface UndoSeam {
  capacity: number;
  depth: () => number;
  redoDepth: () => number;
  labels: () => string[];
  stats: () => {
    attempts: number;
    pushed: number;
    coalesced: number;
    serializations: number;
    dropped: number;
  };
  bytes: () => { entries: number; utf8: number; utf16: number; largest: number };
  undo: () => boolean;
  redo: () => boolean;
  injectRaw: (json: string, label?: string) => void;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type TestWin = { __CLIPPER_TEST__: { getRig: () => any; serializeRig: (r: any) => string; undo: UndoSeam } };

function sse(events: Array<Record<string, unknown>>): string {
  return events.map((e) => `event: ${e.type}\ndata: ${JSON.stringify(e)}`).join('\n\n') + '\n\n';
}

async function mockHealthOk(route: Route) {
  await route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({ ok: true, hasKey: true }),
  });
}

const FOLLOWUP = sse([
  { type: 'message_start', message: { id: 'm_f', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Done.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'message_delta', delta: { stop_reason: 'end_turn' } },
  { type: 'message_stop' },
]);

// ---------------------------------------------------------------------------
// 1. The audit's headline case: Swap threw away a dialled-in pedal.
// ---------------------------------------------------------------------------

test('undo: swapping a dialled-in pedal is reversible — the settings come back', async ({
  page,
}) => {
  await page.goto('/');

  const unit = page.getByTestId('board-unit-0');
  const dist = unit.getByRole('slider', { name: 'Distortion' });
  const filter = unit.getByRole('slider', { name: 'Filter' });

  // Dial the RAT in, the way a player does: focus a knob and use the keyboard
  // (0.05 per press). Dist 0.70 -> 0.40, Filter 0.40 -> 0.55.
  await dist.focus();
  for (let i = 0; i < 6; i++) await dist.press('ArrowDown');
  await filter.focus();
  for (let i = 0; i < 3; i++) await filter.press('ArrowUp');

  const dialled = await page.evaluate(() => {
    const p = (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0];
    return { id: p.id, type: p.type, params: p.params };
  });
  expect(dialled.type).toBe('rat');
  expect(dialled.params.distortion).toBeCloseTo(0.4, 6);
  expect(dialled.params.filter).toBeCloseTo(0.55, 6);
  await expect(dist).toHaveAttribute('aria-valuenow', '40');

  // Swap it for an SD-1 from the rack ⇄ menu — the destructive act.
  await page.getByTestId('pedal-swap-0').click();
  await page
    .getByTestId('pedal-swap-menu-0')
    .getByRole('menuitemradio', { name: /SD-type/ })
    .click();

  const swapped = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0]
  );
  expect(swapped.type).toBe('sd1');
  // Fresh defaults — the dialled-in RAT existed nowhere else at this point.
  expect(swapped.params.distortion).toBe(0.5);

  // The affordance says what it will undo, and Ctrl-Z does it.
  await expect(page.getByTestId('undo-label')).toHaveText('Swap RAT → SD-1');
  await page.keyboard.press('Control+z');

  const restored = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0]
  );
  expect(restored.type).toBe('rat');
  expect(restored.id).toBe(dialled.id); // the same INSTANCE, not a new RAT
  expect(restored.params.distortion).toBeCloseTo(0.4, 6);
  expect(restored.params.filter).toBeCloseTo(0.55, 6);
  // And the UI shows it (the knob readout, not just the state object).
  await expect(page.getByTestId('board-unit-0').getByRole('slider', { name: 'Distortion' })).toHaveAttribute(
    'aria-valuenow',
    '40'
  );

  // Redo puts the SD-1 back, so undo is not itself destructive.
  await page.getByTestId('redo').click();
  const redone = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0]
  );
  expect(redone.type).toBe('sd1');
});

test('undo: removing a pedal restores it with its settings, in its chain slot', async ({
  page,
}) => {
  await page.goto('/');

  // Add a Screamer, dial its Drive down, then remove it.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-ts').click();
  const ts = page.getByTestId('board-unit-1');
  const drive = ts.getByRole('slider', { name: 'Drive' });
  await drive.focus();
  for (let i = 0; i < 4; i++) await drive.press('ArrowDown');
  await expect(drive).toHaveAttribute('aria-valuenow', '30');

  await page.getByTestId('pedal-remove-1').click();
  let types = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals.map((p: { type: string }) => p.type)
  );
  expect(types).toEqual(['rat']);

  await expect(page.getByTestId('undo-label')).toHaveText('Remove Screamer');
  await page.getByTestId('undo').click();

  const pedals = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals
  );
  types = pedals.map((p: { type: string }) => p.type);
  expect(types).toEqual(['rat', 'ts']);
  expect(pedals[1].params.distortion).toBeCloseTo(0.3, 6); // the dialled Drive
  await expect(
    page.getByTestId('board-unit-1').getByRole('slider', { name: 'Drive' })
  ).toHaveAttribute('aria-valuenow', '30');
});

// ---------------------------------------------------------------------------
// 2. One assistant turn = one undo unit (audit UI/UX finding 3).
// ---------------------------------------------------------------------------

// A turn that fires TWO tool calls: pedal dist 0.70 -> 0.20 and amp treble
// 0.60 -> 0.90.
const TWO_TOOL_TURN = sse([
  { type: 'message_start', message: { id: 'm_2', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Reshaping the rig.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'tu_1', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"unit":"pedal","param":"dist","value":0.2}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'content_block_start', index: 2, content_block: { type: 'tool_use', id: 'tu_2', name: 'set_param', input: {} } },
  { type: 'content_block_delta', index: 2, delta: { type: 'input_json_delta', partial_json: '{"unit":"amp","param":"treble","value":0.9}' } },
  { type: 'content_block_stop', index: 2 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' } },
  { type: 'message_stop' },
]);

test('undo: an assistant turn with two tool calls is ONE undo step, not two', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    await route.fulfill({
      status: 200,
      contentType: 'text/event-stream',
      body: call === 1 ? TWO_TOOL_TURN : FOLLOWUP,
    });
  });

  await page.goto('/');
  const before = await page.evaluate(() => {
    const t = (window as unknown as TestWin).__CLIPPER_TEST__;
    return t.serializeRig(t.getRig());
  });

  await page.getByTestId('chat-input').fill('tighter and brighter');
  await page.getByTestId('chat-send').click();
  await expect(page.getByTestId('tool-chips')).toContainText('Dist 70 → 20');
  await expect(page.getByTestId('tool-chips')).toContainText('Treble 60 → 90');
  await expect(page.getByText('Done.')).toBeVisible();

  // Both changes landed...
  const applied = await page.evaluate(() => {
    const r = (window as unknown as TestWin).__CLIPPER_TEST__.getRig();
    return { dist: r.pedals[0].params.distortion, treble: r.amp.params.treble };
  });
  expect(applied).toEqual({ dist: 0.2, treble: 0.9 });

  // ...as exactly ONE undo entry, labelled for the turn.
  const state = await page.evaluate(() => {
    const u = (window as unknown as TestWin).__CLIPPER_TEST__.undo;
    return { depth: u.depth(), labels: u.labels() };
  });
  expect(state.depth).toBe(1);
  expect(state.labels).toEqual(['Assistant turn']);

  // One undo returns the rig to the PRE-TURN state — not halfway through it.
  await page.getByTestId('undo').click();
  const after = await page.evaluate(() => {
    const t = (window as unknown as TestWin).__CLIPPER_TEST__;
    return t.serializeRig(t.getRig());
  });
  expect(after).toBe(before);
  expect(await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.undo.depth())).toBe(0);
});

// Two chain edits in ONE turn. This also pins a bug found while building the
// ring: every whole-object mutator read a `rigRef` that React only refreshed on
// re-render, so two chain edits inside one batch clobbered each other and the
// first pedal never appeared. `commitRig()` writes the ref eagerly.
const TWO_ADD_TURN = sse([
  { type: 'message_start', message: { id: 'm_3', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'tool_use', id: 'tu_a', name: 'add_pedal', input: {} } },
  { type: 'content_block_delta', index: 0, delta: { type: 'input_json_delta', partial_json: '{"type":"ts"}' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'tu_b', name: 'add_pedal', input: {} } },
  { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"type":"phaser"}' } },
  { type: 'content_block_stop', index: 1 },
  { type: 'message_delta', delta: { stop_reason: 'tool_use' } },
  { type: 'message_stop' },
]);

test('undo: two chain edits in one turn both land, and undo as one step', async ({ page }) => {
  let call = 0;
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    call += 1;
    await route.fulfill({
      status: 200,
      contentType: 'text/event-stream',
      body: call === 1 ? TWO_ADD_TURN : FOLLOWUP,
    });
  });

  await page.goto('/');
  await page.getByTestId('chat-input').fill('add a screamer and a phaser');
  await page.getByTestId('chat-send').click();
  await expect(page.getByTestId('board-unit-2')).toBeVisible();

  const types = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals.map((p: { type: string }) => p.type)
  );
  expect(types).toEqual(['rat', 'ts', 'phaser']);

  await page.getByTestId('undo').click();
  const after = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals.map((p: { type: string }) => p.type)
  );
  expect(after).toEqual(['rat']);
});

const TEXT_ONLY = sse([
  { type: 'message_start', message: { id: 'm_t', type: 'message', role: 'assistant', content: [] } },
  { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
  { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Try the neck pickup.' } },
  { type: 'content_block_stop', index: 0 },
  { type: 'message_delta', delta: { stop_reason: 'end_turn' } },
  { type: 'message_stop' },
]);

test('undo: a turn that changes nothing leaves no undo entry', async ({ page }) => {
  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', (route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: TEXT_ONLY })
  );

  await page.goto('/');
  await page.getByTestId('chat-input').fill('what pickup should I use?');
  await page.getByTestId('chat-send').click();
  await expect(page.getByText('Try the neck pickup.')).toBeVisible();

  // An advice-only turn must not put a no-op step in the player's history.
  expect(await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.undo.depth())).toBe(0);
  await expect(page.getByTestId('undo')).toBeDisabled();
});

// ---------------------------------------------------------------------------
// 3. Cancel: the rig is untouched and focus is not lost.
// ---------------------------------------------------------------------------

test('assistant: a turn can be stopped — the rig is untouched and focus stays in the input', async ({
  page,
}) => {
  let release: () => void = () => {};
  const gate = new Promise<void>((r) => {
    release = r;
  });

  await page.route('**/api/health', mockHealthOk);
  await page.route('**/api/chat', async (route) => {
    // Hang the turn so it is genuinely in flight when we cancel it.
    await gate;
    await route
      .fulfill({ status: 200, contentType: 'text/event-stream', body: TWO_TOOL_TURN })
      .catch(() => {
        /* the request was aborted — that is the point of this test */
      });
  });

  await page.goto('/');
  const before = await page.evaluate(() => {
    const t = (window as unknown as TestWin).__CLIPPER_TEST__;
    return t.serializeRig(t.getRig());
  });

  // Send from the keyboard, the way a player does.
  const input = page.getByTestId('chat-input');
  await input.click();
  await input.fill('rebuild my whole rig');
  await input.press('Enter');

  // Mid-turn: the typing indicator is up, the input is NOT disabled, and focus
  // has not fallen to <body> (which is exactly what `disabled` used to do).
  await expect(page.getByTestId('typing')).toBeVisible();
  await expect(input).not.toBeDisabled();
  const focusedDuring = await page.evaluate(
    () => document.activeElement?.getAttribute('data-testid') ?? document.activeElement?.tagName
  );
  expect(focusedDuring).toBe('chat-input');
  // The Stop affordance exists and is reachable.
  await expect(page.getByTestId('chat-stop')).toBeVisible();

  // Escape cancels from the input.
  await input.press('Escape');
  await expect(page.getByTestId('chat-notice')).toContainText('stopped');
  await expect(page.getByTestId('typing')).toHaveCount(0);

  // The rig never moved, and no undo entry was created for a turn that changed
  // nothing.
  const after = await page.evaluate(() => {
    const t = (window as unknown as TestWin).__CLIPPER_TEST__;
    return t.serializeRig(t.getRig());
  });
  expect(after).toBe(before);
  expect(await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.undo.depth())).toBe(0);

  // Focus is still where the player was typing, and the chat still works.
  const focusedAfter = await page.evaluate(
    () => document.activeElement?.getAttribute('data-testid') ?? document.activeElement?.tagName
  );
  expect(focusedAfter).toBe('chat-input');
  await expect(page.getByTestId('chat-send')).toBeVisible();

  release();
});

// ---------------------------------------------------------------------------
// 4. The restore path re-validates. A snapshot is untrusted input.
// ---------------------------------------------------------------------------

test('undo: a hostile snapshot is clamped on restore, not trusted', async ({ page }) => {
  await page.goto('/');

  // Well-formed JSON, hostile values: NaN (as null, which is how a non-numeric
  // model emission arrives), out-of-range, a bogus pedal type, a bogus amp and
  // an illegal oversampling factor.
  const hostile = JSON.stringify({
    input: { trim: null },
    pedals: [
      { id: 'x-1', type: 'not-a-pedal', engaged: 'yes', params: { distortion: 1e9, filter: -5, level: null } },
    ],
    amp: {
      type: 'nope',
      engaged: 1,
      cabModel: 'nonesuch',
      params: { volume: 1e300, bass: -1, middle: null, treble: 'loud', chorusMode: 7, reverb: 2 },
    },
    oversampling: 999,
    source: 'wat',
  });

  await page.evaluate((json) => {
    (window as unknown as TestWin).__CLIPPER_TEST__.undo.injectRaw(json, 'hostile');
  }, hostile);

  await page.getByTestId('undo').click();

  const r = await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.getRig());
  // Every knob that reaches the engine is finite and in range.
  const values: number[] = [
    r.input.trim,
    ...r.pedals.flatMap((p: { params: Record<string, number> }) => Object.values(p.params)),
    ...Object.values(r.amp.params as Record<string, number>).filter((v) => typeof v === 'number'),
  ];
  for (const v of values) {
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThanOrEqual(0);
    expect(v).toBeLessThanOrEqual(2); // chorusMode is 0..2; every knob is 0..1
  }
  for (const p of r.pedals) {
    expect(['rat', 'sd1', 'ts', 'muff', 'gold', 'phaser', 'tuner']).toContain(p.type);
  }
  expect(['clean120', 'jcm800', 'twin', 'ac30']).toContain(r.amp.type);
  expect(['clean212', 'brit412', 'custom']).toContain(r.amp.cabModel);
  expect([1, 2, 4, 8]).toContain(r.oversampling);
  expect(['test', 'live']).toContain(r.source);
  expect([0, 1, 2]).toContain(r.amp.params.chorusMode);
});

// ---------------------------------------------------------------------------
// 5. MEASUREMENTS (docs §39): coalescing and memory.
// ---------------------------------------------------------------------------

test('undo: a 3-second knob drag pushes ONE entry (coalescing, measured)', async ({ page }) => {
  test.setTimeout(120_000);
  await page.goto('/');

  const knob = page.getByTestId('board-unit-0').getByTestId('knob-distortion');
  // hover() scrolls it into view and parks the pointer on it, so the boundingBox
  // below is in the same viewport coordinates page.mouse uses.
  await knob.hover();
  const box = (await knob.boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;

  const beforeValue = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0].params.distortion
  );
  const baseline = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.undo.stats()
  );

  // A real 3-second vertical drag: ~180 pointermove events, oscillating so every
  // move is a genuine value change rather than a clamped no-op.
  const t0 = Date.now();
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  const MOVES = 180;
  for (let i = 0; i < MOVES; i++) {
    const dy = Math.round(24 * Math.sin(i / 7));
    await page.mouse.move(cx, cy + dy);
    await page.waitForTimeout(8);
  }
  await page.mouse.up();
  const elapsedMs = Date.now() - t0;

  const stats = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.undo.stats()
  );
  const attempts = stats.attempts - baseline.attempts;
  const pushed = stats.pushed - baseline.pushed;
  const coalesced = stats.coalesced - baseline.coalesced;
  const serializations = stats.serializations - baseline.serializations;

  // eslint-disable-next-line no-console
  console.log(
    `[undo] drag ${elapsedMs} ms, ${MOVES} pointer moves -> ` +
      `${attempts} record attempts (naive ring = ${attempts} entries), ` +
      `${pushed} entries pushed, ${coalesced} coalesced, ${serializations} serializations`
  );

  // The drag was real, and long enough to matter.
  expect(elapsedMs).toBeGreaterThan(2500);
  expect(attempts).toBeGreaterThan(100);
  // ...and it is ONE undo step.
  expect(pushed).toBe(1);
  expect(coalesced).toBe(attempts - 1);
  // A coalesced move does NO snapshot work: exactly one serialization for the
  // whole drag (the pushed entry).
  expect(serializations).toBe(1);

  // The player-observable half: one undo returns the knob to its pre-drag value.
  const dragged = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0].params.distortion
  );
  expect(dragged).not.toBeCloseTo(beforeValue, 3);
  await page.getByTestId('undo').click();
  const undone = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals[0].params.distortion
  );
  expect(undone).toBeCloseTo(beforeValue, 6);
});

test('undo: the ring is bounded in entries and in bytes (measured)', async ({ page }) => {
  test.setTimeout(120_000);
  await page.goto('/');

  const capacity = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.undo.capacity
  );

  // A default rig, snapshotted once — the per-entry cost in the common case.
  await page.getByTestId('power').click(); // one discrete action
  const one = await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.undo.bytes());
  // eslint-disable-next-line no-console
  console.log(
    `[undo] default rig (1 pedal): ${one.utf8} B utf8 / ${one.utf16} B utf16 per entry`
  );

  // Worst case the UI can produce from the tray: a long chain. Add seven pedals
  // (one of each type) so the snapshot is as big as a rig gets, then overflow the
  // ring with discrete reorder actions.
  for (const t of ['sd1', 'ts', 'muff', 'gold', 'phaser', 'tuner', 'rat']) {
    await page.getByTestId('add-pedal').click();
    await page.getByTestId(`add-pedal-${t}`).click();
  }
  const overflow = capacity + 8;
  for (let i = 0; i < overflow; i++) {
    // Alternate a reorder back and forth: each is a discrete, coalescing-free
    // entry, so this fills and then overflows the ring.
    await page.getByTestId(i % 2 === 0 ? 'pedal-move-right-0' : 'pedal-move-left-1').click();
  }

  const stats = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.undo.stats()
  );
  const bytes = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.undo.bytes()
  );
  // eslint-disable-next-line no-console
  console.log(
    `[undo] capacity ${capacity}; after ${stats.pushed} pushes: ${bytes.entries} entries held, ` +
      `${stats.dropped} evicted; largest entry ${bytes.largest} B utf8; ` +
      `ring total ${bytes.utf8} B utf8 / ${bytes.utf16} B utf16 ` +
      `(${Math.round(bytes.utf16 / bytes.entries)} B utf16 per entry)`
  );

  // The bound is real: entries are capped and old ones were evicted.
  expect(bytes.entries).toBeLessThanOrEqual(capacity);
  expect(stats.dropped).toBeGreaterThanOrEqual(8);
  // And the whole ring stays small — a 128 KiB ceiling with a FULL ring of
  // eight-pedal rigs, the largest snapshot the UI can produce.
  expect(bytes.utf16).toBeLessThan(128 * 1024);

  // The oldest state is gone, but the recent ones still work: undo the last
  // reorder and the chain order really changes back.
  const orderBefore = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals.map((p: { id: string }) => p.id)
  );
  await page.getByTestId('undo').click();
  const orderAfter = await page.evaluate(() =>
    (window as unknown as TestWin).__CLIPPER_TEST__.getRig().pedals.map((p: { id: string }) => p.id)
  );
  expect(orderAfter).not.toEqual(orderBefore);
  expect([...orderAfter].sort()).toEqual([...orderBefore].sort()); // same pedals, new order
});

// ---------------------------------------------------------------------------
// 6. Keyboard reach + the text-field carve-out.
// ---------------------------------------------------------------------------

test('undo: Cmd/Ctrl-Z is inert inside the chat input (text undo wins there)', async ({ page }) => {
  await page.goto('/');

  // Make one undoable rig change.
  await page.getByTestId('power').click();
  const powered = await page.evaluate(
    () => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().amp.engaged
  );
  expect(powered).toBe(false);

  // Ctrl-Z with the caret in the chat input must not touch the rig.
  const input = page.getByTestId('chat-input');
  await input.click();
  await input.fill('some text I am typing');
  await page.keyboard.press('Control+z');
  expect(
    await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().amp.engaged)
  ).toBe(false);
  expect(await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.undo.depth())).toBe(1);

  // Outside a text field it works.
  await page.getByTestId('board').click({ position: { x: 2, y: 2 } });
  await page.keyboard.press('Control+z');
  expect(
    await page.evaluate(() => (window as unknown as TestWin).__CLIPPER_TEST__.getRig().amp.engaged)
  ).toBe(true);
});
