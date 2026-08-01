# SD-1 / TS tone network — the documented approximation replaced by the netlist

**Date:** 2026-08-01
**Branch:** claude/sd-ts-tone-network-6f557i
**Roadmap item:** post-v1.1 field-report round, last open item — "SD-1/TS tone-network rolloff"
(CLAUDE.md's own round ordering: *"then SD-1/TS tone-network rolloff and the GOLD gain-gang
law"*; the GOLD half shipped as §50). Owner has A/B'd the model against a **real SD-1** they
own; the reported gap is the **upper-mid body** — how much midrange the pedal pushes and where.
**Docs section: §65. ADR (if needed): 026.**

## Goal

Measure where the model's upper-mid energy actually sits, establish what the real circuits do
from a real netlist, and — if the measurement supports it — replace §11.6/§21's explicitly
documented tone approximation (a first-order ±12 dB treble tilt about a 1 kHz pivot,
transparent at noon) with the two pedals' own tone networks.

## Approach

**This is a DELIBERATE TONE CHANGE, not a fidelity-neutral refit**, and it will be judged by:

1. an **upper-mid metric** measured on both pedals before → after (peak frequency of the
   small-signal response and the level at 3 kHz relative to that peak);
2. the rendered response vs an **independent published transfer function** for the TS tone
   stage (Yeh & Abel, DAFx-07, as implemented in `JamesStubbsEng/TS-808-Ultra`) — worst error
   in dB across the band × knob positions, the §54/§56 acceptance shape;
3. the **five-row golden table** from `--golden-report` (report only — **NOTHING is blessed**;
   the owner blesses on the printed table).

**Nothing will be re-gained to compensate for a level change** — §36 / ADR 008's precedent.

**Sharing constraint.** `SdModel` and `TsModel` are one `OverdriveEngine` + two
`OverdriveConfig`s (§21). The tone networks are the *same topology with different component
values*, so the network belongs in the ENGINE and its component values belong in the CONFIG.
Both pedals are measured and reported.

## Steps

- [ ] Measure the model BEFORE anything changes: small-signal magnitude through the whole
      pedal at TONE 0 / 0.5 / 1, both pedals, and locate the upper-mid energy.
- [ ] Establish the real circuits: parse the LiveSPICE `.schx` netlists, cross-check every
      value against a second and third source; record what could NOT be sourced.
- [ ] Attribute the gap: tone network vs clipping stage vs level staging — separately.
- [ ] Derive the tone network's exact `H(s)` from the netlist; validate against Yeh's
      independent formula for the TS values.
- [ ] Implement it in `OverdriveEngine` with the component values in `OverdriveConfig`.
- [ ] New perturbation-proven bars in `test_sd_model.cpp` / `test_ts_model.cpp`.
- [ ] Re-baseline the M11 player-expectations reference rows for both pedals.
- [ ] `--golden-report`, full table, NOTHING blessed.
- [ ] Rebuild the WASM artifact; run every suite; §65 + CLAUDE.md Current State.

## How this will be measured

- `clipper_sd_tests` / `clipper_ts_tests` — new `testToneNetwork`: rendered response vs the
  analytic network, worst |Δ| in dB across 20 Hz–20 kHz × TONE 0/0.5/1, and the TS's rendered
  response vs **Yeh's** independent `H(s)`.
- The upper-mid metric (peak frequency, 3 kHz-re-peak) before → after, both pedals.
- `./build/clipper_player_expectations_tests --golden-report` — the five-row dB table.
- Perturbation: patch each component value the new bars name → RED; restore → GREEN;
  `touch` after BOTH.

## Manual test steps

- [ ] `clipper-render --gen sweep:20:20000:4 … --pedal sd1 --filter 0/0.5/1` — the tone knob
      must sweep dark→bright and the mid hump must be a HUMP, not a shelf.
- [ ] Web: an SD-1 in the chain, TONE knob audibly changes; the worklet spec still passes.
- [ ] Edge case: TONE at both extremes must stay stable and finite; `reset()` must re-park;
      a NaN must not latch.
- [ ] Edge case: block-size invariance and rate independence must not move.

## Out of scope for this session

- The DRIVE plateau range (`driveMinDb`/`driveMaxDb`) — the netlist says the real feedback leg
  is `Drive pot + a series resistor` (SD-1 33 kΩ, TS 51 kΩ), so the real minimum plateau is
  NOT the model's +12 dB. That is a **gain-staging** item, separable, and it is reported here
  rather than changed (one slice = one concern).
- The LEVEL pot's real divider (SD-1 4.7 k + 10 k pot; TS 100 k pot).
- The input coupling network and the clipper's `Cc` feedback cap.
- Anything in the parallel compressor slice's scope (`CompressorEngine` / `CompModel` /
  pedal registries). No registry change is needed by this slice.

