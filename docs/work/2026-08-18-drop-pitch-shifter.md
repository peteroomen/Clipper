# The "Cellar" polyphonic drop-tune pitch shifter — the lineup's FIRST PITCH effect

**Date:** 2026-08-18
**Branch:** feat/drop-pitch-shifter
**Roadmap item:** the second half of the owner's 2026-08-17 ask ("the mesa finally and
the drop-pedal … it's the grunge unlock, I can play AIC down tuned half step with the
drop pedal through a mesa"). Adjacent to M13.9's Octavia, which explicitly excludes
octave-DOWN as "the hard one".

## Goal

Ship a DigiTech Drop-class **polyphonic downward pitch shifter** as pedal type `drop`,
slot **13** (`PEDAL_TYPE_COUNT` → 14), wired end to end in one slice. Nine shift
positions (1–7 semitones down, OCT, OCT + DRY), and it must shift a **CHORD** — not a
single note — with the attack still intact.

## THIS IS NOT A CIRCUIT MODEL, AND THAT CHANGES THE WHOLE SHAPE OF THE SLICE

Every voice in this repo so far has been a circuit: there was a netlist to find, and
§57's rule ("do not re-tune a constant toward a sound; find the schematic") was the
discipline that kept it honest. **A pitch shifter has no schematic.** The DigiTech
Drop is a DSP algorithm in a fixed-function processor, and no amount of research will
produce component values.

So §57's rule does not apply — and it must be **replaced**, not simply dropped,
because without it there is nothing stopping this slice from becoming a pile of
constants tuned until it "sounds right", which is exactly the failure mode this
project spent §43/§50/§52 undoing.

**The replacement discipline, and it is the core of this plan:** every constant is an
algorithm parameter, and each one must be chosen against a **measurement with an
external reference** — cents error against the exact frequency ratio, latency in
samples, artifact energy in dB, attack preservation against the dry signal. Those are
absolute references in the same sense a datasheet knee is. **A constant that can only
be justified by "it sounds better at this value" does not ship.**

What IS sourced, and it is thin — a product spec, not a circuit:

| Fact | Source |
| --- | --- |
| 1–7 semitones down, OCT, OCT + DRY (9 selector positions) | manufacturer spec |
| Only the OCT + DRY position blends dry; every other position is 100 % wet | manufacturer spec |
| Momentary vs latching footswitch mode (a physical switch) | manufacturer spec |
| True bypass | manufacturer spec |
| Polyphonic — shifts all six strings at once, no monophonic tracking | manufacturer spec |
| "Virtually latency-free"; 24-bit / 44.1 kHz converters | marketing copy — **weak, treated as a direction not a number** |

Everything else is this model's own and will be labelled as such.

## Approach

### The algorithm: a crossfaded variable-delay shifter, built on `DelayLine.h`

Two read taps sweep through a delay buffer at a rate set by the pitch ratio, and are
crossfaded against each other so that whichever tap is about to run off the end of its
window is faded out. For a downward shift by ratio `r < 1` the read pointer advances at
`r` samples per sample, so the delay grows linearly and the tap must be re-seated once
per grain; grain rate is `(1 − r) / window`.

**Why this and not a phase vocoder** (`FFT.h` exists and is the obvious alternative):

1. **It is polyphonic BY CONSTRUCTION.** It performs no pitch detection whatsoever —
   it resamples the waveform, so a chord and a single note are the same operation.
   That is the single most important property of this pedal and it comes free.
   A tracker-based shifter has to *decide* on one f0 and will fail on a chord.
2. **The attack survives.** A phase vocoder smears transients across its analysis
   window, and attack preservation is precisely what this pedal is bought for.
3. **Latency is a design parameter, not a window length.** A vocoder pays a full
   analysis window; this pays the grain window, which can be much shorter.
4. **It reuses validated machinery.** `DelayLine.h` (M13.4) is a plain interpolating
   ring buffer that "knows nothing about bucket brigades, feedback or companders" —
   the primitive/device split that slice was explicitly built for. This is its second
   consumer, and the first one that tests whether that split was right.

**The cost, named up front:** a crossfaded delay shifter has a characteristic artifact
— periodic amplitude/comb modulation at the grain rate, worst on sustained pure tones
and on large shifts. That is a REAL, MEASURABLE quantity and it becomes bar 4 below,
rather than something discovered later by ear.

**A phase vocoder is NOT ruled out forever** — but per ADR 021's discipline it should
be built only when a slice can measure that the time-domain version's artifact floor is
the thing standing between the player and the sound. Not assumed.

### Oversampling: to be DECIDED BY MEASUREMENT, not assumed

A variable delay is **linear time-varying** — the phaser and chorus precedent (§12)
says no oversampling. But the crossfade is a *multiply by an envelope*, which is what
made §64's optical compressor need it. The two cases genuinely differ and this project
has been wrong in both directions before.

**So the plan is: measure the alias floor at 1×/2×/4×/8× BEFORE the loop is written,
and ship whatever the measurement says.** §54's tell applies — a real alias floor MOVES
with the factor; a flat floor means it is not aliasing and oversampling buys nothing.
§60's warning also applies: use a **Hann window**, because a rectangular window's own
sidelobe reads as a −56 dB floor and will fake exactly this measurement.

