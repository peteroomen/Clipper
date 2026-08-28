# The Cellar's frequency-domain shifter

**Date:** 2026-08-27
**Branch:** feat/drop-frequency-domain
**Roadmap item:** §70's named fix for `drop-triad-spread-at-minus-2`, the drop pedal's
one XFAIL

## Goal

Close `drop-triad-spread-at-minus-2` — a 4:5:6 major triad's three partials should
all shift by the same ratio to within 2 cents of spread — without regressing
anything the shipped SOLA shifter currently gets right.

## Why a frequency-domain shifter, and what the defect actually is

The shipped `PitchShifter` is time-domain: an interpolating delay read at a rate `r`,
re-seated once per grain by a SOLA correlation search. That is polyphonic BY
CONSTRUCTION — resampling scales every frequency present, so it never forms an
opinion about pitch — and it measures **0.00 cents** on single notes and power
chords at all nine detents.

It fails on ONE cell: an E major triad at −2 semitones, spread **2.0000 cents**
against this slice's own 2-cent target. §70 diagnosed the cause and it is
structural: **SOLA re-seats the read tap at ONE lag, and one lag cannot align a
4:5:6 triad's three partials simultaneously.** Two candidate fixes were built and
refuted there — a wider search span (55/58 ms) and sub-sample lag refinement — each
moving it by under 0.002 cents.

A phase vocoder does not have that constraint: it advances **each bin's phase
independently** from that bin's own measured instantaneous frequency, so the three
partials never have to share an alignment. That is precisely why §70 named it.

**The perceptual bar is already met and stays met.** The 5-cent absolute bar (under
the ~6-cent JND for a sustained tone) passes everywhere today with 3.75 cents to
spare. So this slice is chasing the project's own tighter target, not a defect a
player can hear — which is exactly why it must not be allowed to cost anything
audible. See the honesty gate below.

## Approach

`core/include/clipper/dsp/PhaseVocoderShifter.h` — a NEW primitive alongside
`PitchShifter`, not a rewrite of it. Both are consumers of the existing allocation-
free `FFT` (already used by `CabConvolver`); neither knows anything about semitone
selectors or footswitches, which is the separation §70 established and that makes a
future harmoniser cheap.

Pitch shift = **time-stretch by `1/r` in the STFT domain, then resample by `r`**.
The resampler is the existing `DelayLine` read the shipped shifter already uses, so
the only new machinery is the analysis/synthesis loop.

Which one SHIPS is a decision this slice will make on measurement, not up front.
Three outcomes are possible and all three are acceptable write-ups:
* the phase vocoder wins on every axis and replaces SOLA;
* it fixes the triad and loses something else, and is REFUSED (§66's precedent —
  a slice whose honest answer is "no");
* it wins on some positions and not others, in which case shipping both behind a
  selector needs an ADR arguing the departure, not a quiet `if`.

## How this will be measured

The validated estimator from the latency slice (`tools/measure/PartialFreqEstimator.h`
— complex demodulation plus a phase-slope fit) does the pitch measurement. It reads
**−0.0002 cents** on a pure sine, **0.0035** on a triad at the octave, and a
deliberate **+5.000-cent detune as +4.9919**, which is what proves it can see an
error rather than echo the target back. It reproduces §70's own XFAIL figure to
**1.90 vs 2.0000**.

A full BASELINE of the shipped shifter is measured FIRST, so every comparison is
against numbers taken on this machine with this harness rather than against §70's
prose.

**The bar this slice exists to move**
1. E major triad at −2: partial spread **< 2.0 cents** (today 2.0000).

**Bars that must not regress — these are what make the slice honest**
2. Absolute accuracy < **5 cents** on every shape at every detent (today: met with
   3.75 to spare).
3. Single notes and power chords stay at **0.00 cents** at all nine detents.
4. Non-harmonic artifact floor no worse than §70's measured **−18.94 / −18.35 /
   −17.42 dB** at −1 / −5 / −12.
5. **TRANSIENT RESPONSE — a NEW bar, and the one most likely to catch a phase
   vocoder out.** §70 never measured it because SOLA preserves attacks naturally;
   smearing them is the classic PV failure and it is exactly what a player would
   notice on a picked note. Measured as attack-envelope rise time and peak
   preservation on a plucked low E, against the shipped shifter.
6. Latency, measured as the TRUE mean read delay (§70's analytic figure understates
   the shipped one by 17 ms). **Reported, not promised** — a PV's latency is its
   analysis window, and resolving 82 Hz wants a long one.
7. CPU, as % of one 48 kHz stream, against the shipped 4.5 %.

## Manual test steps