---

<!-- Filled in during/after the session -->

## What actually happened

Full record: **docs §65**, **ADR 026**.

The item's own title was **CONFIRMED by measurement before any change**: the gap is
the tone network. The mechanism is worse than "rolloff" — the model had **no
low-pass in the tone stage at all**, so the family's mid HUMP measured as a high
SHELF (both pedals peaked at 6467 Hz with 3 k / 6 k / 12 k sitting at
−0.15 / −0.00 / −0.11 dB re that peak, i.e. flat across the top three octaves,
and both pedals measured IDENTICAL because §11.6's approximation was shared
verbatim by §21).

The research channel was better than expected: **LiveSPICE ships BOTH pedals as
example schematics** (`Boss Super Overdrive SD-1.schx`, whose own label names
gmarts.org as its source, and `Ibanez Tube Screamer TS-9.schx`), parsed
node-by-node the way §63 parsed the Rockerverb; cross-checked against
`Cushychicken/ltspice-guitar-pedals`' TS808 LTspice netlist and against **Yeh &
Abel's DAFx-07 transfer function** via `JamesStubbsEng/TS-808-Ultra`. The
netlist-derived H(s) reproduces Yeh's published form TERM FOR TERM and agrees
numerically to **0.000000 dB** — after correcting one dimensional error in the
reference's transcription of the paper (`Y` substituted where `Cs` belongs).

