import { test, expect } from '@playwright/test';

import { installNonFiniteOutputGuard } from './support/finite-output';

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

// 2026-07-25 (test/assert-real-properties). This file had ZERO finiteness checks: every
// assertion in it is a Goertzel bin, an RMS or a max-delta over a WINDOW, so a NaN outside
// that window — or in the right channel of a stereo render — failed nothing. One line here
// covers every render in the file, present and future, in every channel, over every sample.
// See tests/support/finite-output.ts for why it is a prototype patch and not a helper called
// at each of the ~15 render sites.
test.beforeEach(async ({ page }) => {
  await installNonFiniteOutputGuard(page);
});

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

      // Post all setup messages, then a benign oversampling message whose
      // `latency` echo we await: this flushes the port queue so every message is
      // delivered+applied before the (synchronous) offline render starts.
      // Without it, a just-posted message can lose the race and the render comes
      // out unprocessed.
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        for (const m of messages) node.port.postMessage(m);
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });

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

    // Power the amp OFF (bypass amp+cab) so this stays a pure PEDAL DSP proof,
    // exactly as in M3/M4 — the amp/cab path is exercised by its own tests below.
    const ampOff = { type: 'bypass', unit: 'amp', on: true };
    const driven = await render([
      ampOff,
      { type: 'param', unit: 'pedal', id: 0, value: 0.9 }, // distortion
      { type: 'param', unit: 'pedal', id: 1, value: 0.0 }, // filter (bright)
      { type: 'param', unit: 'pedal', id: 2, value: 1.0 }, // level
    ]);
    const halfLevel = await render([
      ampOff,
      { type: 'param', unit: 'pedal', id: 0, value: 0.9 },
      { type: 'param', unit: 'pedal', id: 1, value: 0.0 },
      { type: 'param', unit: 'pedal', id: 2, value: 0.5 },
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
      // Power the amp off so this isolates the PEDAL bypass behavior (as in M4).
      // Await the oversampling `latency` echo as a flush barrier (see test above).
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 0, value: 0.9 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 1, value: 0.0 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 2, value: 1.0 });
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: bypass });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });

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

// v1.1 item 4 + field-fix (docs §24): the Muff "Pi" fuzz renders through the worklet
// and CHANGES THE SOUND MASSIVELY — a fuzz, not an overdrive. Three proofs: (a) it
// generates a huge 3rd harmonic (a near-square wave: h3 is a large fraction of the
// fundamental, far beyond the RAT's hard clip); (b) the wall-of-sustain COMPRESSION at
// HIGH sustain (0.8) — a 20 dB input drop yields almost no output-RMS change (the same
// wall regardless of pick force), which a clean/bypass path does NOT do; and (c) THE FIX
// — at MIN sustain the wall COLLAPSES: the same 20 dB input drop now DOES move the output
// (dynamics return, so single-coil hum is no longer compressed up to playing level).
test('muff worklet: fuzz makes massive harmonics + a compressed sustain wall', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    const sampleRate = 48000;

    async function render(chain: Array<Record<string, unknown>>, amp: number): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      let ctx: OfflineAudioContext | null = null;
      for (let attempt = 0; attempt < 4 && !ctx; attempt++) {
        const c = new OfflineAudioContext(1, length, sampleRate);
        try {
          await Promise.race([
            c.audioWorklet.addModule('/generated/clipper-processor.js'),
            new Promise((_r, rej) => setTimeout(() => rej(new Error('addModule timeout')), 6000)),
          ]);
          ctx = c;
        } catch {
          /* fresh context, retry */
        }
      }
      if (!ctx) throw new Error('addModule failed after retries');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        const t = setTimeout(() => resolve(), 6000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') { clearTimeout(t); resolve(); }
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true }); // pure pedal proof
        node.port.postMessage({ type: 'chain', pedals: chain });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: amp });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.12);
      const N = data.length - start;
      const k = (freq / sr) * N;
      const w = (2 * Math.PI * k) / N;
      const c = 2 * Math.cos(w);
      let s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { const s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w), im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.12);
      let sum = 0, n = 0;
      for (let i = start; i < data.length; i++) { sum += data[i] * data[i]; n++; }
      return Math.sqrt(sum / n);
    }

    // HIGH sustain (0.8) — the wall-of-sustain territory (post field-fix the wall lives
    // at sustain >= 0.6). MIN sustain (0.0) — the escape hatch where dynamics return.
    const muffHi = (a: number) =>
      render([{ id: 'm', type: 'muff', engaged: true, params: { distortion: 0.8, filter: 0.5, level: 0.6 } }], a);
    const muffMin = (a: number) =>
      render([{ id: 'm', type: 'muff', engaged: true, params: { distortion: 0.0, filter: 0.5, level: 0.6 } }], a);
    const bypass = (a: number) =>
      render([{ id: 'm', type: 'muff', engaged: false, params: { distortion: 0.8, filter: 0.5, level: 0.6 } }], a);

    const hot = await muffHi(0.3);        // hot pick, high sustain
    const soft = await muffHi(0.03);      // 20 dB softer pick, high sustain
    const clean = await bypass(0.3);      // true bypass (clean sine)

    return {
      fuzzF1: goertzel(hot, sampleRate, 220),
      fuzzH3: goertzel(hot, sampleRate, 660),
      fuzzH5: goertzel(hot, sampleRate, 1100),
      cleanH3: goertzel(clean, sampleRate, 660),
      hotRms: rms(hot, sampleRate),
      softRms: rms(soft, sampleRate),
      cleanRms: rms(clean, sampleRate),
      cleanRmsSoft: rms(await bypass(0.03), sampleRate),
      // Min-sustain dynamics: the same 20 dB input drop, wall collapsed.
      minHotRms: rms(await muffMin(0.3), sampleRate),
      minSoftRms: rms(await muffMin(0.03), sampleRate),
    };
  }, RENDER_SECONDS);

  // (a) MASSIVE harmonics: the 3rd harmonic is a large fraction of the fundamental
  // (a near-square fuzz wave), and DRAMATICALLY more than the clean sine makes.
  expect(result.fuzzH3).toBeGreaterThan(result.fuzzF1 * 0.2);
  expect(result.fuzzH3).toBeGreaterThan(result.cleanH3 * 50);
  expect(result.fuzzH5).toBeGreaterThan(result.fuzzF1 * 0.05); // rich odd spectrum

  // (b) the sustain WALL at HIGH sustain: a 20 dB input drop barely moves the fuzz
  // output RMS (heavy compression, ~1:1), whereas the clean bypass path tracks input.
  const fuzzRatio = result.hotRms / result.softRms;      // ~1 (compressed wall)
  const cleanRatio = result.cleanRms / result.cleanRmsSoft; // ~10 (tracks input)
  expect(fuzzRatio).toBeLessThan(2.0);
  expect(cleanRatio).toBeGreaterThan(5.0);

  // (c) THE FIX: at MIN sustain the wall COLLAPSES — the same 20 dB input drop now moves
  // the output by several dB (dynamics return), far more than the high-sustain wall does.
  const minRatio = result.minHotRms / result.minSoftRms;  // ~5 (tracks input, dynamic)
  expect(minRatio).toBeGreaterThan(3.0);
  expect(minRatio).toBeGreaterThan(fuzzRatio * 2.0);
});

