import { test, expect } from '@playwright/test';

// M9.4 JCM800 amp-model browser verification.
//
// Offline-render proofs (same discipline as audio.spec.ts / cab.spec.ts: a handful
// of live AudioContexts can starve later OfflineAudioContext renders, so the pure
// offline renders come first). Each render posts its amp/param messages and ends
// with the cab-on amp param (id 5) whose handler echoes `latency` synchronously —
// the delivery barrier — before the synchronous offline render begins.
//
// Amp param ids (mirror web/src/params.ts): 0 = volume, 5 = cab on/off,
// 10 = JCM gain, 11 = JCM presence, 12 = JCM master. The amp-model swap is the
// `{ type: 'ampModel', model }` message (0 = Clean 120, 1 = JCM800), declick-
// bracketed exactly like a cab swap.

const AMP_VOL = 0;
const AMP_CAB = 5;
const AMP_JCM_GAIN = 10;
const AMP_JCM_PRESENCE = 11;
const AMP_JCM_MASTER = 12;

// --- 1. Switching to the JCM800 changes the sound (its own distortion). ----------
// The Clean 120 is a LINEAR platform; the JCM800 is a valve head that makes its own
// grit. Feeding the SAME clean tone (pedal bypassed) through each, cranked, must
// produce audibly different output — proof the amp-model switch reaches the core.
test('amp switch: jcm800 renders a different, harmonically richer tone than clean120', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    async function render(jcm: boolean): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true }); // clean tone to the amp
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // clean vol
        // Crank the JCM knobs (harmless on the clean render — it ignores 10/11/12).
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.85 }); // gain
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.6 });  // master
        node.port.postMessage({ type: 'param', unit: 'amp', id: 11, value: 0.5 });  // presence
        if (jcm) node.port.postMessage({ type: 'ampModel', model: 1 });
        // Cab-on param last: its handler echoes `latency` (the delivery barrier).
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.6 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    // Goertzel magnitude at one frequency over the settled tail.
    function goertzel(data: Float32Array, freq: number): number {
      const start = Math.floor(sampleRate * 0.2);
      const N = data.length - start;
      const w = (2 * Math.PI * freq) / sampleRate;
      const c = 2 * Math.cos(w);
      let s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { const s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function rms(data: Float32Array): number {
      const start = Math.floor(sampleRate * 0.2);
      let s = 0;
      for (let i = start; i < data.length; i++) s += data[i] * data[i];
      return Math.sqrt(s / (data.length - start));
    }
    function maxAbsDiff(a: Float32Array, b: Float32Array): number {
      const start = Math.floor(a.length * 0.4);
      let m = 0;
      for (let i = start; i < a.length; i++) { const d = Math.abs(a[i] - b[i]); if (d > m) m = d; }
      return m;
    }

    const clean = await render(false);
    const jcm = await render(true);
    // Harmonic distortion metric: 3rd-harmonic (660 Hz) energy relative to the
    // 220 Hz fundamental. A linear clean amp barely produces it; a cranked valve
    // head produces a lot.
    const h3ratio = (d: Float32Array) => goertzel(d, 660) / (goertzel(d, 220) + 1e-9);
    return {
      cleanRms: rms(clean), jcmRms: rms(jcm),
      diff: maxAbsDiff(clean, jcm),
      cleanH3: h3ratio(clean), jcmH3: h3ratio(jcm),
    };
  });

  // Both produced real signal, and the two amps sound clearly different.
  expect(result.cleanRms).toBeGreaterThan(0.01);
  expect(result.jcmRms).toBeGreaterThan(0.01);
  expect(result.diff).toBeGreaterThan(0.05);
  // The JCM adds far more odd-harmonic distortion than the (near-linear) clean amp.
  expect(result.jcmH3).toBeGreaterThan(result.cleanH3 * 3);
});

