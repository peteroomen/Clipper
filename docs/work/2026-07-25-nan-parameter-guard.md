# NaN Parameter Guard + Engine Reset Path

**Date:** 2026-07-25
**Branch:** fix/nan-parameter-guard
**Roadmap item:** 2026-07-24 audit, finding 1 (Critical) — "One NaN parameter permanently bricks the rig — and the assistant can send one"

## Goal

A non-finite parameter can never reach a recursive state, and a poisoned engine
can be recovered without destroy + recreate.

## Approach

Three layers, because the audit's root cause is that every layer is transparent
to NaN (`v < 0 ? 0 : (v > 1 ? 1 : v)` is false on both comparisons, so NaN falls
through; `std::clamp` and `Math.min/Math.max` behave the same way).

1. **One shared, NaN-rejecting clamp helper** — `core/include/clipper/dsp/ParamGuard.h`.
   The whole codebase had ~14 private copies of the broken clamp. The helper uses
   the inverted-comparison form `v > hi ? hi : (v > lo ? v : lo)`, which is
   **bit-identical to the old clamp for every finite input** (so this is not a
   tone change) and returns `lo` for NaN. Every private `clamp01` / `std::clamp`
   at a parameter entry point is replaced by a call to it, so there is one place
   to be right.
2. **A hard gate at the C ABI** (`core/src/clipper_c_api.cpp`): every
   `*_set_param` export early-returns on `!std::isfinite(value)`. This is the
   chokepoint that is guaranteed to be on the web path.
3. **Reset exports** so the front-end has a recovery path. `reset()` restores
   recursive state to the *cached, already-solved* DC operating point — it never
   re-solves. `TriodeStage::prepare()` costs ~50 k settling samples per stage
   (~69 ms for a whole `Jcm800Amp`); `reset()` must not be that.

Plus the two front-end boundaries: the worklet `param` path (mirroring the
existing `Number.isFinite` guard on the input-trim handler five lines away) and
the three TypeScript clamps.

Fidelity: **neutral by construction.** The clamp change is bit-identical for
finite inputs; the ABI gate only rejects values that previously destroyed audio;
`reset()` is new API that nothing on the audio path calls.

## Steps

- [x] Add `ParamGuard.h` (NaN-rejecting `clampParam01` / `clampParam`)
- [x] Replace every parameter clamp in `core/src` + `core/include` (grep `clamp`,
      not just the three sites the audit names)
- [x] `if (!std::isfinite(value)) return;` in every `*_set_param` C ABI export
- [x] `reset()` down the whole tree: `Oversampler`, `OnePoleSmoother`,
      `TriodeStage`, `BjtStage`, `LtpInverter`, the three preamps, the three
      power amps, the four amp voices, all six pedals, `Processor`
- [x] `clipper_reset` / `rat_reset` / `sd_reset` / `ts_reset` / `phaser_reset` /
      `muff_reset` / `gold_reset` / `amp_reset` exports
- [x] Guard the worklet `param` path
- [x] Fix `tools.ts`, `App.tsx`, `Knob.tsx`
- [x] New ctest target `clipper_nan_guard_tests`
- [x] Prove the test fails against pristine `core/`

## How this will be measured

`core/tests/test_nan_guard.cpp`, a new ctest target. The number is
**non-finite samples per 48 000-sample window**, before → after, for every unit ×
every parameter × {NaN, +Inf, -Inf}, and the **max deviation from an unpoisoned
reference render** after a subsequent good parameter write (proving the poison
left no residue, not merely that the output is finite).

The reset path is measured the same way: poison by pushing a NaN **audio sample**
through (a vector the parameter guard cannot cover), then `*_reset()`, then assert
the render matches a freshly-constructed instance's render.

Acceptance: the test must FAIL against unmodified `core/`. Verified by restoring
`core/src` + `core/include` from `main` and re-running.

## Manual test steps

- [x] `ctest --test-dir build --output-on-failure` — 16 existing targets + the new one
- [x] `cd web && npx tsc --noEmit`
- [ ] Browser: with the app running, have the assistant set a knob to `"max"`
      (a non-numeric emission) — audio must keep playing and the knob must not move
- [ ] Edge case: `postMessage({type:'param',unit:'amp',id:0,value:NaN})` straight
      at the worklet — audio must be unaffected
- [ ] Edge case: after a poisoning event, `amp_reset` restores audio without a
      dropout longer than one render quantum

## Out of scope for this session

- The other two shipping-blockers (cab-swap allocation on the audio thread;
  `CabConvolver` non-128-multiple corruption)
- Wiring a UI "reset engine" affordance / watchdog that *calls* the new reset
  exports — this slice ships the mechanism, not the policy
- `native/src/ClipperEngine` reset parity (the native engine calls the core
  `setParameter` directly, so it inherits the clamp fix; it has no reset seam yet)