// v1.1 item 6 (docs §27): the GOLD "Myth" overdrive renders through the worklet and
// proves its two defining behaviours end-to-end. (a) TRANSPARENT at GAIN 0: the
// clipped half of the ganged blend is switched OUT, so the output is the input —
// same level (OUTPUT at noon is exactly unity) and essentially no new harmonics, a
// thing no other dirt pedal here can do. (b) At high GAIN it clearly distorts (a big
// 3rd harmonic). (c) The clean blend keeps it TOUCH-SENSITIVE even when pushed: a
// 20 dB softer pick still moves the output a lot (contrast the Muff's wall, whose
// output barely moves at all).
test('gold worklet: transparent at min gain, harmonics + touch response when pushed', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    const sampleRate = 48000;

    async function render(chain: Array<Record<string, unknown>>, amp: number): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      let ctx: OfflineAudioContext | null = null;
      for (let attempt = 0; attempt < 4 && !ctx; attempt++) {
        const c = new OfflineAudioContext(1, length, sampleRate);
        try {
          await Promise.race([
            c.audioWorklet.addModule('/generated/clipper-processor.js'),
            new Promise((_r, rej) => setTimeout(() => rej(new Error('addModule timeout')), 6000)),
          ]);
          ctx = c;
        } catch {
          /* fresh context, retry */
        }
      }
      if (!ctx) throw new Error('addModule failed after retries');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        const t = setTimeout(() => resolve(), 6000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') { clearTimeout(t); resolve(); }
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true }); // pure pedal proof
        node.port.postMessage({ type: 'chain', pedals: chain });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: amp });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.12);
      const N = data.length - start;
      const k = (freq / sr) * N;
      const w = (2 * Math.PI * k) / N;
      const c = 2 * Math.cos(w);
      let s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { const s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w), im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.12);
      let sum = 0, n = 0;
      for (let i = start; i < data.length; i++) { sum += data[i] * data[i]; n++; }
      return Math.sqrt(sum / n);
    }

    // GAIN 0 with OUTPUT at noon: the pedal's documented unity/clean calibration.
    const goldClean = (a: number) =>
      render([{ id: 'g', type: 'gold', engaged: true, params: { distortion: 0.0, filter: 0.5, level: 0.5 } }], a);
    // GAIN 0.9: pushed, but the clean core is still in the mix.
    const goldPushed = (a: number) =>
      render([{ id: 'g', type: 'gold', engaged: true, params: { distortion: 0.9, filter: 0.5, level: 0.5 } }], a);
    // GAIN 0.35: the shipped default — the "always-on" setting the pedal is famous
    // for, where the clean core dominates and the playing dynamics survive.
    const goldDefault = (a: number) =>
      render([{ id: 'g', type: 'gold', engaged: true, params: { distortion: 0.35, filter: 0.5, level: 0.5 } }], a);
    const bypass = (a: number) =>
      render([{ id: 'g', type: 'gold', engaged: false, params: { distortion: 0.9, filter: 0.5, level: 0.5 } }], a);

    const clean = await goldClean(0.3);
    const pushed = await goldPushed(0.3);
    const defHot = await goldDefault(0.3);
    const defSoft = await goldDefault(0.03); // 20 dB softer pick
    const dry = await bypass(0.3);

    return {
      cleanF1: goertzel(clean, sampleRate, 220),
      cleanH3: goertzel(clean, sampleRate, 660),
      cleanRms: rms(clean, sampleRate),
      dryF1: goertzel(dry, sampleRate, 220),
      dryH3: goertzel(dry, sampleRate, 660),
      dryRms: rms(dry, sampleRate),
      pushedF1: goertzel(pushed, sampleRate, 220),
      pushedH3: goertzel(pushed, sampleRate, 660),
      defHotRms: rms(defHot, sampleRate),
      defSoftRms: rms(defSoft, sampleRate),
    };
  }, RENDER_SECONDS);

  // (a) TRANSPARENT at GAIN 0: same level as the bypassed (dry) path within ~1 dB,
  // and the 3rd harmonic stays down at the dry path's own noise level.
  const levelRatio = result.cleanRms / result.dryRms;
  expect(levelRatio).toBeGreaterThan(0.9);
  expect(levelRatio).toBeLessThan(1.1);
  expect(result.cleanH3).toBeLessThan(result.cleanF1 * 0.01);

  // (b) PUSHED: a big 3rd harmonic that the transparent setting simply does not make.
  expect(result.pushedH3).toBeGreaterThan(result.pushedF1 * 0.1);
  expect(result.pushedH3).toBeGreaterThan(result.cleanH3 * 20);

  // (c) TOUCH RESPONSE survives at the shipped GAIN default: 20 dB softer picking
  // still drops the output ~10 dB (measured ratio 3.13 — the parallel clean path is
  // never compressed), where a fuzz's wall barely moves at all (< 2.0, see the Muff
  // test above).
  const dynamics = result.defHotRms / result.defSoftRms;
  expect(dynamics).toBeGreaterThan(2.5);
});

