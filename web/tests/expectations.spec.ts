import { test, expect } from '@playwright/test';

// M11 "Player Expectations Suite" — the FIRST FIVE MINUTES simulated in the real
// browser engine (docs §26). A new player loads the page, drops each dirt pedal
// from the gear tray onto the rig, stomps it on at its opening defaults, plays a
// note, and cycles the amp voices. Four field bugs shipped while every circuit
// metric was green (RAT "no balls", cab fizz, Muff hum blowout, AC30 "muddy") —
// this spec asserts the things those players heard:
//   (1) audible, non-silent output at DEFAULTS,
//   (2) no NaN/inf anywhere in the buffer,
//   (3) output RMS inside a documented sanity window (the level-calibration guard),
//   (4) stomping a pedal on/off mid-note does NOT click (the M11 worklet fix:
//       bypass toggles now ride the same declick bracket as chain edits).
//
// Harness conventions mirror amp.spec.ts: pure OfflineAudioContext renders, the
// worklet 'ready' handshake, and a trailing cab-on param whose synchronous
// 'latency' echo is the message-delivery barrier. Knob defaults mirror
// web/src/rig.ts (PEDAL_KNOB_DEFAULTS / AMP_KNOB_DEFAULTS) — the app's exact
// opening state. The native sibling of this spec (core measurements + per-gear
// windows) is core/tests/test_player_expectations.cpp.

const SR = 48000;

// The gear tray's dirt pedals at their opening defaults (rig.ts).
const DIRT_PEDALS = [
  { type: 'rat', params: { distortion: 0.7, filter: 0.4, level: 0.8 } },
  { type: 'sd1', params: { distortion: 0.5, filter: 0.5, level: 0.7 } },
  { type: 'ts', params: { distortion: 0.5, filter: 0.5, level: 0.75 } },
  { type: 'muff', params: { distortion: 0.6, filter: 0.5, level: 0.6 } },
] as const;

// Documented sanity windows (dBFS RMS over the settled window), derived from the
// native M11 measurements (test_player_expectations.cpp A1/A4) plus the clean-amp
// (vol 0.4) + cab + limiter stage this spec renders through:
//   dirt pedal into the default clean rig, −20 dBFS 220 Hz source:
//     measured 2026-07: rat −17.5, sd1 −14.3, ts −16.4, muff −10.5 dBFS
//   amp voices alone (pedal bypassed), −16.5 dBFS 220 Hz source:
//     measured 2026-07: clean120 −21.7, jcm800 −14.5, twin −28.7, ac30 −26.5 dBFS
// Windows are the measured spread ± ~8 dB — the next "no balls" (−20 dB) or
// "blows the mix apart" drift fails loudly.
const PEDAL_RMS_DB = { lo: -32, hi: -4 };
const AMP_RMS_DB = { lo: -42, hi: -6 };

