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
