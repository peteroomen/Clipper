import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// Cab expansion (Brit 4×12 + user IR upload) browser verification.
//
// Offline-render proofs come FIRST (same reason as audio.spec.ts: creating many
// live AudioContexts can starve later OfflineAudioContext renders), then the
// live/UI flows. Each render posts the cab message and awaits the worklet's
// `latency` echo as a delivery barrier before the synchronous offline render.

const combWavBytes = Array.from(
  readFileSync(fileURLToPath(new URL('./fixtures/comb-ir.wav', import.meta.url)))
);

// --- 1. Built-in swap changes the rendered spectrum: brit412 is darker up top. -
test('cab select: brit412 renders darker highs than clean212', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.5;

    async function render(brit: boolean): Promise<Float32Array> {
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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true }); // clean tone to the amp
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // vol unity
        node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.5 }); // treble flat
        // Select the cab; the built-in swap echoes latency (our delivery barrier).
        node.port.postMessage({ type: 'cab', builtin: brit ? 1 : 0 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 6500 });
      osc.connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function goertzel(data: Float32Array, freq: number): number {
      const start = Math.floor(sampleRate * 0.15);
      const N = data.length - start;
      const w = (2 * Math.PI * freq) / sampleRate;
      const c = 2 * Math.cos(w);
      let s0 = 0, s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }

    const clean = await render(false);
    const brit = await render(true);
    return { clean65: goertzel(clean, 6500), brit65: goertzel(brit, 6500) };
  });

  // The 4×12 is a greenback-ish, darker voicing: it rolls off the top far more
  // than the 2×12, so 6.5 kHz comes out clearly quieter (well over 6 dB down).
  expect(result.clean65).toBeGreaterThan(0);
  expect(result.brit65).toBeLessThan(result.clean65 * 0.5);
});

// --- 2. A loaded custom IR actually convolves the output (comb signature). ------
test('cab upload: output is convolved by the uploaded comb IR', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async (wavBytes) => {
    const sampleRate = 48000;
    const seconds = 0.5;
    const length = Math.floor(sampleRate * seconds);

    // Decode the fixture WAV -> mono Float32 (the same mono-ize the app does).
    const decodeCtx = new OfflineAudioContext(1, 1, sampleRate);
    const bytes = new Uint8Array(wavBytes);
    const decoded = await decodeCtx.decodeAudioData(bytes.buffer.slice(0));
    const ir = decoded.getChannelData(0).slice();
    const irLen = ir.length; // capture BEFORE the transfer detaches ir.buffer

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
    await new Promise<void>((resolve) => {
      node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
      node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
      node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 }); // vol
      node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 }); // bass flat
      node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 }); // mid flat
      node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.5 }); // treble flat
      // Load the custom IR (transfer). amp_load_custom_ir peak-normalizes it.
      node.port.postMessage({ type: 'cab', custom: ir }, [ir.buffer]);
    });

    // Two tones: 400 Hz sits on a comb PEAK, 500 Hz on a comb NOTCH (200 Hz-spaced
    // comb from the 5 ms delay pair). The output must show that comb.
    const osc1 = new OscillatorNode(ctx, { type: 'sine', frequency: 400 });
    const osc2 = new OscillatorNode(ctx, { type: 'sine', frequency: 500 });
    const g = new GainNode(ctx, { gain: 0.5 });
    osc1.connect(g); osc2.connect(g); g.connect(node).connect(ctx.destination);
    osc1.start(); osc2.start();
    const buffer = await ctx.startRendering();
    const data = buffer.getChannelData(0);

    function goertzel(freq: number): number {
      const start = Math.floor(sampleRate * 0.15);
      const N = data.length - start;
      const w = (2 * Math.PI * freq) / sampleRate;
      const c = 2 * Math.cos(w);
      let s0 = 0, s1 = 0, s2 = 0;
      for (let i = start; i < data.length; i++) { s0 = data[i] + c * s1 - s2; s2 = s1; s1 = s0; }
      const re = s1 - s2 * Math.cos(w);
      const im = s2 * Math.sin(w);
      return (2 * Math.sqrt(re * re + im * im)) / N;
    }
    return { peak400: goertzel(400), notch500: goertzel(500), irLen };
  }, combWavBytes);

  // Real signal came through and the comb is unmistakable: the 400 Hz peak is many
  // times louder than the 500 Hz notch — the output was convolved by our IR.
  //
  // 2026-07-25 (test/assert-real-properties): `irLen > 128` asserted a property of the
  // committed FIXTURE, not of the code under test — comb-ir.wav either is longer than
  // one partition or it is not, and nothing this test does can change that. What is
  // worth pinning is the exact decoded length, because that is what proves the app's
  // decode + mono-ise path did not truncate or resample the upload: comb-ir.wav is
  // mono 16-bit 48 kHz, 512 frames, and the render context is also 48 kHz, so a
  // correct decode yields exactly 512 samples. A wrong number here means the IR that
  // reached the convolver is not the IR on disk.
  expect(result.irLen).toBe(512);
  expect(result.peak400).toBeGreaterThan(0.02);
  // The comb is unmistakable: the 400 Hz peak is many times the 500 Hz notch —
  // the output was genuinely convolved by our uploaded IR (in-place, deep notch).
  expect(result.peak400).toBeGreaterThan(result.notch500 * 5);
});

