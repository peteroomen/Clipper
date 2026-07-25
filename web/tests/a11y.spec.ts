import { test, expect, type Page, type Route } from '@playwright/test';

// Input & accessibility suite (2026-07-24 audit, UI/UX · docs §38).
//
// Every test here asserts a PLAYER-OBSERVABLE property, per CLAUDE.md's
// "measure, don't assert": can a keyboard reach this control, can a screen reader
// name it, can an eye read it, does the app stop doing work it does not need. None
// of them assert an implementation detail, and none of them are satisfied by the
// pre-fix code — each was confirmed red against it.
//
// The contrast tests are the interesting ones: they don't compare a colour against
// a hardcoded expectation (which would just restate the stylesheet). They read the
// colours the BROWSER resolved, composite the chassis exactly as it is painted, and
// compute the WCAG 2.x contrast ratio. So a future token change that quietly makes
// a readout unreadable fails here with the real number, whatever the change was.

// ---- WCAG plumbing ------------------------------------------------------------

type Rgba = [number, number, number, number];

/**
 * Parse a CSS colour.
 *
 * Two forms show up here and they are NOT the same: a *used* value read off a
 * painted property (`getComputedStyle(el).color`) is always serialised
 * `rgb()`/`rgba()`, but a *custom property* is returned as its literal token text
 * — so `--accent-rat: var(--led)` comes back as `#F03B24`. Both must parse, or the
 * before/after comparison silently only covers half the palette.
 */
function parseColor(input: string): Rgba {
  const s = input.trim();
  const fn = s.match(/rgba?\(([^)]+)\)/);
  if (fn) {
    const parts = fn[1].split(/[\s,/]+/).filter(Boolean).map(parseFloat);
    return [parts[0], parts[1], parts[2], parts.length > 3 && Number.isFinite(parts[3]) ? parts[3] : 1];
  }
  const hex = s.match(/^#([0-9a-f]{3,8})$/i);
  if (hex) {
    const h = hex[1];
    const wide = h.length <= 4 ? h.split('').map((c) => c + c).join('') : h;
    const n = (i: number) => parseInt(wide.slice(i * 2, i * 2 + 2), 16);
    return [n(0), n(1), n(2), wide.length >= 8 ? n(3) / 255 : 1];
  }
  throw new Error(`unparseable colour: ${input}`);
}

/** Source-over composite of a translucent colour on an opaque one. */
function composite(fg: Rgba, bg: Rgba): Rgba {
  const a = fg[3];
  return [
    a * fg[0] + (1 - a) * bg[0],
    a * fg[1] + (1 - a) * bg[1],
    a * fg[2] + (1 - a) * bg[2],
    1,
  ];
}

function relativeLuminance(c: Rgba): number {
  const f = (v: number) => {
    const s = v / 255;
    return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}

function contrastRatio(a: Rgba, b: Rgba): number {
  const la = relativeLuminance(a);
  const lb = relativeLuminance(b);
  const hi = Math.max(la, lb);
  const lo = Math.min(la, lb);
  return (hi + 0.05) / (lo + 0.05);
}

/**
 * Everything needed to judge one pedal's readout, read out of the live page.
 *
 * The chassis is two stacked paint layers — a flat `--chassis-tint` wash over the
 * `--panel-grad` gradient — so its colour is a RANGE, and the honest number is the
 * worst of the two gradient stops. The four-colour assertion below is deliberate:
 * if someone restructures `.pedal.raised`'s background, this test must fail loudly
 * rather than silently measure the wrong surface.
 */
async function readPedalPaint(page: Page, pedalType: string) {
  return await page.evaluate((type) => {
    const pedal = document.querySelector<HTMLElement>(`.pedal[data-pedal-type="${type}"]`);
    if (!pedal) throw new Error(`no pedal on the board of type ${type}`);
    // The tuner is the one pedal with no knobs, so it has no `.k-val`. Its accent
    // paints the lock LED and the meter's green centre instead — non-text UI
    // components, judged at 3:1 rather than 4.5:1.
    const kval = pedal.querySelector<HTMLElement>('.k-val');
    const cs = getComputedStyle(pedal);
    const root = getComputedStyle(document.documentElement);
    const kcs = kval ? getComputedStyle(kval) : null;
    return {
      readout: kcs ? kcs.color : null,
      // The accent as the browser resolved it ON THIS PEDAL — what the LED, the
      // value arc and the readout all read.
      accent: cs.getPropertyValue('--pedal-accent').trim(),
      fontSizePx: kcs ? parseFloat(kcs.fontSize) : null,
      fontWeight: kcs ? kcs.fontWeight : null,
      // Layer list, topmost first: the tint wash then the panel gradient.
      backgroundImage: cs.backgroundImage,
      // The PRE-FIX quantities, still live tokens on :root — the accent the page
      // theme would have handed the chassis, and the tint it would have washed it
      // with. Lets one run report before AND after with no second build.
      pageThemeAccent: root.getPropertyValue(`--accent-${type === 'sd1' ? 'sd' : type}`).trim(),
      pageThemeTint: root.getPropertyValue('--chassis-tint').trim(),
    };
  }, pedalType);
}

/** The two chassis stop colours (tint composited over each panel-gradient stop). */
function chassisStops(backgroundImage: string, tintOverride?: string): Rgba[] {
  const colours = backgroundImage.match(/rgba?\([^)]+\)/g) ?? [];
  if (colours.length !== 4) {
    throw new Error(
      `.pedal.raised background is expected to be exactly two 2-stop gradients ` +
        `(tint wash over panel gradient); found ${colours.length} colour stops in: ${backgroundImage}`
    );
  }
  const tint = parseColor(tintOverride ?? colours[0]);
  return [parseColor(colours[2]), parseColor(colours[3])].map((panel) => composite(tint, panel));
}