// M5: amp tone is audibly measurable through the FULL chain (pedal -> amp ->
// cab). Treble at max vs min changes the 5 kHz content (treble shelf @ 3.5 kHz).
test('amp: treble knob changes 5 kHz content through the full chain', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    async function render(trebleKnob: number): Promise<Float32Array> {
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
      // Pedal bypassed (clean 5 kHz tone passes), amp ON, tone via treble knob.
      // Await the worklet's `latency` echo (sent in reply to the cab param) so we
      // KNOW every message above was delivered+applied before offline rendering
      // starts — offline renders run synchronously and can otherwise finish
      // before a just-posted message reaches the processor.
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // volume = unity (M6.1 taper)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: trebleKnob }); // treble
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> echoes latency
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 5000 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.15);
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

    const hi = await render(1.0);
    const lo = await render(0.0);
    return {
      trebleMax: goertzel(hi, sampleRate, 5000),
      trebleMin: goertzel(lo, sampleRate, 5000),
    };
  });

  // Treble at max should lift 5 kHz well above treble at min (the +/-12 dB shelf
  // above 3.5 kHz => ~24 dB swing; > 4x amplitude even after the constant cab
  // rolloff).
  expect(result.trebleMax).toBeGreaterThan(result.trebleMin * 4);
});

// M5: amp power off = amp+cab bypassed (a true passthrough of the pedal output).
test('amp: power off passes the pedal signal through untouched', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    async function render(ampOn: boolean): Promise<Float32Array> {
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
      // Pedal bypassed so the input reaches the amp stage untouched; toggle amp.
      // Await the worklet's `latency` echo (sent in reply to the amp-bypass
      // message) so the toggle is guaranteed applied before offline rendering.
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        // Explicit low amp volume (0.2 -> ~-13 dB in the M6.1 taper) so "amp on"
        // is unambiguously quieter than the passthrough regardless of the taper's
        // loud default (0.4 = unity).
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.2 });
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: !ampOn }); // echoes latency
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.15);
      let sum = 0,
        count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    const off = await render(false);
    const on = await render(true);
    return { offRms: rms(off, sampleRate), onRms: rms(on, sampleRate) };
  });

  // Power off: pure passthrough of the (bypassed-pedal) input == the raw
  // oscillator, RMS ~0.707 (the output soft limiter only kisses the ±1.0 peaks).
  // Power on: with volume set low (0.2) the amp+cab clearly attenuate it.
  expect(result.offRms).toBeGreaterThan(0.6);
  expect(result.offRms).toBeLessThan(0.8);
  expect(result.onRms).toBeLessThan(result.offRms * 0.5);
});

// M6.1: the full default rig lands at a healthy output level (the "no volume"
// fix) and the output soft limiter guarantees no raw overs even when driven hard.
test('output level: default rig is healthily loud and never overs (limiter)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    // Render the FULL default rig (pedal -> amp -> cab) through the worklet, with
    // a 0.1-amplitude 220 Hz input, optionally cranking to test the limiter.
    async function render(hot: boolean): Promise<Float32Array> {
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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        // Default rig pedal + amp params (amp volume 0.4 = unity in the M6.1 taper).
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 0, value: hot ? 1.0 : 0.7 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 1, value: 0.4 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 2, value: 0.8 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: hot ? 1.0 : 0.4 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.6 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.1 }); // ~real interface level
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function peak(data: Float32Array): number {
      let p = 0;
      const start = Math.floor(data.length / 3);
      for (let i = start; i < data.length; i++) p = Math.max(p, Math.abs(data[i]));
      return p;
    }

    const def = await render(false);
    const hot = await render(true);
    return { defPeak: peak(def), hotPeak: peak(hot) };
  });

  // Default rig at a 0.1 input is healthily loud — NOT the old M5 ~-26 dBFS
  // (0.05). M6.6 peak-normalized the cab IR (max |H| = 1, ~-3 dB vs the old
  // 1 kHz-unity norm) so the absolute level sits a few dB lower than M6.5 in
  // exchange for a clean path that can never push the limiter via the cab.
  expect(result.defPeak).toBeGreaterThan(0.15); // > ~-16.5 dBFS, ~3x the old M5 level
  expect(result.defPeak).toBeLessThanOrEqual(1.0);
  // Cranked, the gain-riding limiter holds the output at/below full scale.
  expect(result.hotPeak).toBeLessThanOrEqual(1.0);
});

// M6.3: the worklet goes STEREO at the amp stage. With the chorus engaged the
// two output channels differ (dry L / wet R bloom); with it off they are
// identical. Renders a 2-channel OfflineAudioContext through the full worklet.
test('chorus: engaged makes L/R differ; off leaves them identical (stereo)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    // Render the worklet into a STEREO buffer at a given chorus mode (0/1/2).
    async function render(chorusMode: number): Promise<{ maxDiff: number; rmsR: number }> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(2, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [2],
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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        // Pedal bypassed so a clean tone reaches the amp; amp on, unity volume.
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // volume
        node.port.postMessage({ type: 'param', unit: 'amp', id: 6, value: 0.6 }); // chorus speed
        node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: 0.7 }); // chorus depth
        node.port.postMessage({ type: 'param', unit: 'amp', id: 8, value: chorusMode }); // mode
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      const L = buffer.getChannelData(0);
      const R = buffer.getChannelData(1);
      let maxDiff = 0;
      let sumR = 0;
      const start = Math.floor(L.length / 3); // skip fill-in transient
      for (let i = start; i < L.length; i++) {
        const d = Math.abs(L[i] - R[i]);
        if (d > maxDiff) maxDiff = d;
        sumR += R[i] * R[i];
      }
      return { maxDiff, rmsR: Math.sqrt(sumR / (L.length - start)) };
    }

    const off = await render(0);
    const chorus = await render(1);
    return { off, chorus };
  });

  // Off: the two channels are identical (chorus truly bypassed).
  expect(result.off.maxDiff).toBeLessThan(1e-6);
  // Chorus: the wet R side carries real signal and clearly differs from dry L.
  expect(result.chorus.rmsR).toBeGreaterThan(0.05);
  expect(result.chorus.maxDiff).toBeGreaterThan(0.05);
});