### Controls

Faithful to the reference, which is unusually simple:

| Slot | Control | Notes |
| --- | --- | --- |
| 0 | **AMOUNT** | 9 discrete positions: −1…−7 semitones, −12, −12+dry |
| 1 | **MIX** | *Candidate only — see the open question below* |
| 2 | — | unused |

## THE ACCEPTANCE BARS

Six, and the first three are the ones that carry the slice.

**Bar 1 — PITCH ACCURACY, against the exact ratio.** A rendered tone at every one of
the 8 shift positions must land within **±5 cents** of `f0 · 2^(−n/12)`. Five cents is
under the ~6-cent JND for a sustained tone, and the ratio is arithmetic, so this is an
absolute external reference, not a self-comparison. Measured by parabolic-interpolated
FFT peak or autocorrelation, cross-checked against a Goertzel at the predicted bin.

**Bar 2 — POLYPHONY, and this is the load-bearing one.** A three-note chord (E2/G#2/B2)
rendered through the pedal: **every partial must shift by the SAME ratio**, each within
±5 cents, with the **spread across partials under 2 cents**. A monophonic
pitch-tracking shifter cannot hold this by construction — it picks one f0 and shifts
everything by that. The spread, not the absolute error, is what separates the two, so
the spread is what is asserted.

**Bar 3 — THE ATTACK SURVIVES.** A plucked-note transient through the shifter must
retain its 20 ms peak within **2 dB** of the dry signal's, and its attack must not be
late by more than **5 ms**. This is the property the reference is bought for and the
one a phase vocoder would fail; asserting it here is what stops a later slice from
"upgrading" to a vocoder without noticing the cost. (§61's gate measured 0.01 dB / 0.00
ms on the same shape of test, so the harness exists.)

**Bar 4 — THE ARTIFACT FLOOR IS BOUNDED AND REPORTED.** Non-harmonic energy the
shifter itself adds, on a sustained tone, measured per shift position. This is the
crossfade's own signature and it WILL be audible at −12; the bar is that it is
**measured, monotone in shift depth, and stated** — not that it is inaudible, which
would be a claim this algorithm cannot make.

**Bar 5 — THE DRY PATH IS EXACT.** At the OCT + DRY position the dry component must be
**bit-identical** to the input, and a disengaged pedal must be **bit-identical to
bypass** — the same contract §60 established for the delay's BLEND 0 (measured there as
0/48000 samples differing). Not a tolerance; a `memcmp`.

**Bar 6 — LATENCY IS DERIVED AND ASSERTED.** Reported in samples and ms, asserted
against the grain window the algorithm actually uses rather than read back from the
model (§58's "a bar that could not fail" lesson — an assert against
`latencySamples()` itself is an identity).

Plus the housekeeping every unit in this repo carries: ragged-block invariance,
`reset()` vs a fresh model, one NaN → 0 non-finite after reset, DC on signal, rate
independence over 44.1–96 kHz, zipper on a slammed knob, and ADR 006 denormal scope
**decided by measurement** (the delay ring is a FIFO with no recursion, so §62's chorus
finding is the likely precedent — but it must be measured, not inherited).

## Steps

- [ ] **Measure first, build second:** a scratch harness that renders a tone and a
      chord through a prototype shifter and reports cents error, spread, artifact floor
      and the 1×/2×/4×/8× alias sweep. Decide the oversampling factor and the grain
      window from those numbers.
- [ ] `core/include/clipper/dsp/PitchShifter.h` — the PRIMITIVE: crossfaded
      variable-delay resampling on `DelayLine`, knowing nothing about pedals, knobs or
      the 9-position selector. Its own bars in the test suite, the way `DelayLine` got
      them in §60.
- [ ] `DropModel.{h,cpp}` — the pedal: the 9-position selector, the dry blend at the
      last position, and the declick/bypass contract.
- [ ] C ABI `drop_*`, worklet dispatch, `PEDAL_TYPE_COUNT` → 14.
- [ ] Web: face + accent token + wordmark, `params.ts`, `rig.ts`, assistant tool +
      prompt coaching (including the down-tuning use case that prompted it).
- [ ] Native: engine + APVTS + `PedalCard` face + `pedalMenuLabel` case — **and its own
      case in `identical_core_test`'s plugin driver**, which §64 flagged as a standing
      gap and §67 only partly closed.
- [ ] `core/tests/test_drop.cpp`; register in CMake with `clipper_add_test_flags`.
- [ ] `bash scripts/build-wasm.sh` — and add the new `.cpp` files INSIDE the
      `STAMP:EMCC-ARGS` markers. §60, §64 and §68 have now ALL been bitten by this.

## How this will be measured

`clipper-render` for audible A/Bs; a new `clipper_drop_tests` target for every bar
above; `clipper-bench` for CPU; `--golden-report` to confirm all five goldens are
unchanged (a new pedal is in no golden rig, so they must not move at all).

## Manual test steps

