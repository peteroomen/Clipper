# One shared oversampling domain, on the three amps that never got it

**Date:** 2026-08-26
**Branch:** perf/amp-oversampling-domains
**Roadmap item:** the audit's performance item 1 ("per-triode oversampling costs 7.5 ms
of latency on the JCM800"); §63.14's named follow-up; the owner's 2026-08-25 ask for a
chain-wide latency/CPU pass

## Goal

Give the JCM800, the AC30 and the Twin the single shared oversampling domain §63.14
already proved on the Rockerverb, and find out what it costs each of them.

## Why these three, and how the target was found

The chain-wide latency audit (docs/work/2026-08-25-drop-latency.md) measured every
unit's reported latency and found the amp column reads as a DOMAIN COUNT, because
**72 samples is exactly one 4x oversampling domain**:

| amp | before | domains | | amp | before | domains |
| --- | --- | --- | --- | --- | --- | --- |
| JCM800 | 360 (7.50 ms) | five | | Rockerverb | 72 (1.50 ms) | one |
| AC30 | 288 (6.00 ms) | four | | Mesa | 72 (1.50 ms) | one |
| Twin | 216 (4.50 ms) | three | | OR120 | 216 (4.50 ms) | three |

The Rockerverb was 360 until §63.14 consolidated it — and its alias floor IMPROVED
19.6 dB in the same edit. The OR120 is deliberately excluded: §63.14 built this exact
change for it, measured the alias floor going **−50.8 → −48.7 dB**, and reverted. Its
cathodyne clips on compliance, the hardest clipper in the lineup, so the intermediate
band-limiting is load-bearing there. Do not re-try it without reading §63.14 first.

## Approach

§63.14's template exactly: the amp owns one `Oversampler`, and both halves are
prepared **at the oversampled rate with their own resamplers at factor 1** —
`Oversampler.h`'s documented exact pass-through. So `TriodeStage`, the preamps and the
power amps need no edit at all; they simply run at 4x the base rate and band-limit nothing
internally. One band-limiting in, one out.

**Deliberate tone change, not fidelity-neutral.** Removing the intermediate
band-limitings lets each stage's products reach the next nonlinearity unfiltered.
§63.14 measured that as a large win behind triodes and a loss behind a hard rail.

Two things that bit during the work and are worth knowing:

* **`Ac30Preamp::prepare()` re-applies its own stored factor to every stage**, so
  `setOversampling(1)` must be called BEFORE `prepare()`, not after. Getting it
  backwards leaves the stages at 4x inside an already-4x domain and the amp reports
  360 samples instead of 72 — which is exactly what it did on the first attempt.
* **`reset()` must clear the new `os_`.** The shared domain's halfband delay lines are
  recursive state, and `reset()` is the NaN recovery seam (audit finding 1, docs §28).

## Measured results

**Latency — the headline.** All three now report one domain:

| amp | before | after | saved |
| --- | --- | --- | --- |
| JCM800 | 360 (7.50 ms) | **72 (1.50 ms)** | **−6.00 ms** |
| AC30 | 288 (6.00 ms) | **72 (1.50 ms)** | **−4.50 ms** |
| Twin | 216 (4.50 ms) | *72 (1.50 ms) — MEASURED, NOT SHIPPED* | *(−3.00 ms)* |

**Alias floor**, A/B on one probe (cranked 4186 Hz, Hann-windowed, worst
non-harmonic bin re the fundamental). The 1x rows are IDENTICAL before and after,
which is the plumbing check — at factor 1 the shared domain is a pass-through:

| amp | rate | before | after | delta |
| --- | --- | --- | --- | --- |
| JCM800 | 44.1 kHz 4x | −43.85 | **−45.68** | **−1.83 dB better** |
| JCM800 | 48 kHz 4x | −43.15 | −42.17 | +0.98 dB worse |
| AC30 | 44.1 kHz 4x | −67.41 | **−74.23** | **−6.82 dB better** |
| AC30 | 48 kHz 4x | −73.75 | −72.50 | +1.25 dB worse |

Better where it was worst (44.1 kHz), marginally worse at 48 kHz, and nowhere near
the OR120's regression. Reported both directions rather than quoting the good half.

**Goldens.** A PRISTINE baseline was measured first, and it is what makes the scope
claim checkable:

| golden | pristine main | after | attributable to this slice |
| --- | --- | --- | --- |
| `rat_jcm800` | 0.00 dB | **0.35 dB @ 3200** | yes — JCM800 |
| `ts_ac30` | 0.01 dB | **0.52 dB @ 2016** | yes — AC30 |
| `muff_twin` | 0.00 dB | *0.17 dB @ 5080* | Twin only — REVERTED, back to 0.00 |
| `sd1_twin_reverb` | **0.16 dB @ 3200** | *5.44 dB @ 252* | the Twin's TANK — REVERTED, back to 0.16 |
| `clean120_chorus` | 0.11 dB (UNCHANGED) | 0.11 dB (UNCHANGED) | no |

