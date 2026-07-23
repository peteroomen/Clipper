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
  expect(result.irLen).toBeGreaterThan(128); // a >1-partition custom IR
  expect(result.peak400).toBeGreaterThan(0.02);
  // The comb is unmistakable: the 400 Hz peak is many times the 500 Hz notch —
  // the output was genuinely convolved by our uploaded IR (in-place, deep notch).
  expect(result.peak400).toBeGreaterThan(result.notch500 * 5);
});

// --- 3. Declick: a cab swap mid-tone produces no pop (RMS/continuity). ----------
test('cab swap: no pop (declick continuity)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const result = await page.evaluate(async () => {
    const sampleRate = 48000;
    const seconds = 0.4;

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
      await new Promise<void>((resolve) => {
        node.port.onmessage = (e: MessageEvent) => { if (e.data?.type === 'latency') resolve(); };
        node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true });
        node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
        // Swap render posts a cab change (declick-bracketed); baseline stays clean.
        if (swap) node.port.postMessage({ type: 'cab', builtin: 1 });
        else node.port.postMessage({ type: 'cab', builtin: 0 });
      });
      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
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

    const base = await render(false);
    const swapped = await render(true);
    return {
      baseMaxDelta: maxDelta(base),
      swapMaxDelta: maxDelta(swapped),
      nan: anyNaN(swapped),
    };
  });

  // The declick fade is a smooth raised-cosine, so the swap render's largest
  // sample-to-sample jump stays in the same ballpark as the un-swapped tone — no
  // step/pop — and never NaNs.
  expect(result.nan).toBe(false);
  expect(result.swapMaxDelta).toBeLessThan(result.baseMaxDelta * 1.6 + 0.01);
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
  expect(typeof cab.customCabLabel).toBe('string');

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
