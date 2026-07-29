# ADR 013: A flake is a failure — `retries: 0`, and render integrity is enforced by a prototype patch

Date: 2026-07-25
Status: Accepted

## Context

`web/playwright.config.ts` carried `retries: 2`, justified by a comment claiming that (a) the
intermittent all-zero `OfflineAudioContext` render was an unfixable WebAudio engine quirk
caused by accumulating `AudioContext`s, and (b) retries could not hide a real break because
"a genuine failure loses all attempts". Docs §29 flagged this as the last remaining way for a
real fault to disappear silently, and deliberately did not change it unilaterally.

Measured (docs §41), both claims are false:

- The silence has a real, fixable cause: an `AudioWorkletProcessor` is installed onto the
  offline **render thread** asynchronously, and the `ready`/`latency` messages the specs await
  are posted from `AudioWorkletGlobalScope` — a different thread — so they never ordered
  against the renderer. Not accumulation: a no-worklet context rendered 16/16, and the failing
  path goes silent on the *first* context of a fresh page. (`OfflineAudioContext.prototype.close`
  does not even exist in this Chromium, so the comment's implied fix was never callable.)
- With a 20 %-per-render fault injected into one single-render test, 25 repetitions per arm:
  `--retries=0` gave 5 failed / 20 passed (exit 1); `--retries=2` gave 0 failed, 6 flaky / 19
  passed, **exit 0**. Retries masked the fault completely.

Under a fixed CPU load the real fault cost **41 failures / 355 test-executions across 5 runs,
with 5 of 5 runs red** — and unloaded the same suite passes 71/71, which is why it survived.

## Decision

1. **`retries: 0`.** A flake is a failure. If a residual rate ever forces this above 0, it must
   be `1`, with the measured rate written into the config comment — never `2`.
2. **Fix the race, don't paper over it.** `installOfflineRenderBarrier` (suspend at frame 0,
   `startRendering()`, await the suspension, resume) is a genuine happens-before against the
   render thread, so it does not degrade under load. Every wall-clock barrier measured was
   insufficient. It is bit-transparent (0 differing samples / 24 000 over 5 trials), so it
   cannot perturb a declick or pop assertion.
3. **A silent render must name itself.** `installSilentRenderGuard` fails any render whose peak
   is at or below −140 dBFS with a message saying *the render produced silence, setup failed,
   do not interpret these numbers and do not relax the assertion that failed*.
4. **Render guards are installed as prototype patches in a `beforeEach`, not as helpers called
   at each render site** — the same shape as `finite-output.ts`. Every render in a spec is
   covered, including ones added later, and a new render site cannot opt out by omission.
   There are 37 `new OfflineAudioContext` sites across the specs; guarding them individually
   guards only the ones somebody remembers.
5. **A render that is legitimately silent opts out at the render site** with
   `window.allowSilentRender(ctx)` — never by weakening the guard or the floor. Today the only
   such render is `tuner.spec.ts`'s engaged-tuner mute.
6. **A setup barrier rejects on timeout; it never resolves.** Resolving let a timed-out setup
   render an unconfigured graph and report the result as an audio failure.

## Consequences

**Easier.** An intermittent WebAudio fault now fails loudly and legibly rather than being
retried into silence, and a silent render is diagnosed in its own error message instead of
costing a debugging session chasing a phantom DSP regression. Adopting the guards in a new spec
is one `beforeEach` line. The barrier being bit-transparent means it is safe in the declick and
pop specs, which are the most sensitive in the suite.

**Harder.** With `retries: 0`, any genuinely non-deterministic fault will now break CI, and the
temptation will be to raise `retries` — which is why the measured refutation lives in the config
comment itself. Every spec that renders audio must remember the `beforeEach`; a spec added
without it silently loses both the fix and the guard (the barrier's absence shows up as flakes,
which is at least loud). The guards patch `OfflineAudioContext.prototype` for the whole page, so
they also apply to application renders (`web/src/cab.ts`'s IR resample) — intended, but it means
the silence floor is a page-wide contract, not a per-test one.

**Not addressed.** `web/tsconfig.json` excludes `tests`, so no spec is typechecked by
`npm run build` and Playwright's esbuild transpile does not typecheck either — a type error in a
spec, including in these guards, is invisible to the whole gate. That is its own slice.
