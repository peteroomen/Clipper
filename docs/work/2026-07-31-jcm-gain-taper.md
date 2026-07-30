# JCM800 GAIN taper — breakup onset 20 → 30, top of the knob pinned

**Date:** 2026-07-31
**Branch:** claude/jcm-gain-taper-6f557i
**Roadmap item:** owner feedback round 3 (Drive doc "Clipper Feedback", 2026-07-31):
"Breakup is still slightly early. 20 sounds like what I want 30 to sound like. I like
the total saturation though, 100 is perfect right where it is." Post-#21/#23 re-test at
unity trim; chugs and GAIN 100 confirmed right ("sounds the same as Logic Pro's JCM").

## Goal

The GAIN knob's breakup onset moves from ~20 to ~30 with GAIN 1.0 bit-identical —
a taper-law reshape only, no circuit change.

## Approach

Deliberate tone change (knob-feel), judged by a measured THD-vs-knob curve. The GAIN
pot's audio taper is `audioTaper(x) = (e^{kx} − 1)/(e^k − 1)` with `k = 4`
(Jcm800Preamp.h, exposed for tests). k was calibrated before the finding-7/8 + bright
cap work un-starved the drive path, so the same knob now reaches breakup earlier —
the constant is stale, not wrong-in-kind. Raising k pushes the low knob quieter while
`audioTaper(1) = 1` keeps the top EXACTLY pinned by construction.

Derivation is by measurement, not by picking a k that "feels right": sweep the
composed amp's THD vs knob (220 Hz, the §45 probe level, unity-trim-equivalent
input), find the knob where THD crosses the onset bar (the same ≥ 5 % convention the
round has used), and choose the k that puts today's knob-0.20 drive at knob 0.30
(equivalently: solve audioTaper_new(0.30) = audioTaper_old(0.20) → k ≈ solve, then
verify the measured onset lands 0.30 ± 0.02 and the curve stays monotonic). MASTER
uses the same audioTaper — it must NOT change: the taper constant becomes per-pot
(kGainTaperK new, kMasterTaperK = 4 verbatim) unless measurement shows the master
feel unaffected either way; the owner's report is about GAIN only.

## Steps (for the implementing agent)

- [ ] `Jcm800Preamp`: split the taper constant — GAIN gets the re-derived k, MASTER
      keeps k = 4 byte-for-byte. Comment carries the derivation (old k, new k, the
      measured onset table) per house style.
- [ ] Measurement harness (scratch, not committed): THD vs knob 0.05…1.00 step 0.05
      through the composed Jcm800Amp at the §45 probe convention, before/after table
      printed. GAIN 1.0 render hash identical before/after (the pin).
- [ ] `core/tests/test_jcm800_*.cpp`: a perturbation-proven bar — onset (THD ≥ 5 %)
      at knob 0.28–0.34, and audioTaper(1.0) == 1.0 exactly; reverting k to 4 must
      fail the onset bar (record the perturbation in the plan/PR).
- [ ] Player-expectations A3/A4 jcm rows re-baselined if they move (GAIN default 0.5
      drive drops slightly — the documented rows must match the new measurement).
- [ ] `--golden-report` for rat_jcm800 (default GAIN 0.5 → the golden WILL move a
      little); table prepared for the owner, NO bless in the implementing agent —
      the verifier (main session) presents it.
- [ ] WASM rebuild (`bash scripts/build-wasm.sh`) + all three artifacts committed.
- [ ] Full core ctest; web build + Playwright; node suites. Docs §51 + CLAUDE.md
      entry drafted (verifier reviews before commit).

## How this will be measured

The before/after THD-vs-knob table (220 Hz composed-amp probe): onset 0.20 → 0.30 ±
0.02, monotonic, GAIN 1.0 bit-identical by render hash, MASTER law byte-identical.
Golden delta table for rat_jcm800 presented to the owner before any bless.

## Manual test steps

- [ ] Owner: breakup begins ≈ 30 (was 20); 100 unchanged; sweep still smooth
- [ ] Edge: GAIN 0 still fully clean; knob 0.5 slightly cleaner than before (expected
      — the default sits between onset and saturation)

## Out of scope for this session

Any circuit change (the preamp/PI/power are validated by the Logic A/B), MASTER
taper, the other round slices.

---

## What actually happened

(fill in)

## Measured results

(fill in)

## Files created / modified

(fill in)

## Deferred to next session

(fill in)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
