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

**The plan's own hypothesis was wrong, and so was the config comment's.** Both said
`OfflineAudioContext` resource exhaustion / context accumulation ("37 context sites and zero
`close()` calls"). Measured, that is refuted three ways: a no-worklet context rendered
correctly 16/16; a context that called `addModule` and never instantiated the node rendered
16/16; and the failing path goes silent on the **first** context of a fresh page. Also
`OfflineAudioContext.prototype.close` does not exist in this Chromium, so "close the contexts"
was never a callable fix. Recorded in docs §41 rather than quietly dropped, because a
walked-back theory is the useful part of the record.

**The real mechanism** is a race between the `AudioWorkletProcessor` being installed onto the
offline **render thread** and `startRendering()`. The `ready`/`latency` messages the specs
await are posted from `AudioWorkletGlobalScope` — a different thread from the one that installs
the processor into the render graph — so they order against the worklet, not the renderer. An
offline context renders the whole buffer in one pass, so the install can never land mid-render:
hence the all-or-nothing signature (every failing render is *exactly* 0.0 everywhere).

**The fix** is `installOfflineRenderBarrier`: suspend at frame 0, `startRendering()`, await the
suspension, resume. A genuine happens-before, not a sleep, so it does not degrade under load —
every wall-clock barrier measured was insufficient, and the ones that worked did so only by
buying time. It is bit-transparent (0 differing samples / 24 000 over 5 trials) so it cannot
perturb a declick assertion, and it coexists with a test's own mid-render `suspend()` (8/8).

**What this session added on top of the interrupted one** (which had left `render-guard.ts`,
the `PW_PORT` override and this plan file, with no spec adopting any of it):

- adopted `installRenderGuards` in **all five** specs that render audio — `audio`, `amp`,
  `cab`, `expectations`, `tuner` — via one `test.beforeEach` each, so all 37 render sites and
  any future one are covered without touching the individual helpers. `assistant.spec.ts` has
  no `OfflineAudioContext` and needs nothing;
- `tuner.spec.ts`'s engaged-tuner render is *meant* to be silent, so it opts out at the render
  site with `window.allowSilentRender(ctx)` — the guard was not loosened for anyone else;
- fixed the three resolve-on-timeout `latency` waits (`audio.spec.ts`) to **reject** with a
  message naming setup as the failure;
- `retries: 2` -> **`retries: 0`**, with the measured numbers in the comment replacing the two
  false claims that were there;
- deleted `web/tests/zzprobe.spec.ts` (scratch probe 9 — its findings are the transparency and
  coexistence numbers now recorded in the `render-guard.ts` header and docs §41, so the file
  itself earns nothing as a permanent test);
- docs §41; removed the standing `retries: 2` caveat from `CLAUDE.md` **twice** in Current
  State (a merge from `main` had duplicated the "Known gaps" bullet) plus the Automated Checks
  note, and from docs §29.

**Surprise worth recording:** the fault is nearly invisible unloaded (71/71 pass) and severe
under load (7-9 failures/run). Any measurement of it without a stated, reproducible CPU load is
worthless — which is presumably how it survived so long.

## Measured results

**Flake rate, identical harness both times** (six spinning shell loops on 4 cores, 5 full-suite
runs, `--retries=0`):

| | r1 | r2 | r3 | r4 | r5 | failures / executions | red runs |
|---|---|---|---|---|---|---|---|
| before | 9 | 7 | 7 | 9 | 9 | **41 / 355 (11.5 %)** | **5 / 5** |
| after | 0 | 0 | 0 | 0 | 0 | **0 / 355** | **0 / 5** |

15 distinct tests were affected before; the worst failed in 5/5 runs. Plus one unloaded run
after: 71/71.

**The guard's teeth** — perturbation: delete `osc.start()` from `audio.spec.ts`'s RAT render
helper (`touch`ed after both patch and restore), run the RAT test both ways:

- guard installed: `SILENT RENDER: THE RENDER PRODUCED SILENCE — SETUP FAILED, this is not a
  DSP result. Peak across all 1 channel(s) was 0 (floor 1e-7) over 24000 samples at 48000 Hz;
  0 channel(s) carried any signal.` + *"do NOT relax the assertion that failed"*
- guard removed (pre-slice state): `expect(received).toBeGreaterThan(expected)` /
  `Expected: > 0.01` / `Received: 0`

**`retries: 2` masks an intermittent fault** — a 20 %-per-render zeroing fault injected into one
single-render test, 25 repetitions per arm:

| arm | result | exit |
|---|---|---|
| `--retries=0` | 5 failed / 20 passed | 1 — caught |
| `--retries=2` | 0 failed, 6 flaky / 19 passed | **0 — GREEN** |

Six repetitions hit the fault; all six were retried into a pass. That is the direct refutation
of "a genuine failure loses all attempts".

## Files created / modified

- `web/tests/support/render-guard.ts` — the barrier + silence guard (created in the interrupted
  session; header comment is the long-form diagnosis)
- `web/tests/audio.spec.ts` — `installRenderGuards`; three resolve-on-timeout waits now reject
- `web/tests/amp.spec.ts`, `web/tests/cab.spec.ts`, `web/tests/expectations.spec.ts` —
  `installRenderGuards` in a `beforeEach`
- `web/tests/tuner.spec.ts` — `installRenderGuards` + `allowSilentRender` at the muted render
- `web/tests/zzprobe.spec.ts` — **deleted** (scratch)
- `web/playwright.config.ts` — `retries: 2` -> `0`, comment replaced with measurements;
  `PW_PORT` override
- `docs/DEVELOPMENT.md` — new §41; §29's `retries: 2` caveat marked resolved
- `CLAUDE.md` — Current State (x2) and Automated Checks caveats replaced
- `docs/work/2026-07-25-web-render-silence-guard.md` — this file

Zero application source changes: nothing under `core/`, `web/worklet/`, `web/src/`, `native/`
or `web/public/generated/`. No WASM rebuild. No audio assertion weakened — every bound is
byte-for-byte what it was.

## Deferred to next session

- **`amp twin: the optical tremolo modulates the output envelope` has a second, different
  fault.** One baseline run gave `onCV` 0.019677767321650944 vs `offCV * 4` =
  0.07871106928660378 — `onCV` *exactly* equal to `offCV`, i.e. the INTENSITY 0.0 and 0.8
  renders were bit-identical. Not silence (that gives `envCV` 0): the amp-model/param messages
  did not reach the render, the same shape as §29's `voiceDiff` exactly-0 case. It did not
  recur in 5 clean post-fix runs, so the barrier may have fixed it, but the mechanism was never
  isolated. If it returns, measure it — do not retry it.
- **`web/tsconfig.json` excludes `tests`**, so `npm run build`'s `tsc --noEmit` typechecks no
  spec at all, and Playwright's esbuild transpile does not typecheck either. A type error in a
  spec is invisible to the entire gate. Specs were hand-typechecked for this slice (clean;
  `cab.spec.ts` needs `@types/node`, which is not a dependency). Wiring this up is its own
  slice — it will want `@types/node` added and a `tsconfig.tests.json`.
- Getting `amp_prepare_cab_*` genuinely off the audio thread, and the worklet's
  `_postLatency()`-from-`process()` / `free()`-on-audio-thread residuals (§30) — untouched.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
