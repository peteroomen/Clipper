import { test, expect } from '@playwright/test';

// M3 browser verification. Proves the RAT model runs through the WASM worklet
// (no speakers/mic) in an OfflineAudioContext:
//  a. High-distortion 220 Hz sine -> odd harmonics (660 Hz) present, even
//     harmonics (440 Hz) ~absent (symmetric diode clipping); LEVEL scales RMS.
//  b. Bypass passes the input through ~untouched (no added harmonics).
// A small in-page Goertzel measures single-bin magnitudes (no FFT needed).

const RENDER_SECONDS = 0.5;

test('UI exposes M3 controls: start, source, three knobs, bypass, oversampling', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('button', { name: 'Start audio' })).toBeVisible();
  await expect(page.getByRole('combobox', { name: 'Source' })).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Distortion' })).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Filter' })).toBeVisible();
  await expect(page.getByRole('slider', { name: 'Level' })).toBeVisible();
  await expect(page.getByRole('checkbox')).toBeVisible(); // Bypass
  await expect(page.getByRole('combobox', { name: 'Oversampling' })).toBeVisible();

  // Knob sliders are 0..100 display range.
  const dist = page.getByRole('slider', { name: 'Distortion' });
  await expect(dist).toHaveAttribute('min', '0');
  await expect(dist).toHaveAttribute('max', '100');
});

test('RAT worklet: high distortion yields odd harmonics; LEVEL scales RMS', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper — M3' })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    const sampleRate = 48000;

    // Render the 220 Hz oscillator through the RAT worklet with the given
    // param/config messages. Returns the mono output as a Float32Array.
    async function render(
      messages: Array<Record<string, unknown>>
    ): Promise<Float32Array> {
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

    // Single-frequency magnitude via Goertzel over the steady-state region.
    function goertzel(data: Float32Array, sr: number, freq: number): number {
      const start = Math.floor(sr * 0.1); // skip smoothing ramp
      const N = data.length - start;
      const k = (freq / sr) * N;
      const w = (2 * Math.PI * k) / N;
      const c = 2 * Math.cos(w);
      let s0 = 0, s1 = 0, s2 = 0;
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
      let sum = 0, count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    // High distortion, bright filter (harmonics preserved), default 4x OS.
    const driven = await render([
      { type: 'param', id: 0, value: 0.9 }, // distortion
      { type: 'param', id: 1, value: 0.0 }, // filter (bright)
      { type: 'param', id: 2, value: 1.0 }, // level
    ]);
    // Same, but half level -> RMS should halve (LEVEL is a clean linear gain).
    const halfLevel = await render([
      { type: 'param', id: 0, value: 0.9 },
      { type: 'param', id: 1, value: 0.0 },
      { type: 'param', id: 2, value: 0.5 },
    ]);

    const f1 = goertzel(driven, sampleRate, 220); // fundamental
    const h2 = goertzel(driven, sampleRate, 440); // 2nd (even) — should be tiny
    const h3 = goertzel(driven, sampleRate, 660); // 3rd (odd)  — should be strong

    return {
      f1, h2, h3,
      rmsFull: rms(driven, sampleRate),
      rmsHalf: rms(halfLevel, sampleRate),
    };
  }, RENDER_SECONDS);

  // Odd harmonic present and well above the even harmonic (symmetric clipping).
  expect(result.h3).toBeGreaterThan(0.01);
  expect(result.h3).toBeGreaterThan(result.h2 * 8);
  // Even harmonic sits near the floor relative to the fundamental.
  expect(result.h2).toBeLessThan(result.f1 * 0.1);

  // LEVEL 1.0 vs 0.5 halves RMS (clean linear output gain).
  const ratio = result.rmsFull / result.rmsHalf;
  expect(ratio).toBeGreaterThan(1.8);
  expect(ratio).toBeLessThan(2.2);
});

test('RAT worklet: bypass passes input through untouched', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper — M3' })).toBeVisible();

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
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      // Drive hard, but bypass should ignore all of it and pass input through.
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
      let s1 = 0, s2 = 0, s0 = 0;
      for (let i = start; i < data.length; i++) { s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.1);
      let sum = 0, count = 0;
      for (let i = start; i < data.length; i++) { sum += data[i] * data[i]; count++; }
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

  // Bypass output ~= the raw 220 Hz oscillator (amplitude 1.0 -> RMS ~0.707).
  expect(result.bypassRms).toBeGreaterThan(0.6);
  expect(result.bypassRms).toBeLessThan(0.8);
  // A pure passed-through sine has essentially no 3rd harmonic...
  expect(result.bypassH3).toBeLessThan(result.bypassF1 * 0.02);
  // ...whereas the processed (non-bypass) path clearly does.
  expect(result.processedH3).toBeGreaterThan(result.bypassH3 * 10);
});

// Live-input smoke test. Relies on Chromium's fake media device flags (set in
// playwright.config.ts): --use-fake-device-for-media-stream provides a synthetic
// audio input and --use-fake-ui-for-media-stream auto-accepts the permission
// prompt. Verifies the getUserMedia path builds the graph and the AudioContext
// reaches 'running' with no console errors. If this ever proves flaky on a given
// runner, it is safe to skip — the offline tests already cover the DSP; this
// only exercises the live plumbing.
test('live input: getUserMedia path reaches running with no errors', async ({ page }) => {
  const errors: string[] = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') errors.push(msg.text());
  });
  page.on('pageerror', (err) => errors.push(String(err)));

  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper — M3' })).toBeVisible();

  // Choose the live input source, then start.
  await page.getByRole('combobox', { name: 'Source' }).selectOption('live');
  await expect(page.getByTestId('feedback-hint')).toBeVisible();
  await page.getByRole('button', { name: 'Start audio' }).click();

  // The engine resumes the context; status should settle on 'running'.
  await expect(page.getByTestId('status')).toHaveText('running', { timeout: 8000 });

  // Latency readout is populated once running (a real, non-offline context).
  await expect(page.getByText(/Latency \(model\):/)).toBeVisible();

  expect(errors, `console errors: ${errors.join(' | ')}`).toEqual([]);
});
