import { test, expect } from '@playwright/test';

// Proves audio actually flows through the WASM worklet without speakers/mic:
// render a 220 Hz sine through the Clipper worklet in an OfflineAudioContext at
// two gain settings and check (a) output is non-silent and (b) the RMS ratio
// between gain 1.0 and 0.5 is ~2x.

const RENDER_SECONDS = 0.5;

test('UI exposes Start audio control and a gain slider', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('button', { name: 'Start audio' })).toBeVisible();
  const slider = page.getByRole('slider');
  await expect(slider).toBeVisible();
  await expect(slider).toHaveAttribute('min', '0');
  await expect(slider).toHaveAttribute('max', '2');
});

test('audio flows through WASM worklet; gain scales RMS ~2x', async ({ page }) => {
  await page.goto('/');
  // Wait for the app shell so we know the origin serves correctly.
  await expect(page.getByRole('heading', { name: 'Clipper — M0' })).toBeVisible();

  const result = await page.evaluate(async (seconds) => {
    async function renderAtGain(gain: number): Promise<Float32Array> {
      const sampleRate = 48000;
      const length = Math.floor(sampleRate * seconds);
      const ctx = new OfflineAudioContext(1, length, sampleRate);
      await ctx.audioWorklet.addModule('/generated/clipper-processor.js');

      const node = new AudioWorkletNode(ctx, 'clipper-processor', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
      });

      // Wait for the WASM instance to be ready before rendering.
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

      node.port.postMessage({ type: 'param', id: 0, value: gain });

      const osc = new OscillatorNode(ctx, { type: 'sine', frequency: 220 });
      osc.connect(node).connect(ctx.destination);
      osc.start();

      const buffer = await ctx.startRendering();
      return buffer.getChannelData(0).slice();
    }

    // RMS over the steady-state region (skip the first 0.1 s to let the
    // one-pole gain smoothing settle).
    function rms(data: Float32Array, sr: number): number {
      const start = Math.floor(sr * 0.1);
      let sum = 0;
      let count = 0;
      for (let i = start; i < data.length; i++) {
        sum += data[i] * data[i];
        count++;
      }
      return Math.sqrt(sum / count);
    }

    const sr = 48000;
    const full = await renderAtGain(1.0);
    const half = await renderAtGain(0.5);

    return {
      rmsFull: rms(full, sr),
      rmsHalf: rms(half, sr),
      maxFull: Math.max(...Array.from(full.subarray(0, 2048)).map(Math.abs)),
    };
  }, RENDER_SECONDS);

  // (a) non-silent
  expect(result.rmsFull).toBeGreaterThan(0.01);
  expect(result.rmsHalf).toBeGreaterThan(0.005);

  // (b) gain 1.0 vs 0.5 differ by ~2x (tolerance for smoothing/measurement)
  const ratio = result.rmsFull / result.rmsHalf;
  expect(ratio).toBeGreaterThan(1.8);
  expect(ratio).toBeLessThan(2.2);
});