/** Worst-case contrast of a text colour against the chassis it sits on. */
function worstContrast(textColor: string, stops: Rgba[]): number {
  const fg = parseColor(textColor);
  return Math.min(...stops.map((bg) => contrastRatio(fg, bg)));
}

// ---- page helpers -------------------------------------------------------------

const PEDAL_TYPES = ['rat', 'sd1', 'ts', 'muff', 'gold', 'phaser', 'tuner'] as const;

async function setTheme(page: Page, choice: 'system' | 'light' | 'dark') {
  await page.getByRole('group', { name: 'Theme' }).getByRole('button', { name: choice, exact: true }).click();
  await expect
    .poll(() => page.evaluate(() => document.documentElement.getAttribute('data-theme')))
    .toBe(choice === 'system' ? null : choice);
}

async function addPedal(page: Page, type: string) {
  await page.getByTestId('add-pedal').click();
  await page.getByTestId(`add-pedal-${type}`).click();
  await expect(page.locator(`.pedal[data-pedal-type="${type}"]`)).toBeVisible();
}

/**
 * Tab from the document body until `predicate` matches the focused element, and
 * return how many presses it took (or -1). This is how "reachable by keyboard" is
 * asserted throughout: no `.focus()` shortcut, because a control can be
 * programmatically focusable and still be off the tab order — which is exactly the
 * bug the audit found on "Upload IR…".
 */
async function tabUntil(page: Page, predicate: string, max = 60): Promise<number> {
  for (let i = 1; i <= max; i++) {
    await page.keyboard.press('Tab');
    const hit = await page.evaluate((p) => {
      const el = document.activeElement;
      if (!el) return false;
      // eslint-disable-next-line no-new-func
      return Boolean(new Function('el', `return ${p}`)(el));
    }, predicate);
    if (hit) return i;
  }
  return -1;
}

const activeTestId = (page: Page) =>
  page.evaluate(() => document.activeElement?.getAttribute('data-testid') ?? null);

// ---- 1. CONTRAST: the pedal value readouts, both themes -----------------------

