# Anti-denormal guards for the states the policy missed

**Date:** 2026-07-25
**Branch:** fix/denormal-guards
**Roadmap item:** 2026-07-24 audit finding 11 (denormals: unguarded recursive state)

## Goal

Every recursive state in `core/` that asymptotes to zero on silence is flushed at
the −600 dB `Denormal.h` floor, so the silence-vs-signal CPU ratio for RAT and GOLD
falls from ~1.9× to ~1.0× and GOLD stops emitting subnormal samples downstream —
with the guarded recurrences bit-identical to the unguarded ones on program audio.

## Approach

`core/include/clipper/dsp/Denormal.h` is already the policy and already has both a
`float` and a `double` overload. This slice does not change the policy; it applies it
to the four places that never adopted it. **Fidelity-neutral by construction**: the
guard only fires below 1e-30 (−600 dB), ~456 dB under the 24-bit noise floor, so no
state on any program-level trajectory ever reaches it. That claim gets a
bit-for-bit test rather than an argument.

Four groups:

1. **Output DC blockers** — `dcY1_` in `OverdriveEngine::processChunk` and
   `GoldModel::processChunk`. Every other one-pole state in the same loop is already
   flushed; this one was missed. On silence the recursion degenerates to
   `y = dcR_ * dcY1_` with `dcR_ ~= 0.9984`, which at the smallest subnormals rounds
   back to itself and never reaches zero.

2. **The `chowdsp` WDF capacitor state (double)** — `RatModel` and `GoldModel`.
   `chowdsp::wdft::CapacitorT<double>::z` is private and the library offers no guard,
   but it is kept in lockstep with the **public** `wdf.a` (`incident()` does
   `wdf.a = x; z = wdf.a;`), and `incident(0)` zeroes both. So the flush is expressible
   through the library's own public API with **no fork of the pinned dependency**:
   a `flushDenormalWdfCapacitor()` template in `Denormal.h` that reads `wdf.a` and
   calls `incident(0)` only when the guard fires. It runs *after* the sample's output
   voltage is read, so the flush can never perturb the sample that triggered it.
   Chosen over (a) forking/vendoring `CapacitorT` (drifts from the pinned SHA) and
   (b) subclassing (`CapacitorT` is `final`).

