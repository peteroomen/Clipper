# Muff clip stages — the DC-blocked diode branch (the 4th Newton node)

**Date:** 2026-07-31
**Branch:** claude/muff-dc-diodes-6f557i
**Roadmap item:** ADR 009's named follow-up (finding 16's remaining half) + owner
feedback round 3: "Much much better. It's still a little gutless, it doesn't scream
through either now. … More bass, but yes the deficit remains, still more work to be
done on this pedal." The `finding16-muff-almost-no-bass` XFAIL (low E −14.2 dB re
1 kHz after §49).

## Goal

The clip stages' feedback diode pairs sit behind their real DC-blocking caps, so the
diode branch stops DC-loading the base node: the bass deficit closes (the XFAIL
XPASSes → deleted → hardened), the stage's usable swing rises (max sustain can sing
again — the owner's "doesn't scream through"), and the §49 articulation gain is kept
(max sustain stays a wall you can hear the note through, NOT a return to the 150 %
blowout).

## Approach

Deliberate fidelity change, judged against the schematic (the branch the real pedal
has) and the existing measured ledgers. In the real circuit each clip stage's diode
pair is in series with a DC-blocking cap (the ElectroSmash-documented C6/C7-class
caps; §49's research report established the topology): at DC the branch is OPEN (the
base sees only the bias divider — the impedance §49 measured our model missing at
~1.8 k vs the real ~4 k), and at audio the diodes limit through the cap. `BjtStage`
gains a 4th solver node — the cap's inner node — config-gated exactly like §49's Rs:
**a stage without the branch reduces bit-identically to the 3-node solver** (hash-
verified; RAT/GOLD/SD/TS untouched). §49 already plumbed `Config::Rbg` for the bias
path; use or supersede it per the real topology, and say which in the comment.

Solver discipline (read docs §34 and §49 before touching dampedNewton):
- The new cap state joins `reset()`/`cachePark()` (nan-guard block C must stay green)
- Its REST value is whatever the DC solve says — if nonzero, NO denormal flush, a
  comment with the measured resting value (ADR 006 scope rule); if zero-resting, add
  the flush and extend `maxAbsRestingState()`
- The residual early-out (`kNewtonResidualTolA`) must keep firing at idle (the §34
  perf gates are ctest-enforced) and the tube-solver accuracy gate must hold
- Re-measure the slam ledger (±20 V, all rate×factor): currently 5 cap-exhausting
  combos — must not grow; shrinking is a win to record

## Steps (for the implementing agent)

- [ ] Read docs §34, §43, §49, ADR 009, `BjtStage.{h,cpp}`, `MuffModel.cpp`,
      `test_muff_model.cpp`, the §49 plan file — the whole prior art, before code
- [ ] Establish the schematic values (cap value + the real bias-network impedance)
      from the §49-cited references (network available; the ElectroSmash Big Muff
      analysis; name components in comments)
- [ ] BjtStage: 4-node solve, config-gated; 3-node bit-identity hash proof for a
      stage with the branch disabled
- [ ] Measured before/after: low-E (82 Hz) response re 1 kHz at default + max
      sustain; THD + level at max sustain (the §49 wall must stay articulate:
      ~40 % region, NOT 150 %); fundamental decay/sustain time at max sustain (the
      "scream" proxy — should lengthen); the §43 taper bars (knob 0.14 rows) —
      re-derive the floor ONLY if measurement demands, §49 precedent
- [ ] XFAIL ledger: `finding16-muff-almost-no-bass` should XPASS → delete → harden
      with a perturbation-proven bar (branch disabled must fail it); slam ledger
      re-measured; any NEW honest XFAILs get the full decl treatment
- [ ] Player-expectations muff rows (A1/A2/A3) re-baselined as needed
- [ ] `--golden-report`: `muff_twin` WILL move (bass returning at default sustain) —
      record the table, NO bless, NO golden writes; the other four must be UNCHANGED
- [ ] Perf: `denormal_bench`/muff idle-vs-signal spot check — the §34 early-out must
      still fire; CPU interleaved A/B if the 4th node costs (record honestly)
