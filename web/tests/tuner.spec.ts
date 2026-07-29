import { test, expect } from '@playwright/test';

import { installRenderGuards } from './support/render-guard';

// M7 tuner tests.
//
// Two proofs, mirroring the milestone quality bar:
//  1. DETECTION — feed known-frequency frames through the exact McLeod pitch path
//     the live tuner uses (the app's `__CLIPPER_TEST__.tuner.detect` seam, i.e.
//     pitchy + analyzePitch) and assert the right note + cents (within ±2). Covers
//     low E (82.41 Hz), A4 (440 Hz), a deliberately-sharp A (445 Hz → +~20 c), and
//     the documented lowest note, a 7-string low B (B0 ≈ 30.87 Hz).
//  2. MUTE — an OFFLINE render proving an engaged tuner in the chain silences the
//     output (RMS ≈ 0), and that a disengaged tuner passes audio through.
//
// The offline-render proof runs FIRST (before any live AudioContext), per the same
// ordering rule as the other suites.

const SR = 48000;

// Offline-render integrity for every render in this file — barrier + silence guard +
// non-finite scan (docs §41). NOTE: this is the one spec with a render that is SUPPOSED
// to be silent (the engaged tuner mutes the rig), and it opts out at the render site with
// `window.allowSilentRender(ctx)` rather than the guard being weakened for everyone.
// See tests/support/render-guard.ts.
test.beforeEach(async ({ page }) => {
  await installRenderGuards(page);
});

// ---- 2. MUTE (offline render) -------------------------------------------------

test('tuner: engaged tuner mutes the chain; disengaged passes audio', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (sr) => {
    const seconds = 0.5;

    async function render(chainEngaged: boolean): Promise<Float32Array> {
      const length = Math.floor(sr * seconds);
      const ctx = new OfflineAudioContext(1, length, sr);
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
      // Chain: a single tuner (engaged => mutes). Amp stays ON so the "passes
      // audio" case has a real signal path. Flush via a latency echo.
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({
          type: 'chain',
          pedals: [{ id: 'tuner-1', type: 'tuner', engaged: chainEngaged }],
        });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      // An ENGAGED tuner mutes the chain by design, so THIS render is meant to come back
      // all-zero — the assertion below is `mutedRms < 1e-4`. Opt out of the silence guard
      // explicitly, at the render site, so the intent is documented here instead of the
      // guard being loosened for every other render in the suite (docs §41).
      if (chainEngaged) {
        (
          window as unknown as { allowSilentRender: (c: OfflineAudioContext) => void }
        ).allowSilentRender(ctx);
      }
      const buf = await ctx.startRendering();
      return buf.getChannelData(0).slice();
    }

    function rms(data: Float32Array): number {
      const start = Math.floor(sr * 0.1); // skip the declick/mute fade region
      let sum = 0,
        count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    const muted = await render(true); // tuner engaged -> chain muted
    const passed = await render(false); // tuner bypassed -> audio passes
    return { mutedRms: rms(muted), passedRms: rms(passed) };
  }, SR);

  // Engaged tuner: output is silent (well below -80 dBFS).
  expect(result.mutedRms).toBeLessThan(1e-4);
  // Disengaged tuner: audio passes through the amp.
  expect(result.passedRms).toBeGreaterThan(0.02);
});

// ---- 1. DETECTION -------------------------------------------------------------

test('tuner: detects known pitches to the right note + cents', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  // Wait for the App's test seam (set in a mount effect).
  await page.waitForFunction(() => {
    const w = window as unknown as { __CLIPPER_TEST__?: { tuner?: unknown } };
    return !!w.__CLIPPER_TEST__?.tuner;
  });

  const readings = await page.evaluate((sr) => {
    const w = window as unknown as {
      __CLIPPER_TEST__: {
        tuner: {
          frameSize: number;
          detect: (
            s: Float32Array,
            sampleRate: number
          ) => { note: string; octave: number; cents: number; clarity: number } | null;
        };
      };
    };
    const { frameSize, detect } = w.__CLIPPER_TEST__.tuner;

    // A plucked-guitar-ish tone: fundamental + a few decaying harmonics.
    function tone(freq: number): Float32Array {
      const x = new Float32Array(frameSize);
      const harms = [1, 0.5, 0.33, 0.22, 0.14];
      for (let i = 0; i < frameSize; i++) {
        let s = 0;
        for (let h = 0; h < harms.length; h++) s += harms[h] * Math.sin((2 * Math.PI * freq * (h + 1) * i) / sr);
        x[i] = s * 0.3 * Math.exp(-i / sr / 1.5);
      }
      return x;
    }

    return {
      frameSize,
      lowE: detect(tone(82.41), sr), // E2
      a4: detect(tone(440), sr), // A4
      sharpA: detect(tone(445), sr), // A4, ~+20 cents sharp
      lowB: detect(tone(30.87), sr), // B0 — the documented lowest note
    };
  }, SR);

  // Frame size is the documented 4096.
  expect(readings.frameSize).toBe(4096);

  expect(readings.lowE).not.toBeNull();
  expect(readings.lowE!.note).toBe('E');
  expect(readings.lowE!.octave).toBe(2);
  expect(Math.abs(readings.lowE!.cents)).toBeLessThanOrEqual(2);

  expect(readings.a4).not.toBeNull();
  expect(readings.a4!.note).toBe('A');
  expect(readings.a4!.octave).toBe(4);
  expect(Math.abs(readings.a4!.cents)).toBeLessThanOrEqual(2);

  // 445 Hz is A4 about +20 cents sharp (positive = sharp).
  expect(readings.sharpA).not.toBeNull();
  expect(readings.sharpA!.note).toBe('A');
  expect(readings.sharpA!.cents).toBeGreaterThan(15);
  expect(readings.sharpA!.cents).toBeLessThan(24);

  // Low B (B0 ≈ 30.87 Hz) — the lowest note the 4096 frame resolves reliably.
  expect(readings.lowB).not.toBeNull();
  expect(readings.lowB!.note).toBe('B');
  expect(readings.lowB!.octave).toBe(0);
  expect(Math.abs(readings.lowB!.cents)).toBeLessThanOrEqual(2);
});