Scope held: the tone network only. The DRIVE plateau range is ALSO wrong by the
same netlists (real minima +18.1 / +21.5 dB against the model's +12.0 for both)
and is **reported, not changed** — §65.8 and ADR 026 item 3.

## Measured results

Upper-mid metric, whole pedal, DRIVE 0.5 / TONE noon, 1 mV, 48 kHz:

| | SD-1 before → after | TS before → after |
| --- | --- | --- |
| peak frequency | 6467 → **1106 Hz** | 6467 → **777 Hz** |
| 3 kHz re peak | −0.15 → **−3.63 dB** | −0.15 → **−5.93 dB** |
| 6 kHz re peak | −0.00 → **−9.34 dB** | −0.00 → **−11.75 dB** |
| 12 kHz re peak | −0.11 → **−16.05 dB** | −0.11 → **−18.38 dB** |

Tone-stage DC insertion loss (the `Rin`/`Rbias` divider, rendered): SD-1
**−6.01 dB**, TS **−0.84 dB** — 5.18 dB apart, and **not compensated anywhere**
(§36 / ADR 008 precedent). Field anchor (0.15 V, 220 Hz): SD-1 −5.74 dB and TS
−1.14 dB at every DRIVE position.

TONE authority: 3 kHz span **17.33 (SD-1) / 16.05 dB (TS)** against 100 Hz spans
of **0.30 / 0.35 dB** — still a treble control that leaves the bass alone.

Discretization vs the analytic H(s): worst **1.16 / 0.96 dB** at 16 kHz /
44.1 kHz, ≤ 0.53 dB below 10 kHz (a plain bilinear of the same network measures
19.4 / 13.6 dB). All poles and zeros REAL across the whole knob (worst normalized
discriminant +0.0339). CPU: no measurable change (interleaved same-machine A/B,
3 pairs; both pedals ~1.0 % of one 48 kHz stream).

**Goldens — `--golden-report`, NOTHING blessed, nothing written:**

| golden | RMS Δ | worst band Δ | at |
| --- | --- | --- | --- |
| `rat_jcm800` | +0.00 dB | 0.00 dB | 252 Hz | UNCHANGED |
| `sd1_twin_reverb` | **−5.99 dB** | **11.34 dB** | 3200 Hz | CHANGED |
| `muff_twin` | +0.00 dB | 0.00 dB | 5080 Hz | UNCHANGED |
| `ts_ac30` | **−1.83 dB** | **8.90 dB** | 2016 Hz | CHANGED |
| `clean120_chorus` | −0.00 dB | 0.11 dB | 252 Hz | UNCHANGED |

Core suite is RED at **exactly one assertion** — `compareGolden`'s
`fabs(rmsDeltaDb) < 1.0` on `sd1_twin_reverb` — and green everywhere else
(33 of 34 ctest entries pass; six xfail ledgers Skipped as normal).

Perturbations: 11 patched, **10 RED on a measured bar**, restore GREEN in every
case; P10 (SD-1 tone pot 22 k → 20 k) is red only on the transcription guard and
is reported as the honest miss — 0.19 dB worst, invisible to any player-observable
bar. P11 also showed the shared `clipper_denormal_tests` cannot see a `double`
subnormal in this cascade, which is why `maxAbsRestingState()` exists.

Everything else green: Playwright **82/82**, node 15/10/12, electron 20, native
3/3 (`identical_core` / `chain_edit` / `cab_state`), full `Clipper` target builds,
WASM artifact rebuilt (**92** hashed inputs) and `check-artifact.mjs` passes.

## Files created / modified

- **new** `core/include/clipper/dsp/OverdriveToneStack.h` — the network, its exact
  H(s), and the matched-Z + Nyquist-matched-zero discretization
- `core/include/clipper/dsp/OverdriveEngine.h`, `core/src/dsp/OverdriveEngine.cpp`
- `core/include/clipper/dsp/{SdModel,TsModel}.h`, `core/src/dsp/{SdModel,TsModel}.cpp`
- `core/tests/test_sd_model.cpp`, `core/tests/test_ts_model.cpp` — a new
  `testToneNetwork` in each; the mid-hump and max-plateau probes now divide the
  tone network out (it is no longer transparent at noon)
- `core/tests/test_player_expectations.cpp` — M11 reference rows re-baselined
- `docs/DEVELOPMENT.md` §65, `docs/decisions/026-overdrive-tone-network-is-the-netlist.md`
- `web/public/generated/clipper.js` + `.build-stamp.json`

## Deferred to next session

- **The DRIVE plateau range** (§65.8) — the real feedback leg is the pot IN SERIES
  with 33 kΩ (SD-1) / 51 kΩ (TS), so the minima are 6.1 / 9.5 dB low. The numbers
  are already measured; the pot TAPER between the endpoints is unsourced.
- The LEVEL divider and the output coupling cap (SD-1 `R10` 4.7 k + a 10 k pot;
  TS a bare 100 k pot) against one identity map and one 12 Hz `dcBlockHz` for both.
- The top-octave discretization residual (1.16 dB at 16 kHz / 44.1 kHz) — the cure
  is to run the tone network inside the oversampled domain.
- The SD-1's tone pot value (22 kΩ transcribed vs 20 kΩ claimed by community
  sources) and both pots' taper letters — needs a factory sheet.
- The clipper's `Cc` feedback cap (SD-1 47 pF, TS 51 pF), not modelled.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
