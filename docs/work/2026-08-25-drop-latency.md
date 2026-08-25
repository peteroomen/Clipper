# The Cellar's latency — research phase

**Date:** 2026-08-25
**Branch:** feat/drop-latency
**Roadmap item:** owner field report 2026-08-25 (*"it's too laggy too"*); §70's named follow-up

## Goal

Establish what the drop-tune's latency actually IS, what it is made of, and what
can be bought back and at what price — before changing a constant.

## What was measured, and the headline is a correction

**§70 documents "~36 ms" and that figure is ANALYTIC, not measured.**
`PitchShifter::meanDelaySamples()` returns `dMin + 0.5*(1+x)*W` = 35.79 ms, which
assumes the read tap sweeps `[dMin + xW, dMin + W]` UNIFORMLY. It does not. SOLA
re-seats the tap at the best correlation lag, and for a periodic signal that lag is
a whole PERIOD back rather than near `dMin` — so the tap never visits the bottom of
its range on ordinary material.

Instrumenting the live tap and sampling its delay per output sample, on the shipped
65 ms window:

| stimulus | semis | TRUE mean | p90 | max |
| --- | --- | --- | --- | --- |
| single | −1 | **52.82 ms** | 62.63 | 65.04 |
| single | −12 | **58.99 ms** | 63.83 | 65.04 |
| power5 | −1 | **52.81 ms** | 62.63 | 65.04 |
| power5 | −12 | **52.91 ms** | 62.59 | 65.04 |
| triad | −1 | 39.46 ms | 60.22 | 65.04 |
| triad | −12 | 41.12 ms | 60.21 | 65.04 |

So on the material the pedal is actually played with — single notes and power
chords — the real mean delay is **52.8 ms, not 36**, and the worst case is 65 ms.
That is a documentation error in §70 and it should be corrected whether or not any
latency work ships. It also explains why the lag is more noticeable than the docs
imply.

## The measurement had to be built first, and two estimators were refuted

This is §70's own warning ("two test bugs and one ledger gap, all in the
MEASUREMENT") arriving again. Three estimators were tried:

1. **Zero-crossing f0** — valid for a single sine only. On a power chord it locks
   onto the FIFTH and reports **+714 cents** of "error" that is entirely the
   measurement. Discarded.
2. **Short-window Goertzel peak search** — cannot resolve the octave-down case: at
   −12 a triad's partials sit at 41.2 / 51.9 / 61.7 Hz and a 0.4 s analysis span
   ran to its search boundary, printing a constant −55.55 cents. Discarded.
3. **Complex demodulation + phase-slope fit** (shipped in the harness). Shifts the
   partial of interest to DC, rejects its neighbours with six cascaded one-poles at
   2 Hz (>80 dB at 10 Hz separation), and reads the residual frequency off a
   least-squares fit to the unwrapped phase. O(N) per partial.

**It was validated against signals whose answer is known by construction before
being trusted:** a single 41.203 Hz sine reads **−0.0002 cents**; the three
partials of a triad at the octave read **−0.0035 / −0.0006 / +0.0016 cents**; and a
deliberately +5.000-cent-detuned tone reads **+4.9919 cents** — which is the case
that proves it can SEE an error rather than echoing the target back.

Cross-check against the existing record: it measures the triad spread at −2 on the
shipped configuration as **1.90 cents**, against §70's XFAIL figure of 2.0000. The
two agree, which is what licenses the rest of the table.

## The frontier

The binding constraint is the one `PitchShifter.h` already documents:
`span < (1 - x) * W`, with `span = 50 ms` set by a major triad's COMPOSITE period.
So W cannot go below ~56 ms without giving up span. TRUE (measured) delay and pitch
accuracy across the reachable configurations:

| config | true mean (single / power) | single | power5 | triad −1 spread | triad −12 spread |
| --- | --- | --- | --- | --- | --- |
| **SHIPPED** W65 span50 | **52.8 ms** | 0.01–0.15 c | 0.09–2.07 c | 0.93 c | 2.93 c |
| min-W W56 span50 | **37.7–41.6 ms** | 0.01–0.15 c | 0.09–1.57 c | 0.92 c | 13.92 c |
| W35 span30 | **22.8–29.0 ms** | 0.01–0.12 c | 0.10–1.53 c | 4.20 c | 27.80 c |
| W30 span25 | **17.9–18.0 ms** | 0.01–0.14 c | 0.10–2.47 c | 5.89 c | 14.74 c |

Read across: **single notes and power chords are essentially insensitive to the
window** all the way down to 30 ms — worst 2.47 cents against a 5-cent perceptual
bar. Everything the window buys is being spent on TRIADS, and triads are already
this pedal's one XFAIL.

## One cheap fix was built and REFUTED

The p90 column above says the tap spends most of its time near the top of the
window, so the obvious idea was to make it re-seat lower: SOLA's correlation peaks
at EVERY multiple of the period and those peaks score almost identically, so plain
`argmax` lands on whichever multiple noise favours. Taking the EARLIEST lag within
a tolerance of the best score should re-seat closer to `dMin` for free.

Built and measured. It does move the mean (52.8 → 39.3 ms at a 1 % tolerance) but
it **introduces a pitch error that scales with the shift** — single-note accuracy
goes 0.15 → **9.22 cents** at the octave, past the 5-cent bar. That is exactly
§70's "a FIXED SPLICE gives a systematic pitch error that scales with the shift"
signature: a lag within 1 % of the best correlation is not an exact period, so the
splice carries a phase discontinuity. Refuted, not tuned.

Also learned from the same run: **p90 barely moves** (62.6 → 60.2 ms). The window
top, not the splice point, is what sets perceived latency.

## A latent defect found on the way, reported

`findSplice` runs once per grain — unless `span >= (1 - x) * W`, in which case the
re-seat leaves `phase_` past 1.0, the clamp fires every sample, and **the O(span)
correlation search runs PER SAMPLE**. That is the "hang at a 20 ms window" recorded
in the 2026-08-25 control slice: not a hang, a ~600 000× slowdown. Nothing shipped
can reach it today, but a future slice shortening the window would fall straight
in. It wants a guard (clamp the span to the window, or assert in `prepare`).

## The decision this leaves

Three options, and they differ enough that the owner should pick:

- **(a) W 65 → 56.** Free-ish: **52.8 → 37.7 ms** (−15 ms, −29 %) with single
  notes, power chords and triads-at-−1 all unchanged. Only triad-at-octave
  degrades (2.93 → 13.92 cents of spread) — a case already covered by the XFAIL.
- **(b) W 65 → 30.** **52.8 → 18.0 ms** (−66 %). Single notes and power chords
  still inside the 5-cent bar. Triads leave it (5.89 c at −1, 14.74 c at −12).
- **(c) Frequency-domain shifter.** §70 names it for the triad XFAIL. It would NOT
  obviously help latency — a phase vocoder's latency is its analysis window, and
  resolving an 82 Hz fundamental wants a window at least as long as the one we are
  trying to shorten. It should be argued as an ACCURACY slice, not a latency one.

## Out of scope for this session

Shipping any of (a)/(b)/(c) — this session is the measurement.

## Status

- [x] In progress — research complete, decision pending
- [ ] Complete
