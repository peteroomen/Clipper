import type { Page } from '@playwright/test';

/**
 * Fail any test whose offline render produces a non-finite sample — ANYWHERE, in ANY
 * channel.
 *
 * WHY THIS EXISTS (2026-07-24 audit, Test & process integrity). `web/tests/audio.spec.ts`
 * is 1751 lines and covers every pedal's DSP, and it contained **zero** finiteness checks.
 * Every assertion in it is a Goertzel bin, an RMS, or a max-delta over a WINDOW — so a NaN
 * outside that window, or in the right channel of a stereo render, produced no failure at
 * all. Worse, `NaN` propagates silently through the measurements it does make: `s += d[i]`
 * on a NaN makes the RMS NaN, and `expect(NaN).toBeGreaterThan(x)` fails with an unhelpful
 * message rather than naming the real problem. Audit finding 1 (one NaN parameter bricks
 * the rig forever) lived in exactly this blind spot.
 *
 * The obvious fix is an `allFinite()` helper called at each of ~15 render sites, but that
 * only guards the sites somebody remembers to guard, and a new render added next month gets
 * nothing. So instead this patches `OfflineAudioContext.prototype.startRendering` in the
 * page, once, before any test code runs: EVERY render in the file is scanned, in every
 * channel, over every sample, and a non-finite value throws with the channel and sample
 * index. A new render site is covered automatically and cannot opt out by omission.
 *
 * Cost is negligible — a linear scan of a buffer that was just synthesised sample by sample
 * through a WASM tube-amp model.
 *
 * Call from a `test.beforeEach`, BEFORE `page.goto` (init scripts run at document start).
 * Adopting it in another spec file is the same two lines.
 */
export async function installNonFiniteOutputGuard(page: Page): Promise<void> {
  await page.addInitScript(() => {
    const proto = OfflineAudioContext.prototype as unknown as {
      startRendering: (this: OfflineAudioContext) => Promise<AudioBuffer>;
    };
    const original = proto.startRendering;
    proto.startRendering = async function patched(this: OfflineAudioContext) {
      const buffer = await original.call(this);
      for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
        const data = buffer.getChannelData(ch);
        for (let i = 0; i < data.length; i++) {
          if (!Number.isFinite(data[i])) {
            throw new Error(
              `NON-FINITE OUTPUT: ${data[i]} at sample ${i}/${data.length} of channel ` +
                `${ch}/${buffer.numberOfChannels}. The engine emitted NaN or Inf. This is ` +
                `never acceptable regardless of where the test's measurement window sits.`
            );
          }
        }
      }
      return buffer;
    };
  });
}