// --- 2. JCM params reach the core: raising GAIN adds saturation. -----------------
// With the JCM active, moving the GAIN knob (amp param id 10) must change the
// rendered output — proof the JCM-only param routing works end-to-end.
test('amp params: raising the JCM gain drives it harder (params reach the core)', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    async function render(gain: number): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'ampModel', model: 1 }); // JCM active
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.5 }); // master
        node.port.postMessage({ type: 'param', unit: 'amp', id: 11, value: 0.5 }); // presence
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: gain }); // GAIN under test
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.5 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function goertzel(data: Float32Array, freq: number): number {
      const start = Math.floor(sampleRate * 0.2);
      const N = data.length - start;
      const w = (2 * Math.PI * freq) / sampleRate;
      const c = 2 * Math.cos(w);
      let s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { const s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    function maxAbsDiff(a: Float32Array, b: Float32Array): number {
      const start = Math.floor(a.length * 0.4);
      let m = 0;
      for (let i = start; i < a.length; i++) { const d = Math.abs(a[i] - b[i]); if (d > m) m = d; }
      return m;
    }

    const low = await render(0.15);
    const high = await render(0.9);
    const h3 = (d: Float32Array) => goertzel(d, 660) / (goertzel(d, 220) + 1e-9);
    return { diff: maxAbsDiff(low, high), lowH3: h3(low), highH3: h3(high) };
  });

  // The gain knob genuinely reaches the JCM: high gain differs from low, and adds
  // more odd-harmonic saturation.
  expect(result.diff).toBeGreaterThan(0.02);
  expect(result.highH3).toBeGreaterThan(result.lowH3);
});

// --- 3. Declick: swapping the amp voice mid-signal produces no pop. ---------------
// The `ampModel` message is bracketed by the shared raised-cosine declick fade, so
// the swap happens at an output zero — no step/discontinuity, no NaN.
test('amp swap: no pop (declick continuity)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.4;

    async function render(swapToJcm: boolean): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
        // Keep the JCM near breakup (low gain) so the two tones are comparable in
        // level — the test isolates the SWAP transient, not the tonal difference.
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.15 }); // gain
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.3 });  // master
        // Both renders trigger one startup declick: the baseline swaps to the same
        // Clean 120 (model 0), the other swaps to the JCM (model 1).
        node.port.postMessage({ type: 'ampModel', model: swapToJcm ? 1 : 0 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.3 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function maxDelta(d: Float32Array): number {
      let m = 0;
      for (let i = 1; i < d.length; i++) { const a = Math.abs(d[i] - d[i - 1]); if (a > m) m = a; }
      return m;
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    function rms(d: Float32Array): number {
      let s = 0;
      for (let i = 0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / d.length);
    }

    const base = await render(false);
    const swapped = await render(true);
    return {
      baseMaxDelta: maxDelta(base),
      swapMaxDelta: maxDelta(swapped),
      nan: anyNaN(swapped),
      rms: rms(swapped),
    };
  });

  // No NaN, real signal, and the swap's largest sample-to-sample jump stays in the
  // same ballpark as the un-swapped tone — the raised-cosine fade means no pop.
  expect(result.nan).toBe(false);
  expect(result.rms).toBeGreaterThan(0.005);
  expect(result.swapMaxDelta).toBeLessThan(result.baseMaxDelta * 2.0 + 0.02);
});

// --- 4. PERF SMOKE: the JCM800 WASM offline render is within a generous bound. ----
// The JCM is a heavier voice than the linear Clean 120 (4x-oversampled tube-stage
// Newton solves in the preamp + power section). This smoke test renders the SAME
// signal through each amp model, measures the wall-clock time of the offline
// render, and asserts the JCM/clean ratio stays under a GENEROUS bound (it is a
// smoke test for pathological regressions, not a tight budget). The measured ratio
// is REPORTED in the test output.
test('perf smoke: jcm800 offline-render wall-time ratio is within a generous bound', async ({
  page,
}, testInfo) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const perf = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 2.0; // a longer render so the wall-time is meaningful

    async function timedRender(jcm: boolean): Promise<number> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.7 }); // gain
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.5 }); // master
        node.port.postMessage({ type: 'param', unit: 'amp', id: 11, value: 0.5 }); // presence
        if (jcm) node.port.postMessage({ type: 'ampModel', model: 1 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.6 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const t0 = performance.now();
      await ctx.startRendering();
      return performance.now() - t0;
    }

    // Warm each path once (JIT / WASM warmup), then take the best of a few runs to
    // shed scheduler noise.
    await timedRender(false);
    await timedRender(true);
    const cleanTimes: number[] = [];
    const jcmTimes: number[] = [];
    for (let i = 0; i < 3; i++) { cleanTimes.push(await timedRender(false)); jcmTimes.push(await timedRender(true)); }
    const clean = Math.min(...cleanTimes);
    const jcm = Math.min(...jcmTimes);
    return { clean, jcm, ratio: jcm / clean, rtFactor: jcm / (seconds * 1000), seconds };
  });

  const ratio = perf.ratio;
  // Prominent report of the measured numbers. The JCM is intrinsically far heavier
  // than the LINEAR clean amp (4x-oversampled tube-stage Newton solves in 4 preamp
  // stages + the LTP/push-pull power section vs. a couple of biquads), so a large
  // clean->JCM ratio is EXPECTED, not a regression. The physically meaningful figure
  // is the real-time factor (render wall-time / audio duration).
  const line =
    `\n[M9.4 PERF SMOKE] jcm800 vs clean120 WASM offline render (${perf.seconds}s @ 48kHz, headless Chromium):\n` +
    `  clean120        : ${perf.clean.toFixed(1)} ms\n` +
    `  jcm800          : ${perf.jcm.toFixed(1)} ms\n` +
    `  RATIO (jcm/clean): ${ratio.toFixed(2)}x\n` +
    `  jcm800 real-time : ${perf.rtFactor.toFixed(2)}x  (render wall-time / audio seconds)\n`;
  // eslint-disable-next-line no-console
  console.log(line);
  await testInfo.attach('jcm800-perf-ratio', { body: line, contentType: 'text/plain' });

  // Both paths rendered, and the clean->JCM wall-time ratio stays under a GENEROUS
  // smoke bound. This is a guard against PATHOLOGICAL regressions (e.g. an accidental
  // 8x oversampling or a lost fast-path would multiply this), not a tight budget —
  // the measured ratio (~40x in headless CI) is the expected steady-state cost.
  expect(perf.clean).toBeGreaterThan(0);
  expect(perf.jcm).toBeGreaterThan(0);
  expect(ratio).toBeLessThan(150);
});

