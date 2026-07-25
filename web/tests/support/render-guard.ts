import type { Page } from '@playwright/test';

import { installNonFiniteOutputGuard } from './finite-output';

/**
 * Offline-render integrity: the FIX for the intermittent all-zero render, and the GUARD
 * that makes a silent render name itself.
 *
 * ── WHY THIS EXISTS ────────────────────────────────────────────────────────────────
 *
 * `playwright.config.ts` used to carry `retries: 2` with this explanation:
 *
 *     Chromium's OfflineAudioContext can intermittently render silence once enough
 *     AudioContexts have been created in a single browser process (a known WebAudio
 *     engine flake, not a DSP fault — the offline-render proofs pass in isolation).
 *     A couple of retries makes the suite deterministic without masking a real
 *     break (a genuine failure loses all attempts).
 *
 * The symptom was real and the diagnosis half right: the renders DO intermittently come
 * back all-zero. But the stated *mechanism* is wrong, and both of its claims failed
 * when measured (docs §41):
 *
 *  - **It is not context accumulation.** A `new OfflineAudioContext` + `OscillatorNode`
 *    -> `destination`, with no worklet, rendered correctly 16/16 times; so did a context
 *    that called `audioWorklet.addModule()` and then never used the module (16/16). Only
 *    the path that actually instantiates the `AudioWorkletNode` goes silent, and it does
 *    so on the FIRST context of a fresh page — no accumulation required. (Also:
 *    `OfflineAudioContext.prototype.close` does not exist in this Chromium, so "nobody
 *    closes the contexts" could never have been the fix.)
 *  - **"A genuine failure loses all attempts" is only true of a fault that reproduces
 *    every run.** A fault that fires on ~20 % of renders survives `retries: 2` with high
 *    probability. That is measured in docs §41, and it is the reason `retries` is now 0.
 *
 * The real mechanism: an `AudioWorkletProcessor` is installed onto the offline **render
 * thread** asynchronously, and nothing observable on the main thread reports when. The
 * `ready` and `latency` messages the tests already await are posted from the
 * *AudioWorkletGlobalScope*, which is a different thread from the one that installs the
 * processor into the render graph — so they order against the worklet, not against the
 * renderer. If `startRendering()` wins the race, Blink's handler has no processor and
 * zeroes its output; and because an offline context renders the WHOLE buffer in one
 * uninterrupted pass, the install task can never land mid-render. Hence the observed
 * all-or-nothing signature: every failing render is *exactly* 0.0 everywhere, never
 * partially correct.
 *
 * That also explains why the declick tests were the reliable ones: they all call
 * `ctx.suspend(t)` mid-render, and the suspension is a yield point at which the pending
 * install lands. Measured directly — with no barrier and a mid-render suspend at 0.25 s,
 * renders came back at HALF the expected peak, because the first 0.25 s was silent and
 * only the post-suspend remainder carried audio.
 *
 * ── THE FIX (`installOfflineRenderBarrier`) ────────────────────────────────────────
 *
 * Suspend at frame 0, start rendering, await the suspension, resume. The suspension
 * resolving is a genuine happens-before against the render thread: it proves the
 * renderer has begun and drained the tasks queued ahead of it. It is not a sleep, so it
 * does not degrade under CPU load — which matters, because every wall-clock barrier
 * measured (`setTimeout(0)` x1/x2/x4, a microtask drain, a `MessageChannel` hop, one
 * `requestAnimationFrame`) was insufficient, and the ones that did work (50 ms, two
 * rAFs) worked only by buying time.
 *
 * It is also **bit-transparent**: 0 differing samples out of 24 000, over 5 trials, on a
 * render already made race-free by a long sleep in both arms — identical peak, identical
 * max sample-to-sample delta, identical first-nonzero sample index. So it cannot perturb
 * a declick or pop assertion. It coexists with a test's own mid-render `suspend()`
 * (8/8), because the two suspensions sit at different render quanta.
 *
 * ── THE GUARD (`installSilentRenderGuard`) ─────────────────────────────────────────
 *
 * Belt to the fix's braces, and the more important half of this file. A silent render
 * must fail by *naming itself*, not by failing a spectral comparison twenty lines later.
 * Before this, an all-zero buffer surfaced as `expect(received).toBeGreaterThan(0.6)` /
 * `Received: 0`, or as `Received: Infinity` from a 0/0 ratio — a mystifying number that
 * reads like a DSP regression. Now it reads: the render produced silence, setup failed.
 *
 * Same prototype-patch shape as `finite-output.ts`, and for the same reason: it covers
 * EVERY render in the spec, present and future, and a new render site cannot opt out by
 * omission. See that file's header for the argument.
 *
 * A render that is *supposed* to be silent (the engaged-tuner mute) opts out explicitly
 * by calling `window.allowSilentRender(ctx)` — which documents the intent at the render
 * site rather than weakening the guard for everyone.
 */