3. **The seven valve-amp translation units**, none of which include `Denormal.h`.
   Their state is `double`, so the boundary is ~2.2e-308 rather than 1.2e-38 — the
   same cliff, just further down, and with no FTZ escape on WASM.
   Guard the states whose **steady state under silence is zero**:
   `otHpS_`/`otLpS_` (all three power amps), `presLpS_` (JCM800), `fbDelay_`
   (JCM800, Twin), `topCutS1_`/`topCutS2_` (AC30), the tone stacks'
   `vT_/iT_/vB_/iB_/vM_/iM_` (+ AC30's `vC_/iC_`), `TwinPreamp::brightS_`, and
   `TriodeStage::vCc_`.
   Do **not** add a guard to state that parks at a **nonzero DC operating point** —
   `vCk_`, `vCo_`, `vRail_`, `vScreen_`, `vk_`, `iSagEnv_`, `vCcUp_`/`vCcDown_`,
   `vgUp_`/`vgDown_`, the Newton warm starts `va_`/`vg_`/`vk_` and `LtpInverter`'s
   `va1_`/`va2_`/`vk_`. Those can never be subnormal, and a guard that cannot fire is
   dead instructions in the hottest loop in the codebase. Each such state gets a
   comment saying so, and the claim is **measured**, not assumed.

4. **`core/src/Processor.cpp`** hand-rolls the one-pole smoother recurrence — the one
   place in the tree that reimplements `OnePoleSmoother` instead of using it. Wrap the
   recurrence in `flushDenormal` (keeping the hoisted-local loop shape, so it stays
   bit-identical for every finite trajectory).

## Steps

- [ ] Extend `scripts/denormal_bench.cpp` with real-gear scenarios: silence-vs-signal
      (and hardware-FTZ) timing for RAT / GOLD / the three valve amps / `Processor`,
      plus a subnormal-output-sample count. Its current three scenarios only exercise
      `Biquad` and `OnePoleSmoother` — both already guarded — which is exactly why it
      reports a clean 1.00× cliff and why this bug survived.
- [ ] Capture the BEFORE table.
- [ ] Add `flushDenormalWdfCapacitor()` to `Denormal.h`.
- [ ] Guard group 1 (`dcY1_` × 2).
- [ ] Guard group 2 (the two WDF caps).
- [ ] Guard group 3 (the seven valve-amp TUs), with a comment on each state that
      parks nonzero saying why it is deliberately unguarded.
- [ ] Guard group 4 (`Processor::process`).
- [ ] Extend `core/tests/test_denormal.cpp`: a bit-transparency test per newly guarded
      recurrence (guarded vs a verbatim unguarded local reference, program-level audio,
      `==` not a tolerance) and a "silent tail reaches exact zero / never subnormal"
      test per state class.
- [ ] Capture the AFTER table; confirm goldens untouched and ctest green.
- [ ] Docs §33 + CLAUDE.md Current State.

## How this will be measured

- **`build/denormal_bench`** (extended): per-unit 10 s-signal vs 10 s-silence wall time
  and the hardware-FTZ floor. Acceptance: silence/signal ratio → ~1.0 for RAT and GOLD
  (from a measured 1.94× / 1.92×), and the default-environment time meeting the FTZ
  time (the guard is the only fix available on WASM).
- **Subnormal output count** from the same tool: GOLD 392342/480000 → **0**.
- **`clipper_denormal_tests`**: bit-for-bit equality (`==`) against verbatim unguarded
  references over 96 k samples of program-level audio, per guarded recurrence.
- **`clipper_player_expectations_tests`**: the golden third-octave gates must pass with
  the goldens **untouched** — that is the fidelity-neutrality gate at rig level.
- Full `ctest`.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — all targets green, 6
      `_xfail_ledger` entries Skipped, goldens unmodified (`git status` clean under
      `core/tests/goldens/`).
- [ ] `build/denormal_bench` before/after — the silence column collapses onto the
      signal column.
- [ ] Edge case (guard has teeth): revert a single guard in a scratch copy and confirm
      the matching silent-tail assertion fails. A guard test that cannot fail is not a
      test.
- [ ] Edge case (guard is transparent): the bit-transparency assertions use `==`; if
      any guard fired on program audio the suite goes red rather than drifting quietly.
- [ ] Edge case: `Processor` with `PARAM_GAIN = 0` — `finalGain` must reach exactly
      0.0f, not park at 1.68e-43.

## Out of scope for this session

- Enabling FTZ/DAZ anywhere. Impossible on WASM; that is *why* this policy exists.
- Any filter topology, coefficient, gain or tone change.
- `web/public/generated/*` — the orchestrator rebuilds and commits the WASM artifact.
- Audit finding 6 (valve-amp parameter smoothing), finding 2 (cab-swap RT safety).
- The `native/` chain: no recursive state of its own to guard.
- Fixing the states that park at a nonzero DC point — they cannot go subnormal;
  documented, measured, and left alone.

---

<!-- Fill in below during/after the session -->

## What actually happened

Four surprises, all from measuring instead of assuming. Three of them changed the plan.

**1. The measurement method in the plan was wrong, and the first table it produced was
misleading.** The plan said "silence/signal ratio → ~1.0". That ratio is not a denormal metric:
a unit can be legitimately cheaper or dearer on silence because the tube and BJT Newton solves
converge in a different number of iterations when nothing is moving. The **Muff** proves it —
3.01× dearer on silence and *no denormal problem at all*, since forcing hardware FTZ gives the
same 3.01×. The correct comparison is `silence` vs **the same unit's silence with hardware
FTZ+DAZ forced on**, because FTZ changes nothing else. The benchmark and docs §33 now say so
explicitly, with the Muff as the worked example, so the next person does not repeat it.

**2. `TriodeStage::vCc_` does not decay to zero**, contrary to the audit's list. Measured
8.15e-4 V after 20 s of digital silence (min nonzero 1.40e-5, zero subnormal blocks): the
grid-leak node parks at the grid-current bias point. `vCk_` and `vCo_` park at the cathode and
plate DC by construction. So the most-executed loop in the core is left with **no flush**, and
`TriodeStage.cpp` does not include `Denormal.h` — with a comment recording the measurement and
pointing at the public `couplingCapVoltage()` so the claim can be rechecked. This produced the
general rule the slice adds: **a state that rests at a nonzero DC operating point cannot be
subnormal, so guarding it is unreachable code, and unreachable code in that loop is not free.**

**3. The AC30's cliff was in the OT bandwidth states, not the TOP CUT states.** TOP CUT filters
`va1 − quiescentPlate1()` and looks like the obvious candidate; it rests at 2.84e-14 (one ULP
of the LTP plate voltage) and never goes subnormal. Bisected on the isolated stage: TOP CUT
flushes alone leave it at 1.33×; the OT pair alone takes it to 0.99×. The reason is that this
amp has **no global NFB loop**, so its secondary settles at *exactly* zero — while the JCM800's
and the Twin's idle at ~1e-28 because their feedback loops keep a residual alive, which is also
why those two power sections turned out to have **no** measured denormal cost at all. Their
flushes are kept but relabelled as guard-rails for the post-`reset()` case, not as fixes.

**4. The tone-stack bug is currently masked in-product.** `FenderToneStack` standalone measures
**18.6×** and `MarshallToneStack` **68.2×** — the two largest cliffs in the core — but inside
the composed preamps they measure 0.99×, because the upstream tube stage's own nonzero
coupling-cap residual means the stack never sees true digital silence (measured: 5.7e-11 inside
`TwinPreamp`). Guarded anyway: the masking depends on a tube stage's idle offset and disappears
the moment anything upstream emits exact zeros (a bypassed stage, a muted chain, a `reset()`).

**Two things the plan did not anticipate.** A state I found that the audit's list did not
mention: `TopBoostToneStack::vC_`/`iC_`, the companion pair for the AC30 stack's series input
coupling cap (the other two stacks have no such cap, so the audit's shared-state enumeration
missed it). And the benchmark initially reported **0** subnormal output for SD-1 and Screamer
because `prepare()` snaps the LEVEL smoother onto a 0 target — a pedal left at defaults renders
silence into its own output. With knobs actually set, they emit **429 236** and **428 761**
subnormal samples per 10 s, i.e. the same order as GOLD. The tool now sets every knob.

Testing needed a different shape than the plan assumed. Most of the newly guarded state is
`double`, and `float(1e-310)` is exactly `0.0f`, so **no output-only test can see a double
subnormal** — the mechanism by which this survived a green suite. The affected classes therefore
expose one const diagnostic, `maxAbsRestingState()`, and the test asserts it reaches exactly
0.0 after a silent tail. `TwinPowerAmp`, `Jcm800PowerAmp` and the composed `TwinPreamp`
deliberately do not get one: nothing in them rests at zero, so the assertion would have no
teeth, and a toothless assertion is worse than none.

No XFAIL was affected (none covered finding 11), so nothing to delete. No golden moved.

## Measured results

Per 10 s at 48 kHz in 128-frame blocks, `build/denormal_bench` section 2. Read `silence`
against `hwFTZ silence`.

| Unit | signal | silence before | hwFTZ silence | silence after | subnormal out before → after |
| --- | --- | --- | --- | --- | --- |
| RAT | 328 ms | **647 ms (1.97×)** | 305 ms | **309 ms (0.94×)** | 0 → 0 |
| GOLD | 324 ms | **629 ms (1.94×)** | 296 ms | **305 ms (0.93×)** | **393 607 → 0** |
| SD-1 | 291 ms | 341 ms (1.17×) | 267 ms | **272 ms** | **429 236 → 0** |
| Screamer | 291 ms | 355 ms (1.22×) | 270 ms | **268 ms** | **428 761 → 0** |
| Muff | 2280 ms | 6855 ms | 6780 ms | unchanged | 0 → 0 (not denormals) |
| JCM800 | 5755 ms | 3038 ms | 3010 ms | unchanged | 0 → 0 (no cliff) |
| Twin | 3804 ms | 2025 ms | 2007 ms | unchanged | 0 → 0 (no cliff) |
| AC30 | 3177 ms | 1982 ms | 1584 ms | **1603 ms** | **1588 → 2** |
| Processor GAIN 0 | 42 ms | 23 ms | **2 ms** | **4 ms** | **427 187 → 0** |

Every fixed unit's default-environment silence time now meets its hardware-FTZ floor — the
point of the exercise, since on WASM the in-code flush is the only FTZ available. The AC30's
two residual samples are a float-cast transient during ring-down, not parked state.

Isolated components, silent tail, default vs hardware FTZ:

| Component | before | after |
| --- | --- | --- |
| `MarshallToneStack` | **68.22×** | 1.00× |
| `FenderToneStack` | **18.58×** | 1.00× |
| `Ac30PowerAmp` | **1.35×** (1588 subnormal out) | 0.99× (2) |
| `TopBoostToneStack` | 1.00× | 1.00× (guarded anyway — poles move with the knobs) |

**Fidelity-neutrality.** All eleven units rendered over program audio at knobs min/noon/max,
pre-guard vs post-guard, compared byte for byte: **99 671 of 4 752 000 samples differ, max |Δ|
anywhere = 1.0151e-30** (−600 dB; 578 dB below that render's own peak). GOLD, SD-1, Screamer,
Muff, Phaser, Clean 120, JCM800 and Twin are **bit-identical everywhere**. The only differences
are RAT's silent tail (32–55 samples), `Processor` at GAIN 0 (the intended fix), and the AC30 at
VOLUME 0 whose entire render peaks at 1.475e-15. Goldens untouched; core ctest **24/24** with
the 6 `_xfail_ledger` entries Skipped as normal.

**Teeth.** Each guard removed one at a time, rebuilt, suite required to fail — 7/7 confirmed
(`OverdriveEngine dcY1_`, `GoldModel dcY1_`, Fender/Marshall/TopBoost companions, `Ac30PowerAmp`
OT low-pass, `Processor` residual snap), and the tree verified to pass again after restore. The
WDF test carries its teeth internally: it asserts the *unguarded* reference network really does
go subnormal, so a future `chowdsp` bump cannot make it pass vacuously.

## Files created / modified

Guards: `core/src/dsp/OverdriveEngine.cpp`, `GoldModel.cpp`, `RatModel.cpp`, `TwinPreamp.cpp`,
`Jcm800Preamp.cpp`, `Ac30Preamp.cpp`, `TwinPowerAmp.cpp`, `Jcm800PowerAmp.cpp`,
`Ac30PowerAmp.cpp`, `core/src/Processor.cpp`.
Policy + helpers: `core/include/clipper/dsp/Denormal.h` (`flushDenormalWdfCapacitor`,
`isSubnormal(double)`, `maxAbsState`).
Diagnostics: `core/include/clipper/dsp/TwinPreamp.h`, `Jcm800Preamp.h`, `Ac30Preamp.h`,
`Ac30PowerAmp.h` (+ documented non-decisions in `TwinPowerAmp.h`, `Jcm800PowerAmp.h`).
Documented non-guard: `core/src/dsp/TriodeStage.cpp`.
Tests + tooling: `core/tests/test_denormal.cpp` (block 2), `core/CMakeLists.txt`,
`scripts/denormal_bench.cpp` (section 2 — whole units, signal vs silence vs hwFTZ).
Docs: `docs/DEVELOPMENT.md` §33, `docs/decisions/006-denormal-guards-scope-and-the-wdf-capacitor.md`,
`CLAUDE.md`, this file.

## Deferred to next session

- **`bash scripts/build-wasm.sh` + commit `web/public/generated/*`.** Out of scope for this
  agent by instruction, but **required**: `core/` changed, and this slice fixes a cliff that is
  worst on the WASM target (no FTZ at all), so it is worth nothing to a player until the
  artifact ships.
- **The Muff's 3.01× silence cost** — `BjtStage`'s Ebers-Moll Newton taking more iterations near
  the quiescent point. Confirmed *not* denormals (hwFTZ shows the same ratio). Its own perf
  slice; do not "fix" it with a flush.
- **`Processor` still duplicates `OnePoleSmoother`.** The recurrences now agree exactly
  (residual snap, same rule), but the duplication remains. Collapsing it touches `Processor`'s
  prepare/reset/parameter seams, so it is a `refactor:` slice.
- **The AC30's 2 residual subnormal output samples** — a float-cast transient during ring-down,
  from a normal `double` secondary voltage whose `float` cast lands under 1.18e-38. Harmless
  (bounded, not parked state). Only worth chasing if a guard on the normalized float output is
  wanted, which would be a different policy decision.
- **The branch is 6 commits behind `origin/main`** (other slices landed while this one ran). Not
  rebased here — merge order is the orchestrator's call.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