// M6.7: the spring reverb (amp param id 9). reverb=0 renders identically to a
// render that never touches the param (bit-exact dry passthrough); with reverb up
// a wet tail rings on AFTER the source stops, and it DECAYS.
test('reverb: 0 == dry; up leaves a decaying tail after the input stops', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.6;
    const stopAt = 0.2; // the oscillator plays for 200 ms, then silence

    // Render the worklet (mono) with the amp on, chorus off. `reverb` null = never
    // set the param; a number = set amp id 9 to it.
    async function render(reverb: number | null): Promise<Float32Array> {
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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // volume
        if (reverb != null) {
          node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: reverb }); // reverb mix
        }
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      osc.stop(stopAt);
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    const rms = (x: Float32Array, from: number, to: number) => {
      const a = Math.floor(from * sampleRate);
      const b = Math.floor(to * sampleRate);
      let s = 0;
      for (let i = a; i < b; i++) s += x[i] * x[i];
      return Math.sqrt(s / (b - a));
    };

    const dry = await render(null); // param never set
    const zero = await render(0); // explicitly reverb=0
    const wet = await render(0.8); // drenched

    // reverb=0 vs the untouched render: bit-exact (passthrough).
    let maxDiff = 0;
    for (let i = 0; i < dry.length; i++) maxDiff = Math.max(maxDiff, Math.abs(dry[i] - zero[i]));

    return {
      maxDiff,
      dryTail: rms(dry, 0.32, 0.5), // dry: nothing after the source stops
      wetTail1: rms(wet, 0.32, 0.42), // wet: early tail
      wetTail2: rms(wet, 0.46, 0.56), // wet: later tail (should be quieter)
    };
  });

  // reverb=0 is a bit-exact dry passthrough.
  expect(result.maxDiff).toBeLessThan(1e-6);
  // Dry: essentially silent once the oscillator stops.
  expect(result.dryTail).toBeLessThan(1e-3);
  // Wet: a real tail rings on after the input stops, and it decays.
  expect(result.wetTail1).toBeGreaterThan(5e-3);
  expect(result.wetTail2).toBeLessThan(result.wetTail1);
});

// v1.1: the script phaser sweeps two notches under an LFO. A steady 400 Hz tone
// through an engaged phaser is MODULATED as the lower notch (which sweeps
// ~83-828 Hz) passes through 400 Hz: the output envelope dips deeply when the
// notch sits on the tone (a notch measurably present) and the amplitude in an
// EARLY vs a LATE window differs (the notch has moved = two LFO phases differ).
test('phaser: engaged phaser sweeps a moving notch through a steady tone', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 1.6; // > one LFO cycle at the chosen speed

    async function render(chain: Array<Record<string, unknown>>): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      // De-flake: Chromium's FIRST OfflineAudioContext.addModule in a fresh
      // process can hang (the same WebAudio warmup flake the retries guard); make
      // a context whose addModule resolved, retrying a fresh context on timeout.
      let ctx: OfflineAudioContext | null = null;
      for (let attempt = 0; attempt < 4 && !ctx; attempt++) {
        const c = new OfflineAudioContext(1, length, sampleRate);
        try {
          await Promise.race([
            c.audioWorklet.addModule('/generated/clipper-processor.js'),
            new Promise((_r, rej) => setTimeout(() => rej(new Error('addModule timeout')), 6000)),
          ]);
          ctx = c;
        } catch {
          /* fresh context, try again */
        }
      }
      if (!ctx) throw new Error('addModule failed after retries');
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
      await new Promise<void>((resolve) => {
        const t = setTimeout(() => resolve(), 6000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') {
            clearTimeout(t);
            resolve();
          }
        };
        // Power the amp OFF so this is a pure pedal DSP proof; install a phaser
        // chain with SPEED ~0.6 (a ~1.1 Hz sweep — a full cycle inside the render).
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({
          type: 'chain',
          pedals: [
            { id: 'ph', type: 'phaser', engaged: true, params: { distortion: 0.6, filter: 0.5, level: 0.5 } },
          ],
        });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 400 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    // Windowed RMS over [t0, t1) seconds.
    function winRms(data: Float32Array, t0: number, t1: number): number {
      const a = Math.floor(t0 * sampleRate), b = Math.min(data.length, Math.floor(t1 * sampleRate));
      let sum = 0, n = 0;
      for (let i = a; i < b; i++) { sum += data[i] * data[i]; n++; }
      return n ? Math.sqrt(sum / n) : 0;
    }

    const out = await render([]);
    // Slide a 40 ms window across the render (skip the initial settling) and find
    // the min/max window RMS: the min is where the notch sits on 400 Hz.
    const win = 0.04;
    let minW = Infinity, maxW = 0;
    for (let t = 0.15; t + win < seconds; t += 0.02) {
      const r = winRms(out, t, t + win);
      if (r < minW) minW = r;
      if (r > maxW) maxW = r;
    }
    return {
      minW,
      maxW,
      early: winRms(out, 0.2, 0.35),
      late: winRms(out, 0.9, 1.05),
      overall: winRms(out, 0.15, seconds - 0.05),
    };
  });

  // A notch is measurably present: the deepest window is far below the loudest
  // (the sweeping notch buries the 400 Hz tone as it passes).
  expect(result.maxW).toBeGreaterThan(0.02);
  expect(result.maxW / Math.max(result.minW, 1e-9)).toBeGreaterThan(3);
  // Two LFO phases differ: an early window and a later window are not the same
  // amplitude (the notch has moved between them).
  const rel = Math.abs(result.early - result.late) / Math.max(result.early, result.late, 1e-9);
  expect(rel).toBeGreaterThan(0.08);
  // 2026-07-25 (test/assert-real-properties): `overall` was computed and then never
  // asserted. It is the one number that pins the phaser as a MODULATOR rather than a
  // gate: the whole-render RMS must sit between the deepest and loudest windows, i.e.
  // the notch sweeps THROUGH the tone instead of holding it down. Without this, a
  // phaser stuck at its deepest notch (permanent 400 Hz cut) satisfies every
  // assertion above — a strong min/max ratio and a difference between two windows.
  expect(result.overall).toBeGreaterThan(result.minW);
  expect(result.overall).toBeLessThan(result.maxW);
  // And it is genuinely modulating, not sitting near one extreme: the whole-render RMS
  // is at least a quarter of the loudest window (a gate would leave it near minW).
  expect(result.overall).toBeGreaterThan(result.maxW * 0.25);
});