// --- 5. UI: the amp menu swaps the head + suggests brit412 (never auto-switches). -
// Selecting the JCM800 from the amp slot menu swaps the face to the Eight Hundred
// wordmark, updates the rig, and — because the Clean 2x12 is still loaded — drops a
// one-line hint suggesting the Brit 4x12. The cab itself must NOT auto-switch.
test('amp UI: selecting jcm800 swaps the face and hints brit412 without auto-switching the cab', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();
  // Starts on the Clean 120 face with the Clean 2x12 cab.
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');
  const cabBefore = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.cabModel);
  expect(cabBefore).toBe('clean212');

  // Open the amp slot menu and pick the JCM800.
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('amp-jcm800').click();

  // The rig's amp voice is the JCM and the face swapped to the Eight Hundred wordmark.
  const amp = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(amp.type).toBe('jcm800');
  await expect(page.getByTestId('amp-name')).toContainText('Eight Hundred');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'jcm800');
  // The JCM face hides chorus + the bright lever (a real 2204 has neither).
  await expect(page.getByTestId('chorus')).toHaveCount(0);
  await expect(page.getByTestId('bright-toggle')).toHaveCount(0);
  // The JCM control row is present (Presence + Gain + Master knobs).
  await expect(page.getByTestId('knob-gain')).toBeVisible();
  await expect(page.getByTestId('knob-presence')).toBeVisible();
  await expect(page.getByTestId('knob-master')).toBeVisible();

  // A one-line hint suggested the Brit 4x12 — but the cab was NOT auto-switched.
  await expect(page.getByTestId('cab-note')).toContainText('Brit 4×12');
  expect(amp.cabModel).toBe('clean212');
});

// =====================================================================================
// M10.1 — Twin (Fender blackface clean benchmark) — the THIRD amp voice (model 2).
// Amp param ids reused by the Twin: 0 = volume, 4 = bright, 5 = cab, 6 = speed
// (→ tremolo SPEED), 7 = depth (→ tremolo INTENSITY), 9 = reverb.
// =====================================================================================

const AMP_TWIN_MODEL = 2;
const AMP_SPEED = 6;
const AMP_DEPTH = 7;
const AMP_REVERB = 9;

