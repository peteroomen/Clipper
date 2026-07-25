# Muff: the missing output DC blocker and the missing series base resistors

**Date:** 2026-07-25
**Branch:** fix/muff-dc-and-bass
**Roadmap item:** 2026-07-24 audit finding 16 (`docs/audits/2026-07-24-project-audit.md`) — docs §37, ADR 009

## Goal

The Muff stops sitting on up to **+0.47 V of DC** (28 % of peak) and stops being 41 dB down
at the guitar's low E, by adding the two components the real pedal has and the model does
not: the **0.1 µF output coupling cap** into the 100 k VOLUME pot, and a **series base
resistor** on every `BjtStage` coupling network.

## Approach

This is a **deliberate TONE change** — the largest in the audit — and it is argued as one.

**Defect 1 — no high-pass anywhere.** `MuffModel::processChunk` ends `w[i] = x * outGain;`
and downsamples. `BjtStage::processSample` returns `Vc - vcQ_`, which removes the
*quiescent* DC only; the dynamic DC a common-emitter stage develops when driven into
asymmetric cutoff/saturation rides straight through, four stages deep. Every sibling
carries `dcBlockHz = 12.0` because (`SdModel.cpp`) "the asymmetric clip produces DC".

*Fix:* a one-pole DC blocker after Q4 and **before** `downsample`, at the oversampled rate,
with its corner derived from the real components — `C = 0.1 µF` into the `100 k` VOLUME pot
→ **15.92 Hz**. It sits before the VOLUME multiply because in the real pedal the cap
precedes the pot. Recursive state gets `flushDenormal` and is cleared in `reset()`.

**Defect 2 — no series base resistor.** `BjtStage` drives `Cin = 100 nF` from an *ideal*
voltage source directly onto the base node, so the coupling corner is set by the base-node
shunt impedance alone (`rπ ∥ Rf,Miller ∥ diode conductance`) — back-solved at ≈250 Hz per
stage, ×4 in cascade. The real pedal's coupling caps all see a series resistor.

*Fix:* add `Rb` to `BjtStage::Config` and fold it into the Thévenin the header already
describes. Series `Rb` + the `Cin` backward-Euler companion collapse to one Norton branch
into the base node:

```
gIn = gCin / (1 + Rb·gCin)          # == gCin when Rb == 0, so Rb = 0 is bit-identical
i_base = gIn·(vin − vCin − Vb)
vCin += gIn·(vin − vCin − Vb) / gCin
```

Per-stage values (documented canon, ram's-head family): **Q1 100 k** (the input series
resistor `R1`), **Q2 100 k** (driven from the 100 kA SUSTAIN pot's track), **Q3 10 k**,
**Q4 10 k**.

**Expected consequence, and the honest part:** a series `Rb` into a low-impedance base is
also a *divider*, so the cascade loses gain. The documented level canon (§24: default
SUSTAIN 0.6 / VOLUME 0.6 peaks ~1.2 V; the wall lives at SUSTAIN ≥ 0.6 and collapses at 0)
is a calibration, and it has to be re-established against the corrected topology. Any
change to `kClipDriveMax` / `kOutputTrim` is reported as part of the re-voicing, not
smuggled in — and the pot's authentic full-range audio taper is not touched.

## Steps

- [ ] Reproduce the audit's two tables exactly (scratch probe) so before/after is comparable
- [ ] `BjtStage::Config::Rb` + the Thévenin fold-in (`gIn_`), Jacobian entry, companion update
- [ ] Assert `Rb = 0` is bit-identical to the pre-change stage (regression safety for future reuse)
- [ ] `MuffModel`: per-stage `Rb`, the output DC blocker, `reset()` coverage
- [ ] Re-establish the §24 level canon; report every constant moved
- [ ] Delete the `finding16-muff-no-output-dc-blocker` XFAIL, assert DC-on-signal for real
- [ ] Re-baseline `testHumRejection` against the corrected bass response and say what it now tests
- [ ] Correct the "~−4.5 dB/stage at 60 Hz" canon in `test_muff_model.cpp` and in docs §24
- [ ] Add a real low-E response assertion (the property the audit says is missing)
- [ ] Check the control-rate-sampling XFAIL (worst case is the Muff) — report either way
- [ ] `clipper-bench` muff row before/after; `--golden-report` deltas; docs §37 + ADR 009

## How this will be measured

1. **DC on signal**, 48 kHz, 220 Hz @ 0.3 V, TONE 0.5, VOLUME 0.6, settled tail, at
   SUSTAIN 0.3 / 0.6 / 1.0 — `|mean| / peak` must drop from 20.2 / 9.1 / 28.1 % to under 1 %
   (`clipper::test::kDcFractionBar`).
2. **Small-signal response** at SUSTAIN 0.15, TONE 0.5, 0.002 V input (the level that
   reproduces the audit's table), 30 → 4000 Hz. Low E (82.4 Hz) must come to within ~6 dB
   of 1 kHz, from −41 dB.
3. **`clipper-bench`** muff row, × realtime, before → after.
4. **`clipper_player_expectations_tests --golden-report`** per-golden, per-band dB deltas.
   `muff_twin.wav` WILL move. It is **not** re-blessed in this slice.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — everything green except the golden test
- [ ] `./build/clipper_muff_tests` — no `[XPASS]`, no XFAIL ledger entry for finding 16
- [ ] `clipper-render --gen pluck:110:2.0 … --pedal muff` and confirm the low end arrives
- [ ] Edge case: `Rb = 0` still bit-identical (the Fuzz Face / RG100 reuse path)
- [ ] Edge case: ±10 V slam at 44.1 / 48 / 96 k still finite and bounded; silence → silence

## Out of scope for this session

- **The 2.9 dB mid-scoop** (audit: a real Big Muff is 8–12 dB deep). An in-phase weighted
  sum of a first-order LP and a first-order HP *cannot* notch deeper; that is a topology
  change to `MuffToneStack` and its own slice.
- Re-blessing `muff_twin.wav` — a deliberate human act, gated by a terminal confirmation.
- Rebuilding `web/public/generated/*` (no emsdk here; the staleness gate will flag it).
- The control-rate parameter-sampling defect itself.
- The other three pedals' silent-input-only DC checks (already corrected on `main` by §29).

---

## What actually happened

(filled in below)

## Measured results

(filled in below)

## Files created / modified

(filled in below)

## Deferred to next session

(filled in below)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
