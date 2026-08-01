# The last duplicate envelope follower — unify the wah's with `SidechainDetector`, or prove it should not be

**Date:** 2026-08-01
**Branch:** claude/wah-detector-unify-6f557i
**Roadmap item:** the follow-up named in docs §58.5, §58.7 item 5, ADR 019's "Not covered
by this ADR", ADR 021's consequences, and CLAUDE.md's Deferred list — *"two envelope
followers now exist in this repo … unifying them is a named follow-up"*.

## Goal

Settle, with a measurement rather than an argument, whether `WahModel`'s envelope
follower is the same block as `SidechainDetector` and should be replaced by it — and
either do it, or record the refusal so nobody re-opens it.

## Approach

§61 extracted M13.1's detector into a real component (`SidechainDetector`), now owned by
value by `CompressorEngine` and `GateModel`. The wah's (§58) is the last one standing. It
is a very different-looking thing:

| | wah (§58) | `SidechainDetector` (§59/§61) |
| --- | --- | --- |
| rectifier | `std::fabs(x)` — ideal, no device | 2 clamp legs: coupling cap + node R + 1N4148 + 2N3904 B-E, Newton-solved per leg per sample |
| integrator | one-pole, ASYMMETRIC (attack coef vs release coef) | one RC (`C_env`, `R_env`) — a single time constant |
| attack | a coefficient (τ = 10 ms, published) | NOT a coefficient — a current-starved discharge, level-dependent (§59 measured 14/10/5/3 ms across SUSTAIN) |
| rest value | **0.0** — a zero-resting state, `flushDenormal`-guarded, and in `maxAbsRestingState()` which is asserted EXACTLY 0.0 | the SUPPLY RAIL — explicitly NOT guarded (ADR 006 scope rule, comment in the header) |
| sign | rises with signal | rests high, signal pulls it DOWN |
| threshold | none — linear in level from zero up | intrinsic, and it **is a Vbe** (§59's own words) |
| provenance | published *behavioural* figures (Geofex: attack ~10 ms, drift-back ~500 ms) — there is no GCB-95 envelope follower, the AUTO feature is synthesised | transcribed *component values* off a netlist |

Two of those rows are formal contradictions (the ADR 006 classification is opposite; the
attack is a coefficient on one side and an emergent level-dependent quantity on the
other), so the honest thing is to **build the substitution and measure what it costs**,
not to reason from the table.

So: a scratch build with `WahModel` re-plumbed onto `SidechainDetector`, given the best
component values the topology can offer for the published time constants, calibrated at
ONE point against the shipped follower, and then measured everywhere else. Whatever the
outcome, the deciding numbers go in the docs.

**Explicitly forbidden by ADR 021 and therefore not on the table:** adding a
`rectifierKind` / `idealRectifier` / `asymmetricAttack` field to `SidechainDetector` so
all three consumers fit. That is "a union of three models", named in ADR 021's own
Consequences.

## Steps

- [x] Scratch A/B tree (file copies — never `git stash`, never `git checkout --`)
- [x] Probe 1: the detector's own steady-state transfer (drive amplitude → settled node
      volts) at the wah's base rate, so the normalisation can be calibrated honestly
- [x] Build the substitution: `WahModel` holding a `SidechainDetector`, envelope pair
      picked for the published 166.7 ms release, clamp network from the gate's op-amp
      precision-rectifier form, drive gain + span calibrated so a 0.25 V input reads
      full swing (the shipped `kEnvRefVolts` calibration point, matched exactly)
- [x] Measure the §58.6 AUTO table on the substitution: rest/peak Hz, octaves, t_peak,
      t_back at SENSE 0/0.25/0.50/0.75/1.00 — before → after
- [x] Measure the property the two disagree on: envelope-vs-input-LEVEL, i.e. how far
      the filter opens as a function of pick strength, over the realistic range
- [x] Measure t_peak vs level on both (does the 82.7 ms stay a constant?)
- [x] Measure CPU (interleaved A/B, same machine)
- [x] Decide (a) unify or (b) refuse — write up whichever, with the numbers
- [x] If (b): a NEW TEST BAR in `clipper_wah_tests` that pins the distinguishing
      property, so the refusal has teeth and is not just prose
- [x] Correct `SidechainDetector.h`'s banner, ADR 021's Consequences, ADR 019's
      cross-note, §58.5 / §58.7, §59 and §61's cross-references, CLAUDE.md, ROADMAP
- [x] ADR 023 (reserved for this slice) if the outcome is a decision worth recording
- [x] Full gate run: ctest (exit code UNPIPED), goldens report, native, node, web,
      WASM artifact if `core/` changed

