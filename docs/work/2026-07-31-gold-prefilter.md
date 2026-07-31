# GOLD drive pre-filter — the reference's H_pre, the named §54 follow-up

**Date:** 2026-07-31
**Branch:** claude/gold-prefilter-6f557i
**Roadmap item:** §54's named deferred refit ("kDrivePreScale/kDriveHpHz were re-scoped,
not changed… the real H_pre is a ~1105 Hz one-pole into a 0.93 shelf where the model
has 600 Hz into 0.65 — a named refit candidate"). Owner 2026-07-31: "queue the next
gold pass then if you see improvements for that pedal go for it." Context, not a
target: "potentially still a bit gainy by a touch" (owner chose not to chase it —
if this refit moves perceived gain either way, REPORT it, don't aim for it).

## Goal

The GOLD's drive-path input network matches the reference implementation's H_pre
(the last knowingly-approximate block in the drive path), with a stage-by-stage
match table proving the whole drive path now tracks the reference end to end.

## Approach

Fidelity refit against the cloned KlonCentaur reference (PreAmpStage/FeedForward
netlist — the §52/§54 oracle). §54 measured our stand-in (600 Hz one-pole into
0.65×) at ±1.3 dB from the real H_pre across the core band; replace it with the
netlist-derived response (reported ~1105 Hz into 0.93 — DERIVE it from the netlist
values yourself, don't trust the prior report's summary). Contracts: GAIN 0 stays
bit-exact (the pre-filter lives in the drive branch only — prove by hash);
the §50 DC gang law and §52 weight and §54 trio untouched (their tests pin them).
Owner latitude: if the netlist comparison harness shows OTHER cheap wins in the
drive path (each with an isolated perturbation proof + honesty gate), take them;
anything touching the clean path, the tone control, or the summing needs its own
slice — do not bundle.

## Steps

- [ ] Re-clone the reference; derive H_pre exactly from its component values
      (name them); implement; keep the constants' comments §54-style
- [ ] Stage-by-stage harness: drive-node |H| vs reference at 3 knob points —
      the ±1.3 dB residual should collapse; record worst-case before/after
- [ ] Field rows at 0.15 V / 220 Hz (THD + level + onset table, §54 format) —
      REPORT movement, chase nothing
- [ ] GAIN-0 hash pin ×2 stimuli; perturbation-proven bar (old 600/0.65 → red);
      gold worklet spec margin re-checked (its pushedH3 bar sits at 5.6 % margin —
      re-derive honestly if crossed, never loosen)
- [ ] Player-expectations gold rows re-baselined if moved; --golden-report ZERO
      changed; full core ctest; WASM rebuild + artifacts; web build + Playwright;
      node suites
- [ ] Docs §56 (§55 = AC30 sag, in flight) + CLAUDE.md entry + plan bottom sections
- [ ] ONE commit on claude/gold-prefilter-6f557i, fix: …, tables in body, standard
      trailers; NO push, NO PR, NO golden writes

## How this will be measured

The drive-node |H| three-way table (worst error vs reference before/after), the
GAIN-0 hashes, the field rows, the perturbation transcript.

## Manual test steps

- [ ] Owner: GAIN 0 transparent; default/max character unchanged-or-reported;
      the boost-into-JCM staging still right
- [ ] Edge: NaN/reset/rates; no zipper on GAIN sweeps

## Out of scope

Clean path, tone control, summing stage, all other pedals, the AC30 slice
(running in parallel — do not touch Ac30* files).

---

## What actually happened

Inherited an interrupted working tree (~260 lines across `core/src/dsp/GoldModel.cpp` and
`core/tests/test_gold_model.cpp`). Reviewed both critically:

**KEPT (verified, not assumed).** The model-side derivation was sound and I re-derived it
independently before trusting it. The netlist values match the reference file
component-for-component (`C3` 0.1 µF, `C5` 68 nF, `C16` 1 µF, `R6` 10 k, `R7` 1.5 k,
`ResVs Vbias2` = `R19` 15 k, `ResVs Vbias` = `g·100 k`), the s-domain polynomials and the
bilinear expansion are algebraically correct, the gang-complement recovery
(`g·R_pot = (R_pot + Rleg) − leg`) matches the reference's own `setGain` pair
(`PreAmpStage` sets `g·100k`, `AmpStage` sets `(1−g)·100k + 2k`), and the coefficient
caching / reprepare path is correct.

**REDONE.** (1) The inherited test file **did not compile** — it called a `refPreAmpMag()`
that was never written. (2) Two of its new bars were **vacuous**: they compared a
test-computed reference value against a hardcoded constant and never touched the model at
all, and one carried a literal tautology (`assert(fabs(x−x) < 1e-9)`). Both were rewritten
as absolute one-sided bars measured through the whole dirt path, and perturbation-proven.
(3) Several measured figures quoted in the inherited comments were wrong when checked
(the gang perturbation reads 7.212 dB, not the claimed 3.606).

**FOUND, not on the plan.** The inherited implementation used `float` state for the new
third-order recursion. At the oversampled rate that is an **audible −73 dBFS noise floor**
on the shipped path (see below). Fixed to `double`, and the same measurement then applied
to §54's `AmpStageNetwork`, which had the same latent issue at −120 dBFS. Taken under the
plan's drive-path latitude, with its own isolated measurement.

**FOUND, second, by a test that was already there.** `clipper_denormal_tests` went red:
the GOLD never reached exact digital silence. Root cause is **not** this network — it is the
house anti-denormal idiom. `y1 = flushDenormal(y)` guards only the NEWEST recursive tap,
which is complete for a one-pole and wrong above first order: the older taps re-inject with
coefficients of magnitude ~3, so the state pumps itself back over the floor. Measured
plateau: 3.373e-27 at 2 s of silence, 1.531e-26 at 8 s, 3.300e-27 at 16 s — sitting, not
decaying. Bisected: divider bypassed settles in 1.523 s (HEAD 1.481 s), divider kept with a
whole-state flush settles in 1.501 s. Both networks now zero the whole state as a unit.
Bit-transparent (floor 1e-30); GAIN-0 hashes and every field row unchanged by the fix.

**Two convention traps in the oracle** (documented in §56.3): the reference's
`PreAmpStage::processSample` reports the physical divider **negated** (a `PolarityInverterT`
port convention — measured 180.08° at 41 Hz, magnitudes agreeing to 0.0000 dB) and **one
sample late** (it reads element voltages between the two `incident()` calls — residual
phase error is exactly 360·f/192000 at every probe point). Neither is a disagreement about
the circuit; both would have been read as one.

**Process note for future sessions: `git stash` is NOT safe in a worktree here.** Stashes
are shared across all worktrees of the repo, so an A/B build that stashes raced the parallel
AC30 slice and popped *their* stash into this tree. Recovered both sides (their changes were
re-stashed, mine restored from a saved patch); all subsequent A/B work used plain file
copies. Do not use `git stash` while another agent is active.

## Measured results

**Table 1 — |H_pre| vs the reference netlist**, 41 Hz – 10 kHz × GAIN 0.15/0.35/0.90, model
recovered end to end through `GoldModel`, oracle = the reference's own WDF tree rebuilt on
this repo's pinned `chowdsp_wdf`:

* worst |error| **7.23 dB → 0.0006 dB**
* restricted to §54's core band (82 Hz – 3 kHz): **3.38 dB → 0.0006 dB**
* phase vs the continuous-time divider: worst **0.06°**
* (§54's quoted "±1.3 dB" was the 82 Hz – 1 kHz window at one knob position; the honest
  whole-band figure was 7.23 dB)

Full per-cell table in docs §56.2.

**Field rows**, 0.15 V peak / 220 Hz / TREBLE 0.5 / OUTPUT 0.5:

| GAIN | THD % before → after | RMS dBFS before → after |
|---|---|---|
| 0.00 | 0.000 → 0.000 | −19.499 → −19.499 |
| 0.15 | 3.620 → 2.996 | −11.969 → −12.071 |
| **0.35** | **4.586 → 4.020** | **−11.329 → −11.302** |
| 0.50 | 5.617 → 5.029 | −10.744 → −10.669 |
| 1.00 | 14.588 → 13.563 | −7.905 → −7.521 |

Breakup onset: **5 % at GAIN 0.4163 → 0.4963**, **10 % at 0.8682 → 0.9023**.
REPORTED, not aimed at — the owner's "potentially still a bit gainy by a touch but let's not
change for now" was context. This moves in that direction by accident of the component
values; nothing was tuned to produce it.

**GAIN-0 render hashes — IDENTICAL before and after** (sine and noise × TREBLE 0/0.5/1):
`a0c5464fd80df762` `55796eb6d0d9100c` `bc7f8af7af3b75e2` `0dfffa814cb19735`
`0a168e72781032a1` `b4b142672d039bcf`.

**`--golden-report`: ZERO goldens changed.** rat_jcm800 +0.00 dB / worst band 0.00,
sd1_twin_reverb +0.00 / 0.02, muff_twin +0.00 / 0.00, ts_ac30 +0.00 / 0.00,
clean120_chorus −0.00 / 0.11. No golden written.

**The noise floor (not on the plan).** Added noise vs an otherwise identical long-double
build, 0.15 V / 220 Hz, rms over the settled second:

| rate × OS | float state | double state |
|---|---|---|
| 48 kHz ×4 (shipped) | −73.4 dBFS | **−136.2 dBFS** |
| 48 kHz ×8 | −56.3 | −129.7 |
| 96 kHz ×8 (worst) | −32.0 | **−112.5** |

§54's `AmpStageNetwork`, measured in isolation: float **−120.1 dBFS** shipped /
**−101.7 dBFS** at 96 kHz × 8 → double. Honest residual: −112.5 dBFS at the extreme corner
is still above the −120 dBFS gate; the structural cure (cascade of first-order sections —
all poles and zeros of this network are real) is named and deferred.

**Web `pushedH3` bar — §54's warning came true and the bar was CROSSED** (0.1056 → 0.09774
against a 0.1 bar). Re-derived, not loosened: the movement is a **phase** effect
(`pushedF1` is the vector sum of dirt and clean, and the refit rotates the dirt branch
−15.48°, so f1 rose +0.375 dB while h3 moved only −0.274 dB). Predicted from the netlist
plus PRE-refit measurements only: f1 0.771613 (measured 0.771566, +0.0005 dB), h3 0.076203
(measured 0.075409, +0.091 dB), h3/f1 0.09876 (measured 0.09774, +0.09 dB) → bar
0.1 × 0.93773 = 0.09377. Shipped **0.0938**, rounded UP from the derivation. Margin 4.2 %
against the old 5.3 %. Derivation table in §56.6.

