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

## THE OPTION I RECOMMENDED IS REFUTED BY A FINER SWEEP — read this first

The table above compared only −1 and −12 and I put "W 65 → 56, nothing lost" to the
owner on it. Sweeping the window at 2 ms resolution says that was wrong, and in two
separate ways:

| window | margin on span | TRUE mean (single −1) | triad −1 spread | triad −2 | triad −12 |
| --- | --- | --- | --- | --- | --- |
| **W65 (shipped)** | 8.5 ms | **52.82 ms** | 0.93 c | 1.91 c | **2.93 c** |
| W62 | 5.8 ms | **55.95 ms** | 0.93 c | 1.90 c | **10.84 c** |
| W60 | 4.0 ms | 48.00 ms | 0.92 c | 1.92 c | **15.56 c** |
| W58 | 2.2 ms | 47.60 ms | 0.94 c | 1.91 c | **15.55 c** |
| W56 | 0.4 ms | 37.69 ms | 0.92 c | 1.89 c | **13.92 c** |

**(1) The accuracy cost is a CLIFF, not a slope.** The triad at the octave goes
2.93 → 10.84 cents between 65 ms and 62 ms and never recovers. §70 chose 65 over 60
and recorded the reason as span margin; the margin turns out to be load-bearing for
accuracy, not just headroom. Framing the −12 triad as "already covered by the XFAIL"
was wrong of me: the XFAIL is a 2-cent target at −2, and this is a 5× degradation of
a case that currently passes.

**(2) TRUE latency is NOT MONOTONE in the window.** W62 measures **55.95 ms — worse
than the shipped W65's 52.82**. The mean read delay is set by where SOLA re-seats,
which lands on a multiple of the material's period; how that interacts with the
window top is a modular-arithmetic effect, not a smooth function. So "shorten the
window a bit" is not even reliably a latency win.

**Conclusion: the window is a bad latency lever and no window change is shipped.**
A real reduction needs a different mechanism, and the honest candidates are §70's
frequency-domain shifter (argued as an ACCURACY slice) or accepting the pedal's
latency as inherent. The A/B renders (`cellar_ab_*.wav`, DROP 2 and OCTAVE at
65 / 56 / 30 ms over a low E, a power chord and a triad) are for the owner to judge
the 30 ms case by ear, since that one IS a large latency win with a real cost.

## The rest of the chain — the audit the owner asked for

Every unit's reported latency at shipped defaults, measured:

| unit | samples | ms | | unit | samples | ms |
| --- | --- | --- | --- | --- | --- | --- |
| rat / sd1 / ts / muff / gold | 72 | 1.50 | | **jcm800** | **360** | **7.50** |
| squash / lumen / swirl / wah | 72 | 1.50 | | **ac30** | **288** | **6.00** |
| curfew gate | 0 | 0.00 | | **twin** | **216** | **4.50** |
| ninety phaser | 0 | 0.00 | | **orange or120** | **216** | **4.50** |
| ensemble ce-1 | 0 | 0.00 | | rockerverb | 72 | 1.50 |
| echoman delay | 0 | 0.00 | | mesa | 72 | 1.50 |
| cellar drop | 0 (真 ~52.8) | — | | clean 120 | 0 | 0.00 |
| cab convolver | 128 | 2.67 | | output limiter | 64 | 1.33 |

**72 samples is exactly ONE 4× oversampling domain.** So the amp column reads as a
domain count: JCM800 **five**, AC30 **four**, Twin and OR120 **three**, Rockerverb
and Mesa **one**. The Rockerverb was 360 until §63.14 consolidated it to a single
shared domain — latency 7.50 → 1.50 ms — and the alias floor IMPROVED 19.6 dB in
the same change.

**So the biggest remaining latency win in the whole chain is applying §63.14's own
fix to the three amps that never got it:** JCM800 −6.0 ms, AC30 −4.5 ms, Twin
−3.0 ms. It is NOT free — §63.14 measured the identical change making the OR120
WORSE (alias floor −50.8 → −48.7 dB) and reverted it, so the OR120 is a
known-refuted case and the other three each need their own measurement and probably
a golden re-bless. That is a slice, and it is the one worth doing next.

CPU on the same pass (`clipper-bench`, % of one 48 kHz stream): rockerverb 55.7,
jcm800 53.7, ac30 44.4, twin 34.2, **muff 29.3**, squash 8.9, swirl 6.7, echoman
4.5, lumen 3.4, curfew 2.8, gold 1.9, rat 1.9, ts 1.2, sd1 1.2, ninety 0.4. A rig of
two dirt boxes into a valve amp is already 55–60 % of one core, so the amps are the
CPU story as well as the latency story.

## The decision this leaves

Three options, and they differ enough that the owner should pick:

- ~~**(a) W 65 → 56**~~ **REFUTED above** — the accuracy cost is a cliff and the
  latency is non-monotone in the window. Not shipped.
- **(b) W 65 → 30.** **52.8 → 18.0 ms** (−66 %) — still the only large win available
  inside this architecture. Single notes and power chords stay inside the 5-cent
  bar (worst 2.47 c). Triads leave it (5.89 c at −1, 14.74 c at −12). A/B renders
  are with the owner.
- **(c) Frequency-domain shifter.** §70 names it for the triad XFAIL. It would NOT
  obviously help latency — a phase vocoder's latency is its analysis window, and
  resolving an 82 Hz fundamental wants a window at least as long as the one we are
  trying to shorten. It should be argued as an ACCURACY slice, not a latency one.
- **(d) THE AMP OVERSAMPLING DOMAINS — the recommendation.** Not the drop pedal at
  all: JCM800 7.50 → 1.50 ms, AC30 6.00 → 1.50, Twin 4.50 → 1.50, by the
  consolidation §63.14 already proved on the Rockerverb. Bigger total win than
  anything available on the Cellar, on the units a player always has in the chain.

## Out of scope for this session

Shipping any of (a)/(b)/(c) — this session is the measurement.

## Status

- [x] In progress — research complete, decision pending
- [ ] Complete