- [ ] WASM rebuild + artifacts; full core ctest (player-expectations red ONLY at
      muff_twin is the expected end state); web build + Playwright (the muff worklet
      spec may need probe re-derivation — honesty rules); node suites
- [ ] Docs §53 + ADR if the topology decision warrants one + CLAUDE.md entry + this
      plan file's bottom sections
- [ ] ONE commit on claude/muff-dc-diodes-6f557i, fix: …, measured before→after in
      the body, standard trailers; NO push, NO PR, NO golden writes

## How this will be measured

Low-E re 1 kHz (the XFAIL's own number, −14.2 → target within the hardened bar the
XPASS defines), max-sustain THD (articulate wall held), fundamental sustain time at
max (the scream proxy), slam ledger count, 3-node bit-identity hash, muff_twin
golden table for the owner.

## Manual test steps

- [ ] Owner: low riffs have real weight; max sustain sings/screams through the wall
      while the note stays audible; sus 14 still edge-of-breakup
- [ ] Edge: NaN rejected; reset clean; 44.1/96 k spot check; slam bounded

## Out of scope for this session

The Muff tone stack, the §43 knob law (unless its floor measurement demands the §49
kind of re-derivation), all other pedals, the GOLD decision.

---

## What actually happened

The residual was a **missing component**, not a wrong constant, which is what ADR 009
predicted when it refused to pick a resistor value: the published clip stage puts a
**1 µF cap (C6/C7) in series with the feedback diodes**, and this model did not have it.
Without it the pair carries DC, conducts at idle, clamps the collector 0.26 V above the
base, and shunts the base node to ~1.8 k — which is simultaneously the missing bass
(coupling corner ~900 Hz per clip stage) and the missing swing. `Config::Rbg` (RA = 100 k)
was **used, not superseded**: it only makes sense once the diodes stop shorting the base at
DC, so RA and C6/C7 landed together.

`BjtStage` gained a config-gated 4th node (the cap↔diode junction) solved by a 4×4 Gaussian
elimination with partial pivoting, sharing ONE templated damped Newton with the 3-node path
so the early-out and iteration accounting cannot drift apart. `Cdiode == 0` is bit-identical.

**Three things came out of the slice that were not in the plan:**

1. **The step limiter was rotating the Newton direction.** Enabling the branch first made
   the slam ledger WORSE (5 → 7 of 16 at the cap). Tracing a failing sample showed the step
   pinned at the ±10 V *component* clamp with `lam` collapsed to 2⁻³⁰ and the residual
   frozen for 60 iterations — the solve standing still, not diverging. Scaling the direction
   instead of clipping its components took the ledger to **0 of 16**, which also XPASSed the
   §34 `muff-slam-exhausts-newton-cap` XFAIL. Applied to the 4-node path only; the 3-node
   path keeps the defective clamp because bit-identity is this slice's contract.
2. **`kClipDriveMax` was a compensation and came off** (6.0 → 1.0, the physical maximum of a
   passive pot), and the §43 floor re-derived −70 → −65 against the same player bar. The
   level half of the §43 bar (15 dB below the wall) is honestly re-derived to 4 dB, with the
   reason in the test: on the corrected circuit the diodes clamp near 0.65 V at any drive.
3. **Two tests had no teeth, and checking found both.** `testIdleSolverCost` claimed to
   cover a played-then-quiet stage and never did (it read a fresh `probe` model);
   `testSlamConvergence` can no longer prove the globalization once the drive dropped 6×, so
   the property moved to a new stage-level test.

## Measured results