// M6.4: the pedal CHAIN is dynamic and ordered. Reordering two RATs with
// different settings changes the processed signal (distortion is nonlinear, so
// A->B != B->A); the SAME order renders deterministically (bit-identical).
test('chain: reorder of two RATs changes the output; same order is deterministic', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    const A = { id: 'A', type: 'rat', engaged: true, params: { distortion: 0.95, filter: 0.85, level: 1.0 } };
    const B = { id: 'B', type: 'rat', engaged: true, params: { distortion: 0.3, filter: 0.0, level: 1.0 } };

    async function render(chain: Array<Record<string, unknown>>): Promise<Float32Array> {
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
      // Amp OFF -> a pure pedal-chain proof. Post the chain, then an oversampling
      // message whose `latency` echo we await as the flush barrier (the chain's
      // own latency echo fires later, at the in-render declick commit).
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({ type: 'chain', pedals: chain });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.3 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function metrics(data: Float32Array) {
      const start = Math.floor(data.length / 3); // skip the declick fade-in
      let sum = 0;
      let h3re = 0;
      let h3im = 0;
      const N = data.length - start;
      const w = (2 * Math.PI * 660) / sampleRate;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        h3re += data[i] * Math.cos(w * i);
        h3im += data[i] * Math.sin(w * i);
      }
      return { rms: Math.sqrt(sum / N), h3: (2 * Math.hypot(h3re, h3im)) / N };
    }

    const ab = await render([A, B]);
    const ba = await render([B, A]);
    const ab2 = await render([A, B]);

    // Deterministic: identical order -> bit-identical output.
    let maxDelta = 0;
    for (let i = 0; i < ab.length; i++) maxDelta = Math.max(maxDelta, Math.abs(ab[i] - ab2[i]));

    return { ab: metrics(ab), ba: metrics(ba), maxDelta };
  });

  // Same order is deterministic (bit-identical).
  expect(result.maxDelta).toBeLessThan(1e-6);
  // Order matters: the two orderings differ audibly (RMS and/or 3rd-harmonic).
  const rmsRatio = result.ab.rms / result.ba.rms;
  const h3Ratio = result.ab.h3 / Math.max(1e-9, result.ba.h3);
  expect(Math.abs(rmsRatio - 1) > 0.05 || Math.abs(h3Ratio - 1) > 0.05).toBe(true);
});

// M6.4: add/remove. A two-engaged-RAT chain differs from one; an EMPTY chain
// (guitar straight into the amp) passes the signal through (with the amp off, it
// is a clean passthrough of the source).
test('chain: adding a pedal changes output; empty chain passes through clean', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    // A bright, hard-clipping RAT...
    const BRIGHT = { id: 'bright', type: 'rat', engaged: true, params: { distortion: 0.9, filter: 0.0, level: 1.0 } };
    // ...and a DARK RAT whose post-clip low-pass tames the fizz when placed after.
    const DARK = { id: 'dark', type: 'rat', engaged: true, params: { distortion: 0.5, filter: 0.95, level: 1.0 } };

    async function render(chain: Array<Record<string, unknown>>): Promise<Float32Array> {
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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({ type: 'chain', pedals: chain });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.3 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function rms(data: Float32Array): number {
      const start = Math.floor(data.length / 3);
      let s = 0;
      for (let i = start; i < data.length; i++) s += data[i] * data[i];
      return Math.sqrt(s / (data.length - start));
    }
    function h3(data: Float32Array): number {
      const start = Math.floor(data.length / 3);
      let re = 0;
      let im = 0;
      const w = (2 * Math.PI * 660) / sampleRate;
      for (let i = start; i < data.length; i++) {
        re += data[i] * Math.cos(w * i);
        im += data[i] * Math.sin(w * i);
      }
      return (2 * Math.hypot(re, im)) / (data.length - start);
    }

    const one = await render([BRIGHT]);
    const two = await render([BRIGHT, DARK]);
    const empty = await render([]);
    // The source itself, for the empty-chain comparison (0.3 sine, RMS ~0.2121).
    const srcRms = 0.3 / Math.SQRT2;
    return {
      oneH3: h3(one),
      twoH3: h3(two),
      emptyRms: rms(empty),
      emptyH3: h3(empty),
      srcRms,
    };
  });

  // Adding the DARK RAT after the BRIGHT one demonstrably changes the sound: its
  // post-clip low-pass tames the 3rd harmonic (clearly lower than one pedal alone).
  expect(result.oneH3).toBeGreaterThan(0.01); // the bright pedal makes real fizz
  expect(result.twoH3).toBeLessThan(result.oneH3 * 0.7);
  // Empty chain passes the source through cleanly (amp off): RMS ~ the source's,
  // and no distortion harmonic added.
  expect(result.emptyRms).toBeGreaterThan(result.srcRms * 0.9);
  expect(result.emptyRms).toBeLessThan(result.srcRms * 1.1);
  expect(result.emptyH3).toBeLessThan(result.emptyRms * 0.02);
});

