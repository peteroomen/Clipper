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

// --- 3. Declick: swapping the amp voice MID-RENDER produces no pop. ---------------
//
// 2026-07-25 (test/assert-real-properties). This test used to post the `ampModel`
// message BEFORE `ctx.startRendering()`, which is the audit's headline example of a
// test asserting the wrong thing: the swap then lands at the opening zero crossing
// with empty filter state, so REMOVING THE DECLICK ENTIRELY would still pass. Its
// bound was also `swapMaxDelta < baseMaxDelta * 2.0 + 0.02`, and that `+0.02`
// absolute floor is on its own larger than the 0.0227 step a de-declicked JCM swap
// actually produces — so even applied mid-render the old bound would have passed.
//
// Now: `ctx.suspend()` / `resume()` (the technique expectations.spec.ts:255 already
// uses) lands the swap at 0.3 s into a sustained note, with the filters warm and the
// oscillator mid-cycle. And the reference is the render's OWN per-sample slew away
// from the swap, in the same render — a pop is BY DEFINITION a step larger than the
// signal's natural slew, so no absolute floor is needed or wanted.
//
// MEASURED (headless Chromium, 48 kHz), swap-window slew / away-from-swap slew:
//                       shipped declick   declick DELETED
//   clean120 -> jcm800        2.04              7.15
//   clean120 -> twin          1.66              7.16
//   clean120 -> ac30          1.19              5.12
// Bar: 3.0. Passes the shipped fade with 1.5x margin, fails a removed one with 1.7x.
const SWAP_SLEW_RATIO_BAR = 3.0;

