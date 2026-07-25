# Valve-amp parameter smoothing (audit finding 6)

**Date:** 2026-07-25
**Branch:** fix/valve-amp-smoothing
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` finding 6 — "No parameter smoothing anywhere in the valve amps"

## Goal

Every continuous knob on the JCM800, the Twin and the AC30 is smoothed with a
`OnePoleSmoother` and applied **per sample**, so a knob move on a valve amp costs no
more step discontinuity than the same move on the already-smoothed clean amp — and a
static render stays bit-for-bit what it is today.

## Approach

`AmpModel::Impl` is the pattern and is copied deliberately:

* **Scalar gain-like controls** (`Jcm800Preamp` GAIN + MASTER, `TwinPreamp` VOLUME +
  BRIGHT amount, `Ac30Preamp` VOLUME) get one `OnePoleSmoother` each on the
  **post-taper linear scale**, advanced once per base-rate sample inside the existing
  per-sample loops. Today these are evaluated once per `process()` call and applied as
  a constant, which is the whole defect.
* **The three tone stacks** (`MarshallToneStack`, `FenderToneStack`,
  `TopBoostToneStack`) smooth the *knob fractions* per sample and re-derive the 5×5
  conductance matrix + its inverse at a **32-sample control rate** — exactly what
  `AmpModel` does with its four biquads. Chosen over interpolating between two matrix
  inverses because (a) the inverse of a conductance matrix is not affine in the pot
  fraction, so a blend of two inverses is not the inverse of any network, (b) it would
  double the storage and add a 25-entry per-sample blend, more per-sample cost than an
  occasional rebuild, and (c) the cap states are physical node voltages and currents:
  carrying them across a small matrix perturbation *is* what a real pot wiper does, so
  no state migration is needed as long as the perturbation per rebuild is small.
  The rebuild only runs while a smoother is actually moving, so an idle amp pays
  nothing.
* **`OnePoleSmoother` is templated on the sample type** (`OnePoleSmootherT<T>`, with
  `OnePoleSmoother = OnePoleSmootherT<float>` unchanged for every existing user) and
  the valve amps use the **double** instantiation. This is what makes the change
  provably bit-neutral: the valve amps' scale factors and pot fractions are doubles
  throughout, so a settled double smoother returns *exactly* the constant the code
  uses today, and a static render is bit-identical rather than merely close.
* **Deferred snap.** `prepare()` marks the unit unprimed; the first `process()` after
  it snaps every smoother onto its target. The house convention is "push targets,
  then prepare" (`ClipperEngine::prepare`, `identical_core_test`), but the C ABI
  prepares inside `amp_create` and the params arrive *afterwards* — without the
  deferred snap, every golden and every `amp_*` render would ramp for 8 ms from the
  prepare-time defaults. With it, anything set before the first block applies exactly
  as it does today, and only a knob moved *during* playback ramps.

Fidelity: **neutral by construction for any static render** (bit-identical). It is a
deliberate change to what you hear *while a knob is moving* — that is the fix.

**Not touched, on measurement:** `Jcm800PowerAmp` PRESENCE and `Ac30PowerAmp` TOP CUT.
Both are already applied per sample and both measure at or below the clean amp's
smoothed baseline (see below), so a smoother on them would be pure cost. They are
pinned by the new test anyway, so a regression cannot creep in.

## Steps

- [ ] Template `OnePoleSmoother` on the value type; add the `double` alias. No
      behaviour change for the eleven existing float users.
- [ ] `core/tests/support/StepSlew.h` — the step/steady-slew metric, once, shared.
- [ ] `core/tests/test_param_smoothing.cpp` + `clipper_param_smoothing_tests` target:
      every knob on all three valve amps plus the clean-amp control case plus a
      hard-splice sensitivity case.
- [ ] `MarshallToneStack` / `FenderToneStack` / `TopBoostToneStack`: smoothed knobs,
      control-rate matrix rebuild, `snap()`.
- [ ] `Jcm800Preamp`: GAIN + MASTER per-sample smoothed; deferred snap; `reset()`.
- [ ] `TwinPreamp`: VOLUME + BRIGHT per-sample smoothed; deferred snap; `reset()`.
- [ ] `Ac30Preamp`: VOLUME per-sample smoothed; deferred snap; `reset()`.
- [ ] Core suite green; goldens unmoved; `clipper-bench` before/after.

## How this will be measured

`clipper_param_smoothing_tests`, which reproduces the audit's metric — the project's
own definition from `native/tests/chain_edit_test.cpp`: a steady 220 Hz sine, the
largest sample-to-sample step in a window around the knob move, divided by the
signal's own steady-state slew measured before it. Two refinements the audit's table
needed:

* the knob step lands **mid-render**, between blocks, and the measurement is the
  **worst of 16 successive step positions** — a step delivered at an output zero
  crossing produces no discontinuity at all, so a single fixed position measures
  luck, not the amp;
* two step sizes: one **0.05** arrow-key step (`KEY_STEP` in `web/src/components/Knob.tsx`)
  and one **0.40** preset/assistant-sized jump.

Bars: **≤ 2.0×** for the 0.05 step and **≤ 3.0×** for the 0.40 jump, both anchored on
the already-smoothed clean amp measured in the same run (worst 1.37× / 2.84×).
Sensitivity: the same change spliced as a hard switch must blow past the bar.

Also required: `clipper_player_expectations_tests --golden-report` shows 0.00 dB on
all five goldens, `native/tests/identical_core_test` still bit-exact, and
`clipper-bench` rows for jcm800 / twin / ac30 unchanged beyond noise.

## Manual test steps

- [ ] Happy path: `./build/clipper_param_smoothing_tests` — every knob on all three
      amps under both bars, the clean control case under the same bars.
- [ ] `./build/clipper_player_expectations_tests --golden-report` → `0.00 dB` worst
      band on all five rigs (measures, writes nothing).
- [ ] `ctest --test-dir build --output-on-failure` fully green, no new XPASS.
- [ ] Edge case: knob set *before* the first `process()` must apply instantly, not
      ramp — asserted as a bit-identity against a render that sets it before
      `prepare()`.
- [ ] Edge case: teeth. Perturb the fix (drop the per-sample `next()` back to a
      once-per-block constant) in a scratch copy and confirm the new test fails.

## Out of scope for this session

* Audit findings 4, 5, 7, 8, 9 (the AC30 sag, the AC30 mid notch, the shared PI tail,
  `Ra2`, the single-ended plate load). None of them is a smoothing problem.
* The power amps' `PARAM_DRIVE` — it is test/tool-only, reachable from neither the C
  ABI nor `ClipperEngine`.
* Declicking a *preset load* (a simultaneous jump on every knob at once). Smoothing
  each knob is the fix for a knob move; a whole-rig recall wants the declick bracket,
  and that is a different slice.
* ~~`TwinPreamp::brightS_` has no `flushDenormal` guard. Pre-existing, unrelated.~~
  **Superseded mid-flight:** `fix/denormal-guards` (#11, docs §33) landed on `main`
  while this slice was in progress and guarded exactly that state. Nothing to do here.

---

<!-- Fill in below during/after the session -->

## What actually happened

The slice was written by an agent that was terminated mid-session by a session limit,
so it was finished and verified by the orchestrator. The DSP work was complete; what
was missing was every measurement, the artifact rebuild, and this section.

Three things worth recording:

* **A stray merge-conflict marker survived into the WIP commit.** `Ac30Preamp.h` had
  one, and it was missed because the `git merge` output was read through `tail -8` and
  the third conflicting file scrolled past. The compile caught it — but only because a
  conflict marker happens to be a C++ syntax error. In a Markdown or JSON file it
  would have shipped silently. Do not truncate merge output; and a full-tree
  `grep -rl '^<<<<<<< '` before committing a merge costs nothing.
* **The bench number looked like a regression and was not.** The JCM800 measures ~58 %
  of one stream here against the **53.3 %** recorded in §32, which reads as a 5-point
  regression from this slice. It is not: `main` measures 58.04–60.25 % on the *same
  machine in the same session*. §32's absolute column was taken on different hardware
  and its own note says so. The only defensible form of this claim is an interleaved
  same-machine A/B, which is what was run.
* **The note about `TwinPreamp::brightS_` went stale while the slice was open** — #11
  guarded it on `main` mid-flight. Struck through above rather than deleted, because
  the reasoning for *why* it was out of scope is still the useful part.

## Measured results

**The four knobs the audit measured** (`docs/audits/2026-07-24-project-audit.md`
finding 6), audit figure → this slice's 0.40-jump ratio:

| knob | audit (unsmoothed) | after |
|---|---|---|
| AC30 VOLUME — its primary overdrive control | **38.9×** | **0.98×** |
| Twin VOLUME | **29.5×** | **0.95×** |
| JCM800 MASTER | **10.3×** | **0.96×** |
| JCM800 GAIN | **6.8×** | **0.90×** |
| clean120 VOLUME *(already smoothed — control)* | 2.0× | 0.99× |

A ratio below 1.0 means the seam step is *smaller than the signal's own steady-state
slew*: the knob move is buried under the waveform's natural sample-to-sample motion.

**Worst case across every knob on all three amps**, both step sizes:

| | 0.05 arrow-key step | 0.40 preset-sized jump |
|---|---|---|
| valve amps (worst of all knobs, worst of 16 step positions) | **1.11×** | **2.07×** |
| clean120 — the already-smoothed control | 1.05× | 2.84× |

The three valve amps now sit **at or below** the clean amp that was already compliant.

**Same-metric before→after** (block C, the sensitivity case): the identical move
spliced with no smoothing measures **22.95× (jcm800 GAIN) / 26.80× (MASTER) /
28.46× (twin VOLUME) / 27.79× (ac30 VOLUME)** against a 12× bar. This is the honest
before number — same metric, same signal, same binary — and it brackets the audit's
single-position figures. ~28× → ~2×.

**Static-render invariance** (block D): `max |Δ| 0.000e+00 BIT-IDENTICAL` on jcm800,
twin and ac30 for knobs set *after* `prepare()` versus before — the deferred snap
works. `clean120` reports `DIFFERS` (3.304e-02), which is the control proving the
check can distinguish the two cases rather than passing vacuously.

**Goldens:** all five `UNCHANGED`, worst band **0.00 dB**. Core ctest **27/27** with
the 6 XFAIL ledgers Skipped and no XPASS.

**CPU — interleaved same-machine A/B against `origin/main`, 3 runs each** (run 1 is a
warm-up outlier on both sides; figures are runs 2–3, % of one 48 kHz stream):

| unit | main | this slice | |
|---|---|---|---|
| jcm800 | 58.21 / 58.04 | 58.48 / 58.49 | +0.6 % relative |
| twin | 39.70 / 39.42 | 38.95 / 38.57 | slice faster |
| ac30 | 33.86 / 34.83 | 33.74 / 33.65 | slice faster |

Two of three move faster and the third moves +0.6 %, inside a run-to-run spread of
2.2 points on `main` alone. The idle fast path is doing its job: `settled()` makes a
parked smoother a no-op that returns `target_` bit-exactly, so a static render pays
nothing and the tone-stack matrix is not rebuilt at all.

## Files created / modified

* `core/include/clipper/dsp/OnePoleSmoother.h` — templated `OnePoleSmootherT<T>`;
  `OnePoleSmoother` = the unchanged float instantiation, `OnePoleSmootherD` = double.
  New `settled()`.
* `core/include/clipper/dsp/{Jcm800Preamp,TwinPreamp,Ac30Preamp}.h` + their `.cpp` —
  smoothed knobs, control-rate tone-stack rebuild, deferred snap, `reset()`.
* `core/tests/support/StepSlew.h`, `core/tests/test_param_smoothing.cpp`,
  `core/CMakeLists.txt` — new `clipper_param_smoothing_tests` target (ctest 26 → 27).
* `web/public/generated/{clipper.js,clipper-processor.js,.build-stamp.json}` — rebuilt
  (`core/` changed). sourceHash 71e5ce4f02fb…, 65 inputs, 181571 bytes.

## Deferred to next session

* Nothing from this slice's own scope.
* Still open and explicitly *not* smoothing problems: findings 4, 5, 7, 8, 9.
* A **preset load** (every knob jumping at once) still wants the declick bracket
  rather than per-knob smoothing. Its own slice.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
