import { test, expect } from '@playwright/test';

// SCRATCH PROBE 9 — deleted before commit.
// (a) Is suspend(0)/resume() around startRendering() BIT-TRANSPARENT? Both arms get a
//     long sleep first so neither races; the only difference is the suspend wrapper.
// (b) Does suspend(0) coexist with a test that ALSO suspends mid-render?

test.setTimeout(1_800_000);

test('probe9: suspend(0) transparency + coexistence', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { name: 'Clipper', exact: true })).toBeVisible();

  const out = await page.evaluate(async () => {
    const sampleRate = 48000;
    const length = Math.floor(sampleRate * 0.5);

    async function render(mode: 'plain' | 'susp', alsoSuspendMid: boolean, sleepMs: number) {
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');
      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('ready timeout')), 30000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'ready') {
            clearTimeout(t);
            resolve();
          } else if (e.data?.type === 'error') {
            clearTimeout(t);
            reject(new Error(String(e.data.message)));
          }
        };
      });
      await new Promise<void>((resolve, reject) => {
        const t = setTimeout(() => reject(new Error('latency timeout')), 30000);
        node.port.onmessage = (e: MessageEvent) => {
          if (e.data?.type === 'latency') {
            clearTimeout(t);
            resolve();
          }
        };
        node.port.postMessage({ type: 'bypass', unit: 'amp', on: true });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 0, value: 0.9 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 1, value: 0.0 });
        node.port.postMessage({ type: 'param', unit: 'pedal', id: 2, value: 1.0 });
        node.port.postMessage({ type: 'oversampling', factor: 4 });
      });
      if (sleepMs > 0) await new Promise((r) => setTimeout(r, sleepMs));

      const state = { midFired: false };
      if (alsoSuspendMid) {
        ctx.suspend(0.25).then(async () => {
          state.midFired = true;
          node.port.postMessage({ type: 'param', unit: 'pedal', id: 2, value: 0.5 });
          await new Promise((r) => setTimeout(r, 50));
          await ctx.resume();
        });
      }

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();

      let buffer: AudioBuffer;
      if (mode === 'susp') {
        const s0 = ctx.suspend(0);
        const rendering = ctx.startRendering();
        await s0;
        await ctx.resume();
        buffer = await rendering;
      } else {
        buffer = await ctx.startRendering();
      }
      return { d: buffer.getChannelData(0).slice(), midFired: state.midFired };
    }

    function maxDelta(d: Float32Array) {
      let m = 0;
      for (let i = 1; i < d.length; i++) {
        const x = Math.abs(d[i] - d[i - 1]);
        if (x > m) m = x;
      }
      return m;
    }
    function peak(d: Float32Array) {
      let m = 0;
      for (let i = 0; i < d.length; i++) {
        const x = Math.abs(d[i]);
        if (x > m) m = x;
      }
      return m;
    }

    const rows: Array<Record<string, unknown>> = [];
    for (let t = 0; t < 5; t++) {
      const a = (await render('plain', false, 300)).d;
      const b = (await render('susp', false, 300)).d;
      let diff = 0;
      let maxAbs = 0;
      for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) {
          diff++;
          const x = Math.abs(a[i] - b[i]);
          if (x > maxAbs) maxAbs = x;
        }
      rows.push({
        kind: 'transparency',
        t,
        differingSamples: diff,
        total: a.length,
        maxAbsDiff: maxAbs,
        peakPlain: Number(peak(a).toFixed(6)),
        peakSusp: Number(peak(b).toFixed(6)),
        maxDeltaPlain: Number(maxDelta(a).toFixed(6)),
        maxDeltaSusp: Number(maxDelta(b).toFixed(6)),
        firstNonZeroPlain: a.findIndex((v) => v !== 0),
        firstNonZeroSusp: b.findIndex((v) => v !== 0),
      });
    }
    for (let t = 0; t < 8; t++) {
      const r = await render('susp', true, 0);
      rows.push({ kind: 'coexist-susp0', t, peak: Number(peak(r.d).toFixed(6)), midFired: r.midFired });
    }
    for (let t = 0; t < 8; t++) {
      const r = await render('plain', true, 0);
      rows.push({ kind: 'control-nobarrier', t, peak: Number(peak(r.d).toFixed(6)), midFired: r.midFired });
    }
    return rows;
  });

  for (const r of out) console.log(JSON.stringify(r));
});