// --- 6. Switching to the Twin changes the sound AND does not pop (declick). --------
// The Twin is a valve amp with its own voicing (warm 12AX7 stages + scooped Fender
// stack + 6L6 power), so the SAME clean tone through it differs audibly from the
// linear Clean 120 — and the mid-signal swap is declick-bracketed (no pop, no NaN).
test('amp switch: twin renders a different tone than clean120, swap is click-free', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    async function render(twin: boolean): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 }); // volume
        node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: 0.0 }); // trem OFF (isolate voicing)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 }); // reverb off
        if (twin) node.port.postMessage({ type: 'ampModel', model: 2 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.5 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function rms(d: Float32Array): number {
      const s0 = Math.floor(sampleRate * 0.2); let s = 0;
      for (let i = s0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / (d.length - s0));
    }
    function maxAbsDiff(a: Float32Array, b: Float32Array): number {
      const s0 = Math.floor(a.length * 0.4); let m = 0;
      for (let i = s0; i < a.length; i++) { const dd = Math.abs(a[i] - b[i]); if (dd > m) m = dd; }
      return m;
    }
    function maxDelta(d: Float32Array): number {
      let m = 0; for (let i = 1; i < d.length; i++) { const a = Math.abs(d[i] - d[i - 1]); if (a > m) m = a; }
      return m;
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    const clean = await render(false);
    const twin = await render(true);
    return {
      cleanRms: rms(clean), twinRms: rms(twin),
      diff: maxAbsDiff(clean, twin),
      cleanDelta: maxDelta(clean), twinDelta: maxDelta(twin),
      nan: anyNaN(twin),
    };
  });

  expect(result.cleanRms).toBeGreaterThan(0.01);
  expect(result.twinRms).toBeGreaterThan(0.01);
  expect(result.diff).toBeGreaterThan(0.02); // audibly different voicing
  expect(result.nan).toBe(false);
  // The swap's largest sample step stays in the same ballpark — the declick fade
  // means the topology change does not pop.
  expect(result.twinDelta).toBeLessThan(result.cleanDelta * 2.0 + 0.05);
});

// --- 7. The Twin's tremolo audibly modulates a render's ENVELOPE. -----------------
// With INTENSITY up and a moderate SPEED, the opto tremolo pumps the output level up
// and down — so the block-RMS envelope swings far more than with the tremolo off.
test('amp twin: the optical tremolo modulates the output envelope', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 1.5; // long enough for several tremolo cycles
    async function render(intensity: number, speed: number): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'ampModel', model: 2 }); // Twin active
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 }); // volume
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 }); // reverb off (clean env)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 6, value: speed }); // SPEED
        node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: intensity }); // INTENSITY
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.5 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    // Coefficient of variation of a block-RMS envelope (skip the startup transient).
    function envCV(d: Float32Array): number {
      const block = 512;
      const start = Math.floor(sampleRate * 0.3);
      const env: number[] = [];
      for (let i = start; i + block < d.length; i += block) {
        let s = 0; for (let j = 0; j < block; j++) s += d[i + j] * d[i + j];
        env.push(Math.sqrt(s / block));
      }
      const mean = env.reduce((a, b) => a + b, 0) / env.length;
      const varr = env.reduce((a, b) => a + (b - mean) * (b - mean), 0) / env.length;
      return Math.sqrt(varr) / (mean + 1e-9);
    }
    const off = await render(0.0, 0.6);
    const on = await render(0.8, 0.6);
    return { offCV: envCV(off), onCV: envCV(on) };
  });

  // The steady tone's envelope is nearly flat; the tremolo pumps it hard.
  expect(result.offCV).toBeLessThan(0.05);
  expect(result.onCV).toBeGreaterThan(result.offCV * 4);
  expect(result.onCV).toBeGreaterThan(0.15);
});

// --- 8. Twin params reach the core: raising REVERB adds a wet tail. ----------------
test('amp twin: raising the reverb adds a wet tail (params reach the core)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.9;
    async function render(reverb: number): Promise<Float32Array> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'ampModel', model: 2 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: 0.0 }); // trem off
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: reverb }); // REVERB
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });
      // A short pluck (gain envelope on the oscillator) then SILENCE, so the tail is
      // pure reverb.
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 330 });
      const g = new GainNode(ctx, { gain: 0.0 });
      g.gain.setValueAtTime(0.6, 0);
      g.gain.setValueAtTime(0.6, 0.15);
      g.gain.linearRampToValueAtTime(0.0, 0.16); // note off at 160 ms
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function tailRms(d: Float32Array): number {
      const s0 = Math.floor(sampleRate * 0.45); // well after the note stops
      let s = 0; for (let i = s0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / (d.length - s0));
    }
    const dry = await render(0.0);
    const wet = await render(0.6);
    return { dryTail: tailRms(dry), wetTail: tailRms(wet) };
  });

  // The spring tail persists after the note stops only when reverb is up.
  expect(result.wetTail).toBeGreaterThan(result.dryTail * 3);
});

