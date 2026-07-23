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

  // Default rig at a 0.1 input is loud (~-3 dBFS peak) — NOT the old ~-26 dBFS.
  expect(result.defPeak).toBeGreaterThan(0.35); // > ~-9 dBFS
  expect(result.defPeak).toBeLessThanOrEqual(1.0);
  // Cranked, the soft limiter holds the output at/below full scale (no overs).
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

  // M5 amp panel: Clean 120 with four knobs + bright/cab/power controls.
  await expect(page.getByTestId('amp')).toBeVisible();
  for (const name of ['Volume', 'Bass', 'Middle', 'Treble']) {
    await expect(page.getByRole('slider', { name })).toBeVisible();
  }
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

  // Pure serialize -> deserialize round-trip is exact for a valid rig (incl amp).
  const roundTrip = await page.evaluate(() => {
    const t = (window as any).__CLIPPER_TEST__;
    // Key order must match normalizeRig's output for the exact-JSON round-trip.
    const rig = {
      input: { trim: 0.5 },
      pedal: { type: 'rat', engaged: false, params: { distortion: 0.42, filter: 0.13, level: 0.9 } },
      amp: {
        type: 'clean120',
        engaged: false,
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
