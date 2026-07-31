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