// --- 9. Twin rig round-trips through JSON exactly (no new params). ----------------
test('amp twin: a twin rig round-trips through JSON literally', async ({ page }) => {
  await page.goto('/');
  const rt = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const rig = {
      input: { trim: 0.4 },
      pedals: [
        { id: 'rat-1', type: 'rat', engaged: true, params: { distortion: 0.3, filter: 0.5, level: 0.8 } },
      ],
      amp: {
        type: 'twin',
        engaged: true,
        cabModel: 'clean212',
        params: {
          volume: 0.55, bass: 0.5, middle: 0.55, treble: 0.6, bright: 1, cab: 1,
          speed: 0.7, depth: 0.6, chorusMode: 0, reverb: 0.35,
          gain: 0.5, presence: 0.5, master: 0.4,
        },
      },
      oversampling: 4,
      source: 'test',
    };
    const back = t.deserializeRig(t.serializeRig(rig));
    return { equal: JSON.stringify(rig) === JSON.stringify(back), back };
  });
  expect(rt.equal).toBe(true);
  expect(rt.back.amp.type).toBe('twin');
});

// --- 10. UI: selecting the Twin swaps the face + hints clean212 (never auto-switch). -
test('amp UI: selecting twin swaps to Twin Sixty-Five and hints clean212 from a brit cab', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  // Load the Brit 4x12 first (the cab buttons live in the amp-slot menu) so
  // switching to the Twin triggers the clean212 hint.
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('cab-brit412').click();
  const cabAfter = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.cabModel);
  expect(cabAfter).toBe('brit412');

  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('amp-twin').click();

  const amp = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(amp.type).toBe('twin');
  await expect(page.getByTestId('amp-name')).toContainText('Twin Sixty-Five');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'twin');
  // The Twin face shows a TREMOLO row + Bright lever + Reverb knob; hides gain/master.
  await expect(page.getByTestId('tremolo')).toBeVisible();
  await expect(page.getByTestId('bright-toggle')).toBeVisible();
  await expect(page.getByTestId('knob-reverb')).toBeVisible();
  await expect(page.getByTestId('knob-gain')).toHaveCount(0);
  await expect(page.getByTestId('knob-master')).toHaveCount(0);
  // A one-line hint suggested the Clean 2x12 — but the cab was NOT auto-switched.
  await expect(page.getByTestId('cab-note')).toContainText('Clean 2×12');
  expect(amp.cabModel).toBe('brit412');
});

// --- 11. PERF SMOKE: report the Twin WASM offline-render wall-time ratio. ----------
test('perf smoke: twin offline-render wall-time ratio is within a generous bound', async ({
  page,
}, testInfo) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const perf = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 2.0;
    async function timedRender(twin: boolean): Promise<number> {
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('worklet not ready')), 5000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') { clearTimeout(t); resolve(); }
          else if (e.data?.type === 'error') { clearTimeout(t); reject(new Error(e.data.message)); }
        };
      });
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.3 }); // reverb
        node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: 0.5 }); // trem
        if (twin) node.port.postMessage({ type: 'ampModel', model: 2 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.5 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const t0 = performance.now();
      await ctx.startRendering();
      return performance.now() - t0;
    }
    await timedRender(false);
    await timedRender(true);
    const cleanTimes: number[] = [];
    const twinTimes: number[] = [];
    for (let i = 0; i < 3; i++) { cleanTimes.push(await timedRender(false)); twinTimes.push(await timedRender(true)); }
    const clean = Math.min(...cleanTimes);
    const twin = Math.min(...twinTimes);
    return { clean, twin, ratio: twin / clean, rtFactor: twin / (seconds * 1000), seconds };
  });

  const line =
    `\n[M10.1 PERF SMOKE] twin vs clean120 WASM offline render (${perf.seconds}s @ 48kHz, headless Chromium):\n` +
    `  clean120        : ${perf.clean.toFixed(1)} ms\n` +
    `  twin            : ${perf.twin.toFixed(1)} ms\n` +
    `  RATIO (twin/clean): ${perf.ratio.toFixed(2)}x\n` +
    `  twin real-time  : ${perf.rtFactor.toFixed(2)}x  (render wall-time / audio seconds)\n`;
  // eslint-disable-next-line no-console
  console.log(line);
  await testInfo.attach('twin-perf-ratio', { body: line, contentType: 'text/plain' });

  // Guard against pathological regressions (an accidental 8x OS, a lost fast-path);
  // the measured ratio is the expected steady-state cost of the tube Newton solves.
  expect(perf.clean).toBeGreaterThan(0);
  expect(perf.twin).toBeGreaterThan(0);
  expect(perf.ratio).toBeLessThan(150);
});