## How this will be measured

1. **The deciding measurement** — the envelope's law against input level. The shipped
   follower is `min(1, peak/0.25 V)`: linear in level, no threshold, and so the sweep
   depth in octaves is proportional to pick strength across the whole realistic range.
   The detector's is exponential with an intrinsic Vbe threshold (§59's finding, and
   §61.4 built a 40 dB dB-linear threshold *on* that exponential). Reported as octaves
   of sweep vs input peak, both implementations, same stimulus.
2. **The §58.6 AUTO table**, before → after, every cell. If the substitution moves a
   number, that number is reported and (if unified) re-asserted at its new value with
   its derivation — not fitted back to the old one.
3. **t_peak vs level** — §58 records 82.7 ms as the *rectifier's* behaviour on a 147 Hz
   note, constant across SENSE. A different rectifier moves it; measure by how much and
   whether it is still level-independent.
4. **Bit-identity for the two existing consumers** — `clipper-render` renders of the
   compressor and the gate, `cmp` byte-for-byte against pre-slice binaries.
5. **All five goldens at ±0.00** via `--golden-report` (report only).
6. **CPU** — `clipper-bench` `wah` row, interleaved same-machine A/B.

"It is obviously a different circuit" is not a measurement; neither is "they both
rectify and integrate".

## Manual test steps

- [x] Happy path: `build/clipper_wah_tests` green at 44.1 k and 48 k, AUTO table printed
- [x] `ctest --test-dir build --output-on-failure` — exit code checked UNPIPED
- [x] `./build/clipper_player_expectations_tests --golden-report` — five rows at ±0.00
- [x] Compressor + gate renders `cmp` byte-identical against pre-slice copies
- [x] Edge case: SENS = 0 must still be EXACTLY 0.000 octaves and the render
      bit-identical (`worst |diff| == 0.0`) to a model that never had the feature
- [x] Edge case: a *quiet* pick (0.05 V) must still move the filter — the property a
      Vbe threshold would remove
- [x] Edge case: `maxAbsRestingState()` still EXACTLY 0.0 after a silent tail
- [x] Perturbation: every new/moved bar patched red, restored green, `touch`ed BOTH times

## Out of scope for this session

- M13.3's optical compressor voice (the remaining `CompressorEngine` consumer).
- Any change to `CompressorEngine`, `GateModel`, or `SidechainDetector`'s *behaviour* —
  the two existing consumers are bit-identical, full stop.
- The other §58 open items (the GCB-95 netlist, the Q exponent gap, ADR 018's tank
  placement, inductor core saturation).
- The parallel amp-voice slice's files: `web/src/components/Amp.tsx`, `web/src/rig.ts`
  amp types, native `CabChoice` / amp panel, ROADMAP amp milestones, assistant amp prose.
- Blessing any golden. `--golden-report` only.

---

<!-- Fill in below during/after the session -->

## What actually happened

**Outcome (b): the two are genuinely different circuits. The wah keeps its own
follower; `SidechainDetector` was not widened by a single field.** Full write-up
in docs **§58.8**; the decision is **ADR 023**.

The decisive fact turned up in the very first probe and then got sharper every
time it was looked at: **`SidechainDetector` is a THRESHOLD detector, and so is
the compressor's own instance of it.** Its clamp is a DC restorer into a
base-emitter junction, so conduction is exponential, and a conducting 2N3904
delivers orders of magnitude more current than the envelope resistor's pull-up.
Open loop it goes from nothing to railed across about 2 dB of input.

Both existing consumers *want* that. The compressor's graded response is its
**feedback loop** — §59 measured the same fact from the other side (216:1
feed-back vs 3.3:1 feed-forward) — and the gate feeds it to a comparator, with
§61.4 deliberately building a 40 dB dB-linear threshold on top of the steepness.
**A wah has no gain to reduce, so it is feed-forward by necessity and cannot
borrow the loop.** It needs the opposite thing: proportional, open loop, no
threshold, down to a quiet pick.

Nothing above was accepted as an argument. The substitution was **built and
run** in a scratch tree, given the topology's best shot, and every §58.6 AUTO
number regressed — including §58.6's own acceptance bar.

Two honest notes on the way through:

* **P4 (a half-wave rectifier) was a perturbation that legitimately did NOT
  fail**, and is reported as such rather than quietly dropped. It moves the
  time-to-peak 82.7 → 90.0 ms and scales every sweep depth by 0.783 uniformly,
  but leaves the level-independence and the proportionality intact — which is
  correct, because bar 3 asserts that the attack does not move *with level*, not
  that it equals 82.7 ms. It also confirms §58.6's attribution of the 82.7 ms to
  the rectifier's zero crossings.
