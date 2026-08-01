# Deluxe Memory Man — the lineup's FIRST DELAY (M13.4)

**Date:** 2026-08-01
**Branch:** claude/delay-memory-man-6f557i
**Roadmap item:** M13.4 — *"the single biggest hole in the lineup"*. Owner
2026-07-31 chose delay + noise gate + CE-1 as the next parallel round.

## Goal

An EHX Deluxe Memory Man-style **BBD analog delay** ships as pedal type `delay`,
web and native, so ambient, slapback and country playing are possible for the
first time. It must sound like a *bucket-brigade* delay — repeats that darken and
degrade — not a digital line with a lowpass bolted on.

## Slot (RESERVED — do not choose your own)

`PEDAL_DELAY = 8` · `skin::AccentId::Delay` · `--accent-delay` (deep blue) ·
web type string **`'delay'`**. All allocated in `chore/reserve-pedal-slots`;
`PEDAL_TYPE_COUNT` is already 11. **Do not renumber anything or touch the gate's
(9) or chorus's (10) slots** — two parallel slices own those.

## Approach

**A NEW DSP FAMILY: the delay line.** This is the first real one in the project,
and it is deliberately the most valuable thing this slice leaves behind — M13.6's
flanger and any future tape echo are cheap *only if* the primitive is right. Write
it as a reusable `DelayLine` (fractional read, interpolated) plus the BBD
behaviour on top, not as a monolith.

What must be modelled rather than approximated:

1. **The BBD itself.** A bucket-brigade is a *sampled* device clocked at a rate the
   DELAY knob varies — the delay time changes because the **clock** changes, which
   is why a real Memory Man's repeats get darker as the time gets longer (the
   anti-alias/reconstruction filters are fixed while the clock moves). A fixed
   filter on a variable-length digital line does NOT reproduce that. Model the
   clock.
2. **The compander.** The MN3005-era BBD has poor noise performance, so the pedal
   companders around it (compress in, expand out). That is a real part of the sound
   — it is why repeats *breathe*. Do not omit it and then compensate with a gain.
3. **The repeats degrade cumulatively.** Each pass through the BBD adds its own
   band-limiting and noise. A feedback loop around a *clean* line with one filter
   in it will not converge to the same place.

Research first, into docs §60, with sources and an explicit list of what could not
be sourced. `github.com` clones work; most audio-DSP sites 403 from this proxy.
Look for the MN3005/MN3007 datasheet behaviour and the DMM schematic.