| | before | after |
|---|---|---|
| low E (82.4 Hz) re 1 kHz | −14.24 dB | **−5.48 dB** (bar > −6) |
| open A / 60 Hz / 30 Hz re 1 kHz | −8.74 / −21.83 / −42.87 | −1.87 / −11.13 / −29.36 |
| Q2/Q3 bias Vc, Vc−Vb, Ic | 1.213 V, 0.261 V, 0.777 mA | **4.946 V, 4.160 V, 0.397 mA** |
| idle diode current | conducting | **0.00e+00 A** |
| max-sustain THD (220 Hz, 0.1 V) | 40.8 % @ −4.6 dBFS | **37.6 % @ −4.3 dBFS** |
| knob 0.15 THD / dB below wall | 11.9 % / 2.9 | **9.8 % / 5.8** |
| sustain time @ max, 220 Hz (t−20 dB over the input's own) | +3.275 s | **+4.175 s** |
| sustain time @ max, 110 Hz | +2.875 s | **+4.050 s** |
| ±20 V slam ledger | 5 of 16 at the cap | **0 of 16**, worst 18/60 |
| idle residual ceiling (4-node) | n/a | 7.05e-19 A vs 2.06e-18 (3-node), tol 1e-17 |
| parked Newton iterations | 0 | **0** (48 combos) |
| played-then-quiet evals/solve | 1.00 | **1.75** (0.00 % backtrack burn; pathology was 31.00) |
| tube-solver prod-vs-reference worst | −127.4 dBFS | **−126.4 dBFS** (gate −120) |
| CPU (interleaved A/B) | 5.62–5.63× RT / 17.8 % | **3.21–3.34× / 30.0–31.2 %** |
| denormal_bench signal / silence | 1734 / 382 ms | 3095 / **3728** ms (hwFTZ matches; 0 subnormals) |
| `muff_twin` golden | — | **−1.09 dB RMS / 13.18 dB @ 252 Hz** (NOT blessed) |
| other four goldens | — | UNCHANGED (≤ 0.11 dB) |

Bit-identity (branch disabled): 15 stage digests (5 rates × 3 shapes) identical to the
pre-slice header+library **including Newton iteration counts**; 10 whole-`MuffModel`
digests identical across knob settings and ragged 44.1/96 kHz × 1/2/8 oversampling.

Perturbations, all confirmed RED then reverted (`touch` after both):
delete `clip.Cdiode` · `Sys4::limitStep` clipping components · `kSustainMinDb = −54` ·
`kClipDriveMax = 6.0` · delete `clip.Rbg`.

## Files created / modified

- `core/include/clipper/dsp/BjtStage.h` — `Config::Cdiode`, the 4th node's state,
  `usesDcBlockedDiodes()`, `quiescentDiodeNodeVoltage()`, the rewritten circuit/solver header
- `core/src/dsp/BjtStage.cpp` — `Sys4`, `solve4x4`, `Numerics<N>`, the templated
  `dampedNewton`, per-system `limitStep`, 4-node DC solve / park / reset
- `core/src/dsp/MuffModel.cpp` — `clip.Rbg` + `clip.Cdiode` with the schematic citation,
  `kClipDriveMax` 6.0 → 1.0, `kSustainMinDb` −70 → −65
- `core/tests/test_muff_model.cpp` — both XFAILs deleted and asserted; new
  `testDiodeBranchIsDcBlocked` and `testStageSlamConvergence`; absolute clip-bias block;
  `analyticBias` extended; re-derived §43 level bar; played-then-quiet idle assertion
- `core/tests/test_player_expectations.cpp` — Muff reference rows re-baselined
- `core/CMakeLists.txt` — muff XFAIL ledger registration removed (zero known defects)
- `web/public/generated/{clipper.js,clipper-processor.js,.build-stamp.json}` — rebuilt
- `docs/DEVELOPMENT.md` §53, `docs/decisions/010-muff-dc-blocked-diode-branch.md`,
  `docs/decisions/009-*.md` (status closed), `CLAUDE.md`

## Deferred to next session

1. **The owner's golden bless** for `muff_twin` (−1.09 dB / 13.18 dB @ 252 Hz). Core ctest
   is red at exactly that gate until then — intended.
2. **The CPU cost.** 17.8 % → 30 % of a stream. The 4×4 is sparse (J[2][3] = J[3][2] = 0,
   the emitter row does not see Vd); a specialised solve or a Schur complement onto the
   3-node system should recover most of it. Its own perf slice, own bit-identity bar.
3. **The 3-node path's component-wise step clamp** — the same defect, deliberately left,
   because fixing it changes every existing `BjtStage` user's audio.
4. **`Re` = 390 Ω vs the published 100 Ω** — why this clip stage idles at 4.95 V where a
   real one sits nearer 4.2 V. Out of scope, now the biggest remaining departure.

## Status

- [ ] In progress
- [x] Complete (pending the owner's golden bless)
- [ ] Partial — see deferred
