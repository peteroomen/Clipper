# Muff SUSTAIN taper — give the bottom of the knob its authority back

**Date:** 2026-07-30
**Branch:** claude/amps-pedals-fixes-6f557i
**Roadmap item:** owner field report 2026-07-30 ("way too gainy, even on sustain=14 it's pretty powerful") — measured to be the Muff, not the RAT; the low-knob half of the §37/§34 sustain story

## Goal

SUSTAIN below ~0.3 becomes a genuinely tame fuzz — at knob 0.14 with a realistic 0.1 V pluck the pedal
measures roughly −25 dBFS / ~15 % THD instead of today's −4.7 dBFS / 43 % — while everything from the
shipped default (0.6) up stays **bit-identical**, so the `muff_twin` golden and the wall tests do not move.

## Approach

Deliberate tone change **at low knob positions only**; the circuit is untouched — this is a control-law
(pot taper) fix, the same class as the §23 CUT re-taper.

Measured root cause (diagnosis session 2026-07-30, scratch harnesses in the session scratchpad): the clip
stages' clean window ends at ~1–3 mV at the base (the diodes conduct at idle — documented canon,
BjtStage.h), but the SUSTAIN taper floor is −54 dB, so at knob 0 a 0.1 V input still puts ~11.5 mV into
Q2 — 4–10× past clean. Across the whole travel the output moves < 2.2 dB and THD never drops below 27 %:
the knob has no authority. Knob 0.14 is actually the *hottest* point of the sweep (Q2/Q3's gain-expansion
hump, +6.7 dB at ~30 mV, parks exactly there). A −80 dB floor measured −37.4 dBFS / 3.5 % at knob 0 and
−25.2 / 15.3 % at 0.14 in the counterfactual — "fuzzy but tame".

The new law is **piecewise decibel-linear with the break at the shipped default**:

- knob ≥ 0.6: the existing law, verbatim — `kClipDriveMax · 10^((−54/20)·(1−k))`. Same expression, same
  floating-point result, so the default and everything above it is bit-identical by construction.
- knob < 0.6: decibel-linear from **−80 dB** (knob 0) up to the −21.6 dB the old law gives at 0.6.

A real audio pot is manufactured as two resistive segments meeting mid-rotation — a piecewise-linear-in-dB
law with a slope break is *more* faithful to the part than a single exponent, and the break lands at the
default, which is the one point we must not move.

## Steps

- [ ] Reshape `sustainDrive()` in `core/src/dsp/MuffModel.cpp` (new `kSustainMinDb = −80`,
      `kSustainBreak = 0.6`); update the taper comment block with the measured rationale
- [ ] Measure the full sweep (0 → 1, incl. 0.14) at 0.1 V and 0.316 V, before/after tables
- [ ] Verify bit-identity for knob ≥ 0.6 (render compare at 0.6 / 0.8 / 1.0) and
      `--golden-report` shows `muff_twin` at 0.00 dB
- [ ] Tighten `testSustainRange` to pin the *player property* the owner asked for: at SUSTAIN ≤ 0.15 with
      a realistic input, THD ≤ 20 % and RMS ≥ 15 dB below the wall — bounds set from measurement
- [ ] Re-measure and update the A2/A3 reference tables in `test_player_expectations.cpp` (min-knob rows
      change; default/max rows must not) and confirm A1 min-knob usability + A2 hum bar still pass at the
      new, more linear sustain floor
- [ ] Perturbation-proof the new assertions: revert the floor to −54 in a scratch copy, confirm red,
      restore (touch after both patch and restore)
- [ ] Full core ctest; `bash scripts/build-wasm.sh` (core changed) and commit artifacts
- [ ] Docs: §43 (new — this slice), cross-note in §37's taper paragraph; update CLAUDE.md Current State

## How this will be measured