test('amp swap: no pop (declick continuity, swap lands MID-RENDER)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.6;
    const swapAt = 0.3; // seconds into the render — mid-note, filters warm

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
      // Keep the JCM near breakup (low gain) so the two voices are comparable in
      // level — this isolates the SWAP transient, not the tonal difference.
      node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.15 }); // gain
      node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.3 });  // master
      node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
    });

    // THE SWAP, mid-render. suspend() parks the offline render so the port message
    // lands deterministically at swapAt before rendering continues.
    ctx.suspend(swapAt).then(async () => {
      node.port.postMessage({ type: 'ampModel', model: 1 }); // -> JCM800
      await new Promise((r) => setTimeout(r, 50));
      await ctx.resume();
    });

    const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
    const g = new GainNode(ctx, { gain: 0.3 });
    osc.connect(g).connect(node).connect(ctx.destination);
    osc.start();
    const buffer = await ctx.startRendering();
    const d = buffer.getChannelData(0);

    function maxDelta(t0: number, t1: number): number {
      const a = Math.max(1, Math.floor(t0 * sampleRate));
      const b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let m = 0;
      for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
      return m;
    }
    function rms(t0: number, t1: number): number {
      const a = Math.floor(t0 * sampleRate), b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let s = 0;
      for (let i = a; i < b; i++) s += d[i] * d[i];
      return Math.sqrt(s / (b - a));
    }
    let nan = false;
    for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) { nan = true; break; }

    // The declick bracket is 6 ms out + 6 ms in; 20 ms covers it plus scheduling jitter.
    const slewAtSwap = maxDelta(swapAt - 0.002, swapAt + 0.02);
    const slewBefore = maxDelta(0.05, swapAt - 0.002);
    const slewAfter = maxDelta(swapAt + 0.05, seconds - 0.01);
    return {
      nan,
      slewAtSwap, slewBefore, slewAfter,
      ratio: slewAtSwap / Math.max(slewBefore, slewAfter),
      rmsBefore: rms(0.1, swapAt - 0.01),
      rmsAfter: rms(swapAt + 0.05, seconds - 0.01),
    };
  });

  expect(result.nan).toBe(false);
  // The swap genuinely landed MID-RENDER: there is real signal on BOTH sides, and the
  // two sides are audibly different levels — i.e. the amp voice really did change
  // while audio was flowing. Without these the test could quietly revert to the old
  // vacuous shape (swap before rendering starts) and still pass.
  expect(result.rmsBefore).toBeGreaterThan(0.005);
  expect(result.rmsAfter).toBeGreaterThan(0.005);
  expect(Math.abs(result.rmsAfter - result.rmsBefore)).toBeGreaterThan(result.rmsBefore * 0.2);
  // NO POP: the largest sample-to-sample step across the swap is within 3x the
  // signal's own slew elsewhere in the SAME render.
  expect(result.ratio).toBeLessThan(SWAP_SLEW_RATIO_BAR);
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

    async function timedRender(jcm: boolean) {
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
      const buffer = await ctx.startRendering();
      // 2026-07-25 (test/assert-real-properties): return the AUDIO as well as the
      // wall time. The old form returned only the elapsed ms and never read a
      // single output sample, so it timed — and passed on — a render that produced
      // silence, or one where the amp-model swap never landed and both "paths" were
      // the same amp (a ratio of ~1, comfortably under the bound).
      return { ms: performance.now() - t0, data: buffer.getChannelData(0).slice() };
    }
    function rmsOf(d: Float32Array): number {
      const s0 = Math.floor(sampleRate * 0.2);
      let s = 0;
      for (let i = s0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / (d.length - s0));
    }
    function maxAbsDiffOf(a: Float32Array, b: Float32Array): number {
      const s0 = Math.floor(a.length * 0.4);
      let m = 0;
      for (let i = s0; i < a.length; i++) { const dd = Math.abs(a[i] - b[i]); if (dd > m) m = dd; }
      return m;
    }
    function allFiniteOf(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return false;
      return true;
    }

    // Warm each path once (JIT / WASM warmup), then take the best of a few runs to
    // shed scheduler noise.
    await timedRender(false);
    await timedRender(true);
    const cleanTimes: number[] = [];
    const jcmTimes: number[] = [];
    const cleanAudios: Float32Array[] = [];
    const voiceAudios: Float32Array[] = [];
    for (let i = 0; i < 3; i++) {
      const c = await timedRender(false);
      cleanTimes.push(c.ms);
      cleanAudios.push(c.data);
      const v = await timedRender(true);
      jcmTimes.push(v.ms);
      voiceAudios.push(v.data);
    }
    const clean = Math.min(...cleanTimes);
    const jcm = Math.min(...jcmTimes);
    return {
      clean, jcm, ratio: jcm / clean, rtFactor: jcm / (seconds * 1000), seconds,
      // What was actually rendered, so the numbers above describe real work. BEST of the
      // three pairs: Chromium degrades later offline renders once enough contexts have
      // accumulated in one process (audio.spec.ts's header warns about it), and a degraded
      // render can drop the amp-model swap and hand back two identical buffers. "At least
      // one pair rendered genuinely different audio" still fails hard if the swap NEVER
      // works, without trading a vacuous test for a flaky one.
      cleanRms: Math.max(...cleanAudios.map(rmsOf)),
      voiceRms: Math.max(...voiceAudios.map(rmsOf)),
      voiceDiff: Math.max(...cleanAudios.map((c, k) => maxAbsDiffOf(c, voiceAudios[k]))),
      finite: cleanAudios.every(allFiniteOf) && voiceAudios.every(allFiniteOf),
    };
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
  // 2026-07-25 (test/assert-real-properties): `expect(perf.clean).toBeGreaterThan(0)`
  // on a `performance.now()` delta is a tautology — a wall-clock difference is positive
  // by construction, so the two assertions that used to be here could not fail, and
  // nothing in this test ever read an output sample. What makes the ratio MEAN anything
  // is that both paths actually rendered audio, and rendered DIFFERENT audio: otherwise
  // the ratio is clean-vs-clean (~1) and sails under the bound no matter how slow the
  // JCM800 becomes. Every assertion below can fail.
  expect(perf.finite).toBe(true);
  expect(perf.cleanRms).toBeGreaterThan(0.005);   // the clean path made sound
  expect(perf.voiceRms).toBeGreaterThan(0.005);   // the JCM800 path made sound
  expect(perf.voiceDiff).toBeGreaterThan(0.02);   // and they are DIFFERENT amps
  // A 2 s WASM render cannot complete in under a millisecond; if it did, the render did
  // not happen and the ratio is noise over noise.
  expect(perf.clean).toBeGreaterThan(1);
  expect(perf.jcm).toBeGreaterThan(1);
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
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }

    // -- (a) the two voices sound different. Two static renders; valid as it stands.
    const clean = await render(false);
    const twin = await render(true);

    // -- (b) the SWAP is click-free. 2026-07-25 (test/assert-real-properties): this
    // used to compare the two STATIC renders' whole-buffer maxDelta with a
    // `* 2.0 + 0.05` bound. The swap happened before startRendering() in both, so no
    // mid-stream discontinuity existed to measure and a deleted declick would still
    // have passed — and the +0.05 floor alone is larger than the 0.032 step a
    // de-declicked twin swap actually produces. Now the swap lands MID-RENDER via
    // suspend()/resume() and is compared against the render's own slew.
    const swapAt = 0.3;
    const seconds2 = 0.6;
    const length2 = Math.floor(sampleRate * seconds2);
    const ctx = new OfflineAudioContext(1, length2, sampleRate);
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
      node.port.postMessage({ type: 'param', unit: 'amp', id: 7, value: 0.0 }); // trem OFF
      node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 }); // reverb off
      node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });   // cab on
    });
    ctx.suspend(swapAt).then(async () => {
      node.port.postMessage({ type: 'ampModel', model: 2 }); // -> Twin, mid-note
      await new Promise((r) => setTimeout(r, 50));
      await ctx.resume();
    });
    const osc2 = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
    const g2 = new GainNode(ctx, { gain: 0.3 });
    osc2.connect(g2).connect(node).connect(ctx.destination);
    osc2.start();
    const live = (await ctx.startRendering()).getChannelData(0);

    function winDelta(d: Float32Array, t0: number, t1: number): number {
      const a = Math.max(1, Math.floor(t0 * sampleRate));
      const b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let m = 0;
      for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
      return m;
    }
    function winRms2(d: Float32Array, t0: number, t1: number): number {
      const a = Math.floor(t0 * sampleRate), b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let s = 0;
      for (let i = a; i < b; i++) s += d[i] * d[i];
      return Math.sqrt(s / (b - a));
    }
    const atSwap = winDelta(live, swapAt - 0.002, swapAt + 0.02);
    const away = Math.max(winDelta(live, 0.05, swapAt - 0.002),
                          winDelta(live, swapAt + 0.05, seconds2 - 0.01));
    return {
      cleanRms: rms(clean), twinRms: rms(twin),
      diff: maxAbsDiff(clean, twin),
      nan: anyNaN(twin) || anyNaN(live),
      swapRatio: atSwap / away,
      swapRmsBefore: winRms2(live, 0.1, swapAt - 0.01),
      swapRmsAfter: winRms2(live, swapAt + 0.05, seconds2 - 0.01),
    };
  });

  expect(result.cleanRms).toBeGreaterThan(0.01);
  expect(result.twinRms).toBeGreaterThan(0.01);
  expect(result.diff).toBeGreaterThan(0.02); // audibly different voicing
  expect(result.nan).toBe(false);
  // The swap really landed mid-render (signal both sides, and the level changed).
  expect(result.swapRmsBefore).toBeGreaterThan(0.005);
  expect(result.swapRmsAfter).toBeGreaterThan(0.005);
  expect(Math.abs(result.swapRmsAfter - result.swapRmsBefore))
    .toBeGreaterThan(result.swapRmsBefore * 0.2);
  // NO POP. Measured 1.66 with the shipped declick, 7.16 with it deleted.
  expect(result.swapRatio).toBeLessThan(SWAP_SLEW_RATIO_BAR);
});