// ---- UI: tuner on the board ---------------------------------------------------

test('tuner: add from gear tray renders the tuner pedal (muted footswitch)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  // Open the gear tray and add a tuner.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-tuner').click();

  const tuner = page.getByTestId('tuner');
  await expect(tuner).toBeVisible();
  // Fresh tuner starts DISENGAGED (adding it does not mute the rig).
  await expect(tuner).toHaveAttribute('data-engaged', 'false');
  // No pitch yet -> note reads the placeholder.
  await expect(page.getByTestId('tuner-note')).toHaveText('—');

  // Stomping the footswitch engages it (label flips to "Muted").
  await page.getByTestId('tuner-footswitch').click();
  await expect(tuner).toHaveAttribute('data-engaged', 'true');
});

// ---- UI: the TC-style segmented face (M6.8) ----------------------------------
//
// Drives the tuner UI with synthetic readings through the additive test seam
// (`__CLIPPER_TEST__.pushTunerReading`) and asserts the segmented meter: red wells
// light on the FLAT (left) side for a flat note, on the SHARP (right) side for a
// sharp note, and the GREEN center lights (after the lock hold) when in tune.
test('tuner: segmented meter lights red flat/sharp and green in tune (M6.8)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-tuner').click();

  const tuner = page.getByTestId('tuner');
  await expect(tuner).toBeVisible();
  // Engage so the meter reads (showReading requires engaged).
  await page.getByTestId('tuner-footswitch').click();
  await expect(tuner).toHaveAttribute('data-engaged', 'true');

  await page.waitForFunction(() => {
    const w = window as unknown as { __CLIPPER_TEST__?: { pushTunerReading?: unknown } };
    return typeof w.__CLIPPER_TEST__?.pushTunerReading === 'function';
  });
  const push = (r: unknown) =>
    page.evaluate((reading) => {
      (window as unknown as { __CLIPPER_TEST__: { pushTunerReading: (r: unknown) => void } }).__CLIPPER_TEST__.pushTunerReading(
        reading
      );
    }, r);

  // The note screen renders the letter (A) — the a11y text carries it.
  await push({ freq: 220, note: 'A', octave: 3, cents: -22, clarity: 1 });
  await expect(page.getByTestId('tuner-note')).toHaveText('A');

  // Flat (−22 c): red wells light on the LEFT (data-seg < 0), none on the right.
  const litLeft = () => page.locator('.tuner-meter .seg[data-seg="-1"][data-on="true"]');
  const litRight = () => page.locator('.tuner-meter .seg[data-seg="1"][data-on="true"]');
  const green = () => page.locator('.tuner-meter .seg-green[data-on="true"]');
  await expect(litLeft()).toHaveCount(1);
  await expect(litRight()).toHaveCount(0);
  await expect(green()).toHaveCount(0);

  // Sharp (+22 c): red wells swap to the RIGHT side.
  await push({ freq: 224, note: 'A', octave: 3, cents: 22, clarity: 1 });
  await expect(litRight()).toHaveCount(1);
  await expect(litLeft()).toHaveCount(0);

  // In tune (0 c): after the lock hold, the green center lights and reds go dark.
  await push({ freq: 220, note: 'A', octave: 3, cents: 0, clarity: 1 });
  await expect(green()).toHaveCount(1, { timeout: 2000 });
  await expect(litLeft()).toHaveCount(0);
  await expect(litRight()).toHaveCount(0);
});