**M11 ragged-block window re-derived** (§56.7). `gold_ ABI` at 100 frames went red at the
50 ms window (1.22e-02 vs a 2e-03 bar). The difference **decays to exactly 0 by 400 ms** —
not corruption. `kBlockSettleSecs` 0.05 → **0.25** with the derivation corrected (what has
to settle is the slowest recursive STATE the trajectory excites, not the smoother; §56's
divider has a ~30 ms corner), and a **new, much harder convergence bar** added
(`kRaggedTailBar = 1e-4` over the final quarter — every unit measures exactly 0.0 except the
Muff at 3.34e-05, which is §53's τ ≈ 5 s diode caps). Post-change `gold_ ABI`: settled
1.47e-06, tail 0.00e+00.

**Perturbation transcript** (each patched, `touch`ed, built, run, restored, `touch`ed):

| perturbation | result |
|---|---|
| divider → §50's 0.65 × HP600 stand-in | bar 4 **12.480 vs 13.764** (fails by 1.284 dB); bar 5a **−17.01 vs −13.3**; bar 5b **−23.90 vs −22.7** — all three red |
| gang halves wired backwards (`R10b` instead of its complement) | bar 4 **7.212 vs 13.764**, fails by 6.552 dB |
| `PreAmpNetwork` state narrowed back to `float` | `testDrivePathNumericalFloor` red (−72 to −80 dBFS vs a −120 bar) |

**Gate status, honestly.** Core `ctest` green (all 24 entries; `clipper_gold_tests`,
`clipper_player_expectations_tests`, `clipper_denormal_tests` and `clipper_nan_guard_tests`
specifically re-run after every edit). Web build + `tsc --noEmit` green. Root node suites
`test:server` 15/0, `test:history` 10/0, `test:scripts` 12/0, `electron` 20/0.
**Playwright is NOT reliably runnable in this container right now** — three or four other
agents are building and running suites in parallel (load average 7+), and the offline
`OfflineAudioContext` renders time out indiscriminately. The one run made on a quiet machine
was **67 passed / 2 failed / 2 flaky, with the gold spec PASSING on the new 0.0938 bar**; a
later run under load produced 12 failures spread across RAT bypass, cab select, phaser and
chorus specs that this slice cannot touch. The two failures in the quiet run
(`amp switch: jcm800…`, `amp twin: optical tremolo`) were **reproduced on a pre-slice tree**
(HEAD `GoldModel.cpp` + HEAD artifact + HEAD spec): both are flaky there too, passing on
retry, and the JCM one passes 4/5 under `--repeat-each=5`. Its failure mode is
`result.diff === 0` — the two renders identical, i.e. the amp-swap message never landed.
The gold spec's failure mode under load is `dryRms === 0` (the bypass render came back
silent), also a harness race and not a bar. This is exactly the `retries: 2` hazard
CLAUDE.md documents; CI on an unloaded runner is the real check.