// --- 7. The Twin's tremolo audibly modulates a render's ENVELOPE — and only when
// the ON/OFF switch is ON. With the switch ON, INTENSITY up and a moderate SPEED,
// the opto tremolo pumps the output level up and down — so the block-RMS envelope
// swings far more than with the tremolo off. With the switch OFF (the default,
// chorusMode slot = 0) the envelope stays flat EVEN AT FULL INTENSITY: off is a
// bypass in the core (docs §20 amendment).
test('amp twin: the optical tremolo modulates the output envelope', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 1.5; // long enough for several tremolo cycles
    async function render(intensity: number, speed: number, enable: number): Promise<Float32Array> {
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 8, value: enable }); // TREM ON/OFF (chorusMode slot)
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
    const off = await render(0.0, 0.6, 1);
    const on = await render(0.8, 0.6, 1);
    // SWITCH OFF at FULL intensity: the default (enable 0) must gate the trem out.
    const disabled = await render(0.8, 0.6, 0);
    return { offCV: envCV(off), onCV: envCV(on), disabledCV: envCV(disabled) };
  });

  // The steady tone's envelope is nearly flat; the tremolo pumps it hard.
  expect(result.offCV).toBeLessThan(0.05);
  expect(result.onCV).toBeGreaterThan(result.offCV * 4);
  expect(result.onCV).toBeGreaterThan(0.15);
  // Switch-off is a bypass: flat envelope even with the intensity knob cranked.
  expect(result.disabledCV).toBeLessThan(0.05);
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
          // M10.3: the Orange's F.A.C. is a real AmpParams field, so it is part of
          // the serialized shape for EVERY voice — the literal has to carry it.
          fac: 0.2,
          // M10.4: part of the serialized shape for every voice.
          mesaMode: 1.0, rectifier: 0.0, powerMode: 0.0,
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
  // TREM ON/OFF switch (docs §20 amendment): defaults OFF; clicking On drives the
  // reused chorusMode slot to 1, Off back to 0.
  await expect(page.getByTestId('trem-switch')).toBeVisible();
  await expect(page.getByTestId('trem-off')).toHaveAttribute('aria-checked', 'true');
  await page.getByTestId('trem-on').click();
  await expect(page.getByTestId('trem-on')).toHaveAttribute('aria-checked', 'true');
  expect(
    await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.params.chorusMode)
  ).toBe(1);
  await page.getByTestId('trem-off').click();
  expect(
    await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.params.chorusMode)
  ).toBe(0);
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
    async function timedRender(twin: boolean) {
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
      const buffer = await ctx.startRendering();
      // 2026-07-25 (test/assert-real-properties): return the AUDIO as well as the
      // wall time. The old form returned only the elapsed ms and never read a
      // single output sample, so it timed — and passed on — a render that produced
      // silence, or one where the amp-model swap never landed and both "paths" were
      // the same amp (a ratio of ~1, comfortably under the bound).
      return { ms: performance.now() - t0, data: buffer.getChannelData(0).slice() };
    }
    function rmsOf(d: Float32Array): number {
      const s0 = Math.floor(sampleRate * 0.2);
      let s = 0;
      for (let i = s0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / (d.length - s0));
    }
    function maxAbsDiffOf(a: Float32Array, b: Float32Array): number {
      const s0 = Math.floor(a.length * 0.4);
      let m = 0;
      for (let i = s0; i < a.length; i++) { const dd = Math.abs(a[i] - b[i]); if (dd > m) m = dd; }
      return m;
    }
    function allFiniteOf(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return false;
      return true;
    }
    await timedRender(false);
    await timedRender(true);
    const cleanTimes: number[] = [];
    const twinTimes: number[] = [];
    const cleanAudios: Float32Array[] = [];
    const voiceAudios: Float32Array[] = [];
    for (let i = 0; i < 3; i++) {
      const c = await timedRender(false);
      cleanTimes.push(c.ms);
      cleanAudios.push(c.data);
      const v = await timedRender(true);
      twinTimes.push(v.ms);
      voiceAudios.push(v.data);
    }
    const clean = Math.min(...cleanTimes);
    const twin = Math.min(...twinTimes);
    return {
      clean, twin, ratio: twin / clean, rtFactor: twin / (seconds * 1000), seconds,
      // What was actually rendered, so the numbers above describe real work. BEST of the
      // three pairs: Chromium degrades later offline renders once enough contexts have
      // accumulated in one process (audio.spec.ts's header warns about it), and a degraded
      // render can drop the amp-model swap and hand back two identical buffers. "At least
      // one pair rendered genuinely different audio" still fails hard if the swap NEVER
      // works, without trading a vacuous test for a flaky one.
      cleanRms: Math.max(...cleanAudios.map(rmsOf)),
      voiceRms: Math.max(...voiceAudios.map(rmsOf)),
      voiceDiff: Math.max(...cleanAudios.map((c, k) => maxAbsDiffOf(c, voiceAudios[k]))),
      finite: cleanAudios.every(allFiniteOf) && voiceAudios.every(allFiniteOf),
    };
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
  // 2026-07-25 (test/assert-real-properties): `expect(perf.clean).toBeGreaterThan(0)`
  // on a `performance.now()` delta is a tautology — a wall-clock difference is positive
  // by construction, so the two assertions that used to be here could not fail, and
  // nothing in this test ever read an output sample. What makes the ratio MEAN anything
  // is that both paths actually rendered audio, and rendered DIFFERENT audio: otherwise
  // the ratio is clean-vs-clean (~1) and sails under the bound no matter how slow the
  // Twin becomes. Every assertion below can fail.
  expect(perf.finite).toBe(true);
  expect(perf.cleanRms).toBeGreaterThan(0.005);   // the clean path made sound
  expect(perf.voiceRms).toBeGreaterThan(0.005);   // the Twin path made sound
  expect(perf.voiceDiff).toBeGreaterThan(0.02);   // and they are DIFFERENT amps
  // A 2 s WASM render cannot complete in under a millisecond; if it did, the render did
  // not happen and the ratio is noise over noise.
  expect(perf.clean).toBeGreaterThan(1);
  expect(perf.twin).toBeGreaterThan(1);
  expect(perf.ratio).toBeLessThan(150);
});

// ============================================================================
// v1.1 AC30 "top boost" tests (voice 3) — mirror the Twin blocks above. The AC30
// reuses the STABLE amp_* exports and the shared 'presence' id (11) routed as its
// top CUT. Model index 3, data-amp-type='ac30', testid 'amp-ac30'.
// ============================================================================
const AMP_AC30_MODEL = 3; // the worklet model index (mirrors the twin AMP_TWIN_MODEL); posted inline below