- [ ] Set −2 semitones, play in standard, confirm it sounds like Eb standard — the
      owner's actual use case (AIC down a half step is −1; most of their catalogue is
      −2 from E standard).
- [ ] Play a full open chord and confirm it does not warble or pick one note.
- [ ] Palm-muted chugs at −5 through the Mesa on Red Modern: confirm the attack is
      still there and the pedal is not smearing the pick.
- [ ] Edge case: switch shift position mid-note — must not click (declick bracket).
- [ ] Edge case: OCT + DRY, confirm the dry note is audibly present and in tune.
- [ ] Edge case: NaN into the engine, then `reset()` → 0 non-finite samples.

## OPEN QUESTION FOR THE OWNER — one, and it has precedent

**Should the pedal have a MIX knob the real one does not have?**

The reference is 100 % wet at every position except OCT + DRY. That is a deliberate
design choice — it is a *tuning* pedal, not a harmoniser, and a wet/dry blend on a
drop-tune is a chorus-y mess in most cases.

But this board has a standing rule against dead UI, and a two-slot pedal with one
unused slot is the shape §59's compressor and §61's gate both shipped with, so it is
not unprecedented to leave slot 1 empty.

- **Faithful (recommended):** AMOUNT only, slot 1 unused, exactly like the reference.
  §61.3's precedent — the gate's MODE was refused because shipping it would have been
  dead UI or a footswitch that meant something different on one pedal.
- **Add MIX:** more useful for octave-down bass-doubling, but it is a feature the
  reference does not have and ADR 020 requires that kind of departure be recorded with
  its cost.

The **momentary/latching switch is NOT in question** — this board's footswitch is
bypass on every pedal, and §61.3 settled that a mode switch which changes what the
footswitch does is not shippable here. Documented, not built.

## Out of scope for this session

- **Upward shifting and harmonised intervals** (the Whammy DT's territory). The Drop is
  down-only and this stays down-only.
- **A phase-vocoder implementation** — see the approach section; it is a later slice if
  and only if a measurement demands it.
- **M13.9's Octavia** — a different effect entirely (octave UP via rectification, no
  pitch shifting at all).
- Re-voicing anything on the Mesa.

---

<!-- Fill in below during/after the session -->

## What actually happened

**Step 1 of the plan — "measure first, build second" — did its job, and it took two
structural corrections to get a working shifter. Neither was a tuning change.**

**Correction 1: the textbook two-tap arrangement is wrong.** Taps a half window
apart, sin/cos crossfaded across the whole cycle, is what the plan described and
what was built first. Both taps then sit at ~0.707 gain for most of the cycle with
a fixed W/2 delay between them, so the output is a permanent deep comb: **96 % of
the energy landed off the harmonics**, and the apparent pitch was up to **150 cents**
wrong. Replaced by ONE live tap at unity gain for 75 % of the window, with a short
equal-power handover.

**Correction 2: a fixed splice point gives a systematic pitch error.** With the
structure fixed the output was still sharp, by a margin that scaled with the shift:
`r_eff = 0.9697*r + 0.0303`, i.e. +3.1 cents at a semitone and **+51.7 cents at an
octave**, with the exact target sitting **60 dB below the spectral peak**. Bisected:
the resampling itself is EXACT (0.000 cents with the wrap disabled), so the splice
was the whole error. A fixed splice jumps the read by a fixed number of samples,
which is a fixed FRACTION of any given input period — so every grain slips the
phase the same way, and a constant phase slip per unit time *is* a frequency
offset. Fixed with **SOLA**: choose the splice by normalised cross-correlation.
It stays polyphonic because a correlation search forms no opinion about pitch.

**After both: single notes 0.006–0.084 cents, power chords 0.00 cents.**

**The open item is triads, and it is a measured trade rather than a defect.** See
"Measured results". Bar 2 as written (spread < 2 cents on a major triad) is NOT met
at the shipped window; it IS met at a wider search span, for 2x the latency.

## Measured results

48 kHz, 50 ms window, 17.5 ms SOLA span:

| stimulus | worst peak offset |
| --- | --- |
| single note, −1 … −12 semitones | **0.006 … 0.084 cents** |
| E5 power chord (−1 / −2) | **0.00 / 0.25 cents** |
| E5 + octave | **0.00 cents** |
| E major triad (−1 / −2) | 9.75 / 22.75 cents |
| non-harmonic energy, −1 semitone | −17.0 dB |
| mean algorithmic latency | 31 ms |

The triad/latency trade, measured rather than argued:

| SOLA span | window | latency | E major triad |
| --- | --- | --- | --- |
| 17.5 ms | 50 ms | 31 ms | 9.75 … 22.75 cents |
| 50 ms | 100 ms | 62 ms | 0.75 … 1.25 cents |
| 50 ms | 120 ms | 75 ms | 0.50 … 1.75 cents |

## Files created / modified

## Deferred to next session

## Status

- [x] In progress — the PRIMITIVE works and is measured; the pedal wrapper, the
      ABI/web/native wiring and the test suite are NOT built yet, pending the
      owner's call on the latency/triad trade above.
- [ ] Not complete
- [ ] Complete
- [ ] Partial — see deferred