* **P1 trips §58.6's *existing* `prevOct > 1.0` bar before any new one is
  reached**, so it had to be re-run twice with a reordered / reduced test file to
  prove each new bar independently. Recorded below.

## Measured results

### The deciding metric — proportional range (open loop, 146.83 Hz)

Input dB span over which the envelope travels 10 % → 90 % of its swing.

| detector | envelope pair | 10 % at | 90 % at | range |
| --- | --- | --- | --- | --- |
| M13.1 compressor | 10 µF / 150 kΩ | 0.45147 V | 0.56511 V | **1.950 dB** |
| M13.6a gate | 47 nF / 220 kΩ | 0.43749 V | 0.57566 V | **2.384 dB** |
| best wah-plausible config | 10 µF / 16.67 kΩ | 0.54975 V | 1.19046 V | **6.711 dB** |
| the wah's shipped one-pole on `\|x\|` | — | — | — | **19.085 dB, no threshold** |

The sidechain drive gain is a **pure level translation** (range identical to 3 dp
across 1 → 300×). With the release pinned to the published 166.7 ms, the range is
set by the current balance alone, and reaches the follower's figure only in a
limit that is not a part:

| R_env | C_env | range |
| --- | --- | --- |
| 16.7 kΩ | 10 µF | 6.639 dB |
| 5.0 kΩ | 33.3 µF | 10.672 dB |
| 1.67 kΩ | 100 µF | 13.812 dB |
| 556 Ω | 300 µF | 15.861 dB |
| 167 Ω | 1 000 µF | 17.085 dB |
| 55.6 Ω | 3 000 µF | 17.685 dB |
| 16.7 Ω | 10 000 µF | 18.094 dB |

The wah's ideal rectifier is the **R_env → 0 limit** of the detector.

### The §58.6 AUTO table, before → after the substitution (48 kHz, 0.30 V pluck)

| SENSE | oct before | oct after | t_peak before | after | t_back before | after |
| --- | --- | --- | --- | --- | --- | --- |
| 0.00 | 0.000 | 0.000 | — | — | 0.0 | 0.0 |
| 0.25 | 0.381 | **0.239** | 82.7 ms | **144.0** | 842.7 ms | **438.7** |
| 0.50 | 0.762 | **0.478** | 82.7 | **144.0** | 841.3 | **438.7** |
| 0.75 | 1.146 | **0.718** | 82.7 | **144.0** | 841.3 | **438.7** |
| 1.00 | **1.534** | **0.958** | 82.7 | **144.0** | 840.0 | **438.7** |

44.1 kHz agrees to the decimal (t_peak 143.7 ms). **0.958 fails §58.6's own
`prevOct > 1.0`.** t_back moved 840 → 439 ms *even though `R_env·C_env` is
exactly the shipped release τ*.

### Sweep depth vs pick strength (SENS 1.00, 48 kHz) — the real damage

| pick peak | shipped oct | substituted oct | shipped t_peak | substituted t_peak |
| --- | --- | --- | --- | --- |
| 0.02 V | 0.101 | **0.000** | 82.7 ms | 178.7 ms |
| 0.05 V | 0.254 | **0.000** | 82.7 | 104.0 |
| 0.10 V (house clean probe) | 0.508 | **0.000** | 82.7 | 56.0 |
| 0.15 V | 0.762 | **0.014** | 82.7 | 56.0 |
| 0.20 V | 1.018 | **0.207** | 82.7 | 89.3 |
| 0.30 V | 1.534 | 0.958 | 82.7 | 144.0 |
| 0.50 V | 2.094 (railed) | 2.094 (railed) | 29.3 | 74.7 |
| 0.80 V | 2.094 (railed) | 2.094 (railed) | 14.7 | 32.0 |

### Other consequences

| property | shipped | substituted |
| --- | --- | --- |
| `maxAbsRestingState()` after 20 s silence | **exactly 0.0** | **2.675e-13** (fails the §33/ADR 006 bar) |
| SENS = 0 vs never-set | 0.000e+00 | 0.000e+00 (survives — the term is × 0) |
| CPU, 10 s of audio, interleaved A/B ×6, median | **0.484 s** (4.8 % of a stream) | **0.617 s** (6.1 %) |