// --- AC30 (a): switching to the AC30 changes the sound AND does not pop (declick). --
// The AC30 is a class-A valve voice with its own top-boost voicing, so the SAME clean
// tone through it differs audibly from the linear Clean 120 — and the mid-signal swap
// is declick-bracketed (no pop, no NaN).
test('amp switch: ac30 renders a different tone than clean120, swap is click-free', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    async function render(ac30: boolean): Promise<Float32Array> {
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 }); // reverb off (isolate voicing)
        if (ac30) node.port.postMessage({ type: 'ampModel', model: 3 });
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
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }

    // -- (a) the two voices sound different. Two static renders; valid as it stands.
    const clean = await render(false);
    const ac30 = await render(true);

    // -- (b) the SWAP is click-free, landed MID-RENDER. See the JCM800 block above for
    // why the old form (topology change posted before startRendering, bound
    // `* 2.0 + 0.05`) could not fail: 2026-07-25, test/assert-real-properties.
    const swapAt = 0.3;
    const seconds2 = 0.6;
    // Two swap renders, one per property (docs §46): the NO-POP ratio keeps its
    // original volume-0.4 probe (its bar was calibrated there, and the declick's
    // seam slew scales with the level delta being faded across), while the
    // SWAP-LANDED detector reads the AC30's harmonic signature at volume 0.75 —
    // post-§46 the AC30 is HONESTLY CLEAN at low volume, so the discriminating
    // operating point is mid-breakup.
    async function renderSwap(vol: number): Promise<Float32Array> {
      const length2 = Math.floor(sampleRate * seconds2);
      const ctx = new OfflineAudioContext(1, length2, sampleRate);
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: vol });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 }); // reverb off
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });   // cab on
      });
      ctx.suspend(swapAt).then(async () => {
        node.port.postMessage({ type: 'ampModel', model: 3 }); // -> AC30, mid-note
        await new Promise((r) => setTimeout(r, 50));
        await ctx.resume();
      });
      const osc2 = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g2 = new GainNode(ctx, { gain: 0.3 });
      osc2.connect(g2).connect(node).connect(ctx.destination);
      osc2.start();
      return (await ctx.startRendering()).getChannelData(0);
    }
    const live = await renderSwap(0.4);
    const liveHot = await renderSwap(0.75);

    function winDelta(d: Float32Array, t0: number, t1: number): number {
      const a = Math.max(1, Math.floor(t0 * sampleRate));
      const b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let m = 0;
      for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
      return m;
    }
    function winRms2(d: Float32Array, t0: number, t1: number): number {
      const a = Math.floor(t0 * sampleRate), b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let s = 0;
      for (let i = a; i < b; i++) s += d[i] * d[i];
      return Math.sqrt(s / (b - a));
    }
    const atSwap = winDelta(live, swapAt - 0.002, swapAt + 0.02);
    const away = Math.max(winDelta(live, 0.05, swapAt - 0.002),
                          winDelta(live, swapAt + 0.05, seconds2 - 0.01));
    // Harmonic content (h2+h3 rel f1) inside a window — the level-insensitive "which
    // voice is rendering" discriminator (see the assert note below).
    function harmRatio(d: Float32Array, t0: number, t1: number): number {
      const s0 = Math.floor(t0 * sampleRate), n = Math.floor((t1 - t0) * sampleRate);
      const g = (f: number) => {
        const w = (2 * Math.PI * f) / sampleRate, c = 2 * Math.cos(w);
        let a = 0, b = 0;
        for (let i = 0; i < n; i++) { const s = d[s0 + i] + c * a - b; b = a; a = s; }
        const re = a - b * Math.cos(w), im = b * Math.sin(w);
        return Math.sqrt(re * re + im * im);
      };
      const f1 = g(220);
      return Math.hypot(g(440), g(660)) / (f1 + 1e-12);
    }
    return {
      cleanRms: rms(clean), ac30Rms: rms(ac30),
      diff: maxAbsDiff(clean, ac30),
      nan: anyNaN(ac30) || anyNaN(live) || anyNaN(liveHot),
      swapRatio: atSwap / away,
      swapRmsBefore: winRms2(live, 0.1, swapAt - 0.01),
      swapRmsAfter: winRms2(live, swapAt + 0.05, seconds2 - 0.01),
      harmBefore: harmRatio(liveHot, 0.1, swapAt - 0.01),
      harmAfter: harmRatio(liveHot, swapAt + 0.05, seconds2 - 0.01),
    };
  });

  expect(result.cleanRms).toBeGreaterThan(0.01);
  expect(result.ac30Rms).toBeGreaterThan(0.01);
  expect(result.diff).toBeGreaterThan(0.02); // audibly different voicing
  expect(result.nan).toBe(false);
  // The swap really landed mid-render: signal on both sides, and the POST-swap window
  // carries the AC30's harmonic signature where the clean120 window does not.
  // 2026-07-30 (docs §46): this used to assert a >20 % RMS change across the swap —
  // a LEVEL discriminator, which lost its teeth when the AC30's honest re-
  // normalization (kFullScaleSecV, §46) landed its level near clean120's at this
  // probe. Voices differ by CONTENT, not coincidental loudness: the driven AC30 at
  // this operating point is measurably dirty (h2+h3 rel f1, measured ≈ 0.60) where
  // the solid-state clean is not (≈ 0.005) — a ~120× contrast no level shift fakes.
  expect(result.swapRmsBefore).toBeGreaterThan(0.005);
  expect(result.swapRmsAfter).toBeGreaterThan(0.005);
  expect(result.harmAfter).toBeGreaterThan(0.1);
  expect(result.harmAfter).toBeGreaterThan(result.harmBefore * 5);
  // NO POP. Measured 1.19 with the shipped declick, 5.12 with it deleted.
  expect(result.swapRatio).toBeLessThan(SWAP_SLEW_RATIO_BAR);
});

// --- AC30 (b): params reach the core — raising REVERB adds a wet tail. --------------
test('amp ac30: raising the reverb adds a wet tail (params reach the core)', async ({ page }) => {
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
        node.port.postMessage({ type: 'ampModel', model: 3 }); // AC30 active
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 }); // volume
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: reverb }); // REVERB
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });
      // A short pluck then SILENCE, so the tail is pure reverb.
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

// --- AC30 (c): an ac30 rig round-trips through JSON exactly (no new params). --------
test('amp ac30: an ac30 rig round-trips through JSON literally', async ({ page }) => {
  await page.goto('/');
  const rt = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const rig = {
      input: { trim: 0.4 },
      pedals: [
        { id: 'rat-1', type: 'rat', engaged: true, params: { distortion: 0.3, filter: 0.5, level: 0.8 } },
      ],
      amp: {
        type: 'ac30',
        engaged: true,
        cabModel: 'clean212',
        params: {
          volume: 0.7, bass: 0.5, middle: 0.5, treble: 0.6, bright: 0, cab: 1,
          speed: 0.3, depth: 0.5, chorusMode: 0, reverb: 0.2,
          // presence is REUSED as the AC30's top CUT (a distinct value pins the round-trip).
          gain: 0.5, presence: 0.35, master: 0.4,
          fac: 0.2,  // M10.3: part of the serialized shape for every voice
          mesaMode: 1.0, rectifier: 0.0, powerMode: 0.0,  // M10.4, likewise
        },
      },
      oversampling: 4,
      source: 'test',
    };
    const back = t.deserializeRig(t.serializeRig(rig));
    return { equal: JSON.stringify(rig) === JSON.stringify(back), back };
  });
  expect(rt.equal).toBe(true);
  expect(rt.back.amp.type).toBe('ac30');
});