- The WASM artifact rebuild (handled centrally by the orchestrator)
- Signal-path (as opposed to parameter-path) NaN hardening, e.g.
  `OutputLimiter::clamp1`, which is also NaN-transparent

---

## What actually happened

Went as planned; the surprises were all in the **test**, not the fix.

- **The clamp fix was broader than the audit's three sites.** Grepping for `clamp`
  across `core/` turned up ~14 NaN-transparent parameter clamps, not 3. The ones the
  audit did not name and that matter most: the `setKnobs` pot-fraction clamps in all
  three tone stacks (`std::clamp(v, 1e-3, 1-1e-3)`) — a NaN pot fraction stamps a NaN
  conductance into the MNA matrix and its **inverse**, so every sample forever after
  is NaN — the three `audioTaper` laws, the three `clampR` resistance floors,
  `OptoTremolo`'s two knobs, and `Processor::setParameter`'s `[0,2]` range clamp.
  Also `AmpModel`'s `PARAM_CHORUS_MODE`, which fed a raw parameter to
  `std::lround()` — unspecified for NaN — hence `paramToInt()`.

- **Two false alarms while writing the test, both worth recording** (they are in
  docs §28 so the next session doesn't rediscover them as bugs):

  1. *Knob hysteresis.* My first design compared "poisoned, then good value written
     back" against a never-touched reference and demanded near-exactness. The JCM
     failed at 3.2e-2. It turned out an **in-range** 0.5 → 1.0 → 0.5 MASTER sweep
     leaves bit-identically the same residue (blocking caps τ = 22 ms, rail
     τ = 7.5 ms; converges to 3.5e-6 given a 3 s settle instead of 0.4 s). Fixed by
     changing the claim to an **equivalence**: a non-finite write must be
     bit-identical to writing the in-range value it documents itself as clamping to.
     That is exact, tolerance-free, and immune to hysteresis and to LFO phase.
  2. *Smoother ULP stall.* `reset()` was not bit-identical to a settled fresh unit
     (3.19e-6 on the RAT). Bisected it: a `OnePoleSmoother` settling toward its target
     **stalls one float ULP short forever**, because `value_ += coeff*(target_-value_)`
     underflows to `+= 0` long before the residual reaches `next()`'s 1e-30 denormal
     snap. `reset()` snaps exactly, so it is marginally *more* accurate than settling.
     Fixed by having block C compare reset-against-reset, where bit-exactness is the
     right bar.

- **Deliberate behaviour change:** the ABI now rejects `Inf` as well as NaN, where it
  previously clamped `Inf` to the rail. Argued in ADR 002.

- The 17-target suite, the goldens, Playwright 70/70 and all three node suites are
  green, which is the independent evidence that swapping ~14 clamps for one shared
  helper really is fidelity-neutral.

## Measured results

**The finding's own metric — non-finite samples in a 1 s window at 48 kHz, after ONE
NaN written to parameter 0.** Third column is the audit's key claim (a good value
never cleared it); it is now moot.

| unit | before: after 1 NaN | before: after a GOOD value back | after (both) |
|---|---|---|---|
| `RatModel`    | 47999/48000 | 48000/48000 | **0/48000** |
| `SdModel`     | 48000/48000 | 48000/48000 | **0/48000** |
| `TsModel`     | 48000/48000 | 48000/48000 | **0/48000** |
| `MuffModel`   | 48000/48000 | 48000/48000 | **0/48000** |
| `GoldModel`   | 48000/48000 | 48000/48000 | **0/48000** |
| `PhaserModel` | 47999/48000 | 48000/48000 | **0/48000** |
| `AmpModel`    | 48000/48000 | 48000/48000 | **0/48000** |
| `Jcm800Amp`   | 47996/48000 | 48000/48000 | **0/48000** |
| `TwinAmp`     | 47999/48000 | 48000/48000 | **0/48000** |
| `Ac30Amp`     | 47999/48000 | 48000/48000 | **0/48000** |

(The audit measured three units; all ten behave the same way.)

**Test-fails-before-fix, the acceptance criterion.** With `core/src` + `core/include`
restored from `main` and `-DCLIPPER_NAN_TEST_NO_RESET` (so blocks A and B link without
the new `reset()` API):

- Block A aborts on its **first** combination:
  `FAIL rat_* (ABI) param 0 <- NaN : 14399/14400 non-finite samples`, exit 134.
- With block A disabled, block B aborts equivalently:
  `FAIL RatModel (direct C++) param 0 <- NaN : 14399/14400 non-finite samples`, exit 134.
- With the fix restored: **all three blocks pass**, 210 ABI combinations + 153 direct
  C++ combinations + 20 reset recoveries.

**Guard strength (after).** Through the C ABI, a non-finite write to any of the 210
(unit, param, poison) combinations leaves the following render **bit-identical**
(max deviation `0.0e+00`) to a run that never received it — all ten units.