for (const theme of ['light', 'dark'] as const) {
  test(`contrast: every pedal value readout clears WCAG AA on the dark chassis (${theme} theme)`, async ({
    page,
  }) => {
    await page.goto('/');
    await expect(page.getByTestId('board')).toBeVisible();
    await setTheme(page, theme);
    for (const t of PEDAL_TYPES) {
      if (t !== 'rat') await addPedal(page, t);
    }

    const rows: string[] = [];
    const failures: string[] = [];
    for (const t of PEDAL_TYPES) {
      const paint = await readPedalPaint(page, t);

      // Text (`.k-val`) is 11 px at weight 600 — NOT WCAG "large text" (which
      // starts at 18.66 px bold / 24 px regular), so its bar is the full 4.5:1.
      // The tuner has no readout; its accent only paints UI components, at 3:1.
      const isText = paint.readout !== null;
      if (isText) expect(paint.fontSizePx!).toBeLessThan(18.66);
      const bar = isText ? 4.5 : 3.0;
      const measured = isText ? paint.readout! : paint.accent;

      const after = worstContrast(measured, chassisStops(paint.backgroundImage));
      // Reconstruct the pre-fix pairing from tokens that are still live: the page
      // theme's accent, on the page theme's tint over the same pinned panel.
      const before = worstContrast(
        paint.pageThemeAccent,
        chassisStops(paint.backgroundImage, paint.pageThemeTint)
      );

      rows.push(
        `  ${t.padEnd(7)} ${(isText ? 'readout' : 'LED/arc').padEnd(8)} bar ${bar.toFixed(1)}:1  ` +
          `before ${before.toFixed(2).padStart(5)}:1   after ${after.toFixed(2).padStart(5)}:1   (${measured})`
      );
      if (after < bar) failures.push(`${t}: ${after.toFixed(2)}:1 < ${bar}:1`);
    }
    console.log(`\n[contrast · ${theme} theme] pedal accent vs the chassis it paints on\n${rows.join('\n')}\n`);
    expect(failures, `readouts under WCAG AA:\n${failures.join('\n')}`).toEqual([]);
  });
}

test('contrast: the knob focus ring clears the 3:1 non-text bar on the dark chassis', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();
  await setTheme(page, 'light'); // the theme where the ring used to be a light-bench blue

  const steps = await tabUntil(page, `el.getAttribute('data-testid') === 'knob-distortion'`);
  expect(steps, 'the knob must be reachable by Tab').toBeGreaterThan(0);

  const ring = await page.evaluate(() => {
    const el = document.querySelector<HTMLElement>('[data-testid="knob-distortion"]')!;
    const pedal = el.closest<HTMLElement>('.pedal')!;
    const cs = getComputedStyle(el);
    return {
      style: cs.outlineStyle,
      width: parseFloat(cs.outlineWidth),
      color: cs.outlineColor,
      radius: parseFloat(cs.borderTopLeftRadius),
      backgroundImage: getComputedStyle(pedal).backgroundImage,
    };
  });

  // A ring that exists, is thick enough to see, and is not a square that slices
  // through the neighbouring knobs' arcs.
  expect(ring.style).not.toBe('none');
  expect(ring.width).toBeGreaterThanOrEqual(2);
  expect(ring.radius).toBeGreaterThan(0);

  const ratio = worstContrast(ring.color, chassisStops(ring.backgroundImage));
  console.log(`\n[contrast · focus ring] ${ring.color} on chassis = ${ratio.toFixed(2)}:1 (bar 3:1)\n`);
  expect(ratio).toBeGreaterThanOrEqual(3);
});

// ---- 2. KNOB: keyboard operation, live ARIA, fine adjust ----------------------

test('knob: the full ARIA slider keyboard contract moves the value AND the engine', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  const knob = page.getByTestId('knob-distortion');
  await expect(knob).toHaveAttribute('role', 'slider');
  const steps = await tabUntil(page, `el.getAttribute('data-testid') === 'knob-distortion'`);
  expect(steps, 'the knob must be reachable by Tab, not just by script').toBeGreaterThan(0);

  // The engine seam: proves each keystroke is not just repainting an arc.
  const lastEngineValue = () =>
    page.evaluate(() => (window as unknown as { __CLIPPER_TEST__: { lastParam?: { value: number } } }).__CLIPPER_TEST__.lastParam?.value ?? null);

  // Home / End are absolute — a keyboard player could not previously reach an end
  // exactly, because 0.05 steps from a persisted value never land on 0 or 1.
  await page.keyboard.press('Home');
  await expect(knob).toHaveAttribute('aria-valuenow', '0');
  await expect(knob).toHaveAttribute('aria-valuetext', '0%');
  expect(await lastEngineValue()).toBeCloseTo(0, 5);

  await page.keyboard.press('End');
  await expect(knob).toHaveAttribute('aria-valuenow', '100');
  await expect(knob).toHaveAttribute('aria-valuetext', '100%');
  expect(await lastEngineValue()).toBeCloseTo(1, 5);

  // PageDown must be a materially bigger step than an arrow, or it is decoration.
  await page.keyboard.press('PageDown');
  await expect(knob).toHaveAttribute('aria-valuenow', '80');
  await page.keyboard.press('ArrowDown');
  await expect(knob).toHaveAttribute('aria-valuenow', '75');
  await page.keyboard.press('ArrowUp');
  await expect(knob).toHaveAttribute('aria-valuenow', '80');
  await page.keyboard.press('PageUp');
  await expect(knob).toHaveAttribute('aria-valuenow', '100');

  // Shift is the fine modifier on the keyboard too: a fifth of an arrow step.
  await page.keyboard.press('Home');
  await page.keyboard.press('Shift+ArrowUp');
  await expect(knob).toHaveAttribute('aria-valuenow', '1');
  expect(await lastEngineValue()).toBeCloseTo(0.01, 5);
});