/** Peak below which a render is treated as silence: −140 dBFS. */
const SILENCE_PEAK_FLOOR = 1e-7;

/**
 * Install the render-thread barrier that stops the all-zero render happening at all.
 * Call from `test.beforeEach` BEFORE `page.goto` (init scripts run at document start).
 *
 * Must be installed LAST of the `startRendering` patches so it is the outermost wrapper:
 * its `suspend(0)` has to be scheduled before the real `startRendering` is entered.
 */
export async function installOfflineRenderBarrier(page: Page): Promise<void> {
  await page.addInitScript(() => {
    const proto = OfflineAudioContext.prototype as unknown as {
      startRendering: (this: OfflineAudioContext) => Promise<AudioBuffer>;
    };
    const inner = proto.startRendering;
    proto.startRendering = async function patched(this: OfflineAudioContext) {
      let suspended: Promise<void> | null = null;
      try {
        // Schedule a suspension at frame 0. Rendering will stop before producing any
        // audio, which is what makes this free: no samples are rendered before the
        // barrier, so nothing downstream can observe it.
        suspended = this.suspend(0);
      } catch {
        // A test that already scheduled its own suspension at frame 0 would collide.
        // Nothing in the suite does, but fall back to buying time rather than throwing.
        suspended = null;
      }
      if (!suspended) {
        const rendering = inner.call(this);
        await new Promise((r) => setTimeout(r, 150));
        return rendering;
      }
      const rendering = inner.call(this);
      await suspended;
      await this.resume();
      return rendering;
    };
  });
}

/**
 * Fail any offline render that comes back silent, with a message that says so.
 * Call from `test.beforeEach` BEFORE `page.goto`.
 */
export async function installSilentRenderGuard(page: Page): Promise<void> {
  await page.addInitScript(
    ({ floor }) => {
      const allowed = new WeakSet<OfflineAudioContext>();
      (window as unknown as { allowSilentRender: (c: OfflineAudioContext) => OfflineAudioContext }).
        allowSilentRender = (ctx: OfflineAudioContext) => {
        allowed.add(ctx);
        return ctx;
      };

      const proto = OfflineAudioContext.prototype as unknown as {
        startRendering: (this: OfflineAudioContext) => Promise<AudioBuffer>;
      };
      const inner = proto.startRendering;
      proto.startRendering = async function patched(this: OfflineAudioContext) {
        const buffer = await inner.call(this);
        if (allowed.has(this)) return buffer;

        let peak = 0;
        let nonZeroChannels = 0;
        for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
          const data = buffer.getChannelData(ch);
          let chPeak = 0;
          for (let i = 0; i < data.length; i++) {
            const a = data[i] < 0 ? -data[i] : data[i];
            if (a > chPeak) chPeak = a;
          }
          if (chPeak > floor) nonZeroChannels++;
          if (chPeak > peak) peak = chPeak;
        }
        if (peak <= floor) {
          throw new Error(
            `SILENT RENDER: THE RENDER PRODUCED SILENCE — SETUP FAILED, this is not a DSP ` +
              `result. Peak across all ${buffer.numberOfChannels} channel(s) was ${peak} ` +
              `(floor ${floor}) over ${buffer.length} samples at ${buffer.sampleRate} Hz; ` +
              `${nonZeroChannels} channel(s) carried any signal.\n` +
              `An offline render is all-zero when the AudioWorkletProcessor was not yet ` +
              `installed on the render thread when startRendering() was called — see ` +
              `tests/support/render-guard.ts and docs §41. Do NOT interpret the numbers ` +
              `from this render, and do NOT relax the assertion that failed: the audio was ` +
              `never computed. Check that installOfflineRenderBarrier() is installed for ` +
              `this spec. If this render is MEANT to be silent, opt out at the render site ` +
              `with window.allowSilentRender(ctx).`
          );
        }
        return buffer;
      };
    },
    { floor: SILENCE_PEAK_FLOOR }
  );
}

/**
 * One-line adoption for a spec that renders audio: the non-finite scan
 * (`finite-output.ts`), the silence guard, and the render-thread barrier.
 *
 * Order matters — the barrier goes on last so it wraps the others.
 */
export async function installRenderGuards(page: Page): Promise<void> {
  await installNonFiniteOutputGuard(page);
  await installSilentRenderGuard(page);
  await installOfflineRenderBarrier(page);
}
