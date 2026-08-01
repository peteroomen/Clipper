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