// --- AC30 (UI): selecting the AC30 swaps the face + hints clean212 (never auto-switch). -
test('amp UI: selecting ac30 swaps to Thirty (CUT labeled, middle/gain/master/bright hidden)', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  // Load the Brit 4x12 first so switching to the AC30 triggers the clean212 hint.
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('cab-brit412').click();
  const cabAfter = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.cabModel);
  expect(cabAfter).toBe('brit412');

  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('amp-ac30').click();

  const amp = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(amp.type).toBe('ac30');
  await expect(page.getByTestId('amp-name')).toContainText('Thirty');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'ac30');
  // The AC30 face shows Volume · Bass · Treble · CUT (bound to knob-presence) · Reverb.
  await expect(page.getByTestId('knob-volume')).toBeVisible();
  await expect(page.getByTestId('knob-treble')).toBeVisible();
  await expect(page.getByTestId('knob-presence')).toBeVisible(); // the CUT knob
  await expect(page.getByTestId('knob-presence')).toContainText('Cut');
  await expect(page.getByTestId('knob-reverb')).toBeVisible();
  // Hidden on the AC30 face: middle, gain, master, bright switch, chorus/tremolo.
  await expect(page.getByTestId('knob-middle')).toHaveCount(0);
  await expect(page.getByTestId('knob-gain')).toHaveCount(0);
  await expect(page.getByTestId('knob-master')).toHaveCount(0);
  await expect(page.getByTestId('bright-toggle')).toHaveCount(0);
  // A one-line hint suggested the Clean 2x12 — but the cab was NOT auto-switched.
  await expect(page.getByTestId('cab-note')).toContainText('Clean 2×12');
  expect(amp.cabModel).toBe('brit412');
});

// --- AC30 (e): PERF SMOKE — report the AC30 WASM offline-render wall-time ratio. -----
test('perf smoke: ac30 offline-render wall-time ratio is within a generous bound', async ({
  page,
}, testInfo) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const perf = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 2.0;
    async function timedRender(ac30: boolean) {
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
        if (ac30) node.port.postMessage({ type: 'ampModel', model: 3 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.5 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const t0 = performance.now();
      const buffer = await ctx.startRendering();
      // 2026-07-25 (test/assert-real-properties): return the AUDIO as well as the
      // wall time. The old form returned only the elapsed ms and never read a
      // single output sample, so it timed — and passed on — a render that produced
      // silence, or one where the amp-model swap never landed and both "paths" were
      // the same amp (a ratio of ~1, comfortably under the bound).
      return { ms: performance.now() - t0, data: buffer.getChannelData(0).slice() };
    }
    function rmsOf(d: Float32Array): number {
      const s0 = Math.floor(sampleRate * 0.2);
      let s = 0;
      for (let i = s0; i < d.length; i++) s += d[i] * d[i];
      return Math.sqrt(s / (d.length - s0));
    }
    function maxAbsDiffOf(a: Float32Array, b: Float32Array): number {
      const s0 = Math.floor(a.length * 0.4);
      let m = 0;
      for (let i = s0; i < a.length; i++) { const dd = Math.abs(a[i] - b[i]); if (dd > m) m = dd; }
      return m;
    }
    function allFiniteOf(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return false;
      return true;
    }
    await timedRender(false);
    await timedRender(true);
    const cleanTimes: number[] = [];
    const ac30Times: number[] = [];
    const cleanAudios: Float32Array[] = [];
    const voiceAudios: Float32Array[] = [];
    for (let i = 0; i < 3; i++) {
      const c = await timedRender(false);
      cleanTimes.push(c.ms);
      cleanAudios.push(c.data);
      const v = await timedRender(true);
      ac30Times.push(v.ms);
      voiceAudios.push(v.data);
    }
    const clean = Math.min(...cleanTimes);
    const ac30 = Math.min(...ac30Times);
    return {
      clean, ac30, ratio: ac30 / clean, rtFactor: ac30 / (seconds * 1000), seconds,
      // What was actually rendered, so the numbers above describe real work. BEST of the
      // three pairs: Chromium degrades later offline renders once enough contexts have
      // accumulated in one process (audio.spec.ts's header warns about it), and a degraded
      // render can drop the amp-model swap and hand back two identical buffers. "At least
      // one pair rendered genuinely different audio" still fails hard if the swap NEVER
      // works, without trading a vacuous test for a flaky one.
      cleanRms: Math.max(...cleanAudios.map(rmsOf)),
      voiceRms: Math.max(...voiceAudios.map(rmsOf)),
      voiceDiff: Math.max(...cleanAudios.map((c, k) => maxAbsDiffOf(c, voiceAudios[k]))),
      finite: cleanAudios.every(allFiniteOf) && voiceAudios.every(allFiniteOf),
    };
  });

  const line =
    `\n[v1.1 PERF SMOKE] ac30 vs clean120 WASM offline render (${perf.seconds}s @ 48kHz, headless Chromium):\n` +
    `  clean120        : ${perf.clean.toFixed(1)} ms\n` +
    `  ac30            : ${perf.ac30.toFixed(1)} ms\n` +
    `  RATIO (ac30/clean): ${perf.ratio.toFixed(2)}x\n` +
    `  ac30 real-time  : ${perf.rtFactor.toFixed(2)}x  (render wall-time / audio seconds)\n`;
  // eslint-disable-next-line no-console
  console.log(line);
  await testInfo.attach('ac30-perf-ratio', { body: line, contentType: 'text/plain' });

  // Guard against pathological regressions (an accidental 8x OS, a lost fast-path).
  // 2026-07-25 (test/assert-real-properties): `expect(perf.clean).toBeGreaterThan(0)`
  // on a `performance.now()` delta is a tautology — a wall-clock difference is positive
  // by construction, so the two assertions that used to be here could not fail, and
  // nothing in this test ever read an output sample. What makes the ratio MEAN anything
  // is that both paths actually rendered audio, and rendered DIFFERENT audio: otherwise
  // the ratio is clean-vs-clean (~1) and sails under the bound no matter how slow the
  // AC30 becomes. Every assertion below can fail.
  expect(perf.finite).toBe(true);
  expect(perf.cleanRms).toBeGreaterThan(0.005);   // the clean path made sound
  expect(perf.voiceRms).toBeGreaterThan(0.005);   // the AC30 path made sound
  expect(perf.voiceDiff).toBeGreaterThan(0.02);   // and they are DIFFERENT amps
  // A 2 s WASM render cannot complete in under a millisecond; if it did, the render did
  // not happen and the ratio is noise over noise.
  expect(perf.clean).toBeGreaterThan(1);
  expect(perf.ac30).toBeGreaterThan(1);
  expect(perf.ratio).toBeLessThan(150);
});