The resting-state row is a **formal contradiction**, not a voicing regression:
the wah's `env` rests at zero (guarded, asserted exact), the detector's node
rests at the supply rail (explicitly not guarded, by ADR 006's scope rule). One
object cannot be on both sides.

### The new bar — `testFollowerLevelLaw`, shipped values (44.1 k and 48 k identical)

| bar | measured |
| --- | --- |
| quiet 0.05 V pick opens the filter (`> 0.15 oct`) | **0.254 oct** |
| 2× pick → 2× sweep (2.00 ± 0.20) | **2.002** |
| 4× pick → 4× sweep (4.00 ± 0.40) | **4.014** |
| t_peak spread across 12 dB of pick strength (≤ 2 blocks) | **0.00 ms** |

### Perturbations (patch → `touch` → rebuild → run; restore → `touch` → rebuild → run)

| # | patch | result |
| --- | --- | --- |
| P1 | the whole `SidechainDetector` substitution | **RED** (trips §58.6's existing bar first); re-run with the new test ordered ahead → **RED on bar 1**; re-run with bars 1–2 removed → **RED on bar 3** |
| P2 | Vbe-style deadband on `\|x\|` (`max(0, \|x\| − 0.06)`) | **RED on bar 1** |
| P3 | attack coefficient scaled DOWN by the envelope | **RED on bar 1** |
| P3b | attack coefficient scaled UP by the envelope (`×(1+40·env)`) | **RED on bar 2** |
| P4 | HALF-wave rectifier instead of `\|x\|` | **GREEN — reported, not hidden.** t_peak 82.7 → 90.0 ms, depths ×0.783 uniformly, spread still 0.00 ms, ratios still 2.001/4.010. Bar 3 asserts level-independence, not the constant. |

Every restore GREEN.

### The gate run

| check | result |
| --- | --- |
| core `ctest` (exit code read UNPIPED) | **100 % passed, 0 failed out of 32; CTEST_EXIT=0** (5 xfail ledgers Skipped as always) |
| all five goldens (`--golden-report`) | **UNCHANGED ±0.00** — rat_jcm800 +0.00, sd1_twin_reverb +0.00, muff_twin +0.00, ts_ac30 +0.00, clean120_chorus −0.00. Nothing written. |
| compressor bit-identity | 2 renders `cmp` **byte-for-byte identical** |
| gate bit-identity | 2 renders `cmp` **byte-for-byte identical** |
| WASM artifact | rebuilt; **`clipper.js` and the served worklet come out BYTE-IDENTICAL** (git shows only `.build-stamp.json` changed) — `check-artifact.mjs` green on 85 hashed inputs |
| `cd web && npm run build` (tsc --noEmit + vite) | **exit 0** |
| node suites | `test:server` **15/0**, `test:history` **10/0**, `test:scripts` **12/0**, `electron` **20/0** |
| native (`identical_core` / `chain_edit` / `cab_state`) | **3/3 passed** |
| Playwright | **not run, and it cannot be affected**: the served engine and worklet are byte-identical to `main`, so no web behaviour can have changed. (Port 4173 is also contended between agent worktrees.) |

## Files created / modified

- `core/tests/test_wah_model.cpp` — new `testFollowerLevelLaw` + header block (l)
- `core/include/clipper/dsp/SidechainDetector.h` — "THIS IS A *THRESHOLD*
  DETECTOR" banner (comment only; no code change)
- `core/include/clipper/dsp/CompressorEngine.h` — cross-slice note settled
  (comment only)
- `docs/DEVELOPMENT.md` — new **§58.8**; §58.5's cross-slice note and §58.7 item 5
  closed
- `docs/decisions/023-the-wah-follower-is-not-the-shared-detector.md` — new
- `docs/decisions/019-…` / `021-…` — outcome recorded against the follow-up each
  of them named
- `CLAUDE.md`, `ROADMAP.md` — the follow-up closed wherever it appeared
- `web/public/generated/{clipper.js,clipper-processor.js,.build-stamp.json}` —
  rebuilt, because two `core/include` comment blocks changed

## Deferred to next session

- **M13.3's optical compressor** is still expected to reuse `SidechainDetector`
  (a feed-back compressor with a gain cell — the case the component is for). The
  rule this slice adds is procedural: **build the substitution and measure it**.
- The other §58 open items are untouched: the GCB-95 netlist, the Q exponent gap
  (−1.000 derived vs −0.870 measured), ADR 018's tank placement, inductor core
  saturation, `kMaxChain`'s one-instance-per-type limit.
- Nothing about this slice is deferred. The follow-up is **closed**, not punted.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