**Knobs:** the shipped surface is three slots (the house ABI). Use **DELAY /
FEEDBACK / BLEND** (the real pedal's core three). The DMM's chorus/vibrato section
is **out of scope** — say so; a parallel slice is doing the CE-1.

**Real-time discipline.** The delay buffer is sized in `prepare()` for the maximum
delay at the maximum supported rate — **no allocation in `process()`**. Changing
DELAY must not reallocate; it moves a read pointer / clock rate. Modulating the
delay time produces pitch shift (that is physical and correct), so the smoother on
DELAY is part of the voice — state what you chose and why.

## Steps

- [ ] Research → docs §60 (sources + gaps) BEFORE code
- [ ] `DelayLine` primitive + `DelayModel` (BBD clock, compander, feedback path)
- [ ] C ABI `delay_*`; `scripts/build-wasm.sh` (new TU + exports, INSIDE the
      `STAMP:EMCC-ARGS` markers); worklet dispatch
- [ ] `web/src/rig.ts` (add `'delay'` to `PedalType` **and**
      `AVAILABLE_PEDAL_TYPES`), `Pedal.tsx` face, `pedal.css`, assistant
      `tools.ts` + `prompt.ts` coaching (where a delay goes in a chain is real
      advice)
- [ ] Native `ClipperEngine` (fill `PEDAL_DELAY`), APVTS params, `PedalCard` face
- [ ] `core/tests/test_delay_model.cpp` + `clipper_add_test_flags()`
- [ ] NaN-guard + denormal + param-smoothing suites gain the delay
- [ ] Rebuild + commit the three WASM artifacts
- [ ] Docs §60, CLAUDE.md entry, this file's bottom sections

## How this will be measured

- **Delay time vs knob**: measured impulse-to-echo latency in ms across the travel,
  against the published range (~30–550 ms for a DMM — confirm). A table.
- **Repeat degradation**: the spectral centroid (or a fixed HF band) of repeat 1 vs
  2 vs 5 — it must fall monotonically. THIS is the BBD property; a digital line
  with one filter will not do it the same way.
- **Darkening with TIME**: measure repeat 1's HF content at short vs long DELAY.
  A real BBD gets darker as it slows. State the measured difference.
- **Feedback stability**: at FEEDBACK max the loop must not run away — measure the
  peak over 30 s and show it is bounded. At FEEDBACK 0, exactly one repeat.
- **Compander action**: gain vs input level around the BBD (report the curve).
- **BLEND 0 is bit-identical to bypass** (hash), or state why not.
- **No zipper / no click** on a DELAY sweep; state the pitch-shift behaviour.
- Alias floor, DC on signal, `reset()` clean, block-size invariance.
- `--golden-report`: **all five UNCHANGED** — a new pedal must not move a rig.

## Manual test steps

- [ ] Slapback (~90 ms, low feedback) into the JCM — rockabilly works
- [ ] Long ambient (max DELAY, high feedback) — repeats darken and blur, never
      turn into a runaway squeal
- [ ] Sweep DELAY while repeats ring — pitch bends like tape (correct), no clicks
- [ ] Edge: FEEDBACK max + hard strum → bounded; reset clean; 44.1/96 k

## Out of scope

The DMM's chorus/vibrato section, tape echo (EP-3), digital delay, the noise gate
and CE-1 slices (parallel — do not touch their files or slots), any golden bless.


---

<!-- Filled in during/after the session -->

## What actually happened

Built as planned, with four decisions the measurements forced:

1. **`DelayLine` is genuinely load-bearing, not a token separation.** The first
   design had the BBD as a 4096-entry shift register clocked at `f_clk`, which
   would have left the primitive's fractional read untested and unused. The
   shipped structure is equivalent and better: the input is sample-and-held at the
   clock and the bucket propagation IS a fractional delay of `4096/f_clk` seconds
   read out of `DelayLine`. That removes the sub-sample jitter a grid-quantised
   tick would add, and it makes the pitch bend under a DELAY sweep fall out for
   free instead of being special-cased.

2. **The oversampling factor is 8×, DERIVED, not the house 4×.** The BBD's clock
   reaches 136.53 kHz at 30 ms, so the internal rate has to clear 273.07 kHz or
   the device's own clock images fold inside the oversampled domain. 4× at 44.1 k
   is 176.4 kHz and does not clear it. Measured: worst non-harmonic product at
   DELAY 0.0 goes **−65.5 dB at 4× → −122.1 dB at 8×**, for 2.9 % → 5.4 % of one
   stream and zero latency change. The plan did not anticipate this; the alias
   measurement is what produced it.

3. **`latencySamples()` returns 0, and the wet path carries the oversampler's
   group delay as a reported constant.** Mixing dry + wet inside the oversampled
   domain would have broken BLEND-0 bit-identity (the halfband cascade is not
   transparent). Mixing at base rate keeps it exact and puts ~1.75 ms on every
   echo. Compensating the read would have made repeat SPACING wrong instead, so
   the offset is reported in the delay-time table and the spacing is asserted
   separately — it carries no offset at all.

4. **The `pedalkernel` reference was disagreed with, on the netlist.** It delays
   the compressor's envelope and feeds the delayed envelope to the expander. A real
   NE570 has one rectifier per half, each looking at its own input, so nothing
   delays an envelope — and that is precisely the mechanism behind the breathing.
   Two independent rectifiers ship.

Two traps found, both recorded in docs §60.9: a rectangular-window Goertzel
reported a fake −55.8 dB "alias floor" that was its own sidelobe (and, being
window leakage, did not move with the factor — which is exactly what a real alias
floor is supposed to do, so the tell nearly went the wrong way); and "FEEDBACK 0
gives exactly one repeat" is 100 dB down, not bit-zero, because the input filters'
ring-down keeps arriving through a continuously-fed line.

**One thing found that is NOT this slice's and was not fixed:** `native/src/
PedalCard.cpp`'s `kFaces` table has the WAH and COMP entries at indices 6 and 7
**swapped** relative to `PedalType` (`PEDAL_WAH = 6`, `PEDAL_COMP = 7`), so the
native editor labels a Weeper card "Squash" and vice versa. Pre-existing on `main`
from the parallel wah/comp merge. Reported, not touched — it belongs to those
slices, and fixing it here would change two other pedals' UI inside a delay PR.

## Measured results

Full tables in docs §60.4-§60.7. Headlines:

- **Delay time**: 30.00 → 550.00 ms across the knob, linear (the CD4047's own law),
  device law `delay·f_clk == 4096` exact at every position. Clock 136 533 Hz down
  to 7 447 Hz — past BOTH ends of the MN3005's rated 10–100 kHz, as a real DMM is.
- **Repeat degradation** (DELAY 0.5, FEEDBACK 0.7), HF/LF per repeat:
  **−29.91 / −32.45 / −35.23 / −37.43 / −39.83 dB** — monotone, **9.92 dB** lost
  from repeat 1 to 5, ~2.5 dB per pass through the device.
- **Darkening with TIME** (repeat 1, FEEDBACK 0): **−24.90 dB at 30 ms →
  −33.90 dB at 550 ms = 8.99 dB**. The charge-smear corner is `0.2254·f_clk`,
  derived from the stage count and the transfer efficiency: 30.8 kHz → 1.68 kHz.
- **Feedback**: FEEDBACK 0 gives echo1 6.14e-03 / echo2 **1.70e-22 (−391 dB)**.
  FEEDBACK max over 30 s: peak 1.135 / 0.920 / 0.920 at DELAY 0/0.5/1.0, tails
  0.000 / 0.294 / 0.349 and **converging** (a 60 s run asymptotes at 0.3116).
- **Compander**: through-gain **−0.09 dB at 0.005 V → −2.57 dB at 0.6 V**, product
  of the two gains unity to 0.2 % over the bottom 30 dB; at 0.005 V the compressor
  puts **+16.64 dB** into the device and the expander takes **−16.61 dB** back.
- **BLEND 0 bit-identical**: 0/48000 samples differ, hash `6276ea7e3109b4c7` both
  sides.
- **Oversampling 8× derived**: alias floor −65.5 dB (4×) → **−122.1 dB** (8×).
- **Hygiene**: DC on signal 0.0000 % (with and without a +0.1 V input offset),
  block-size invariance **exactly 0.0**, rate spread **0.0001 dB** over 44.1–96 kHz,
  `maxAbsRestingState()` **exactly 0.0** after 40 s of silence, one NaN →
  **0/96000** after `reset()`, latency **0**, CPU **5.4 %** of a 48 kHz stream.
- **`DelayLine`**: integer read exact (0/350 mismatches), fractional read flat to
  0.005 dB at 1 kHz, quarter-sample steps tracked to 0.0003 samples, cubic
  **−0.226 dB** vs linear **−1.249 dB** at 8 kHz.
- **Perturbations 6/6 RED, restore GREEN** (docs §60.8).
- **Goldens: all five UNCHANGED at ±0.00 dB. Nothing blessed.**

## Files created / modified

Core: `core/include/clipper/dsp/DelayLine.h` (new primitive),
`core/include/clipper/dsp/DelayModel.h` + `core/src/dsp/DelayModel.cpp` (new),
`core/src/clipper_c_api.cpp` (`delay_*`), `core/CMakeLists.txt`,
`core/tests/test_delay_model.cpp` (new, `clipper_delay_tests`),
`core/tests/test_nan_guard.cpp`, `core/tests/test_denormal.cpp`,
`core/tools/render/main.cpp`, `core/tools/bench/main.cpp`.
Web: `web/worklet/clipper-processor.js`, `web/src/rig.ts`,
`web/src/components/Pedal.tsx`, `web/src/styles/pedal.css`,
`web/src/assistant/tools.ts`, `web/src/assistant/prompt.ts`,
`web/tests/audio.spec.ts`, `scripts/build-wasm.sh`, and the three regenerated
artifacts under `web/public/generated/`.
Native: `native/src/ClipperEngine.h/.cpp`, `native/src/PluginProcessor.h/.cpp`,
`native/src/PedalCard.cpp`, `native/tests/identical_core_test.cpp`.
Docs: `docs/DEVELOPMENT.md` §60, `CLAUDE.md` Current State, this file.

## Deferred to next session

- **An ADR is needed and its number was NOT chosen** (§ and ADR numbers are
  assigned centrally; 019 is the highest taken). Its subject: *the delay line is a
  reusable primitive separate from the device behaviour, and the BBD's own clock —
  not the house 4× convention — sets the oversampling factor.* Both are decisions a
  future slice could reasonably "fix" back without one.
- **Unify `DelayLine` with `ChorusModel`'s private ring buffer.** `ChorusModel`
  has its own cubic-interpolating ring, written before this primitive existed, and
  it still indexes with `%` (docs §32's cost). Folding it onto `DelayLine` is
  fidelity-sensitive (`ChorusModel` is in the `clean120_chorus` golden), so it is
  its own slice with a bit-identity bar.
- **The DMM's chorus/vibrato section.** Out of scope here by the plan; a parallel
  slice ships the CE-1, and once both have landed the DMM's own modulation is a
  small addition on top of this delay line.
- **Find the DMM schematic.** The anti-alias / reconstruction filters are the
  JUNO-60's, not the Memory Man's (docs §60.1). Do not re-tune them toward a sound
  — find the sheet.
- **The `kFaces` index swap in `native/src/PedalCard.cpp`** (see above). One-line
  fix, belongs to the wah/comp slices.
- **`kMaxChain` is one-instance-per-type natively**, so two delays in series (a
  very normal request — a short slap into a long wash) works on the web and not in
  the plugin. Same open item the wah slice recorded.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
