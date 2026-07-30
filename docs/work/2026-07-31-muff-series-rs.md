# The Muff clip stages' series base resistors — the max-sustain blowout's root cause

**Date:** 2026-07-31 (overnight)
**Branch:** claude/muff-series-rs-6f557i (stacked on claude/twin-reverb-6f557i)
**Roadmap item:** audit finding 16 (bass half, ADR 009's deferred slice) + the owner's
"if I max it like Billy Corgan it's so distorted I can basically hear nothing" — the
2026-07-30 research verdict: same defect

## Goal

Max sustain is a huge, ARTICULATE wall (the fundamental survives), the bass comes most of the
way back, and the sustain knob keeps the §43 player feel on the corrected circuit.

## Approach

The minimal defensible slice from the research: `BjtStage::Config` gains **Rs** (series base
resistor; the solver's residual/Jacobian/DC-solve/companion carry it with **Rs = 0 reducing
EXACTLY to the stock solver — verified bit-identical by render hash on every existing user**),
and the Muff's two clip stages get the schematic **10 k**. Rbg is plumbed but unused: adding
it without DC-blocking the diode feedback branch parks the stage deeper in the knee
(measured); the DC-blocked branch needs a 4th Newton node — ADR 009's named follow-up, which
also owns the bass residual.

**§43 taper re-derived on the corrected circuit** (the clean window widened ~10×): −84 left
the knob bottom nearly silent-clean, −54 collapses the level authority again; **−70** lands
the same player bars (0.14 → −25.9 dBFS / 10.7 %).

## Measured (220 Hz, 48 kHz, TONE 0.5 / VOLUME 0.6)

| knob (0.1 V) | before (§43 circuit) | after (Rs + −70 floor) |
|---|---|---|
| 0.00 | −41.5 dBFS / 1.9 % | −38.1 / 2.6 % |
| 0.14 | −26.0 / 14.2 % | −25.9 / 10.7 % |
| 0.60 (default) | −6.1 / 38.6 % | −4.6 / 35.9 % |
| **1.00 (the Corgan test)** | **−4.8 / 150.5 %** | **−4.6 / 39.8 %** |

At 110 Hz the research measured the old circuit at 1177 % THD with the fundamental partially
cancelled; the Rs stage pins its output ~0.65 V at any drive (the feedback diodes finally form
their divider). Bass: low E −41 → **−14.2 dB** re 1 kHz (XFAIL stays, re-owned to the 4-node
follow-up with the numbers updated). Slam ledger: cap-exhaustion 6 → 5 combos (stays, note
updated). Hum/DC/aliasing/wall/solver-cost/tube-solver/nan-guard suites all green; A2/A3
reference rows re-baselined.

## Steps

- [x] Port the research prototype's Rs into `BjtStage` (bit-identity at Rs = 0 verified)
- [x] Q2/Q3 Rs = 10 k with the measured rationale; §43 floor −84 → −70 re-derived
- [x] Muff suite + tube-solver + nan-guard green; XFAIL decls honestly updated (neither XPASSed)
- [x] Golden report: `muff_twin` **−3.06 dB RMS / 3.76 dB @ 4032 Hz** — bless HELD
- [x] WASM rebuild, docs §49, CLAUDE.md
- [ ] Owner bless + merge (morning)

## Manual test steps

- [ ] Max sustain, Corgan-style: a huge wall you can HEAR THE NOTE through; chords readable
- [ ] Sustain 10–15: tame fuzz, cleans up with the guitar volume (the §43 feel, kept)
- [ ] Low riffs: clearly more bass than before (not yet full — the −14 dB residual is owned)
- [ ] RAT/SD-1/TS/GOLD: bit-identical (Rs = 0 — verified by hash, but ears confirm)

## Out of scope

The DC-blocked diode branch + Rbg (4th Newton node — ADR 009's follow-up, owns the bass
residual and the remaining hot-input max-sustain THD); kClipDriveMax's ×6-vs-passive question.

## Status

- [x] Complete (pending the owner's golden bless)