// M8: routed through the worklet's `chain` dispatch, an SD-1 pedal CHANGES the
// sound — a clean 220 Hz sine comes out with real harmonic content (it clips),
// and its soft, ASYMMETRIC feedback clip yields a substantial EVEN harmonic
// (2nd / 440 Hz), comparable to the odd 3rd — the SD-1 warmth a symmetric clipper
// cannot make. A pure pedal-chain proof (amp off), single render.
test('SD-1 worklet: adds harmonics (changes the sound) with a strong even harmonic', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
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
    // Amp OFF -> a pure SD-1 pedal proof. An sd1 instance via the `chain` message
    // exercises the worklet's sd_* dispatch (create/set_param/process).
    const SD = { id: 'sd', type: 'sd1', engaged: true, params: { distortion: 0.6, filter: 0.5, level: 0.8 } };
    await new Promise<void>((resolve) => {
      node.port.onmessage = (e: MessageEvent) => {
        if (e.data?.type === 'latency') resolve();
      };
      node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
      node.port.postMessage({ type: 'chain', pedals: [SD] });
      node.port.postMessage({ type: 'oversampling', factor: 4 });
    });
    const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
    const g = new GainNode(ctx, { gain: 0.3 });
    osc.connect(g).connect(node).connect(ctx.destination);
    osc.start();
    const buffer = await ctx.startRendering();
    const data = buffer.getChannelData(0);

    function goertzel(freq: number): number {
      const start = Math.floor(sampleRate * 0.15);
      const N = data.length - start;
      const w = (2 * Math.PI * freq) / sampleRate;
      const c = 2 * Math.cos(w);
      let s1 = 0, s2 = 0, s0 = 0;
      for (let i = start; i < data.length; i++) {
        s0 = data[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    return { f1: goertzel(220), h2: goertzel(440), h3: goertzel(660) };
  });

  // Real signal came through (guard against the known OfflineAudioContext silence
  // flake) and it CHANGED — a clean sine has no harmonics; the SD-1 clips (odd 3rd).
  expect(result.f1).toBeGreaterThan(0.05);
  expect(result.h3).toBeGreaterThan(result.f1 * 0.02);
  // The asymmetric feedback clip makes a STRONG even harmonic — comparable to the
  // odd one, and well above any measurement floor (a symmetric clipper makes ~none).
  expect(result.h2).toBeGreaterThan(result.f1 * 0.03);
  expect(result.h2).toBeGreaterThan(result.h3 * 0.2);
});

// v1.1: routed through the worklet's `chain` dispatch (ts_* exports), a TS808
// "Screamer" CHANGES the sound (it clips — odd 3rd harmonic present), AND — the
// mirror of the SD-1 — its SYMMETRIC feedback clip makes essentially NO even (2nd)
// harmonic, where the SD-1 on the identical stimulus makes a strong one. Both
// pedals are rendered in the same page so the symmetric-vs-asymmetric spectral
// difference is measured directly (the 2nd-harmonic ratio).
test('TS worklet: Screamer clips SYMMETRICALLY — strong 3rd, ~no 2nd vs the SD-1', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    async function renderPedal(type: string) {
      const sampleRate = 48000;
      const seconds = 0.5;
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
      // Amp OFF -> a pure pedal proof. Same knobs for both pedals (drive 0.6).
      const P = { id: 'p', type, engaged: true, params: { distortion: 0.6, filter: 0.5, level: 0.8 } };
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') resolve();
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({ type: 'chain', pedals: [P] });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.3 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      const data = buffer.getChannelData(0);
      function goertzel(freq: number): number {
        const start = Math.floor(sampleRate * 0.15);
        const N = data.length - start;
        const w = (2 * Math.PI * freq) / sampleRate;
        const c = 2 * Math.cos(w);
        let s1 = 0, s2 = 0, s0 = 0;
        for (let i = start; i < data.length; i++) {
          s0 = data[i] + c * s1 - s2;
          s2 = s1;
          s1 = s0;
        }
        const re = s1 - s2 * Math.cos(w);
        const im = s2 * Math.sin(w);
        return (2 * Math.sqrt(re * re + im * im)) / N;
      }
      return { f1: goertzel(220), h2: goertzel(440), h3: goertzel(660) };
    }
    const ts = await renderPedal('ts');
    const sd1 = await renderPedal('sd1');
    return { ts, sd1 };
  });

  // The TS produced real signal and CHANGED it — it clips (odd 3rd harmonic).
  expect(result.ts.f1).toBeGreaterThan(0.05);
  expect(result.ts.h3).toBeGreaterThan(result.ts.f1 * 0.02);
  // SYMMETRIC clip: the TS's 2nd (even) harmonic is a tiny fraction of its
  // fundamental — essentially absent (the mirror of the SD-1's asymmetry).
  const tsEvenRatio = result.ts.h2 / result.ts.f1;
  const sdEvenRatio = result.sd1.h2 / result.sd1.f1;
  expect(tsEvenRatio).toBeLessThan(0.01);
  // The SD-1 on the identical stimulus makes a MUCH larger even harmonic — the
  // spectral difference is measurable in the render (the family contrast).
  expect(sdEvenRatio).toBeGreaterThan(tsEvenRatio * 10);

  // 2026-07-25 (test/assert-real-properties): `result.sd1.h3` was computed and never
  // asserted, which left the SD-1 half of this comparison resting entirely on its 2nd
  // harmonic. An SD-1 that had stopped clipping altogether — outputting a level-shifted
  // but otherwise clean tone, all 2nd and no 3rd — would still beat the TS on
  // `sdEvenRatio` and pass. The SD-1 is an ASYMMETRIC clipper: it must make a strong
  // even harmonic AND a real odd one, because it is still clipping both half-cycles,
  // just unequally.
  expect(result.sd1.f1).toBeGreaterThan(0.05);          // real signal came through
  expect(result.sd1.h3).toBeGreaterThan(result.sd1.f1 * 0.02);  // it is genuinely clipping
  // And its asymmetry shows up as a 2nd harmonic COMPARABLE to its 3rd, where the
  // symmetric TS buries the 2nd far below its own 3rd. This is the family contrast
  // stated as a property of each pedal's own spectrum, not just a ratio between them.
  expect(result.sd1.h2).toBeGreaterThan(result.sd1.h3 * 0.1);
  expect(result.ts.h2).toBeLessThan(result.ts.h3 * 0.1);
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

  // M6.1 input calibration: the Trim knob and the peak meter render.
  await expect(page.getByTestId('input-stage')).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Input trim' })).toBeVisible();
  await expect(page.getByTestId('input-meter')).toBeVisible();
  // Default trim is unity (0 dB), knob ~33/100.
  await expect(page.getByTestId('input-trim-db')).toHaveText('0 dB');

  // M5 amp panel: Clean 120 with four tone knobs + the M6.7 Reverb knob +
  // bright/cab/power controls.
  await expect(page.getByTestId('amp')).toBeVisible();
  for (const name of ['Volume', 'Bass', 'Middle', 'Treble', 'Reverb']) {
    await expect(page.getByRole('slider', { name })).toBeVisible();
  }
  // Reverb ships at 0 (dry) and drags to a wet mix, updating the rig.
  const reverbKnob = page.getByRole('slider', { name: 'Reverb' });
  await expect(reverbKnob).toHaveAttribute('aria-valuenow', '0');
  await reverbKnob.focus();
  await page.keyboard.press('ArrowUp');
  const reverbVal = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.params.reverb
  );
  expect(reverbVal).toBeGreaterThan(0);
  await expect(page.getByRole('switch', { name: 'Bright' })).toBeVisible();
  await expect(page.getByRole('switch', { name: 'Cab' })).toBeVisible();
  await expect(page.getByRole('switch', { name: 'Power' })).toBeVisible();

  // M6.3 chorus/vibrato: Speed + Depth knobs and an Off/Chorus/Vibrato 3-way.
  await expect(page.getByRole('slider', { name: 'Chorus speed' })).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Chorus depth' })).toBeVisible();
  const modeGroup = page.getByTestId('chorus-mode');
  await expect(modeGroup).toBeVisible();
  const off = page.getByTestId('chorus-mode-off');
  const chorus = page.getByTestId('chorus-mode-chorus');
  const vibrato = page.getByTestId('chorus-mode-vibrato');
  // Default is Off (mode 0).
  await expect(off).toHaveAttribute('aria-checked', 'true');
  await expect(chorus).toHaveAttribute('aria-checked', 'false');
  // Selecting Vibrato updates the group and the rig state.
  await vibrato.click();
  await expect(vibrato).toHaveAttribute('aria-checked', 'true');
  await expect(off).toHaveAttribute('aria-checked', 'false');
  const mode = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().amp.params.chorusMode
  );
  expect(mode).toBe(2);
});

