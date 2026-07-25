# Muff: the missing output coupling cap (audit finding 16, DC half)

**Date:** 2026-07-25
**Branch:** fix/muff-output-dc-blocker
**Roadmap item:** 2026-07-24 audit finding 16 — docs §37, ADR 009

## Goal

The Muff stops sitting on up to **+0.47 V of DC (28 % of peak)** by adding the one component
the real pedal has and the model did not: the **0.1 µF output coupling cap** into the 100 k
VOLUME pot. The finding's *bass* half is deliberately NOT in this slice.

## Approach

A deliberate tone change, but a small one — argued as one.

Finding 16 names two defects with very different evidentiary standing:

* The **cap** is unarguable. The pedal has it; every sibling already carries
  `dcBlockHz = 12.0` for the same stated reason; the golden barely moves.
* The **series base resistor** that fixes the bass is a judgement call whose value departs
  from the schematic to compensate for a clip-stage base-node impedance this model measures
  at ~1.8 kΩ against the real stage's ~4 kΩ (ADR 009). It also accounts for essentially the
  whole tone change.

Splitting lets the uncontroversial fix land now without buying the contested argument. The
bass defect stays **measured, printed and named** as XFAIL `finding16-muff-almost-no-bass`,
so nothing is quietly half-fixed.

Cap placement: after Q4, **before** the VOLUME multiply (in the pedal the cap precedes the
pot) and **inside** the oversampled domain, so it blocks the rectified DC before the
decimator. `f = 1/(2π·100k·0.1µ) = 15.92 Hz`, derived from the Muff's own components rather
than copied from the siblings' 12 Hz. `kOutputTrim` is NOT touched.

## Steps

- [x] `kOutCouplingF` / `kVolumePotOhms` / `kOutCouplingHz` + the one-pole in `processChunk`
- [x] DC-on-signal asserted for real; XFAIL `kXfMuffDc` deleted (ratchet: a fixed defect's
      XFAIL must go in the same slice, or it XPASSes)
- [x] Bass half re-declared as XFAIL `kXfMuffBass` with its real measured numbers
- [x] `kXfSlamIterCap` restored — fixed by the base resistors, so still broken here
- [x] `ledgerMain` restored in `main()` (see the trap below)
- [x] Golden re-blessed; artifact rebuilt; core suite green

## How this will be measured

`clipper_muff_tests` DC-on-signal block (`support/DcOffset.h`, 1 % bar), across three SUSTAIN
settings **and a +0.1 V input-offset case** — the offset case matters because deleting a
coupling cap changes nothing on a clean input (§29). Plus the golden dB table, and the 30 Hz
rejection assert as the opposite-direction guard.

## Manual test steps

- [x] `./build/clipper_muff_tests` — DC under 1 % of peak at every SUSTAIN; ledger prints the
      two open XFAILs with real numbers
- [x] `./build/clipper_muff_tests --xfail-ledger` exits **77** (so ctest says `***Skipped`)
- [x] `--golden-report` shows only `muff_twin` moved, and by how much
- [x] Edge case: 30 Hz must STILL be rejected (> 12 dB down) — the guard against "fix" by
      deleting coupling caps

## Out of scope for this session

* The bass half of finding 16 (series base resistors) — its own slice, ADR 009 names the
  order of work: settle the ~1.8 kΩ vs ~4 kΩ base-node question FIRST, then choose Rb.
* Re-deriving `kOutputTrim` — must be done against the *combination*, not either half.
* Findings 4, 5, 7, 8, 9, 10 (the valve-amp physics cluster).

---

## What actually happened

Split out of a larger draft (`fix/muff-dc-and-bass`) on the owner's instruction after the
combined slice measured a **26.04 dB** worst-band golden move. Splitting turned out to be
justified by measurement rather than caution: the cap alone moves the golden **0.80 dB**, so
the two halves were doing very different amounts of work and deserved separate arguments.

Two traps surfaced, both recorded in §37:

1. **The removed `ledgerMain` call.** The bundled slice deleted it expecting no XFAILs to
   remain, while CMake still registered the ledger test. That combination makes
   `--xfail-ledger` run the whole suite and exit 0, so ctest reports **Passed** instead of
   `***Skipped` — the line that advertises open defects becomes a silent duplicate run.
   Third instance of the same shape after the advisory native job and the existence-only
   artifact check: **a guard that is present but inert is worse than an absent one.**
2. **`kOutputTrim` as a stacking hazard** — 2.0096 V at SUSTAIN 1.0 from the cap alone,
   already over the 2.0 V ceiling its own comment cites.

## Measured results

| property | before | after |
|---|---|---|
| DC on signal (worst of 6 cases) | **+0.47 V, 28 % of peak** | **0.00003 % of peak** (bar 1 %) |
| `muff_twin` golden | — | **−0.03 dB broadband, 0.80 dB @ 5080 Hz** |
| low E re 1 kHz *(XFAIL, unfixed)* | −41 dB | **−41.14 dB** (reproduces the audit) |
| ±20 V slam at the Newton cap *(XFAIL, unfixed)* | 6 of 16 | 6 of 16 |
| 30 Hz rejection *(hard assert)* | — | **−72.01 dB** |

Artifact: sourceHash 5a5754e2bc3a…, 65 inputs, 177176 bytes.

## Files created / modified

`core/src/dsp/MuffModel.cpp`, `core/tests/test_muff_model.cpp`,
`core/tests/goldens/muff_twin.wav` + `GOLDENS.md`, `docs/DEVELOPMENT.md` (§37),
`docs/decisions/009-muff-output-coupling-cap.md`, `web/public/generated/*`.

## Deferred to next session

* **The bass half** — `fix/muff-dc-and-bass` holds a working implementation (47 kΩ on Q2/Q3,
  `kOutputTrim` 0.45, the low-end assertions, and the slam-convergence win). Do not merge it
  as-is: settle the base-node impedance first per ADR 009, then re-derive the trim.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