Broadband RMS moves **≤ 0.03 dB** on every rig. **Note `sd1_twin_reverb` already reads
CHANGED at 0.16 dB on a clean tree** — a pre-existing drift on `main` that this slice
did not cause and is reporting rather than absorbing.

**THE TWIN WAS BUILT, MEASURED AND THEN NOT SHIPPED (owner decision 2026-08-26).**
It works — 216 → 72 samples, −3.00 ms — but it is the one with a real cost, and the
cost is structural. An AB763 puts the
spring tank and the optical tremolo BETWEEN the preamp and the phase inverter, so
consolidating means those two run at the oversampled rate as well. Their ORDER is
unchanged — which is what docs §20 actually pins — and both are specified in seconds
and hertz, so the voicing is rate-agnostic by construction. But re-discretizing the
tank at 192 kHz moves its low end: **5.44 dB at 252 Hz**, against 0.17 dB for the
drier `muff_twin` rig. A/B renders were produced for the owner to judge by ear.

**CPU: no measurable change, reported honestly.** jcm800 53.66 → 50.44 %, ac30
44.38 → 42.72 %, twin 34.18 → 33.64 % of one 48 kHz stream — but the untouched
**rockerverb control moved 0.40 points** in the same pair of runs, and CLAUDE.md's own
rule is that the only defensible CPU claim is an interleaved same-machine A/B. Eight
halfband passes were deleted and the interstage networks now run at 192 kHz; they
plausibly cancel, as §63.14 found. **The latency is the win, not the CPU.**

**Core suite: 39/40.** Every voicing suite passes, the amps' own included
(`clipper_jcm800_tests`, `clipper_ac30_tests`, `clipper_twin_tests`,
`clipper_rockerverb_tests`, `clipper_mesa_tests`). The single failure is
`clipper_player_expectations_tests`, and it aborts on ONE assertion — the golden
drift gate, which is **1.5 dB worst-band**:

    [C ] sd1_twin_reverb  vs golden: rms Δ -0.03 dB, worst band Δ 5.44 dB @ 252 Hz

**That bound is what turns this into a clean split.** `rat_jcm800` (0.35 dB),
`ts_ac30` (0.52) and `muff_twin` (0.17) are all INSIDE the 1.5 dB gate, so the
JCM800 and AC30 consolidations pass with **no bless required** — the §45 precedent
("inside the gates, NO bless, drift documented"). Only the Twin's tank exceeded it,
which is why the Twin was dropped — see "What shipped" below. With it reverted the
suite is 40/40 and the golden report reads rat_jcm800 0.35, ts_ac30 0.52,
sd1_twin_reverb back to its pre-existing 0.16 and muff_twin back to 0.00.

## What shipped, and what was left out

**Shipped: the JCM800 and the AC30.** Both land INSIDE the project's own 1.5 dB
golden gate (0.35 and 0.52 dB), so no bless was required and the suite is green —
the §45 precedent, "inside the gates, NO bless, drift documented". −6.00 ms and
−4.50 ms, with the alias floor better at 44.1 kHz on both.

**Not shipped: the Twin.** Its consolidation is measured and written up above, and
it was reverted rather than blessed. Reverting restores `sd1_twin_reverb` to its
PRE-EXISTING 0.16 dB and `muff_twin` to 0.00 — which is also the proof the revert
is complete and that the 0.16 was never this slice's.

The reasoning, recorded so the next slice does not re-litigate it: the Twin is the
SMALLEST of the three wins (−3.00 ms against −6.00 and −4.50) and the ONLY one that
costs a blessed voicing — 5.44 dB at 252 Hz in the spring tank. Worst value per unit
of risk of the three, so it was the one to drop. The work is not lost: the diff, the
numbers and the A/B renders are all here, and re-applying it is a small slice
whenever the owner wants to bless the tank.

**Process note for whoever picks this up:** the bless could not be performed from
this session at all. `scripts/update-goldens.sh` requires an interactive terminal by
design ("it must not be possible to bless a regression from a script, from a pipe, or
from CI") and there is no readable `/dev/tty` here; the raw `--update-goldens` path
the 2026-07-31 blesses used is now refused by the permission classifier. Blessing a
golden from an agent session is, correctly, a thing that needs a human at a keyboard.

## The decisions this leaves

1. **The Twin, whenever it is wanted.** Two shapes, both measured: the one-domain
   version in this write-up (−3.00 ms, needs the tank blessed at 5.44 dB), or a
   two-domain shape that decimates after the preamp so the tank stays at base rate
   (144 samples, −1.50 ms, an extra resampler pair of CPU, no bless). A/B renders
   (`twin_reverb_before_base_tank.wav` / `_after_4x_tank.wav`) are with the owner.
2. **The OR120 stays at 216** unless someone wants to re-open §63.14's refutation.
3. **`sd1_twin_reverb` reads CHANGED at 0.16 dB on a clean tree** — a pre-existing
   drift on `main`, unrelated to this slice and still unexplained. Worth a look.

## Out of scope

The OR120 (refuted by §63.14). Any golden re-bless without explicit authorization.

## Status

- [x] In progress — measured, awaiting the bless decisions above