// --- 3. Declick: a cab swap landed MID-RENDER produces no pop. ------------------
//
// Rewritten 2026-07-25. The 2026-07-24 audit flagged the previous version of this
// test as doubly vacuous, and it was:
//
//   1. It posted the `cab` message BEFORE startRendering(), so the swap landed at
//      the opening zero crossing with empty filter state — removing the declick
//      entirely would still have passed.
//   2. Its "swap" selected brit412, which test 1 above asserts is DARKER. A darker
//      cab has lower slew by construction, so "swapMaxDelta < baseMaxDelta * 1.6"
//      was nearly free.
//
// Both are fixed here: the swap lands mid-note via ctx.suspend()/resume() (the
// technique expectations.spec.ts uses), and it goes brit412 -> clean212, i.e.
// toward the BRIGHTER cab, so a lower slew is not handed to us. The baseline is
// the same brit412 tone never swapped, plus the settled clean212 segment's own
// slew — a brighter cab legitimately slews faster, and that is tone, not a click.
test('cab swap: no pop when the swap lands mid-note (declick continuity)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 1.0;

    async function render(swap: boolean): Promise<Float32Array> {
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
      // Both renders START on the brit412 so the pre-swap signal is identical.
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
        node.port.postMessage({ type: 'cab', builtin: 1 });
      });

      if (swap) {
        // Swap to the BRIGHTER clean212 half a second in, with the note sustaining
        // and the convolver's FDL full. suspend() parks the offline render so the
        // port message lands deterministically before resume.
        ctx.suspend(0.5).then(async () => {
          node.port.postMessage({ type: 'cab', builtin: 0 });
          await new Promise((r) => setTimeout(r, 50));
          await ctx.resume();
        });
      }

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.1 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function maxDelta(d: Float32Array, t0: number, t1: number): number {
      const a = Math.max(1, Math.floor(t0 * sampleRate));
      const b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let m = 0;
      for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
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

    const steady = await render(false);  // brit412 throughout, never swapped
    const swapped = await render(true);  // brit412 -> clean212 at 0.5 s
    return {
      nan: anyNaN(swapped),
      rms: rms(swapped),
      // Steady-state slew reference: the un-swapped tone, and the SETTLED
      // post-swap segment's own slew (the brighter cab slews faster — tone).
      baseline: Math.max(maxDelta(steady, 0.2, 1.0), maxDelta(swapped, 0.65, 1.0)),
      // The swap window. Any step discontinuity shows up here, far above slew.
      swapDelta: maxDelta(swapped, 0.47, 0.62),
    };
  });

  expect(result.nan).toBe(false);
  expect(result.rms).toBeGreaterThan(0.005); // the note is audibly there
  // The raised-cosine declick means the swap costs no step: the largest
  // sample-to-sample jump across the swap window stays in the ballpark of
  // steady-state slew.
  expect(result.swapDelta).toBeLessThan(result.baseline * 2.0 + 0.02);
});