test.describe('M11 player expectations (first five minutes)', () => {
  // --- 1+2+3. Each dirt pedal, added at defaults onto the default rig. ----------
  test('each dirt pedal at defaults: audible, finite, inside its level window', async ({
    page,
  }, testInfo) => {
    await page.goto('/');
    await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

    const results = await page.evaluate(
      async ({ pedals }) => {
        const sampleRate = 48000;
        const seconds = 1.0;

        async function render(pedal: {
          type: string;
          params: { distortion: number; filter: number; level: number };
        }) {
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
            // The gear-tray add: this pedal alone, ENGAGED, at its defaults.
            node.port.postMessage({
              type: 'chain',
              pedals: [{ id: `${pedal.type}-1`, type: pedal.type, engaged: true, params: pedal.params }],
            });
            node.port.postMessage({ type: 'oversampling', factor: 4 });
            // Default amp: Clean 120 at its opening knobs.
            node.port.postMessage({ type: 'ampModel', model: 0 });
            node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
            node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });
            node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });
            node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.6 });
            node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
          });
          // The standard note: 220 Hz at −20 dBFS (a normal DI single-note level).
          const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
          const g = new GainNode(ctx, { gain: 0.1 });
          osc.connect(g).connect(node).connect(ctx.destination);
          osc.start();
          const buffer = await ctx.startRendering();
          return buffer.getChannelData(0).slice();
        }

        function metrics(d: Float32Array) {
          const start = Math.floor(0.3 * sampleRate); // past declick + smoothers
          let sum = 0, peak = 0, nan = false;
          for (let i = 0; i < d.length; i++) {
            if (!Number.isFinite(d[i])) nan = true;
            const a = Math.abs(d[i]);
            if (a > peak) peak = a;
            if (i >= start) sum += d[i] * d[i];
          }
          const rms = Math.sqrt(sum / (d.length - start));
          return { rmsDb: 20 * Math.log10(rms + 1e-12), peak, nan };
        }

        const out: Record<string, { rmsDb: number; peak: number; nan: boolean }> = {};
        for (const p of pedals) out[p.type] = metrics(await render(p));
        return out;
      },
      { pedals: DIRT_PEDALS.map((p) => ({ type: p.type, params: { ...p.params } })) },
    );

    for (const p of DIRT_PEDALS) {
      const m = results[p.type];
      testInfo.annotations.push({
        type: 'measurement',
        description: `${p.type}: rms ${m.rmsDb.toFixed(1)} dBFS, peak ${m.peak.toFixed(3)}`,
      });
      expect(m.nan, `${p.type}: NaN in output`).toBe(false);
      expect(m.peak, `${p.type}: exceeded the limiter ceiling`).toBeLessThanOrEqual(1.0);
      expect(m.rmsDb, `${p.type}: below the audible window`).toBeGreaterThan(PEDAL_RMS_DB.lo);
      expect(m.rmsDb, `${p.type}: above the sanity window`).toBeLessThan(PEDAL_RMS_DB.hi);
    }
  });

  // --- Amp voices cycled the same way (pedal bypassed, defaults). ---------------
  test('each amp voice at defaults: audible, finite, inside its level window', async ({
    page,
  }, testInfo) => {
    await page.goto('/');
    await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

    const results = await page.evaluate(async () => {
      const sampleRate = 48000;
      const seconds = 1.0;

      async function render(model: number) {
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
          node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true }); // straight into the amp
          node.port.postMessage({ type: 'ampModel', model });
          // rig.ts AMP_KNOB_DEFAULTS (shared ids; JCM-only ids are ignored elsewhere).
          node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });  // volume
          node.port.postMessage({ type: 'param', unit: 'amp', id: 1, value: 0.5 });  // bass
          node.port.postMessage({ type: 'param', unit: 'amp', id: 2, value: 0.5 });  // mid
          node.port.postMessage({ type: 'param', unit: 'amp', id: 3, value: 0.6 });  // treble
          node.port.postMessage({ type: 'param', unit: 'amp', id: 10, value: 0.5 }); // JCM gain
          node.port.postMessage({ type: 'param', unit: 'amp', id: 11, value: 0.5 }); // presence / AC30 CUT
          node.port.postMessage({ type: 'param', unit: 'amp', id: 12, value: 0.4 }); // JCM master
          node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 });    // cab on -> latency echo
        });
        const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
        const g = new GainNode(ctx, { gain: 0.15 });
        osc.connect(g).connect(node).connect(ctx.destination);
        osc.start();
        const buffer = await ctx.startRendering();
        return buffer.getChannelData(0).slice();
      }

      function metrics(d: Float32Array) {
        const start = Math.floor(0.3 * sampleRate);
        let sum = 0, peak = 0, nan = false;
        for (let i = 0; i < d.length; i++) {
          if (!Number.isFinite(d[i])) nan = true;
          const a = Math.abs(d[i]);
          if (a > peak) peak = a;
          if (i >= start) sum += d[i] * d[i];
        }
        const rms = Math.sqrt(sum / (d.length - start));
        return { rmsDb: 20 * Math.log10(rms + 1e-12), peak, nan };
      }

      const names = ['clean120', 'jcm800', 'twin', 'ac30'];
      const out: Record<string, { rmsDb: number; peak: number; nan: boolean }> = {};
      for (let m = 0; m < 4; m++) out[names[m]] = metrics(await render(m));
      return out;
    });

    for (const name of ['clean120', 'jcm800', 'twin', 'ac30']) {
      const m = results[name];
      testInfo.annotations.push({
        type: 'measurement',
        description: `${name}: rms ${m.rmsDb.toFixed(1)} dBFS, peak ${m.peak.toFixed(3)}`,
      });
      expect(m.nan, `${name}: NaN in output`).toBe(false);
      expect(m.peak, `${name}: exceeded the limiter ceiling`).toBeLessThanOrEqual(1.0);
      expect(m.rmsDb, `${name}: below the audible window`).toBeGreaterThan(AMP_RMS_DB.lo);
      expect(m.rmsDb, `${name}: above the sanity window`).toBeLessThan(AMP_RMS_DB.hi);
    }
  });

  // --- 4. Stomping a pedal on/off mid-note must not click. ----------------------
  // Pre-M11 the worklet flipped `engaged` instantly: a clean −20 dBFS tone and a
  // driven RAT output could differ by hundreds of millivolts at the flip sample —
  // an audible pop. The fix routes stomps through the shared raised-cosine
  // declick (like chain edits), so the largest sample-to-sample jump across the
  // toggle must stay in the same ballpark as steady-state signal slew.
  test('stomp declick: engaging/disengaging a pedal mid-note does not click', async ({
    page,
  }) => {
    await page.goto('/');
    await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

    const result = await page.evaluate(async () => {
      const sampleRate = 48000;
      const seconds = 1.2;

      async function render(toggles: boolean) {
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
          // Start DISENGAGED: the player has just dropped a RAT on the board.
          node.port.postMessage({
            type: 'chain',
            pedals: [{ id: 'rat-1', type: 'rat', engaged: false, params: { distortion: 0.7, filter: 0.4, level: 0.8 } }],
          });
          node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
          node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
        });

        if (toggles) {
          // Stomp ON mid-note, then OFF again. suspend() parks the offline render
          // so the port message lands deterministically before resume.
          ctx.suspend(0.4).then(async () => {
            node.port.postMessage({ type: 'bypass', unit: 'pedal', pedalId: 'rat-1', on: false });
            await new Promise((r) => setTimeout(r, 50));
            await ctx.resume();
          });
          ctx.suspend(0.8).then(async () => {
            node.port.postMessage({ type: 'bypass', unit: 'pedal', pedalId: 'rat-1', on: true });
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

      function maxDelta(d: Float32Array, t0: number, t1: number) {
        const a = Math.max(1, Math.floor(t0 * sampleRate)), b = Math.min(d.length, Math.floor(t1 * sampleRate));
        let m = 0;
        for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
        return m;
      }
      function anyNaN(d: Float32Array) {
        for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
        return false;
      }
      function rms(d: Float32Array) {
        let s = 0;
        for (let i = 0; i < d.length; i++) s += d[i] * d[i];
        return Math.sqrt(s / d.length);
      }

      const steady = await render(false);   // never toggled: clean tone throughout
      const toggled = await render(true);   // stomped on at 0.4 s, off at 0.8 s
      // Steady-state slew reference: the larger of the clean tone's slew and the
      // ENGAGED (driven) segment's own slew (a clipped wave legitimately slews
      // faster than a sine — that is tone, not a click).
      const baseline = Math.max(
        maxDelta(steady, 0.3, 1.2),
        maxDelta(toggled, 0.5, 0.75),  // settled engaged segment
      );
      return {
        nan: anyNaN(toggled),
        rms: rms(toggled),
        baseline,
        // The toggle windows: any instant flip shows up as a step far above slew.
        toggleDelta: Math.max(maxDelta(toggled, 0.38, 0.5), maxDelta(toggled, 0.78, 0.9)),
      };
    });

    expect(result.nan).toBe(false);
    expect(result.rms).toBeGreaterThan(0.005); // the note is audibly there
    // The stomp transient stays in the ballpark of steady-state slew: the
    // raised-cosine declick means no step. (An instant flip measured ~an order
    // of magnitude above the sine's slew before the M11 worklet fix.)
    expect(result.toggleDelta).toBeLessThan(result.baseline * 2.0 + 0.02);
  });

  // --- Amp power stomp rides the same bracket (the other half of the M11 fix). --
  test('stomp declick: amp power on/off mid-note does not click', async ({ page }) => {
    await page.goto('/');
    await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

    const result = await page.evaluate(async () => {
      const sampleRate = 48000;
      const seconds = 1.0;

      async function render(toggles: boolean) {
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
          node.port.postMessage({ type: 'bypass', unit: 'pedal', on: true }); // pedal out of the way
          node.port.postMessage({ type: 'param', unit: 'amp', id: 0, value: 0.4 });
          node.port.postMessage({ type: 'param', unit: 'amp', id: 5, value: 1 }); // cab on -> latency echo
        });
        if (toggles) {
          ctx.suspend(0.5).then(async () => {
            node.port.postMessage({ type: 'bypass', unit: 'amp', on: true }); // power OFF mid-note
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

      function maxDelta(d: Float32Array, t0: number, t1: number) {
        const a = Math.max(1, Math.floor(t0 * sampleRate)), b = Math.min(d.length, Math.floor(t1 * sampleRate));
        let m = 0;
        for (let i = a; i < b; i++) { const x = Math.abs(d[i] - d[i - 1]); if (x > m) m = x; }
        return m;
      }
      function anyNaN(d: Float32Array) {
        for (let i = 0; i < d.length; i++) if (!Number.isFinite(d[i])) return true;
        return false;
      }

      const steady = await render(false);
      const toggled = await render(true);
      const baseline = Math.max(maxDelta(steady, 0.3, 1.0), maxDelta(toggled, 0.7, 1.0));
      return {
        nan: anyNaN(toggled),
        baseline,
        toggleDelta: maxDelta(toggled, 0.48, 0.62),
      };
    });

    expect(result.nan).toBe(false);
    expect(result.toggleDelta).toBeLessThan(result.baseline * 2.0 + 0.02);
  });
});