The SUSTAIN sweep table (RMS dBFS + THD % at 220 Hz, 0.1 V and 0.316 V), before → after:
knob 0.14 from −4.7 dBFS / 43 % to ≈ −25 dBFS / ≈ 15 %; knob 0 from −6.2 / 36 % to ≈ −37 / ≈ 3.5 %;
knob 0.6 / 0.8 / 1.0 **bit-identical** (memcmp on renders); `muff_twin` golden 0.00 dB in
`--golden-report`. Tool: a scratch sweep harness against `libclipper_dsp.a` + the existing test targets.

## Manual test steps

- [ ] Web or native: SUSTAIN at ~0.15, volume 0.6, play — a tame, dynamic fuzz that cleans up with
      pick attack; sweep up to 0.6 — smoothly rises into today's exact default sound; above 0.6 —
      unchanged wall
- [ ] Edge: SUSTAIN 0 is quiet-but-not-silent (the escape hatch survives, just deeper); no zipper
      noise sweeping through the 0.6 break (knob smoothing is upstream of the taper)
- [ ] Edge: NaN/inf to PARAM_SUSTAIN still rejected at the ABI (no taper code runs on it)

## Out of scope for this session

The clip stages' ~2 mV headroom question (whether the Big-Muff-canon idle bias is itself right — research
slice, external reference needed); the Muff bass defect (kXfMuffBass, ADR 009's deferred half); every
other pedal/amp finding from the 2026-07-30 diagnosis round (they follow as their own slices).

---

<!-- Fill in below during/after the session -->

## What actually happened

As planned, with one measured deviation: the floor went to **−84 dB, not −80**. −80 left
knob 0.14 at 20.5 % THD (above the ~15 % target the field report implies); −84 measures
14.2 % and keeps the knob-0 escape hatch at −41.5 dBFS, well above the A1 audibility
floor. Chosen by running the sweep at both values, per §43.2.

A useful property fell out of the parameterization: `kSustainMinDb = −54` reproduces the
old single-exponent law *exactly* (both dB segments collapse onto one line), so the
perturbation proof is a one-constant flip — done, red on the new THD bar (43 %), restored,
`touch` after both edits.

Bit-identity of the pinned region was verified at suite level against unmodified `main`
(git stash → rebuild → compare): A1 defaults, A2 default-gain, A3 probes 0.6/1.0 and the
golden report are identical to the digit. Two reference figures in the A1/A2 comment
tables (`muff def −55.8`, `defaults 1.41 V peak`) turned out to be stale on `main`
*before* this slice — left for their own cleanup; only the rows this slice moved were
re-baselined.

## Measured results

See docs §43.3 for the full tables. Headlines (0.1 V, 220 Hz, 48 kHz):
knob 0.14: **−4.7 dBFS / 42.8 % THD → −26.0 / 14.2 %**; knob 0: −6.2 / 36.4 → −41.5 / 1.9;
knob ≥ 0.6 **bit-identical** (`muff_twin` golden Δ 0.00 dB — no blessing needed).
A2 hum at min gain improved 10.6 dB (−51.4 → −62.0). New test bars: SUSTAIN 0.15 < 20 %
THD and ≥ 15 dB below the wall (measured 16.1 % / 19.9 dB × 3 rates), perturbation-proven.
Core ctest 26/26.

## Files created / modified

- `core/src/dsp/MuffModel.cpp` — the piecewise taper + rationale comment
- `core/tests/test_muff_model.cpp` — the tightened `testSustainRange`
- `core/tests/test_player_expectations.cpp` — A2/A3 Muff reference rows re-baselined
- `web/public/generated/` — clipper.js / clipper-processor.js / .build-stamp.json (rebuilt)
- `docs/DEVELOPMENT.md` §43, this plan file, `CLAUDE.md` Current State

## Deferred to next session

- The clip stages' ~2 mV idle headroom question (research slice, external reference)
- The Muff bass defect (ADR 009 / `finding16-muff-almost-no-bass`) and the slam ledger (§34)
- The stale A1 comment figures on `main` noted above (comment-only cleanup)
- The rest of the 2026-07-30 round: Twin volume position, JCM Ra2, JCM bright cap, AC30

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