// --- 3b. Two topology edits inside ONE declick fade window both land at zero. ---
//
// 2026-07-24 audit finding 2, adjacent defect (b). Every staging path in the worklet
// used to begin with `if (this._hasPendingEdit()) this._commitPending();`. So a
// second edit arriving before the fade reached zero force-committed the first one at
// whatever gain the ramp was at — exactly the step discontinuity the declick exists
// to prevent. It is reachable by ordinary use: a drag-reorder, a rapid add/remove,
// or the assistant issuing two tool calls in one turn.
//
// Worst case, and the one this test lands: all the messages arrive in the SAME
// audio-thread message drain, so no render has advanced the ramp yet and
// _declickGain is still 1.0. Pre-fix, the first two edits therefore committed at
// FULL gain — a full-amplitude waveform step, with only the last edit getting a
// fade. Post-fix the edits merge and the whole batch lands together at the zero.
//
// Four edits, so every merge path is exercised: a chain edit removing one pedal, a
// SECOND chain edit removing the other (this is the case that needs both the
// pending-nodes diff base and the `removed`-list union — without the union the first
// pedal's WASM handle leaks forever, because it is already absent from the base the
// second edit diffs against), an amp-power stomp (_pendingAmpBypass), and a cab swap
// (_pendingCab). The amp-power toggle is what makes the pre-fix step large and
// unmistakable: powering the amp down routes the raw chain signal straight out, so
// committing it at full gain steps between two completely different waveforms.
//
// Note the pedal edits must be REMOVALS rather than disengages. `_prepareChain` sets
// `existing.engaged` on the live node object, so an engaged-flag change from a
// `chain` message takes effect at the next render quantum rather than at the fade
// zero — a separate pre-existing declick hole (M11 fixed it for STOMP messages via
// _pendingBypass, but not for the `chain` path). Out of scope here; ledgered.
test('four edits in one declick fade window: all land at output zero', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 1.0;
    const RAT = { id: 'rat-1', type: 'rat', params: { distortion: 0.85, filter: 0.4, level: 0.9 } };
    const TS = { id: 'ts-1', type: 'ts', params: { distortion: 0.5, filter: 0.5, level: 0.7 } };

    async function render(edits: boolean): Promise<Float32Array> {
      const ctx = new OfflineAudioContext(1, Math.floor(sampleRate * seconds), sampleRate);
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
        node.port.postMessage({
          type: 'chain',
          pedals: [{ ...RAT, engaged: true }, { ...TS, engaged: true }],
        });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
        node.port.postMessage({ type: 'cab', builtin: 1 });
        // Sentinel LAST, purely as the delivery barrier. `chain`, `param` and `cab`
        // all stage silently — none of them echoes `latency` from the message
        // handler (only `bypass` and `oversampling` do; a cab's echo comes from
        // _commitPending, which needs process() to have run, so awaiting it before
        // startRendering() deadlocks). Messages on one port are delivered in order,
        // so an echo from this no-op (4 is already the default) proves the three
        // above were consumed.
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });

      if (edits) {
        ctx.suspend(0.5).then(async () => {
          // All four in one drain — inside the ~6 ms fade by construction.
          node.port.postMessage({ type: 'chain', pedals: [{ ...RAT, engaged: true }] }); // remove ts-1
          node.port.postMessage({ type: 'chain', pedals: [] });                          // remove rat-1 too
          node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });               // amp power off
          node.port.postMessage({ type: 'cab', builtin: 0 });                             // and a cab swap
          await new Promise((r) => setTimeout(r, 50));
          await ctx.resume();
        });
      }

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      const g = new GainNode(ctx, { gain: 0.1 });
      osc.connect(g).connect(node).connect(ctx.destination);
      osc.start();
      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    function maxDelta(d: Float32Array, t0: number, t1: number): number {
      const a = Math.max(1, Math.floor(t0 * sampleRate));
      const b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let m = 0;
      for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
      return m;
    }
    function anyNaN(d: Float32Array): boolean {
      for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
      return false;
    }
    function rms(d: Float32Array, t0: number, t1: number): number {
      const a = Math.floor(t0 * sampleRate), b = Math.min(d.length, Math.floor(t1 * sampleRate));
      let s = 0;
      for (let i = a; i < b; i++) s += d[i] * d[i];
      return Math.sqrt(s / (b - a));
    }

    const steady = await render(false);
    const edited = await render(true);
    return {
      nan: anyNaN(edited),
      beforeRms: rms(edited, 0.2, 0.45),
      afterRms: rms(edited, 0.7, 1.0),
      // Steady-state slew reference: the un-edited driven tone, plus the settled
      // post-edit (clean) segment's own slew.
      baseline: Math.max(maxDelta(steady, 0.2, 1.0), maxDelta(edited, 0.7, 1.0)),
      editDelta: maxDelta(edited, 0.47, 0.65),
    };
  });

  expect(result.nan).toBe(false);
  // The batch genuinely landed: both pedals left the circuit and the amp powered
  // down, so the level fell to the raw trimmed input.
  expect(result.beforeRms).toBeGreaterThan(0.01);
  expect(result.afterRms).toBeGreaterThan(0.001);       // still a real note
  expect(result.afterRms).toBeLessThan(result.beforeRms * 0.6);
  // ...and it landed at output zero, so there is no step. Pre-fix the first two
  // edits committed at a declick gain of 1.0 — a full-amplitude waveform step.
  expect(result.editDelta).toBeLessThan(result.baseline * 2.0 + 0.02);
});