## Files created / modified

* `core/src/dsp/GoldModel.cpp` — `PreAmpNetwork` (the netlist divider), `driveLegOhms()` /
  `preAmpGangOhms()` shared by both gang halves, `AmpStageNetwork::setFromLeg`, both
  networks' state widened to `double`; `kDrivePreScale` / `kDriveHpHz` **deleted**
* `core/tests/test_gold_model.cpp` — `refPreAmpMag()`, bar (4) restated, bar (5) new,
  `testDrivePathNumericalFloor()` new (44.1 + 48 kHz)
* `core/tests/test_player_expectations.cpp` — `kBlockSettleSecs` re-derived, convergence bar
  added, GOLD reference rows re-baselined (A1 −22.6 → −22.7, A2 def −35.6 → −33.6,
  A3 THD 0.0→4.2→13.1 → 0.0→3.7→12.3, A3 treble −29.4→−18.9 → −30.2→−19.7, A4 +12.5 → +12.4)
* `web/tests/audio.spec.ts` — `pushedH3` bar 0.1 → 0.0938, derivation in the comment
* `docs/DEVELOPMENT.md` — §56
* `web/public/generated/*` — rebuilt WASM artifact (core changed)

## Deferred to next session

* **The pre-amp divider as a cascade of first-order sections.** Its worst arithmetic floor
  is −112.5 dBFS at 96 kHz × 8, above the project's −120 dBFS gate. Every pole and every
  zero of this network is real (`num = s·C3·n1·n2` factors; a passive RC ladder has no
  complex poles), so it factors exactly, and each first-order section is orders of magnitude
  better conditioned. Not taken here so this slice keeps one isolated change per proof.
  Watch the `g = 0` degeneracy: `p1 = C5·R6·Rg` vanishes and the order drops to 2.
* **The clean path is still idealized flat** (`kSumGain = 2.0`, §27/ADR 016) while the real
  FF1 leg is fed from this same divider node. `R19` appears in the drive network here only
  as the load it is. Making the clean feed the real FF1/FF2 network is its own slice and it
  is the one that would end GAIN-0 bit-exactness, so it needs an explicit owner decision.
* **The control-rate parameter sampling defect** (the standing XFAIL) is what makes the
  ragged-block startup transient exist at all. The GOLD now has the longest tail of any unit
  because of its 30 ms drive-path corner.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