test('knob: holding Shift makes the SAME drag five times finer', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  const knob = page.getByTestId('knob-distortion');
  // The board sits below the fold at the default viewport, so the knob's bounding
  // box would be outside it and every synthetic mouse coordinate would miss.
  await knob.scrollIntoViewIfNeeded();
  const box = (await knob.boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;
  const readValue = () => knob.evaluate((el) => parseFloat(el.getAttribute('data-value') ?? 'NaN'));

  // Park at a mid value so neither drag can hit an end stop.
  await tabUntil(page, `el.getAttribute('data-testid') === 'knob-distortion'`);
  await page.keyboard.press('Home');
  await page.keyboard.press('PageUp');
  await page.keyboard.press('PageUp'); // 0.40

  const DRAG_PX = 40;

  const drag = async (fine: boolean) => {
    const start = await readValue();
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    if (fine) await page.keyboard.down('Shift');
    await page.mouse.move(cx, cy - DRAG_PX);
    if (fine) await page.keyboard.up('Shift');
    await page.mouse.up();
    const end = await readValue();
    // Put it back for the next measurement.
    await page.keyboard.press('Home');
    await page.keyboard.press('PageUp');
    await page.keyboard.press('PageUp');
    return end - start;
  };

  const coarse = await drag(false);
  const fine = await drag(true);
  const pxPerUnitCoarse = DRAG_PX / coarse / 100;
  const pxPerUnitFine = DRAG_PX / fine / 100;
  console.log(
    `\n[knob fine adjust] ${DRAG_PX} px drag: coarse Δ${(coarse * 100).toFixed(2)} %` +
      ` (${pxPerUnitCoarse.toFixed(2)} px per 1 %), fine Δ${(fine * 100).toFixed(2)} %` +
      ` (${pxPerUnitFine.toFixed(2)} px per 1 %) — ratio ${(coarse / fine).toFixed(2)}×\n`
  );

  // Both must move the knob in the same direction, and the fine drag must be
  // dramatically finer. Before this slice a modifier did nothing at all, so the
  // ratio was exactly 1.0.
  expect(coarse).toBeGreaterThan(0);
  expect(fine).toBeGreaterThan(0);
  expect(coarse / fine).toBeGreaterThan(3);
  expect(coarse).toBeCloseTo(DRAG_PX / 160, 2);
  expect(fine).toBeCloseTo(DRAG_PX / 800, 2);
});

test('knob: releasing the fine modifier mid-drag does not make the value jump', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  const knob = page.getByTestId('knob-distortion');
  await knob.scrollIntoViewIfNeeded();
  const box = (await knob.boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;
  const readValue = () => knob.evaluate((el) => parseFloat(el.getAttribute('data-value') ?? 'NaN'));

  await tabUntil(page, `el.getAttribute('data-testid') === 'knob-distortion'`);
  await page.keyboard.press('Home');
  await page.keyboard.press('PageUp');
  await page.keyboard.press('PageUp'); // 0.40
  const parked = await readValue();

  await page.mouse.move(cx, cy);
  await page.mouse.down();
  await page.keyboard.down('Shift');
  await page.mouse.move(cx, cy - 20); // fine: +20/800 = 0.025
  const atModifierChange = await readValue();
  // Teeth: if the drag did nothing at all, the "no jump" assertion below would
  // pass trivially (0 === 0). It must have actually moved, by the FINE amount.
  expect(atModifierChange - parked).toBeCloseTo(20 / 800, 3);
  await page.keyboard.up('Shift');
  // The pointer has NOT moved, so nothing may change. An anchored drag would
  // recompute from the press point here and snap by (20/160 - 20/800) = 0.10.
  await page.mouse.move(cx, cy - 20);
  const afterModifierChange = await readValue();
  await page.mouse.up();

  console.log(
    `\n[knob modifier change] value ${atModifierChange.toFixed(4)} → ${afterModifierChange.toFixed(4)}` +
      ` on releasing Shift with a stationary pointer\n`
  );
  expect(Math.abs(afterModifierChange - atModifierChange)).toBeLessThan(0.005);
});