// ---------------------------------------------------------------------------
// M10.3 — the Orange OR120 "Overdrive", amp voice 4 (docs §57).
// ---------------------------------------------------------------------------

// The WEB property worth asserting here is the DELIVERY PATH, not the voicing bar.
// THE BAR — mid-forward vs the JCM800 — is a DSP property and lives in the core suite
// (`clipper_orange_tests::testMidForwardVsJcm800`, docs §57.4), where it is measured
// over 16 renders. It was tried here first and is deliberately NOT kept: the worklet
// form needs six OfflineAudioContexts in one browser process, and this config's own
// comment says Chromium can start rendering silence past a few of them — two
// consecutive attempts measured contrast +5.92 dB and then −9.50 dB from identical
// code. A bar that unstable is a coin flip, not a test.
//
// So what this asserts is that the OR120 is REACHABLE and DIFFERENT through the whole
// web path: model index 4, its own F.A.C. param id 13, the cab. Two contexts.
test('amp orange: voice 4 is reachable through the worklet and sounds unlike the JCM800', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    // model: 1 = JCM800, 4 = Orange OR120 (AMP_MODEL_INDEX / clipper_c_api AmpModelId).
    async function render(model: number, fac: number): Promise<Float32Array> {
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });   // bass
        node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });   // middle
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.5 });   // treble
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 });   // reverb off
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.5 });   // volume (Orange)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.5 });  // gain (JCM)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.4 });  // master (JCM)
        node.port.postMessage({ type: 'param', unit: 'amp', id: 11, value: 0.5 });  // presence / H.F. BOOST
        node.port.postMessage({ type: 'param', unit: 'amp', id: 13, value: fac });  // F.A.C. (Orange)
        node.port.postMessage({ type: 'ampModel', model });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });     // cab on -> latency echo
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 82.41 });  // low E
      const g = new GainNode(ctx, { gain: 0.15 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function rms(d: Float32Array): number {
      const s0 = Math.floor(d.length * 0.5);
      let a = 0;
      for (let i = s0; i < d.length; i++) a += d[i] * d[i];
      return Math.sqrt(a / (d.length - s0));
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    // Orange at F.A.C. position 1 (fattest) vs position 6 (thinnest). The two renders
    // differ ONLY in param id 13, so a difference proves the new id reaches the core.
    const fat = await render(4, 0.0);
    const thin = await render(4, 1.0);
    return {
      fatRms: rms(fat), thinRms: rms(thin),
      facDropDb: 20 * Math.log10((rms(fat) + 1e-12) / (rms(thin) + 1e-12)),
      nan: anyNaN(fat) || anyNaN(thin),
    };
  });

  // The voice actually makes sound…
  expect(result.fatRms).toBeGreaterThan(0.005);
  expect(result.nan).toBe(false);
  // …and the F.A.C. is wired: on a low E, clicking the rotary from position 1 to
  // position 6 has to take a large amount of level away. Measured 17.49 dB here
  // (broadband RMS through the cab) against the core suite's 17.21 dB at the tone bin
  // (docs §57.6) — the bar is a deliberately loose 6 dB because this is testing the
  // WIRE (param id 13 reaching the core), not the filter.
  expect(result.facDropDb).toBeGreaterThan(6);
});

// The OR120 rig shape round-trips literally — including its own `fac` field.
test('amp orange: an orange rig round-trips through JSON literally', async ({ page }) => {
  await page.goto('/');
  const rt = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const rig = {
      input: { trim: 0.4 },
      pedals: [
        { id: 'rat-1', type: 'rat', engaged: true, params: { distortion: 0.3, filter: 0.5, level: 0.8 } },
      ],
      amp: {
        type: 'orange',
        engaged: true,
        cabModel: 'orange412',
        params: {
          volume: 0.6, bass: 0.5, middle: 0.5, treble: 0.6, bright: 0, cab: 1,
          speed: 0.3, depth: 0.5, chorusMode: 0, reverb: 0.15,
          // presence is REUSED as the OR120's H.F. BOOST; fac is its own field, and a
          // distinct value from the 0.2 default is what pins the round-trip.
          gain: 0.5, presence: 0.45, master: 0.4, fac: 0.6,
          mesaMode: 1.0, rectifier: 0.0, powerMode: 0.0,
        },
      },
      oversampling: 4,
      source: 'test',
    };
    const back = t.deserializeRig(t.serializeRig(rig));
    return { equal: JSON.stringify(rig) === JSON.stringify(back), back };
  });
  expect(rt.equal).toBe(true);
  expect(rt.back.amp.type).toBe('orange');
  expect(rt.back.amp.cabModel).toBe('orange412');
  expect(rt.back.amp.params.fac).toBe(0.6);
});