**Recovery.** All 20 units (10 via ABI, 10 via C++) poison to **14400/14400
non-finite** from a single NaN *audio* sample, and after `reset()` render
**bit-identical** to a clean instance that was also reset. `reset()` shifts a healthy
unit's level by 0.0000–0.0002 dB (0.0387 dB for the Twin, whose tremolo LFO restarts).

**Recovery cost — the number that justified `cachePark()`/`parkState()`:**

| | measured |
|---|---|
| `Jcm800Amp::prepare()` | 87.59 ms |
| `Jcm800Amp::reset()` | **0.0003 ms** |
| ratio | **~302 000× cheaper** |

**Suites:** ctest **17/17** (new target 236 s), `tsc --noEmit` + `vite build` clean,
Playwright **70/70**, `test:server` 11/11, `test:history` 10/10, electron 16/16. One
pre-existing compiler warning in the tree, unchanged.

## Files created / modified

**Created**
- `core/include/clipper/dsp/ParamGuard.h` — the one shared parameter clamp
- `core/tests/test_nan_guard.cpp` — the acceptance suite (`clipper_nan_guard_tests`)
- `docs/decisions/002-non-finite-parameters-are-rejected-not-clamped.md`
- this plan file

**Modified — the guard**
- `core/src/clipper_c_api.cpp` — `CLIPPER_REJECT_NON_FINITE` in all 8 `*_set_param`
  exports, plus the 8 new `*_reset` exports
- clamp fixes: `AmpModel`, `RatModel`, `OverdriveEngine`, `SdModel`/`TsModel`
  (via the engine), `MuffModel`, `GoldModel`, `PhaserModel`, `ChorusModel`,
  `ReverbModel`, `OptoTremolo`, `Jcm800Preamp`, `Jcm800PowerAmp`, `TwinPreamp`,
  `TwinPowerAmp`, `Ac30Preamp`, `Ac30PowerAmp`, `Processor`

**Modified — the reset tree**
- `OnePoleSmoother.h`, `Oversampler.h` (new `reset()`)
- `TriodeStage.{h,cpp}`, `BjtStage.{h,cpp}` (new `reset()` + `cachePark()`)
- `Jcm800PowerAmp`, `TwinPowerAmp`, `Ac30PowerAmp` (new `reset()` + `parkState()`;
  `LtpInverter::reset()`)
- `Jcm800Preamp`, `TwinPreamp`, `Ac30Preamp`, `Jcm800Amp`, `TwinAmp`, `Ac30Amp`,
  `AmpModel`, `RatModel`, `OverdriveEngine`, `SdModel`, `TsModel`, `MuffModel`,
  `GoldModel`, `Processor` (new `reset()`); `ChorusModel`, `ReverbModel`,
  `PhaserModel`, `OptoTremolo` (existing `reset()` extended to snap smoothers)

**Modified — front end**
- `web/worklet/clipper-processor.js` — `Number.isFinite` guard on the `param` path
- `web/src/assistant/tools.ts`, `web/src/App.tsx`, `web/src/components/Knob.tsx`,
  `web/src/params.ts` — finite-safe clamps

**Modified — docs/build**
- `core/CMakeLists.txt` — the new test target
- `docs/DEVELOPMENT.md` — new §28
- `CLAUDE.md` — Current State

## Deferred to next session

- **Nothing calls the reset exports yet.** This slice ships the mechanism, not the
  policy. A follow-up should add either (a) a worklet watchdog that notices
  non-finite output in `process()` and schedules a declick-bracketed `*_reset`, or
  (b) an explicit "reset engine" affordance. (a) is the better fix and needs a cheap
  per-block finite check plus the existing declick machinery.
- **`native/src/ClipperEngine` has no reset seam.** It inherits the clamp fix (it
  calls model `setParameter()` directly, which is exactly why fixing the clamps at
  source mattered) but cannot be recovered without a re-`prepare`. Adding
  `ClipperEngine::reset()` is a small parity slice.
- **The signal path is still NaN-transparent.** `OutputLimiter::clamp1` has the same
  broken `v > 1 ? 1 : (v < -1 ? -1 : v)` shape. Out of scope here (this slice is
  parameters) but it is the last line of defence before the DAC and should be fixed.
- **WASM artifact rebuild.** `core/` and `web/worklet/` both changed, so
  `web/public/generated/{clipper.js,clipper-processor.js}` are stale and must be
  regenerated with `bash scripts/build-wasm.sh`. Handled centrally by the
  orchestrator for this slice — the Playwright run above therefore exercised the OLD
  committed engine (it passes, since nothing in it tests the new guard).
- **The new test target costs 236 s.** Acceptable (the phaser suite is 75 s) but it is
  now the slowest single target; if CI time becomes a constraint, block A's settle can
  be shortened without weakening any bar, since its claim is bit-identity and holds at
  any settle length.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