// ---- 3. POPOVERS: Escape, outside-pointer, focus trap -------------------------

test('popover: Escape closes the amp menu and returns focus to its trigger', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();

  const trigger = page.getByTestId('amp-select');
  await expect(trigger).toHaveAttribute('aria-expanded', 'false');
  await trigger.click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await expect(trigger).toHaveAttribute('aria-expanded', 'true');

  await page.keyboard.press('Escape');
  await expect(page.getByTestId('amp-menu')).toBeHidden();
  await expect(trigger).toHaveAttribute('aria-expanded', 'false');
  // Focus must come BACK. Dropping it to <body> leaves a keyboard player at the
  // top of the document with no idea where they were.
  expect(await activeTestId(page)).toBe('amp-select');
});

test('popover: Escape closes the gear tray and the pedal swap menu too', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  await page.getByTestId('add-pedal').click();
  await expect(page.getByTestId('add-pedal-menu')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByTestId('add-pedal-menu')).toBeHidden();
  expect(await activeTestId(page)).toBe('add-pedal');

  await page.getByTestId('pedal-swap-0').click();
  await expect(page.getByTestId('pedal-swap-menu-0')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByTestId('pedal-swap-menu-0')).toBeHidden();
  expect(await activeTestId(page)).toBe('pedal-swap-0');
});

test('popover: a pointer press outside dismisses the menu, and the trigger still toggles', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  await page.getByTestId('add-pedal').click();
  await expect(page.getByTestId('add-pedal-menu')).toBeVisible();

  // Press somewhere inert and far away.
  await page.getByRole('heading', { name: 'Clipper', exact: true }).click();
  await expect(page.getByTestId('add-pedal-menu')).toBeHidden();

  // And the trigger must still be a toggle: the outside-handler excludes it, so
  // press → close-then-reopen (which reads as "the menu won't close") must NOT
  // happen, and press → open must still work.
  await page.getByTestId('add-pedal').click();
  await expect(page.getByTestId('add-pedal-menu')).toBeVisible();
  await page.getByTestId('add-pedal').click();
  await expect(page.getByTestId('add-pedal-menu')).toBeHidden();
});

test('popover: focus enters the menu on open and Tab cycles inside it', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  await page.getByTestId('add-pedal').click();
  const menu = page.getByTestId('add-pedal-menu');
  await expect(menu).toBeVisible();

  // Focus moved INTO the panel — otherwise Escape and the trap are unreachable.
  const insideAtOpen = await page.evaluate(() =>
    Boolean(document.querySelector('[data-testid="add-pedal-menu"]')?.contains(document.activeElement))
  );
  expect(insideAtOpen).toBe(true);

  const count = await menu.locator('button').count();
  expect(count).toBeGreaterThan(1);

  // One full cycle of Tab must land back on the first item and never leave.
  const firstId = await activeTestId(page);
  for (let i = 0; i < count; i++) {
    await page.keyboard.press('Tab');
    const inside = await page.evaluate(() =>
      Boolean(document.querySelector('[data-testid="add-pedal-menu"]')?.contains(document.activeElement))
    );
    expect(inside, `focus escaped the menu after ${i + 1} Tab press(es)`).toBe(true);
  }
  expect(await activeTestId(page)).toBe(firstId);

  // Shift+Tab from the first item wraps backwards, still inside.
  await page.keyboard.press('Shift+Tab');
  const insideBack = await page.evaluate(() =>
    Boolean(document.querySelector('[data-testid="add-pedal-menu"]')?.contains(document.activeElement))
  );
  expect(insideBack).toBe(true);
});

// ---- 4. "Upload IR…" is a real, named, keyboard-operable control --------------

