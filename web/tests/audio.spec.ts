import { test, expect } from '@playwright/test';

// M4 browser verification.
//
// Test ordering is deliberate: the offline-render audio proofs run FIRST, before
// any test spins up a real (non-offline) AudioContext. Creating many live
// AudioContexts in one browser process can starve later OfflineAudioContext
// renders (they go silent), so the DSP proofs go up front and the tests that
// press Start / stomp the pedal come last and stop the engine when done.
//
// Audio-path proofs (unchanged from M3, independent of the UI):
//  a. High-distortion 220 Hz sine -> odd harmonics (660 Hz) present, even
//     harmonics (440 Hz) ~absent (symmetric diode clipping); LEVEL scales RMS.
//  b. Bypass passes the input through ~untouched (no added harmonics).
//
// M4 UI/state proofs: neumorphic knobs (role=slider) drive params, the
// footswitch toggles bypass + LED, and the whole rig round-trips through JSON
// and survives a reload via localStorage.

const RENDER_SECONDS = 0.5;

test('RAT worklet: high distortion yields odd harmonics; LEVEL scales RMS', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    const sampleRate = 48000;

    async function render(messages: Array<Record<string, unknown>>): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');

      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });

      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') {
            clearTimeout(t);
            resolve();
          } else if (e.data?.type === 'error') {
            clearTimeout(t);
            reject(new Error(e.data.message));
          }
        };
      });

      for (const m of messages) node.port.postMessage(m);

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();

      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.1); // skip smoothing ramp
      const N = data.length - start;
      const k = (freq / sr) * N;
      const w = (2 * Math.PI * k) / N;
      const c = 2 * Math.cos(w);
      let s0 = 0,
        s1 = 0,
        s2 = 0;
      for (let i = start; i < data.length; i++) {
        s0 = data[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }

    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.1);
      let sum = 0,
        count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    const driven = await render([
      { type: 'param', id: 0, value: 0.9 }, // distortion
      { type: 'param', id: 1, value: 0.0 }, // filter (bright)
      { type: 'param', id: 2, value: 1.0 }, // level
    ]);
    const halfLevel = await render([
      { type: 'param', id: 0, value: 0.9 },
      { type: 'param', id: 1, value: 0.0 },
      { type: 'param', id: 2, value: 0.5 },
    ]);

    const f1 = goertzel(driven, sampleRate, 220);
    const h2 = goertzel(driven, sampleRate, 440);
    const h3 = goertzel(driven, sampleRate, 660);

    return {
      f1,
      h2,
      h3,
      rmsFull: rms(driven, sampleRate),
      rmsHalf: rms(halfLevel, sampleRate),
    };
  }, RENDER_SECONDS);

  expect(result.h3).toBeGreaterThan(0.01);
  expect(result.h3).toBeGreaterThan(result.h2 * 8);
  expect(result.h2).toBeLessThan(result.f1 * 0.1);

  const ratio = result.rmsFull / result.rmsHalf;
  expect(ratio).toBeGreaterThan(1.8);
  expect(ratio).toBeLessThan(2.2);
});

test('RAT worklet: bypass passes input through untouched', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    const sampleRate = 48000;

    async function render(bypass: boolean): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') {
            clearTimeout(t);
            resolve();
          } else if (e.data?.type === 'error') {
            clearTimeout(t);
            reject(new Error(e.data.message));
          }
        };
      });
      node.port.postMessage({ type: 'param', id: 0, value: 0.9 });
      node.port.postMessage({ type: 'param', id: 1, value: 0.0 });
      node.port.postMessage({ type: 'param', id: 2, value: 1.0 });
      node.port.postMessage({ type: 'bypass', on: bypass });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.1);
      const N = data.length - start;
      const k = (freq / sr) * N;
      const w = (2 * Math.PI * k) / N;
      const c = 2 * Math.cos(w);
      let s1 = 0,
        s2 = 0,
        s0 = 0;
      for (let i = start; i < data.length; i++) {
        s0 = data[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.1);
      let sum = 0,
        count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    const bypassed = await render(true);
    const processed = await render(false);
    return {
      bypassRms: rms(bypassed, sampleRate),
      bypassF1: goertzel(bypassed, sampleRate, 220),
      bypassH3: goertzel(bypassed, sampleRate, 660),
      processedRms: rms(processed, sampleRate),
      processedH3: goertzel(processed, sampleRate, 660),
    };
  }, RENDER_SECONDS);

  expect(result.bypassRms).toBeGreaterThan(0.6);
  expect(result.bypassRms).toBeLessThan(0.8);
  expect(result.bypassH3).toBeLessThan(result.bypassF1 * 0.02);
  expect(result.processedH3).toBeGreaterThan(result.bypassH3 * 10);
});

