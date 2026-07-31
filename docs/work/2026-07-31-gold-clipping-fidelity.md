# GOLD clipping-stage fidelity — fix the three errors the summing weight was hiding

**Date:** 2026-07-31
**Branch:** claude/gold-fidelity-6f557i (stacked on claude/gold-summing-6f557i, draft PR #31)
**Roadmap item:** owner decision 2026-07-31 ("Full fidelity slice") on the §52 finding:
the schematic-faithful summing weight (4.1702, derived, kept) exposed that the old fit
was compensating three named departures from the real circuit. Field target (owner):
edge of breakup well past knob 5; max = creamy/crunchy, NOT Marshall; GAIN 0 transparent.

## Goal

The three §52-named errors fixed against the reference (the cloned Chowdhury
KlonCentaur netlist + DAFx-19 paper + schematic figures, already validated by
reproducing the §50 gang law), with the derived summing weight retained:

1. **The diode node** — the reference's measured clipper fit (Is = 15 µA,
   Vt = 25.85 mV — a 0.109 V soft knee) behind the schematic's **R13 = 1 kΩ** source
   resistance (ours ran a 0.286 V knee behind 2.2 kΩ — ~2.1× hot at the node).
2. **The 495 Hz summing pole** (R20 ∥ C13 = 392 kΩ ∥ 820 pF) — the "creamy" filter —
   modeled TOGETHER with whatever clean-path normalization keeps the GAIN-0 contract
   (the pole alone carves −14 dB of mids from the clean path; the real unit's
   following stages sit after it too).
3. **Gain-dependent drive shaping** — C7 = 82 nF across R10b makes the drive amp's
   gain frequency- AND knob-dependent (1 kHz gain ~100× at g = 1 vs the 25.8× DC
   law); ours is flat.

The §52 XFAILs (`gold-summing-rails-engage`, `gold-summing-alias-at-treble-max`) name
this slice as their fix: they must XPASS → delete → harden, or their measured numbers
must improve with an honest re-owned decl.

## Approach

Deliberate fidelity change judged against the reference implementation itself: the
agent can RENDER the reference topology's transfer curves from the netlist values and
compare ours stage-by-stage (drive node level, diode node voltage, post-summing
spectrum) — the strongest oracle this pedal has ever had. Contracts that stand:

- **GAIN-0 transparency**: the composed clean path at GAIN 0 / TREBLE noon /
  OUTPUT 0.5 stays unity-flat. Prefer bit-exact by construction (normalize the
  clean-path composition so H(noon) ≡ the §27 clean response); if the faithful
  topology cannot deliver bit-exact, the fallback contract is |H| within 0.25 dB
  20 Hz–10 kHz with the decision documented ADR-style — do NOT silently change what
  "transparent" means.
- **The Ge/Si counterfactual**: refitting the germanium moves the Ge-vs-Si contrast;
  re-derive kSiIdeality's counterpart honestly (the property is the CONTRAST, ~6 dB
  at high drive — keep the property, re-derive the probe, §50 discipline).
- The drive gang law A(g) at DC stays the §50 schematic law by construction (C7 adds
  the frequency dependence on top; the DC law is the identity check).

Field acceptance (0.15 V anchor, 220 Hz): near-clean at knob ≤ 0.10; audible-grit
onset mid-knob or later; max THD inside the reference 15–25 % band WITH the summing
pole's spectral tilt present (measure the dirt's 3 kHz-vs-500 Hz ratio before/after —
"creamy" is a measurable darkening); output level at max within a few dB of the §50
level (the +13.3 dB overshoot must come back down as the diode node softens).
HONESTY GATE: if the faithful trio still misses the percept, ship faithful + report
the gap with stage-by-stage tables against the reference render.

## Steps (for the implementing agent)

- [ ] Start from origin/claude/gold-summing-6f557i (fetch + branch); read §52's code
      comments, draft PR #31's description, the cloned reference (re-clone if needed)
- [ ] Stage-by-stage comparison harness FIRST (ours vs reference-derived curves):
      drive-node |H| at 3 knob points, diode-node clip curve, post-summing spectrum
- [ ] Fix 1: diode fit + R13; Fix 3: C7 shaping (netlist zero/pole); Fix 2: the
      summing pole + clean-path normalization per the contract above
- [ ] Re-measure EVERYTHING §52 tabled (THD/level/dirt-ratio at both input levels)
      plus the creamy-tilt metric; before/after/reference three-way tables
- [ ] XFAILs: rails-engage + alias-shadow re-measured → XPASS→harden or re-own
      honestly; knee/contrast probes re-derived as needed (node-voltage arguments)
- [ ] Perturbation-proven bars for each of the three fixes (revert each alone → red)
- [ ] Player-expectations gold rows re-baselined; A4 window re-centred if needed
- [ ] `--golden-report` ZERO changed (GOLD in no golden rig; GAIN-0 contract);
      full core ctest green modulo ledgers; WASM rebuild; web build + Playwright
      (gold worklet spec: honest re-derivation if moved); node suites
- [ ] Docs §54 (§53 is the Muff slice, in flight) + ADR for the transparency-contract
      decision if the fallback fired + CLAUDE.md entry + plan bottom sections
- [ ] ONE commit on claude/gold-fidelity-6f557i, fix: …, tables in the body,
      standard trailers; NO push, NO PR, NO golden writes

## How this will be measured

The three-way tables (before / after / reference), the onset + THD + level + tilt
acceptance rows at the 0.15 V anchor, the GAIN-0 contract proof (hash or |H| table +
ADR), the XFAIL dispositions, and the owner's ear afterwards.

## Manual test steps

- [ ] Owner: GAIN 0 transparent; breakup onset well past 5; 100 creamy/crunchy;
      GOLD-as-boost into the JCM
- [ ] Edge: Ge/Si contrast preserved as a property; NaN/reset/rates; no zipper

## Out of scope for this session

The buffer/input stage, the OUTPUT pot law, all other pedals, the Muff slice
(running in parallel — do not touch BjtStage or MuffModel).

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