test('upload IR: the control is in the accessibility tree, named, and keyboard-operable', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();

  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();

  // Named + in the a11y tree with a real role. Before this slice it was a <label>
  // wrapping a display:none input: no role, no name, no tab stop.
  const item = page.getByRole('menuitem', { name: 'Upload IR…' });
  await expect(item).toBeVisible();

  // Reachable by Tab from wherever focus landed on open (it is the last item).
  const steps = await tabUntil(page, `el.getAttribute('data-testid') === 'cab-upload'`, 30);
  expect(steps, '"Upload IR…" must be reachable by Tab').toBeGreaterThan(0);

  // And activating it by KEYBOARD must actually reach the file input — a named
  // button that forwards nowhere would be dead UI that passes every check above.
  await page.evaluate(() => {
    const input = document.querySelector<HTMLInputElement>('[data-testid="cab-upload-input"]')!;
    (window as unknown as { __irPickerOpened: boolean }).__irPickerOpened = false;
    input.addEventListener('click', (e) => {
      e.preventDefault(); // don't actually open an OS dialog in the harness
      (window as unknown as { __irPickerOpened: boolean }).__irPickerOpened = true;
    });
  });
  await page.keyboard.press('Enter');
  expect(
    await page.evaluate(() => (window as unknown as { __irPickerOpened: boolean }).__irPickerOpened)
  ).toBe(true);
});

// ---- 5. CHAT: live region + auto-scroll only when pinned ----------------------

function sseLongReply(text: string): string {
  const events = [
    { type: 'message_start', message: { id: 'msg', type: 'message', role: 'assistant', content: [] } },
    { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
    { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text } },
    { type: 'content_block_stop', index: 0 },
    { type: 'message_delta', delta: { stop_reason: 'end_turn' }, usage: { output_tokens: 9 } },
    { type: 'message_stop' },
  ];
  return events.map((e) => `event: ${e.type}\ndata: ${JSON.stringify(e)}`).join('\n\n') + '\n\n';
}

const LONG = 'Rolling the filter back tames the fizz without losing the bite. '.repeat(14);

async function mockAssistant(page: Page) {
  await page.route('**/api/health', (route: Route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true, hasKey: true }) })
  );
  await page.route('**/api/chat', (route: Route) =>
    route.fulfill({ status: 200, contentType: 'text/event-stream', body: sseLongReply(LONG) })
  );
}

test('chat: the transcript is a live region', async ({ page }) => {
  await page.goto('/');
  const log = page.getByTestId('chat-scroll');
  // A screen reader is told nothing about a plain <div>: the coach's reply, and
  // every rig change it applied, arrived silently before this slice.
  await expect(log).toHaveAttribute('role', 'log');
  await expect(log).toHaveAttribute('aria-live', 'polite');
});

