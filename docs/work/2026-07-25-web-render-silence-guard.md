# Web render silence guard — kill `retries: 2`

**Date:** 2026-07-25
**Branch:** test/web-render-silence-guard
**Roadmap item:** audit / docs §29 residual — "`web/playwright.config.ts:34` sets `retries: 2`, so a fault appearing in under a third of runs is retried away — the last remaining way for a real fault to vanish silently"
**Docs section:** §41 · **ADR (if needed):** 013

## Goal

A Playwright suite where a flake is a failure: find and fix the real cause of the
intermittent all-zero offline renders, make a silent render name itself instead of
failing on a bogus spectral comparison, turn the three resolve-on-timeout setup waits
into loud failures, and only then reduce `retries`.

## Approach

Evidence in hand (measured by the orchestrator on a loaded machine, `--retries=0`):
4 failures / 71 tests, two of them with `Received: 0` — i.e. **silence**, not a wrong
number. `bypassRms === 0` is impossible for a working bypass path. With `retries: 2`
the same run reported `1 failed, 2 flaky, 68 passed`.

Hypothesis to *test, not assume*: `OfflineAudioContext` resource exhaustion. There are
**37 `new OfflineAudioContext(...)` sites across the 6 specs and zero `close()` calls**.
Each one also calls `audioWorklet.addModule()`, compiling the 176 KB WASM module afresh.
Chromium appears to return zeros rather than throwing once some limit is hit.

Plan of attack, in order:

1. **Measure the mechanism.** Instrument the page: count contexts created / still
   un-closed / `ctx.state` at render time, and correlate a zero render against that
   count. Then test the fix candidates independently (close contexts; cap concurrent
   contexts; reuse one context per spec) and see whether the failure rate *moves*.
   If the mechanism is something else, fix what is actually there and say so.
2. **A silence guard, as a prototype patch** — the same shape as
   `web/tests/support/finite-output.ts`, which already patches
   `OfflineAudioContext.prototype.startRendering` so every render in a spec is covered
   including future ones. A render that comes back all-zero (or under a plausible floor)
   must throw a message that says *the render produced silence — setup failed*, with the
   peak/RMS it measured and the context ordinal. Adopt it in every spec that renders
   audio (audio, amp, cab, expectations, tuner).
3. **Fix the three resolve-on-timeout waits** (`audio.spec.ts:293`, `:419`, `:958`) so a
   setup timeout rejects. These are an *independent* defect — they cannot explain
   failures 1–3, which already reject on timeout.
4. **Only then** reduce `retries`. Target `retries: 0`. `retries: 1` is acceptable only
   with a measured residual rate written into the comment; never 2.

**This is a test + config slice: zero application source changes.** Not touching
`core/`, `web/worklet/`, `native/`, `web/public/generated/`, or any file under
`web/src/` (two sibling web slices own `Knob`/`Board`/`Tuner`/`tokens.css` and
`App.tsx`/`rig.ts`/`assistant/`). If I find an application bug I report it, I don't fix
it here.

## Steps

- [ ] Baseline: run the full suite with `--retries=0` >=5x under a *reproducible* CPU
      load (a scripted N-way busy-loop, same N every run), record failures per run
- [ ] Instrument: live/created `OfflineAudioContext` count + `ctx.state` at the moment a
      render returns silence; capture the number at the point of failure
- [ ] Confirm or refute the exhaustion hypothesis with that number
- [ ] Write `web/tests/support/silent-output.ts` (prototype patch, throws naming silence)
- [ ] Adopt the guard in every spec that renders audio
- [ ] Fix the three `setTimeout(() => resolve(), 6000)` sites to reject
- [ ] Apply the real fix for the mechanism found
- [ ] Re-measure: >=5x `--retries=0` under the *same* load
- [ ] Perturbation proof: break setup deliberately (drop the `chain` message), confirm
      the guard fires with its clear message, revert, `touch` both times
- [ ] Set `retries` per the measured residual rate
- [ ] Docs: §41, `CLAUDE.md` Current State + the Automated Checks note, and the docs §29
      caveat — all three must stop saying "`retries: 2` is the last remaining way..."

## How this will be measured

- **Failures per run, `--retries=0`, >=5 runs, before -> after**, under an explicitly
  stated and reproducible CPU load. One green run proves nothing.
- **The resource number at the point of failure** (live `OfflineAudioContext` count, or
  whatever the real limit turns out to be), before -> after.
- **The guard's teeth**: a deliberately broken setup must fail with the silence message,
  not a spectral comparison. Recorded as the exact perturbation + the message emitted.

## Manual test steps

- [ ] `cd web && npx playwright test --retries=0` -> all pass under load
- [ ] Edge case: drop the `chain` message from one render's setup -> the failure names
      *silence / setup failed*, and does NOT read as a DSP result
- [ ] Edge case: make a `latency` echo never arrive -> the setup wait **rejects**, and the
      test fails on the timeout rather than rendering an unconfigured graph
- [ ] `cd web && npm run build` (includes `tsc --noEmit`) is clean

## Out of scope for this session

- Any change under `core/`, `web/worklet/`, `native/`, `web/public/generated/` — and no
  WASM rebuild
- Any change to `web/src/**` application source (sibling slices own those files)
- Weakening any audio assertion. `toBeGreaterThan(0.6)` stays `0.6`
- Fixing whatever application bug the instrumentation turns up (report only)

---

<!-- Fill in below during/after the session -->

## What actually happened

(pending)

## Measured results

(pending)

## Files created / modified

(pending)

## Deferred to next session

(pending)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