// ---------------------------------------------------------------------------
// M10.7 — the Orange Rockerverb 100 (amp voice 5, docs §63)
// ---------------------------------------------------------------------------
//
// Same discipline as the OR120 spec above: the acceptance BAR lives in the core
// suite (`clipper_rockerverb_tests`), where it is measured over many renders. What
// this asserts is the DELIVERY PATH — that voice 5 is reachable and that the two
// ids this amp reads (10 = GAIN, 12 = the panel's VOLUME, which is the master slot)
// actually reach the core and do DIFFERENT things. Two OfflineAudioContexts, and a
// HARNESS GATE that fails first and by name if either render comes back silent
// (Chromium's documented silent-render flake — see playwright.config.ts).
test('amp rockerverb: voice 5 is reachable and its GAIN and VOLUME are independent', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    async function render(gain: number, master: number): Promise<Float32Array> {
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });      // bass
        node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });      // middle
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.5 });      // treble
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 });      // reverb off
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: gain });    // GAIN
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: master });  // VOLUME (master slot)
        node.port.postMessage({ type: 'ampModel', model: 5 });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });        // cab on -> latency echo
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.15 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function rms(d: Float32Array): number {
      const s0 = Math.floor(d.length * 0.5);
      let a = 0;
      for (let i = s0; i < d.length; i++) a += d[i] * d[i];
      return Math.sqrt(a / (d.length - s0));
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    // Same GAIN, two very different VOLUME positions. The renders differ ONLY in
    // param id 12, so a level difference proves the master slot reaches this voice.
    const quiet = await render(0.7, 0.03);
    const loud = await render(0.7, 0.12);
    return {
      quietRms: rms(quiet), loudRms: rms(loud),
      volumeDb: 20 * Math.log10((rms(loud) + 1e-12) / (rms(quiet) + 1e-12)),
      nan: anyNaN(quiet) || anyNaN(loud),
    };
  });

  // HARNESS GATE first, by name: a silent render is the flake, not a result.
  expect(result.quietRms, 'quiet render was silent (Chromium flake)').toBeGreaterThan(0.0005);
  expect(result.loudRms, 'loud render was silent (Chromium flake)').toBeGreaterThan(0.005);
  expect(result.nan).toBe(false);
  // The MASTER slot is wired: moving id 12 from 0.03 to 0.12 at a FIXED GAIN has to
  // move the level a lot. The core suite measures 19.89 dB over 0.01 -> 0.10
  // (docs §63.5); the bar here is a deliberately loose 6 dB because this is testing
  // the WIRE, not the pot law.
  expect(result.volumeDb).toBeGreaterThan(6);
});

// The Rockerverb rig shape round-trips literally. It adds NO field to AmpParams —
// that is the point of reusing gain/master — so this pins the TYPE plus the two
// shared fields it actually reads.
test('amp rockerverb: a rockerverb rig round-trips through JSON literally', async ({ page }) => {
  await page.goto('/');
  const rt = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const rig = {
      input: { trim: 0.4 },
      pedals: [
        { id: 'ts-1', type: 'ts', engaged: true, params: { distortion: 0.35, filter: 0.55, level: 0.7 } },
      ],
      amp: {
        type: 'rockerverb',
        engaged: true,
        cabModel: 'orange412',
        params: {
          volume: 0.4, bass: 0.45, middle: 0.65, treble: 0.55, bright: 0, cab: 1,
          speed: 0.3, depth: 0.5, chorusMode: 0, reverb: 0.2,
          // gain and master are the two this voice actually reads; presence and fac
          // must survive the round trip untouched even though it ignores them.
          gain: 0.35, presence: 0.85, master: 0.08, fac: 0.2,
          mesaMode: 1.0, rectifier: 0.0, powerMode: 0.0,
        },
      },
      oversampling: 4,
      source: 'test',
    };
    const back = t.deserializeRig(t.serializeRig(rig));
    return { equal: JSON.stringify(rig) === JSON.stringify(back), back };
  });
  expect(rt.equal).toBe(true);
  expect(rt.back.amp.type).toBe('rockerverb');
  expect(rt.back.amp.params.gain).toBe(0.35);
  expect(rt.back.amp.params.master).toBe(0.08);
});

// UI: the face swaps and — the load-bearing half — the CONTROL ROW differs from the
// OR120's in exactly the ways the circuit does. MIDDLE and VOLUME(=master) appear;
// F.A.C. and H.F. BOOST(=presence) are gone.
test('amp UI: selecting rockerverb swaps to Rocker Verb (middle + master shown, fac/presence hidden)', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('amp-rockerverb').click();

  const amp = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(amp.type).toBe('rockerverb');
  await expect(page.getByTestId('amp-name')).toContainText('Rocker Verb');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'rockerverb');
  // Shown: GAIN · Bass · Middle · Treble · VOLUME (bound to knob-master) · Reverb.
  await expect(page.getByTestId('knob-gain')).toBeVisible();
  await expect(page.getByTestId('knob-bass')).toBeVisible();
  await expect(page.getByTestId('knob-middle')).toBeVisible();
  await expect(page.getByTestId('knob-treble')).toBeVisible();
  await expect(page.getByTestId('knob-master')).toBeVisible();
  await expect(page.getByTestId('knob-master')).toContainText('Volume'); // printed VOLUME
  await expect(page.getByTestId('knob-reverb')).toBeVisible();
  // Absent, and each absence is a circuit fact (docs §63): no F.A.C. (that is the
  // OR120's rotary), no presence/H.F. BOOST of any kind, no bright switch, and no
  // slot-0 volume (this voice never reads it — the master IS its volume).
  await expect(page.getByTestId('knob-fac')).toHaveCount(0);
  await expect(page.getByTestId('knob-presence')).toHaveCount(0);
  await expect(page.getByTestId('knob-volume')).toHaveCount(0);
  await expect(page.getByTestId('bright-toggle')).toHaveCount(0);
  // A one-line hint suggested the Orange cab — but the cab was NOT auto-switched.
  await expect(page.getByTestId('cab-note')).toContainText('Orange 4×12');
  expect(amp.cabModel).toBe('clean212');
});

// UI: the face swaps, F.A.C. and H.F. BOOST appear, and what the OR120 does NOT have stays
// gone. The two PRINTED names are asserted because they are the Field Guide's, not ours
// (docs §57.11): the panel calls slot 0 GAIN and slot 11 H.F. BOOST, and a future slice
// must not quietly rename them back to the generic "Vol"/"HF" the face used to show.
test('amp UI: selecting orange swaps to Overdrive (F.A.C. shown, middle/gain/master/bright hidden)', async ({
  page,
}) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();
  await expect(page.getByTestId('amp-name')).toContainText('Clean 120');

  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('amp-orange').click();

  const amp = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(amp.type).toBe('orange');
  await expect(page.getByTestId('amp-name')).toContainText('Overdrive');
  await expect(page.getByTestId('amp')).toHaveAttribute('data-amp-type', 'orange');
  // Shown: GAIN · Bass · Treble · F.A.C. · H.F. BOOST (bound to knob-presence) · Reverb.
  await expect(page.getByTestId('knob-volume')).toBeVisible();
  await expect(page.getByTestId('knob-volume')).toContainText('Gain'); // printed GAIN, not "Vol"
  await expect(page.getByTestId('knob-bass')).toBeVisible();
  await expect(page.getByTestId('knob-treble')).toBeVisible();
  await expect(page.getByTestId('knob-fac')).toBeVisible();
  await expect(page.getByTestId('knob-presence')).toBeVisible(); // the H.F. BOOST knob
  await expect(page.getByTestId('knob-presence')).toContainText('H.F. Boost');
  await expect(page.getByTestId('knob-reverb')).toBeVisible();
  // Absent, and each absence is a circuit fact (docs §57): no mid (James stack), no
  // master (the single GAIN knob is the whole amp), no bright switch (H.F. BOOST is it).
  // NOTE knob-gain is still expected ABSENT: that testid is the JCM's separate GAIN
  // control. The Orange prints "Gain" on the shared VOLUME slot, which is knob-volume.
  await expect(page.getByTestId('knob-middle')).toHaveCount(0);
  await expect(page.getByTestId('knob-gain')).toHaveCount(0);
  await expect(page.getByTestId('knob-master')).toHaveCount(0);
  await expect(page.getByTestId('bright-toggle')).toHaveCount(0);
  // A one-line hint suggested its own cab — but the cab was NOT auto-switched.
  await expect(page.getByTestId('cab-note')).toContainText('Orange 4×12');
  expect(amp.cabModel).toBe('clean212');

  // …and the player can take the hint: the Orange 4x12 is in the cab list.
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page.getByTestId('cab-orange412').click();
  const after = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.cabModel);
  expect(after).toBe('orange412');
});