test('UI exposes M4 controls: pedal, three knobs, footswitch, source, oversampling', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();
  await expect(page.getByTestId('pedal')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Start audio' })).toBeVisible();
  await expect(page.getByRole('combobox', { name: 'Source' })).toBeVisible();

  // Knobs are accessible sliders named by their param.
  const dist = page.getByRole('slider', { name: 'Distortion' });
  const filter = page.getByRole('slider', { name: 'Filter' });
  const level = page.getByRole('slider', { name: 'Level' });
  await expect(dist).toBeVisible();
  await expect(filter).toBeVisible();
  await expect(level).toBeVisible();
  await expect(dist).toHaveAttribute('aria-valuemin', '0');
  await expect(dist).toHaveAttribute('aria-valuemax', '100');

  // Footswitch (bypass) + oversampling select render.
  await expect(page.getByTestId('footswitch')).toBeVisible();
  await expect(page.getByRole('combobox', { name: 'Oversampling' })).toBeVisible();
});

test('rig state: JSON round-trips exactly and restores from localStorage', async ({ page }) => {
  await page.goto('/');

  // Pure serialize -> deserialize round-trip is exact for a valid rig.
  const roundTrip = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const rig = {
      pedal: { type: 'rat', engaged: false, params: { distortion: 0.42, filter: 0.13, level: 0.9 } },
      oversampling: 8,
      source: 'live',
    };
    const back = t.deserializeRig(t.serializeRig(rig));
    return { rig, back, equal: JSON.stringify(rig) === JSON.stringify(back) };
  });
  expect(roundTrip.equal).toBe(true);
  expect(roundTrip.back).toEqual(roundTrip.rig);

  // Mutate the live rig via the UI, then reload — localStorage should restore it.
  const dist = page.getByRole('slider', { name: 'Distortion' });
  await dist.focus();
  await dist.press('ArrowDown'); // -0.05
  const savedDist = await dist.getAttribute('aria-valuenow');
  await page.getByTestId('footswitch').click(); // engaged -> false

  await page.reload();

  await expect(page.getByRole('slider', { name: 'Distortion' })).toHaveAttribute(
    'aria-valuenow',
    savedDist!
  );
  // Bypassed state persisted: pedal restores without the `on` class.
  await expect(page.getByTestId('pedal')).not.toHaveClass(/\bon\b/);
});

test('knob interaction: keyboard changes the readout and the worklet param message', async ({
  page,
}) => {
  const errors: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(String(e)));

  await page.goto('/');
  // Start the (test-tone) engine so param changes actually post to the worklet.
  await page.getByRole('button', { name: 'Start audio' }).click();
  await expect(page.getByTestId('status')).toHaveText('running', { timeout: 8000 });

  const dist = page.getByRole('slider', { name: 'Distortion' });
  const before = Number(await dist.getAttribute('aria-valuenow'));

  await dist.focus();
  for (let i = 0; i < 3; i++) await dist.press('ArrowUp'); // +0.05 * 3 = +15

  const after = Number(await dist.getAttribute('aria-valuenow'));
  expect(after).toBeGreaterThan(before);

  // Visible readout matches the aria value.
  await expect(page.getByTestId('knob-distortion-value')).toHaveText(String(after));

  // The change reached the worklet as a param message (id 0 = distortion).
  const lastParam = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.lastParam as { id: number; value: number }
  );
  expect(lastParam.id).toBe(0);
  expect(Math.round(lastParam.value * 100)).toBe(after);

  await page.getByRole('button', { name: 'Stop' }).click(); // release the AudioContext
  expect(errors, `console errors: ${errors.join(' | ')}`).toEqual([]);
});

test('footswitch: toggles engaged/LED state and sends the bypass message', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Start audio' }).click();
  await expect(page.getByTestId('status')).toHaveText('running', { timeout: 8000 });

  const pedal = page.getByTestId('pedal');
  const fsw = page.getByTestId('footswitch');

  // Default rig has the pedal engaged (LED lit).
  await expect(pedal).toHaveClass(/\bon\b/);
  await expect(fsw).toHaveAttribute('aria-checked', 'true');

  // Stomp -> bypassed: LED off (no `on` class), aria flips, worklet gets bypass.
  await fsw.click();
  await expect(pedal).not.toHaveClass(/\bon\b/);
  await expect(fsw).toHaveAttribute('aria-checked', 'false');
  let engaged = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedal.engaged);
  expect(engaged).toBe(false);
  let lastBypass = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastBypass);
  expect(lastBypass).toBe(true);

  // Stomp again -> engaged.
  await fsw.click();
  await expect(pedal).toHaveClass(/\bon\b/);
  engaged = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedal.engaged);
  expect(engaged).toBe(true);
  lastBypass = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastBypass);
  expect(lastBypass).toBe(false);

  await page.getByRole('button', { name: 'Stop' }).click(); // release the AudioContext
});

// Live-input smoke test (Chromium fake media flags in playwright.config.ts).
test('live input: getUserMedia path reaches running with no errors', async ({ page }) => {
  const errors: string[] = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') errors.push(msg.text());
  });
  page.on('pageerror', (err) => errors.push(String(err)));

  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  await page.getByRole('combobox', { name: 'Source' }).selectOption('live');
  await expect(page.getByTestId('feedback-hint')).toBeVisible();
  await page.getByRole('button', { name: 'Start audio' }).click();

  await expect(page.getByTestId('status')).toHaveText('running', { timeout: 8000 });
  await expect(page.getByText(/Latency · model/)).toBeVisible();

  expect(errors, `console errors: ${errors.join(' | ')}`).toEqual([]);
});
