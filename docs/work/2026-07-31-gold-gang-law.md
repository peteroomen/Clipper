# GOLD gain gang law — the schematic's dual-gang, not a linear pot

**Date:** 2026-07-31 (overnight)
**Branch:** claude/gold-gang-law-6f557i (stacked on claude/muff-series-rs-6f557i)
**Roadmap item:** field-report round, GOLD slice ("warm, vowley, tinny… gainy at even
35+"; "still warm, and too saturated really") + the 2026-07-31 Klon research report

## Goal

The GOLD's GAIN knob behaves like the real pedal's: mostly-clean with a little grit at
the shipped 0.35 default (the "always-on" setting), real saturation arriving in the top
half, drive spanning the schematic's 4.6–25.8× instead of the linear 1–67.7×.

## Approach

Deliberate TONE change, judged against the published schematic law and the research
report's reference THD rows (the A/B the owner and I agreed the round runs on).
Three corrections, one slice (same physical network): the drive law
`A = 1 + 422k/((1−g)·100k + 17k)` (gang 1 in the op-amp's ground leg, end-loaded), the
drive-branch input attenuation (`kDrivePreScale = 0.65` + a 600 Hz HP — the 106 Hz
corner belongs to the always-on clean-bass path and had been mis-assigned), and the
blend laws (clean feed flat, dirt summing weight fixed at 0.65; the knob changes DRIVE,
not mix). One idealization kept deliberately and documented in-code: `clipBlend(0) = 0`
via a short fade-in below g = 0.15, preserving the model's bit-exact-clean GAIN-0
contract (the real unit measures 0.2–3.9 % even at min).

This is the one slice in the overnight stack that needs **no golden bless**: GOLD is in
no golden rig, and GAIN 0 is bit-exact unchanged.

## Steps

- [x] GoldModel.cpp: new law constants + `driveGainAt`/`cleanBlendAt`/`clipBlendAt`
      re-derived; drive-loop pre-scale; header-comment architecture block corrected
      (including the OUTPUT-1 = +6.02 dB note)
- [x] test_gold_model.cpp: gang law asserted at both ends + symbolically; flat clean
      feed asserted; germanium-knee and Ge-vs-Si probes re-derived to reach their
      properties under the attenuated drive (comments explain why)
- [x] Perturbation proof: restore the pre-§50 linear law → suite fails at the gang-law
      assert → restore → green (touch after patch AND restore)
- [x] Player-expectations gold reference rows re-baselined (A1/A2/A3/A4 + the A4
      window re-centred 3..23 → −3..17)
- [x] `--golden-report`: exactly the three inherited stack deltas, nothing new
- [x] Docs §50, this file, CLAUDE.md; WASM rebuild; web Playwright; PR #26

## How this will be measured

THD at 220 Hz (0.1 V / 0.3 V input) vs the research report's reference rows:
default 0.35 lands "mostly clean, a little grit" (≤ ~7 %), max lands 15–25 %,
monotonic, GAIN 0 exactly 0.00 (contract). Drive span 4.61×–25.82× asserted.

## Manual test steps

- [ ] Web: GOLD at defaults (35/50/70) — clean core with a little hair, touch-responsive;
      roll GAIN to 80+ — real saturation arrives end-loaded
- [ ] GAIN 0: bit-transparent boost (unity at OUTPUT 50)
- [ ] Edge: GAIN swept 0→100 while playing — no zipper (smoothers unchanged); NaN
      param rejected; Ge/Si switch still ~6 dB apart at high drive

## Out of scope for this session

The op-amp GBW/slew model (unchanged), the treble network (unchanged — re-judge
"tinny" after this lands), RAT polish, SD-1/TS rolloff (next slices in the queue).

---

## What actually happened

As planned. Two suite probes turned out to be calibrated to the old too-hot drive and
had to be re-derived to keep reaching their own properties (germanium knee: max gain +
0.025–0.25 V sweep; Ge-vs-Si contrast: 0.6 V at gain ≥ 0.35) — bounds unchanged in the
knee test, and both re-derivations are argued in comments in units of diode-node
voltage. The A4 default-rig window was re-centred on the new measured +7.3 dB.

## Measured results

Default 0.35: 19.2/13.0 % → **1.41/6.37 %** THD (0.1/0.3 V, 220 Hz). Max: 30.6 →
**15.3/15.2 %** against the reference 15–25 % band. Drive law 4.6068×/25.8235×
asserted at both ends. Defaults RMS −22.0 → −27.8 dBFS; default-rig delta +13.2 →
+7.3 dB. Perturbation: pre-§50 law fails the gang-law assert, restore green. Golden
report: only the three inherited stack deltas (rat_jcm800 −0.44, sd1_twin_reverb
−0.83, muff_twin −3.06); ts_ac30 + clean120_chorus UNCHANGED. Full GOLD suite green;
core ctest 24/25 with the single failure being the stack's held-bless rat_jcm800.

## Files created / modified

- `core/src/dsp/GoldModel.cpp` (laws, constants, drive pre-scale, comments)
- `core/tests/test_gold_model.cpp` (gang-law asserts, probe re-derivations)
- `core/tests/test_player_expectations.cpp` (gold reference rows re-baselined)
- `web/public/generated/*` (WASM artifact), docs §50, this file, `CLAUDE.md`

## Deferred to next session

- Re-judge "tinny" with the owner after this lands (treble network untouched)
- The fixed-weight/fade-in constants are fits; if the owner's A/B against a real
  Centaur-family unit disagrees, re-fit from that measurement
- RAT polish, SD-1/TS tone rolloff, JCM taper decision (queue order stands)

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