- [ ] Play a low E power chord at DROP 2 — pitch is right and the attack still snaps.
- [ ] Play a full E major chord at DROP 2 — the chord stays in tune with itself.
- [ ] Sweep the selector through all nine detents on a held chord — no glitching.
- [ ] OCTAVE and OCT+DRY still behave (OCT+DRY sums the dry, so the original pitch
      is audible beside it — that is the reference's design, docs §70.2).
- [ ] Edge: fast repeated picking — the case a phase vocoder smears if it is going to.
- [ ] Edge: silence in, silence out; and a NaN in recovers via `reset()`.

## Out of scope

- Changing the pedal's controls, detents or the OCT+DRY position.
- Latency as a TARGET. It is measured and reported; §70's own note stands that a
  PV's latency is its window and may not beat the shipped 52.8 ms mean.
- Any other pedal or amp.

## What actually happened

**The candidate was built, measured and REFUSED** (docs §74, ADR 027), and the
slice's real value is a defect it found on the way that §70 could not have seen.

The plan's ship criterion was met exactly as written — *"if the PV closes the
triad XFAIL but measurably degrades transients or the artifact floor, refuse it
and keep SOLA"* — except that the degradation is far larger than "measurably":
171.6 ms of latency and 112.5 ms of transient rise at the octave.

Order of work, and each step changed the next:

1. **The battery came first.** `tools/measure/bench_shifter.h` measures both
   implementations identically, so nothing is compared against §70's prose.
2. **The estimator was validated before being trusted** (+5.000-cent detune reads
   +4.9919) and then moved to `core/tests/support/PartialFreq.h`, because a bar
   set by an unvalidated measurement is worse than no bar — the test file's own
   Hann-peak helper reads the E major triad at −5 as 5.25 cents where the
   validated estimator reads 4.53, and a 5-cent bar on the first number would have
   failed correct code.
3. **The SOLA baseline exposed a defect nobody was looking for.** §70's polyphony
   test drives DROP 1 and DROP 2 only; sweeping all nine detents shows the error
   growing with depth to **12.47 cents on an ordinary E major triad at OCT**,
   2.5× the perceptual bar §70's own XFAIL text reports as met everywhere.
4. **The PV vindicated §70's diagnosis and failed on cost.** Exact at N = 8192
   (0.000 cents); worse than SOLA at the largest affordable N = 4096.
5. **This slice's own hypothesis was refuted by its own measurement.** A longer
   SOLA window should reduce a splice-rate error; swept 2.8× it moves the octave
   triad from 12.516 to 11.914 cents. The error is per-splice misalignment, not
   splice frequency.

Two implementation findings inside the candidate, both from measuring: the
overlap-add group delay is the **whole window**, not `n − hop` (the obvious
reading, and the accessor was corrected against the measurement); and the octave
broke completely (−126.7 cents on the lowest partial) until the peak criterion
went from ±2 bins to ±1, because a downward shift **halves the bin spacing between
partials** on relocation.

## Measured results

Through `DropModel`, validated estimator, worst absolute partial error in cents:

| detent | E5 power chord | E major triad |
| --- | --- | --- |
| DROP 1 | 0.10 | 0.63 |
| DROP 4 | 0.42 | 3.87 |
| DROP 6 | 0.68 | 4.62 |
| DROP 7 | 0.82 | 3.99 |
| **OCT** | 1.58 | **12.47** |

Head to head (identical stimuli, one machine):

| | SOLA (W 65 ms) | PV N=4096 | PV N=8192 |
| --- | --- | --- | --- |
| E major triad −2 | 1.163 | 11.997 | **0.000** |
| rich triad −2 | **1.497** | 14.625 | 1.151 |
| rich triad −12 | 10.069 | 9.939 | **2.518** |
| transient rise −1 / −12 | **10.33 / 27.88 ms** | 35.81 / 39.96 | 25.65 / 112.48 |
| true latency (envelope) | **8.83 / 19.08 ms** | 83.73 / 82.33 | 171.62 / 174.56 |
| artifact floor −1 / −5 / −12 | −38.2 / −24.8 / −36.6 dB | −39.6 / −36.1 / −36.1 | −38.9 / −36.4 / −36.5 |
| CPU (% of one 48 kHz stream) | 2.25 | **1.51** | 1.65 |

SOLA window sweep, E major triad worst absolute error (mean latency 35.8 → 99.0 ms):

| −2 | −7 | −12 |
| --- | --- | --- |
| 1.390 → 1.118 | 4.627 → 5.555 | **12.516 → 11.914** |

Perturbation: **P1** (SOLA removed, fixed splice) trips BAR 1 first, so **P1b**
re-ran it with the new bar ordered ahead — RED at DROP 2 on the power chord
(**7.88 cents** against the 5-cent bar). Restore GREEN, `PitchShifter.h`
byte-identical to its pre-perturbation state.

**No file under `core/src/` or `core/include/` changed**, so no golden can move
and no WASM rebuild was needed — the artifact gate reports 115 hashed inputs still
in step. Core ctest is unchanged at **41 entries, 41/41** (34 targets + 7 ledger
registrations); the new entry joins `clipper_drop_tests`' existing ledger, so the
count of known-bad PROPERTIES goes **8 → 9**.

## Files created / modified

- `tools/measure/PhaseVocoderShifter.h` — the candidate, kept OUTSIDE `core/` with
  the refusal in its banner
- `tools/measure/bench_shifter.h`, `shifter_baseline.cpp`, `shifter_pv.cpp`,
  `shifter_pv_probe.cpp`, `shifter_head_to_head.cpp`, `drop_rich_triad.cpp`
- `core/tests/support/PartialFreq.h` — moved from `tools/measure/`, now shared
- `core/tests/test_drop.cpp` — `testShiftDepthAccuracy()`, the new ledger entry,
  and the re-scoped `fix` on the old one
- `docs/DEVELOPMENT.md` §74 · `docs/decisions/027-…md`

## Deferred to next session

- **The octave triad defect has NO identified fix.** The only untried route the
  diagnosis points at is a **multi-lag** splice (per-partial or per-band
  alignment) — "one lag cannot align three partials" is about the count of lags,
  not the domain.
- `findSplice` runs **per sample** when `span ≥ (1−x)·W` (~600 000× slowdown) and
  wants a guard. Unchanged from §70.
- An UPWARD shift does not suffer the bin-spacing halving, so this refusal does
  not transfer to a harmoniser or detune built on the same header.

## Status

- [x] Complete
