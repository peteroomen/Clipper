# Cry Baby wah + envelope filter — the first filter pedal (M13.1)

**Date:** 2026-07-31
**Branch:** claude/wah-envelope-6f557i
**Roadmap item:** M13.1. Owner 2026-07-31 chose **"Position knob + envelope auto
mode"** — one resonant bandpass primitive serving both a foot-driven Cry Baby and
an envelope-driven Mu-Tron-style filter.

## Goal

A wah pedal (`wah`) ships web and native: a GCB-95-style inductor bandpass whose
POSITION is a normal automatable parameter, plus an AUTO mode where an envelope
follower sweeps the same filter — so funk is playable for the first time.

## Approach

**New effect family — the first filter pedal.** The circuit is the Dunlop GCB-95:
a common-emitter BJT stage (reuse `BjtStage` — it exists and is exactly this
device class) around an **inductor-based resonant bandpass** with the pot setting
the sweep. Research the real topology; do not ship a generic biquad and call it a
wah — the whole character is the inductor's Q and the specific sweep law.

Two decisions that need to be made by measurement, not taste:

- **The sweep law.** A real wah's treadle-to-frequency mapping is not linear and
  not a plain log; it is set by the pot taper plus the circuit. Derive it, state
  it, and make POSITION 0→1 traverse the real range (roughly 400 Hz–2.2 kHz on a
  GCB-95, to be confirmed). A wrong law is the most common way a modelled wah
  feels wrong even when the filter is right.
- **Q and the "vocal" peak.** The resonance height vs frequency is what makes it
  talk. Measure the peak height across the sweep and pin it.

**AUTO mode** reuses the same filter, driven by an envelope follower over the
input. Attack/release constants matter more than the filter here — a slow
follower is a swell, a fast one is a quack. Expose SENSITIVITY. **Note the
compressor slice running in parallel is building an envelope detector too** — do
NOT try to share code across branches; if both land, a later slice can unify them
(record that as a named follow-up rather than coupling two in-flight slices).

**Real-time discipline:** the filter coefficients change per sample as POSITION
or the envelope moves. Follow the phaser (`docs §20`) — per-sample coefficient
updates, no zipper, no oversampling needed if the path is linear time-varying.
**If a nonlinearity is included (the BJT stage is one), it needs oversampling and
an alias measurement** like every other nonlinear stage in the project.

## Steps

- [ ] Research the GCB-95: inductor value, the Fasel/halo difference, the sweep
      law, the buffer, the BJT stage. Write it into §58. Record what could not be
      sourced.
- [ ] Read docs §20 (phaser — the per-sample-coefficient LTV precedent), §24
      (`BjtStage`), §53 (BjtStage's 4-node path and its solver traps — read the
      damped-Newton notes before using it), the pedal-adding checklist implied by
      §21/§27 (a new pedal touches core, C ABI, worklet, web UI, native, assistant)
- [ ] Implement `WahModel`: resonant bandpass + sweep law + BJT stage; MODE
      (manual/auto), POSITION, SENSITIVITY (auto), and a LEVEL if the circuit
      warrants it. Params smoothed, `reset()` + park, ParamGuard, denormal policy
      per ADR 006 (state resting at zero → flush; **and read §56.4b: the house
      `flushDenormal` one-liner does NOT converge above first order** — use the
      whole-state form for any order-≥2 filter)
- [ ] Wire end-to-end: C ABI, worklet, web pedal face + UI, native `ClipperEngine`
      + `PedalCard`, assistant `add_pedal` + coaching. **Both fronts, this slice.**
- [ ] Tests `clipper_wah_tests`: measured sweep law vs the derived one, resonance
      peak height across the sweep, the bandpass shape vs its own analytic H(jω),
      **auto-mode envelope tracking as a player-observable property** (a pluck
      sweeps the peak up then back — measure it, don't assert a tautology),
      no-zipper on a fast POSITION sweep, alias floor if nonlinear, DC offset on
      signal, `reset()` clean
- [ ] Player-expectations rows for the new pedal; `--golden-report` all five
      UNCHANGED (a new pedal must not move an existing rig)
- [ ] WASM rebuild + artifacts; full core ctest; web build + Playwright; node suites
- [ ] Docs §58 + CLAUDE.md entry + plan bottom sections
- [ ] ONE commit on claude/wah-envelope-6f557i, feat: …, NO push, NO PR

## How this will be measured

The measured sweep law (POSITION → peak frequency) against the derived one; peak
height vs frequency; the auto-mode envelope response to a real pluck; zipper
absence on a fast sweep; alias floor; five goldens unchanged.

## Manual test steps

- [ ] Owner: add the wah, automate POSITION in Logic (or map an expression pedal)
      — it should talk, and the sweep should feel like a wah rather than a filter
      knob; into a driven amp it should be aggressive, not polite
- [ ] AUTO mode: hard picking opens it, soft picking doesn't — funk works
- [ ] Edge: fast POSITION sweep = no zipper/click; MODE switch mid-note declicked;
      reset clean; 44.1/96 k

## Out of scope

The standalone Mu-Tron III pedal (M13.6 — this slice builds the primitive and the
auto MODE, not a second pedal), the compressor slice (parallel — do not touch its
files), the Orange slice (parallel — do not touch `Orange*` or amp files).