// --- 4. UI upload flow: uploading a WAV selects + persists the custom cab. ------
test('cab upload UI: uploading a WAV selects the custom cab and persists', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('board-amp')).toBeVisible();

  // Open the amp slot menu, then feed the fixture into the (hidden) file input.
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('amp-menu')).toBeVisible();
  await page
    .getByTestId('cab-upload-input')
    .setInputFiles(fileURLToPath(new URL('./fixtures/comb-ir.wav', import.meta.url)));

  // The app reports the load and the rig now uses the custom cab.
  await expect(page.getByTestId('cab-note')).toContainText('Loaded');
  const cab = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(cab.cabModel).toBe('custom');
  // 2026-07-25 (test/assert-real-properties): `typeof cab.customCabLabel === 'string'`
  // passes for `''`, for `'undefined'`, and for any wrong label — it asserted the TYPE
  // of a field whose VALUE is the whole point. The label is what the player reads back
  // in the cab menu, and cab.ts:103 derives it from the uploaded filename with the
  // extension stripped, so for ./fixtures/comb-ir.wav it must be exactly 'comb-ir'.
  expect(cab.customCabLabel).toBe('comb-ir');

  // Persisted: reload restores cabModel:'custom' and the custom entry in the menu.
  await page.reload();
  const after = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp);
  expect(after.cabModel).toBe('custom');
  await page.getByTestId('amp-select').click();
  await expect(page.getByTestId('cab-custom')).toBeVisible();
});

// --- 5. Missing custom IR -> falls back to the Clean 2×12 with a note. ----------
test('cab: a rig referencing a missing custom IR falls back to clean212', async ({ page }) => {
  await page.goto('/');
  // Seed a rig that SAYS custom but with NO custom-IR data stored (simulates a
  // rig shared to another browser). Ensure the IR key is absent.
  await page.evaluate(() => {
    localStorage.removeItem('clipper.customCab.v1');
    const rig = {
      input: { trim: 0.4 },
      pedals: [{ id: 'rat-1', type: 'rat', engaged: true, params: { distortion: 0.7, filter: 0.4, level: 0.8 } }],
      amp: {
        type: 'clean120', engaged: true, cabModel: 'custom', customCabLabel: 'Ghost IR',
        params: { volume: 0.4, bass: 0.5, middle: 0.5, treble: 0.6, bright: 0, cab: 1, speed: 0.3, depth: 0.5, chorusMode: 0 },
      },
      oversampling: 4, source: 'test',
    };
    localStorage.setItem('clipper.rig.v1', JSON.stringify(rig));
  });
  await page.reload();

  // The app fell back to the Clean 2×12 and told the player.
  await expect(page.getByTestId('cab-note')).toContainText('Custom IR not found');
  const cab = await page.evaluate(() => (window as any).__CLIPPER_TEST__.getRig().amp.cabModel);
  expect(cab).toBe('clean212');
});
