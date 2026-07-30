# GOLD dirt summing weight — the schematic's network, not a fit

**Date:** 2026-07-31
**Branch:** claude/gold-summing-6f557i
**Roadmap item:** owner feedback round 3 (Drive doc "Clipper Feedback"): "Much better,
but still too much gain. Edge of breakup is around 5, 0 is fully transparent which is
good, 100 gain and it sounds like a marshall at mid-high gain. So still too much gain
for sure, really they only get creamy/crunchy at max, they don't reach marshall levels.
Not too warm/tinny or vowley now, sounds great."

## Goal

The GOLD's dirt loudness comes from the published schematic's summing network instead
of the §50 fit (`kClipBlendWeight = 0.65`): audible breakup onset moves well past
knob 5, and max gain reads creamy/crunchy — while the tone character (which the owner
has signed off) and the GAIN-0 bit-exact-clean contract stay untouched.

## Approach

Deliberate tone change, judged against the published schematic + the reference THD
rows the §50 slice already used. The insight from the feedback: our max measures
15.3 % THD — inside the real unit's published 15–25 % band — yet reads as "Marshall
mid-high gain". Perceived gain tracks the DIRT'S LOUDNESS AGAINST THE CLEAN, not THD
alone, and `kClipBlendWeight = 0.65` is the last constant in this pedal that is a fit
rather than a derivation.

Derive the real summing ratio: in the published circuit the output summing stage
mixes the clean feed and the diode path through fixed resistors (the knob changes
drive, not mix — §50). The implementing agent must pull the actual component values
from the named references (the ElectroSmash Centaur analysis and/or the Chowdhury
KlonCentaur source, both already cited in GoldModel.cpp §50 comments; network access
is available) and compute the dirt-branch weight RELATIVE to the clean feed at the
summing node, including any post-diode attenuation the real network has before the
summing resistor. Replace 0.65 with the derived value (name the resistors in the
comment); reconsider `kClipBlendFadeTo` in the same derivation (the fade-in span is
also a §50 idealization — if the real network implies dirt stays buried under the
clean at low knob, the fade may become unnecessary or shorter; keep `clipBlend(0)=0`
as the documented contract either way).

HONESTY GATE: if the schematic-derived weight does NOT land the owner's percept
(breakup onset still early in an A/B render), do NOT re-fit to taste — ship the
derived value with the measured tables and report the gap; the next probe is then the
diode-node drive, not the mix.

## Steps (for the implementing agent)

- [ ] Research: extract the summing-network component values from the references;
      document the derivation (resistor names + arithmetic) in the plan file and the
      code comment
- [ ] `GoldModel.cpp`: derived weight replaces 0.65; fade-in span re-examined in the
      same derivation; GAIN-0 render hash IDENTICAL before/after (the transparency
      contract — assert by hash in the report)
- [ ] Measure: THD + output level vs GAIN (0.05 steps, 220 Hz, 0.1 V AND 0.15 V peak
      — the unity-trim pickup level the JCM slice established as the field-report
      anchor), plus the dirt-to-clean level ratio at the summing node per knob.
      Before/after tables in the plan file
- [ ] Acceptance: near-clean at knob ≤ 0.10 at the 0.15 V anchor (THD within ~2× of
      the GAIN-0 floor); audible-grit onset mid-knob; max THD stays inside the 15–25 %
      reference band; dirt-vs-clean ratio = the schematic value by construction
- [ ] `test_gold_model.cpp`: re-derive the crossfade/level-contrast/knee probes IF the
      new weight moves them off their properties (§50 precedent — argue each in
      diode-node/summing-node units in comments); add a perturbation-proven bar
      pinning the derived weight (reverting to 0.65 must fail); prove teeth with the
      touch-after-patch-AND-restore discipline
- [ ] Player-expectations gold rows re-baselined (A1/A3/A4 + the A4 window if the
      default level moves)
- [ ] `--golden-report`: must show ZERO changed goldens (GOLD is in no golden rig;
      the GAIN-0 contract protects the transparency spec) — record the clean report
- [ ] WASM rebuild + artifacts committed; full core ctest (expect 25/25 — no golden
      moves on this branch); web build + Playwright (the gold worklet spec may need
      probe re-derivation if its pushed-harmonic bar moves — same honesty rules);
      node suites
- [ ] Docs §52 + CLAUDE.md entry + this plan file's bottom sections
- [ ] ONE commit on claude/gold-summing-6f557i (fix: …), measured before→after in the
      body, standard trailers; NO push, NO PR, NO golden writes

## How this will be measured

The before/after THD + level tables at both input levels, the dirt-to-clean summing
ratio table, the GAIN-0 hash pin, and the derivation arithmetic from named schematic
components. Owner's ear-test afterwards: edge of breakup well past 5; 100 creamy.

## Manual test steps

- [ ] Owner: GAIN 0 transparent; breakup onset well past 5; 35 mostly clean;
      100 creamy/crunchy, NOT Marshall; then GOLD-as-boost into the JCM (the famous
      use) — staging sanity
- [ ] Edge: Ge/Si contrast still ~6 dB at high drive; NaN rejected; sweep no zipper

## Out of scope for this session

The drive gang law (§50, validated), the treble network (owner signed off the tone),
the op-amp model, all other pedals.

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
