# Noise gate — the second consumer of the compressor's detector (M13.6a)

**Date:** 2026-08-01
**Branch:** claude/noise-gate-6f557i
**Roadmap item:** M13.6a. Named in §59 and ADR 019 as a consumer of M13.2's
`CompressorEngine` detector. Owner 2026-07-31 chose it for the parallel round.

## Goal

A Boss NS-2 / ISP-style **noise gate** ships as pedal type `gate`, web and native,
so a high-gain rig is playable — silence between phrases, and no gain-pumping
artefacts on the note itself. This is also the pedal that makes M10.4's Mesa
usable when that lands.

## Slot (RESERVED — do not choose your own)

`PEDAL_GATE = 9` · `skin::AccentId::Gate` · `--accent-gate` (slate, deliberately
muted — it is a utility, not a voice) · web type string **`'gate'`**. Allocated in
`chore/reserve-pedal-slots`; `PEDAL_TYPE_COUNT` is already 11. **Do not renumber
anything or touch the delay's (8) or chorus's (10) slots** — parallel slices own
those.

## Approach

**REUSE THE DETECTOR — this is the whole point of the slice.** ADR 019 says in
terms that the compressor engine was written config-parameterized from its first
line with the gate as a named consumer, and that the seam is documented in
`CompressorEngine.h`. **Read that banner first.** A gate is the same envelope
detector feeding a *different* `ControlMap` — the gain decision is inverted.

**ADR 019 also names the risk, and this slice is the test of it:** a seam written
before its second consumer exists can be the *wrong* seam. If the gate genuinely
needs something the header says is "reused as-is", **that is a finding to REPORT
and the header to CORRECT — not a reason to quietly widen the engine until both
fit.** An engine that grows a parameter every time a voice disagrees has stopped
being a shared model and become a union of two, which is worse than either. Say
plainly in your report which of the two happened.

What a gate needs that a compressor does not:

1. **A THRESHOLD knob that really is a threshold** — unlike the Dyna Comp's
   SUSTAIN, which §59 measured as moving 25.33 dB of gain against 0.28 dB of
   output. State the contrast; it is a genuinely different control law.
2. **Hysteresis.** A single threshold chatters on a decaying note. Real gates open
   at one level and close at a lower one. Model it and measure the gap.
3. **Attack / hold / release.** Too fast an attack chops the pick transient; too
   slow and the noise is audible before it closes. Whether these are knobs or
   fixed is a research question — find out what the reference pedal does rather
   than assuming.

Research first, into docs §61, with sources and gaps. `github.com` clones work;
most audio sites 403 from this proxy.

**Knobs:** three slots. THRESHOLD plus two more chosen by what the reference
actually exposes (likely DECAY/RELEASE and a mode or level). Do not ship a knob
the circuit does not have.

## Steps

- [ ] Read `CompressorEngine.h`'s seam banner, docs §59, ADR 019, then research
      → docs §61 BEFORE code
- [ ] Implement the gate as a `ControlMap` on the existing detector — or report
      why that was not possible
- [ ] C ABI `gate_*`; `scripts/build-wasm.sh` (new TU + exports, INSIDE the
      `STAMP:EMCC-ARGS` markers); worklet dispatch
- [ ] `web/src/rig.ts` (add `'gate'` to `PedalType` **and**
      `AVAILABLE_PEDAL_TYPES`), `Pedal.tsx` face, `pedal.css`, assistant coaching
      (a gate goes AFTER the dirt, or in a loop — that is real advice)
- [ ] Native `ClipperEngine` (fill `PEDAL_GATE`), APVTS params, `PedalCard` face
- [ ] `core/tests/test_gate_model.cpp` + `clipper_add_test_flags()`
- [ ] NaN-guard + denormal + param-smoothing suites gain the gate
- [ ] Rebuild + commit the three WASM artifacts
- [ ] Docs §61, CLAUDE.md entry, this file's bottom sections

## How this will be measured

- **Threshold accuracy**: input level at which the gate opens, measured, vs the
  knob's stated dB. A table across the travel.
- **Hysteresis gap**: open level minus close level, in dB. Must be > 0, and stated.
- **Chatter test — the one that matters**: feed a *decaying* note that crosses the
  threshold and count gate transitions. A gate with no hysteresis will oscillate;
  this must show **one** close, not many.
- **Attack transient preserved**: pick attack through an open gate vs bypassed —
  the leading edge must not be chopped. Report the dB and ms lost.
- **Noise reduction, in dB**, on a realistic noise floor with the gate closed.
- **THRESHOLD is a real threshold**: show the control law contrasted against §59's
  SUSTAIN measurement (25.33 dB gain travel vs 0.28 dB output).
- DC on signal, `reset()` clean, no zipper, block-size invariance.
- `--golden-report`: **all five UNCHANGED**.

## Manual test steps

- [ ] High-gain rig (Muff or RAT → JCM), gate after the dirt — hiss disappears
      between phrases and the note still starts cleanly
- [ ] Palm mutes stay tight, no stuttering on the decay
- [ ] THRESHOLD too high → notes get chopped (correct, and the knob should make
      that obvious)
- [ ] Edge: silence in → stays closed, no hunting; reset clean; 44.1/96 k

## Out of scope

The optical compressor (M13.3), unifying the wah's envelope follower with the
compressor's (still a named follow-up), the delay and CE-1 slices (parallel — do
not touch their files or slots), any golden bless.