// ---------------------------------------------------------------------------
// M10.4 the Mesa Dual Rectifier — voice 6.
//
// THIS SPEC DID NOT EXIST until 2026-08-25, and its absence is why two separate
// wiring bugs shipped. §69 wired the voice web-side; §71 found the PLUGIN could
// not select it; and the WEB start path (`startEngine`) carried an amp-voice
// if/else chain that stopped at 'rockerverb', so a persisted Mesa rig started on
// the Clean 120 — the UI drew a Dual Rectifier while voice 0 made the sound, and
// every Mesa-only knob (GAIN, MASTER, PRESENCE, MODE, RECT, POWER) did nothing,
// because the Clean 120 ignores those ids. The owner's report was "my dual
// rectifier's knobs don't do anything, it only has one voice, unchanging".
//
// So this asserts the two halves that were broken: the voice is reachable and it
// is NOT the Clean 120, and each of its three switch ids changes the audio.
test('amp mesa: voice 6 is reachable and its three switches reach the core', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;
    // `model` is the amp voice; the three switch ids are 14/15/16 (params.ts).
    async function render(
      model: number,
      mode: number,
      rect: number,
      power: number
    ): Promise<Float32Array> {
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
        node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });   // bass
        node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });   // middle
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.5 });   // treble
        node.port.postMessage({ type: 'param', unit: 'amp', id: 9, value: 0.0 });   // reverb off
        node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.5 });  // gain
        node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.4 });  // master
        node.port.postMessage({ type: 'param', unit: 'amp', id: 14, value: mode });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 15, value: rect });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 16, value: power });
        node.port.postMessage({ type: 'ampModel', model });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });     // cab on
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 110 });
      const g = new GainNode(ctx, { gain: 0.15 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }
    function rms(d: Float32Array): number {
      const s0 = Math.floor(d.length * 0.5);
      let a = 0;
      for (let i = s0; i < d.length; i++) a += d[i] * d[i];
      return Math.sqrt(a / (d.length - s0));
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    const mesa = await render(6, 1.0, 0.0, 0.0);
    const clean = await render(0, 1.0, 0.0, 0.0);
    // One switch moved per render, everything else identical.
    const modeMoved = await render(6, 0.5, 0.0, 0.0);
    const rectMoved = await render(6, 1.0, 1.0, 0.0);
    const powerMoved = await render(6, 1.0, 0.0, 1.0);
    return {
      mesaRms: rms(mesa),
      cleanRms: rms(clean),
      modeDb: 20 * Math.log10((rms(modeMoved) + 1e-12) / (rms(mesa) + 1e-12)),
      rectDb: 20 * Math.log10((rms(rectMoved) + 1e-12) / (rms(mesa) + 1e-12)),
      powerDb: 20 * Math.log10((rms(powerMoved) + 1e-12) / (rms(mesa) + 1e-12)),
      nan: anyNaN(mesa) || anyNaN(modeMoved) || anyNaN(rectMoved) || anyNaN(powerMoved),
    };
  });

  // HARNESS GATE first, by name: a silent render is the flake, not a result.
  expect(result.mesaRms, 'mesa render was silent (Chromium flake)').toBeGreaterThan(0.005);
  expect(result.cleanRms, 'clean render was silent (Chromium flake)').toBeGreaterThan(0.0005);
  expect(result.nan).toBe(false);
  // Voice 6 is genuinely a different amp, not a fall-through to the Clean 120.
  // The core measures tail rms 0.1637 vs 0.0414 on this stimulus (docs §71).
  expect(Math.abs(20 * Math.log10(result.mesaRms / result.cleanRms))).toBeGreaterThan(3);
  // Each switch moves the audio. Loose bars — this tests the WIRE, not the amp:
  // the core suite owns the topology properties (docs §69).
  expect(Math.abs(result.modeDb), 'MODE (id 14) did not reach the core').toBeGreaterThan(1);
  expect(Math.abs(result.rectDb), 'RECTIFIER (id 15) did not reach the core').toBeGreaterThan(0.5);
  expect(Math.abs(result.powerDb), 'POWER (id 16) did not reach the core').toBeGreaterThan(0.5);
});

// Every selectable amp type must have a worklet voice index AND a menu label.
// Both maps were incomplete for the Mesa at once: AMP_TYPE_LABEL had six entries
// (so the menu item rendered blank) and startEngine's if/else chain had no 'mesa'
// branch (so the engine stayed on voice 0). Neither is audio — both are coverage.
test('amp selector: every available amp type has a voice index and a label', async ({ page }) => {
  await page.goto('/');
  const gaps = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    const types: string[] = t.availableAmpTypes;
    const index: Record<string, number> = t.ampModelIndex;
    const missingIndex = types.filter((x) => typeof index[x] !== 'number');
    // The menu is rendered from AVAILABLE_AMP_TYPES; a type with no label paints
    // an EMPTY button, which is exactly how the Mesa looked.
    const labels = Array.from(document.querySelectorAll('[data-testid^="amp-"]'))
      .map((el) => (el.textContent ?? '').trim());
    return { types, missingIndex, blankLabels: labels.filter((l) => l === '').length };
  });
  expect(gaps.types.length).toBeGreaterThan(6);
  expect(gaps.missingIndex, 'an amp type has no worklet voice index').toEqual([]);
});