test('chat: a new message does NOT yank the view while you are reading back', async ({ page }) => {
  await mockAssistant(page);
  await page.goto('/');

  const scroll = page.getByTestId('chat-scroll');
  const send = async (text: string) => {
    const before = await page.getByTestId('ai-bubble').count();
    await page.getByTestId('chat-input').fill(text);
    await page.getByTestId('chat-send').click();
    await expect(page.getByTestId('ai-bubble')).toHaveCount(before + 1);
  };

  await send('one');
  await send('two');
  await send('three');

  const metrics = () =>
    scroll.evaluate((el) => ({ top: el.scrollTop, max: el.scrollHeight - el.clientHeight }));
  const m0 = await metrics();
  expect(m0.max, 'the transcript must actually overflow for this test to mean anything').toBeGreaterThan(50);

  // Scroll UP with a real wheel gesture (so a genuine scroll event fires — the
  // component tracks pinned-ness from `onScroll`, and a programmatic scrollTop
  // write would be testing something the player cannot do).
  const overTranscript = async () => {
    const box = (await scroll.boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  };
  await overTranscript();
  await page.mouse.wheel(0, -100000);
  await expect.poll(async () => (await metrics()).top).toBeLessThan(20);
  const readingAt = (await metrics()).top;

  await send('four');
  const afterSend = await metrics();
  console.log(
    `\n[chat scroll] reading back at scrollTop ${readingAt}; after a new turn ` +
      `scrollTop ${afterSend.top} (bottom would be ${afterSend.max})\n`
  );
  // The whole point: the view stayed where the player put it.
  expect(Math.abs(afterSend.top - readingAt)).toBeLessThan(12);
  expect(afterSend.top).toBeLessThan(afterSend.max - 50);

  // ...and when they ARE at the bottom, it still follows. (Sending moved the mouse
  // onto the send button, so put it back over the transcript first — otherwise the
  // wheel scrolls the page and this measures nothing.)
  await overTranscript();
  await page.mouse.wheel(0, 100000);
  await expect.poll(async () => { const m = await metrics(); return m.max - m.top; }).toBeLessThan(20);
  await send('five');
  const followed = await metrics();
  expect(followed.max - followed.top).toBeLessThan(20);
});

// ---- 6. TUNER: no rAF loop while disengaged -----------------------------------

test('tuner: the animation loop only runs while engaged', async ({ page }) => {
  // Count every requestAnimationFrame REQUEST. For a self-rescheduling loop that
  // is exactly the frame rate; nothing else in the app schedules frames except a
  // board drag/reorder, which this test does not perform.
  await page.addInitScript(() => {
    const w = window as unknown as { __rafCount: number };
    w.__rafCount = 0;
    const orig = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = (cb: FrameRequestCallback) => {
      w.__rafCount++;
      return orig(cb);
    };
  });
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();
  await addPedal(page, 'tuner');
  await expect(page.getByTestId('tuner')).toHaveAttribute('data-engaged', 'false');

  const countOverASecond = async () => {
    await page.evaluate(() => ((window as unknown as { __rafCount: number }).__rafCount = 0));
    await page.waitForTimeout(1000);
    return await page.evaluate(() => (window as unknown as { __rafCount: number }).__rafCount);
  };

  const disengaged = await countOverASecond();

  await page.getByTestId('tuner-footswitch').click();
  await expect(page.getByTestId('tuner')).toHaveAttribute('data-engaged', 'true');
  const engaged = await countOverASecond();

  console.log(
    `\n[tuner rAF] frames scheduled per second — disengaged ${disengaged}, engaged ${engaged}\n`
  );
  // Disengaged: no loop at all. (A couple of stray frames from an unrelated
  // layout pass would be tolerable; a running loop is 50+.)
  expect(disengaged).toBeLessThan(5);
  // Engaged: the meter must still animate, or this "fix" broke the tuner.
  expect(engaged).toBeGreaterThan(30);

  // And the meter parks DARK when it stops, rather than freezing mid-sweep.
  await page.getByTestId('tuner-footswitch').click();
  await expect(page.getByTestId('tuner')).toHaveAttribute('data-engaged', 'false');
  const lit = await page.evaluate(
    () => document.querySelectorAll('[data-testid="tuner-meter"] .seg[data-on="true"]').length
  );
  expect(lit).toBe(0);
});

// ---- 7. BOARD: no cable from an unlaid-out jack -------------------------------

test('board: no patch cable starts off the board when the source jack is hidden', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  // Wide: the source jack is laid out, so its cable exists. This is the control —
  // without it, "no stray cable" could be satisfied by drawing no cables at all.
  await page.setViewportSize({ width: 1280, height: 900 });
  await expect(page.getByTestId('board-source')).toBeVisible();
  await expect.poll(() => page.getByTestId('board-cables').locator('path.cable-body').count()).toBe(2);

  // Narrow: board.css hides .board-source below 760 px. Its jack is still in the
  // DOM and still returns a DOMRect — an all-zero one.
  await page.setViewportSize({ width: 700, height: 900 });
  await expect(page.getByTestId('board-source')).toBeHidden();

  // Exactly one cable remains: pedal → amp. The source's segment is not drawn
  // stray, it is not drawn at all, because the jack has no position.
  await expect.poll(() => page.getByTestId('board-cables').locator('path.cable-body').count()).toBe(1);

  const starts = await page.evaluate(() => {
    const board = document.querySelector<HTMLElement>('[data-testid="board"]')!;
    const r = board.getBoundingClientRect();
    return Array.from(document.querySelectorAll<SVGPathElement>('path.cable-body')).map((p) => {
      const m = (p.getAttribute('d') ?? '').match(/^M\s+(-?[\d.]+)\s+(-?[\d.]+)/);
      return { x: m ? parseFloat(m[1]) : NaN, y: m ? parseFloat(m[2]) : NaN, w: r.width, h: r.height };
    });
  });

  // Every cable must begin somewhere on the board. The pre-fix code produced a
  // segment starting at the viewport origin in board coordinates — negative, i.e.
  // above and to the left of the board.
  for (const s of starts) {
    expect(Number.isFinite(s.x) && Number.isFinite(s.y)).toBe(true);
    expect(s.x, `cable starts off the board at x=${s.x}`).toBeGreaterThanOrEqual(0);
    expect(s.y, `cable starts off the board at y=${s.y}`).toBeGreaterThanOrEqual(0);
    expect(s.x).toBeLessThanOrEqual(s.w);
    expect(s.y).toBeLessThanOrEqual(s.h);
  }
});
