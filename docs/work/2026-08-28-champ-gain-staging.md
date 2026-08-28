# Champ field report — "too distorted at 20, unusable maxed"

**Date:** 2026-08-28
**Branch:** fix/champ-gain-staging
**Roadmap item:** M10.10 field report (docs §73), owner report 2026-08-28

## The report

> "even on 20 it's quite distorted, maxed out it's completely unusable. not sure
> when they start saturating but not this much and not at 20 surely."

## What was measured BEFORE anything changed (§43's rule)

Identical 0.15 V / 220 Hz input, every amp at its own shipped defaults:

| amp | out | THD |
|---|---|---|
| **Champ @ 0.20** | **−11.03 dBFS** | **16.65 %** |
| JCM800 | −23.46 | 10.31 |
| AC30 | −27.06 | 0.63 |
| Twin | −40.05 | 0.64 |

**The Champ is 12.4 dB louder than the JCM and 29.0 dB louder than the Twin.**

And the knob is dead over most of its travel — §63.14's exact pathology:
VOL 0.30 → −8.33 dBFS / 28.6 % · 0.50 → −7.55 / 65.9 % · 1.00 → −8.36 / 68.3 %.
**0.03 dB of level across the top 70 % of the knob, and 29 % → 68 % of THD.**

**What was RULED OUT first, each against an external reference:**
- Preamp stage gains: **59.29× / 59.42×** against an analytic 59.5×. Right.
- The 6V6 card at the datasheet's OWN condition: **5.04 W at 10 % THD** against a
  rated 4.5 W, asymmetry 1.15×. Right — the card is not the problem.
- The DC point: still matches Fender's published 19 V / 37 mA / 305 V.
- The OT primary: 5 kΩ, confirmed sourced. Right.

## THE ROOT CAUSE, and it is a transcription error in the one netlist source

**A stock 5F1 leaves V1B's 1.5 kΩ cathode resistor UNBYPASSED.** That is the
original design — deliberate cathode degeneration (local negative feedback), and
adding a bypass cap there is a widely-documented MOD people make specifically to
get "a very useful gain/treble boost". `valtyr/rust-5f1`'s capacitor table carries
`C_K1B: 25µF`, and §73.1 already records TWO other errors in that same source (it
omits the 6V6's cathode bypass cap, and its 100 Ω supply lands the plate node 15 V
high). **This is the third, and it is the one that matters.**

Measured: V1B bypassed **59.42×**, unbypassed **30.00×** against the analytic
`µRL/(RL+rp+(µ+1)Rk)` = 29.9×. **−5.95 dB of drive to the 6V6 grid**, and the
whole preamp halves from ~3600× to ~1800×.

## Approach

Deliberate TONE CHANGE, and it is a correction rather than a re-voice.

1. **`ChampPreamp`: V1B `Ck` 25 µF → 0** (unbypassed). Sourced.
2. **Re-derive `kFullScaleSecV`** — the current value was derived from the CRANKED
   peak, which is the wrong reference for an amp that compresses this hard: pinning
   the cranked peak to 0.9 drags the whole usable range up with it. Re-derive so the
   Champ sits IN THE LINEUP at its default, the §68 staging question.
3. **Re-derive the shipped default** `champVolume` from the corrected geometry.
4. **Do NOT re-taper.** §63.14 refused exactly this for the Rockerverb and the same
   arithmetic applies here: spreading this amp's useful wiper range across the knob
   needs k ≈ 10–12, which is outside §58's documented 10–20 %-at-half-rotation
   audio-taper spec, i.e. a pot that does not exist.

## How this will be measured

- **Lineup staging:** the Champ's default output must land within a few dB of the
  JCM/AC30 rather than 12–29 dB above. New bar in `clipper_champ_tests`.
- **Drive to the 6V6 grid** at VOL 0.20 / 0.15 V input: 12.05 Vpk → expect ~6 Vpk.
- **THD at the default** and across the knob, before → after.
- **V1B's gain against the analytic unbypassed form** — a hard assert, so nobody
  "restores" the bypass cap thinking it is the bug.
- All five goldens UNCHANGED (no shared class edited) — the scope check.
- Core ctest, native, Playwright, node/electron all green.