test('rig state: JSON round-trips exactly and restores from localStorage', async ({ page }) => {
  await page.goto('/');

  // Pure serialize -> deserialize round-trip is exact for a valid rig (incl a
  // multi-pedal chain + amp). Key order must match normalizeRig's output.
  const roundTrip = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    // Key order must match normalizeRig's output for the exact-JSON round-trip.
    const rig = {
      input: { trim: 0.5 },
      pedals: [
        { id: 'rat-a', type: 'rat', engaged: false, params: { distortion: 0.42, filter: 0.13, level: 0.9 } },
        { id: 'rat-b', type: 'rat', engaged: true, params: { distortion: 0.8, filter: 0.6, level: 0.5 } },
      ],
      amp: {
        type: 'clean120',
        engaged: false,
        // Cab expansion: cabModel sits between engaged and params in the
        // normalized shape; customCabLabel is omitted when unset (custom key
        // present only for 'custom' rigs), so a built-in rig round-trips exactly.
        cabModel: 'brit412',
        params: {
          volume: 0.33,
          bass: 0.6,
          middle: 0.4,
          treble: 0.7,
          bright: 1,
          cab: 0,
          speed: 0.6,
          depth: 0.8,
          chorusMode: 2,
          reverb: 0.5,
          // M9.4 JCM800 knobs — additive; sit after reverb in the normalized shape.
          gain: 0.55,
          presence: 0.35,
          master: 0.7,
        },
      },
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
  let engaged = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals[0].engaged);
  expect(engaged).toBe(false);
  let lastBypass = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastBypass);
  expect(lastBypass).toBe(true);

  // Stomp again -> engaged.
  await fsw.click();
  await expect(pedal).toHaveClass(/\bon\b/);
  engaged = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals[0].engaged);
  expect(engaged).toBe(true);
  lastBypass = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastBypass);
  expect(lastBypass).toBe(false);

  await page.getByRole('button', { name: 'Stop' }).click(); // release the AudioContext
});

// M5: an M4-shaped saved rig (no `amp`) migrates to amp defaults on load.
test('rig migration: an M4 rig without an amp gets amp defaults', async ({ page }) => {
  await page.goto('/');
  // Seed localStorage with an old (M4) rig shape, then reload.
  await page.evaluate(() => {
    const m4 = {
      pedal: { type: 'rat', engaged: true, params: { distortion: 0.5, filter: 0.5, level: 0.5 } },
      oversampling: 4,
      source: 'test',
    };
    localStorage.setItem('clipper.rig.v1', JSON.stringify(m4));
  });
  await page.reload();

  // Amp panel present with default knob readouts (volume 0.4 -> 40, treble 0.6 -> 60).
  await expect(page.getByTestId('amp')).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Volume' })).toHaveAttribute('aria-valuenow', '40');
  await expect(page.getByRole('slider', { name: 'Treble' })).toHaveAttribute('aria-valuenow', '60');
  // Amp powered by default (migrated engaged = true).
  await expect(page.getByTestId('amp')).toHaveClass(/\bon\b/);
  // Cab on by default.
  await expect(page.getByRole('switch', { name: 'Cab' })).toHaveAttribute('aria-checked', 'true');
  // The migrated rig is what the app now holds.
  const rig = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig());
  expect(rig.amp.params.volume).toBe(0.4);
  expect(rig.amp.engaged).toBe(true);
  // Cab expansion: a rig with no cabModel migrates to the Clean 2×12.
  expect(rig.amp.cabModel).toBe('clean212');
});

