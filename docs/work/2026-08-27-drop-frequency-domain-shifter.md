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

## Status

- [ ] In progress