## Manual test steps

- [ ] Play the Champ at VOL 20 — should be usable, edge-of-breakup rather than fizz.
- [ ] Sweep the knob — the level should keep rising further up the travel than before.
- [ ] Switch Champ ↔ JCM800 at defaults — no jarring level jump.
- [ ] Edge case: maxed is still a fuzzbox (that IS a cranked Champ), but it should
      get LOUDER as well as dirtier over more of the knob.

## Out of scope

- Re-tapering the volume pot (see above — refused with the arithmetic).
- The Koren screen law's missing `Vp` dependence (§73's standing follow-up).
- Audit findings 9/10 on the push-pull amps.

---

## What actually happened

The hypothesis in "Approach" was half right. `kFullScaleSecV` was **checked and NOT
changed** — cranked, all four amps already agree (Champ peak 0.834, JCM 0.839, AC30
0.939, Twin 0.421), so the normalisation was never wrong. What differed was how far
each amp's DEFAULT sits below its own maximum: Champ **7.8 dB**, JCM 16.4, AC30 18.9,
Twin 28.7. So the fix is the circuit correction plus the default — §63.14's conclusion
for the Rockerverb, reached independently a second time.

The re-taper was refused with arithmetic rather than by taste: spreading this amp's
useful wiper range (0.001–0.05) across the knob needs k ≈ 10–12, which puts the wiper
under 1 % at half rotation, outside §58's documented 10–20 % audio-taper spec.

## Measured results

| | before | after |
|---|---|---|
| V1B stage gain | 59.42× | **29.92×** (analytic 29.86×) |
| default | 0.20 | **0.10** |
| output at default | −11.03 dBFS | **−23.90** (JCM: −23.46) |
| vs the JCM at defaults | **+7.29 dB** | **−0.43 dB** |
| THD at default | 16.65 % | **4.41 %** |
| headroom above the default | 7.8 dB | **16.18 dB** |
| touch sensitivity (soft→hard) | 2.84× | **5.8×** (1.51 % → 8.75 %) |
| composed cranked power | 3.89 W | **4.47 W** (rated ~5) |
| h2 contrast vs the Twin | 24.88 dB | 21.90 dB (bar 12) |

Unchanged and re-verified: Fender's measured DC point (cathode 0.9943×, Ip 0.9805×,
Ik 0.9950×, Vpk 1.0004×), the datasheet screen fit, the plate knee at 88.4 mA, no-NFB
bit-identity, `kFullScaleSecV`, and the `tweed8` cab.

**Suites:** core ctest **41/41** with the goldens gate passing (all five UNCHANGED,
nothing blessed — no shared class edited); native **4/4** plus a clean editor compile;
node 15/10/12; electron 20; tsc + web build clean; artifact rebuilt (115 inputs).

**Perturbations, both halves independently (§58.8):** P1 restore the bypass cap → RED;
P1b same with the earlier bars removed → RED **on the staging bar**; P2 circuit correct
but default back to 0.20 → RED **on the staging bar**. All restores GREEN.

## Files created / modified

`core/include/clipper/dsp/ChampPreamp.h` (+`kCkV1b`), `core/src/dsp/ChampPreamp.cpp`,
`core/tests/test_champ_amp.cpp` (+`testLineupStaging`, re-derived breakup bars),
`web/src/rig.ts`, `web/src/assistant/{tools,prompt}.ts`, `web/tests/{amp,audio}.spec.ts`,
`native/src/ClipperEngine.h`, `native/src/PluginProcessor.cpp`,
`web/public/generated/*`, `docs/DEVELOPMENT.md` (§75), this file.

## Deferred to next session

- **Give every other amp voice the same staging bar.** This slice adds it to the Champ
  only. The other six defaults have never been compared against each other, and the
  Twin sits 28.7 dB below its own maximum — possibly the same defect inverted.
- The `rust-5f1` netlist now has **three** recorded errors; nothing else from it should
  be trusted without an independent check.
- §73's standing items are unchanged (the Koren screen law's missing `Vp` dependence,
  findings 9/10 on the push-pull amps, `kRptSecondary` as a reconstruction).

## Status

- [ ] In progress
- [x] Complete