// M6.4: the board renders neumorphic SVG cables between the units, and the gear
// tray adds / removes pedals (empty chain works). Cable count tracks the chain.
test('board: cables connect the units; the gear tray adds and removes pedals', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  // Default rig = one pedal. Cables: source->pedal0, pedal0->amp = 2 paths.
  const cables = page.getByTestId('board-cables').locator('path.cable-body');
  await expect(page.getByTestId('board-unit-0')).toBeVisible();
  await expect(cables).toHaveCount(2);

  // Add a pedal via the gear tray.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-rat').click();
  await expect(page.getByTestId('board-unit-1')).toBeVisible();
  // Two pedals -> three cables (source->0->1->amp).
  await expect(cables).toHaveCount(3);
  const count2 = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals.length);
  expect(count2).toBe(2);

  // Remove the first pedal.
  await page.getByTestId('pedal-remove-0').click();
  await expect(page.getByTestId('board-unit-1')).toHaveCount(0);
  await expect(cables).toHaveCount(2);

  // Remove the last pedal -> empty chain (guitar straight into the amp).
  await page.getByTestId('pedal-remove-0').click();
  await expect(page.getByTestId('board-unit-0')).toHaveCount(0);
  await expect(page.getByTestId('empty-chain-note')).toBeVisible();
  // Empty chain -> a single source->amp cable.
  await expect(cables).toHaveCount(1);
  const count0 = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals.length);
  expect(count0).toBe(0);
  // The amp is still there at the end of the chain.
  await expect(page.getByTestId('board-amp')).toBeVisible();
});

// v1.1: the script phaser has its own single-knob FACE (dark chassis, orange
// accent, one big SPEED knob, "Ninety" wordmark). The gear tray adds it, the face
// renders with exactly one knob, and the choice round-trips through a reload.
test('phaser: gear tray adds it, the one-knob face renders, and it persists', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  // Add a phaser from the gear tray.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-phaser').click();

  // A second unit appears; it is a phaser face with the single SPEED knob.
  const phaser = page.getByTestId('board-unit-1').getByTestId('pedal');
  await expect(phaser).toBeVisible();
  await expect(phaser).toHaveAttribute('data-pedal-type', 'phaser');
  await expect(phaser).toHaveAttribute('data-face', 'single');
  await expect(phaser.getByTestId('knob-speed')).toBeVisible();
  // The one-knob face carries exactly ONE knob (no dist/tone/level trio).
  await expect(phaser.locator('.knob')).toHaveCount(1);
  await expect(phaser.getByText('Ninety')).toBeVisible();

  // It is a real phaser in the rig state.
  const type = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().pedals[1].type
  );
  expect(type).toBe('phaser');

  // Persists across a reload (localStorage round-trip).
  await page.reload();
  await expect(page.getByTestId('board')).toBeVisible();
  const persisted = await page.evaluate(
    () => (window as any).__CLIPPER_TEST__.getRig().pedals.map((p: any) => p.type)
  );
  expect(persisted).toEqual(['rat', 'phaser']);
});

// M6.4: keyboard-accessible reorder (the move buttons) changes the chain order in
// the rig state and re-pushes the chain to the worklet.
test('board: move buttons reorder the pedal chain', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board')).toBeVisible();

  // Start with two pedals; make them distinguishable by distortion.
  await page.getByTestId('add-pedal').click();
  await page.getByTestId('add-pedal-rat').click();
  await expect(page.getByTestId('board-unit-1')).toBeVisible();

  // Set pedal 0 dist to 20 (keyboard), leaving pedal 1 at the default 70.
  const dist0 = page.getByTestId('board-unit-0').getByRole('slider', { name: 'Distortion' });
  await dist0.focus();
  for (let i = 0; i < 10; i++) await dist0.press('ArrowDown'); // 70 -> 20

  const before = await page.evaluate(() =>
    (window as any).__CLIPPER_TEST__.getRig().pedals.map((p: any) => Math.round(p.params.distortion * 100))
  );
  expect(before).toEqual([20, 70]);
  // Capture the ids the WORKLET currently holds, so the order can be asserted
  // literally after the move rather than just counted.
  const idsBefore = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastChain);
  expect(idsBefore).toHaveLength(2);
  expect(idsBefore[0]).not.toBe(idsBefore[1]);

  // Move the first pedal later (index 0 -> 1).
  await page.getByTestId('pedal-move-right-0').click();

  const after = await page.evaluate(() =>
    (window as any).__CLIPPER_TEST__.getRig().pedals.map((p: any) => Math.round(p.params.distortion * 100))
  );
  expect(after).toEqual([70, 20]);

  // 2026-07-25 (test/assert-real-properties): this used to assert only
  // `Array.isArray(chainIds)` and `chainIds.length === 2` under a comment reading
  // "ids swapped order" — so the ONE thing the test is named for, the order the
  // WORKLET was told about, went unchecked. A move button that updated the React rig
  // and posted the chain in its original order passed.
  //
  // `__CLIPPER_TEST__.lastChain` is the id list of the last `chain` message posted to
  // the worklet (App.tsx:220), which is the audio thread's actual view of the rig. So
  // assert it literally, against the ids captured BEFORE the move.
  const idsAfter = await page.evaluate(() => (window as any).__CLIPPER_TEST__.lastChain);
  expect(idsAfter).toEqual([idsBefore[1], idsBefore[0]]);
});

// M6.4: an OLD single-`pedal` saved rig (M4..M6.3 shape) migrates into a
// one-element chain on load.
test('rig migration: an old single-pedal rig loads as a one-element chain', async ({ page }) => {
  await page.goto('/');
  await page.evaluate(() => {
    const old = {
      input: { trim: 0.4 },
      pedal: { type: 'rat', engaged: true, params: { distortion: 0.61, filter: 0.3, level: 0.7 } },
      amp: {
        type: 'clean120',
        engaged: true,
        params: { volume: 0.4, bass: 0.5, middle: 0.5, treble: 0.6, bright: 0, cab: 1, speed: 0.3, depth: 0.5, chorusMode: 0 },
      },
      oversampling: 4,
      source: 'test',
    };
    localStorage.setItem('clipper.rig.v1', JSON.stringify(old));
  });
  await page.reload();

  // The single pedal became a one-element chain, params preserved.
  const pedals = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().pedals);
  expect(pedals.length).toBe(1);
  expect(Math.round(pedals[0].params.distortion * 100)).toBe(61);
  expect(typeof pedals[0].id).toBe('string');
  // Exactly one pedal unit rendered.
  await expect(page.getByTestId('board-unit-0')).toBeVisible();
  await expect(page.getByTestId('board-unit-1')).toHaveCount(0);
  await expect(page.getByTestId('board-unit-0').getByRole('slider', { name: 'Distortion' })).toHaveAttribute(
    'aria-valuenow',
    '61'
  );
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
