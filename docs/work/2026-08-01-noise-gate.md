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

---

<!-- Filled in during/after the session -->

## What actually happened

**The seam was tested, and the verdict is: the claim held, the seam as written
did not, and it was CORRECTED rather than widened.** Full finding in docs §61.2
and in the corrected `CompressorEngine.h` banner. In one line: `Sidechain` was a
config struct with no code attached — the detector was two PRIVATE methods of
`CompressorEngine` with their state in that class's members, so there was nothing
a second consumer could hold. It is now the standalone `SidechainDetector`, owned
by value by both pedals, and the move is **bit-identical** for the compressor
(two `clipper-render` renders `cmp` byte-for-byte). `ControlMap` was **not**
widened: it is three resistor values that make an OTA bias current, a gate has no
such thing, so `GateModel` carries its own `GateControl`. And
`detectorFromOutput` stays OTA-only, because for a gate feed-back is not a
variant — it is a latch (measured, §61.7).

**Two knobs, not three, and that is a research finding.** The reference's third
control (MODE) does not change the audio at all — per Roland's own support
article it changes what the FOOTSWITCH does. This board's footswitch is bypass on
every pedal, so shipping MODE would have been either dead UI or a footswitch that
means something different on one pedal. Not shipped, documented (§61.3).

**The gate is NOT oversampled, and that was decided by measurement mid-slice.**
The first draft ran the whole thing at 4×, as every nonlinear pedal here does.
The alias measurement then said the floor was already −176.93 dB at 1× and got
slightly WORSE with oversampling, because the gate's signal path is a multiply.
72 samples of latency and ~2× CPU deleted; `setOversampling` is the phaser's
no-op, asserted bit-identical at 1/2/4/8× (§61.7).

**Two pre-existing native bugs found on the way in** (§61.10): the wah's and
compressor's face-table entries were in the wrong ORDER (the editor drew each
other's face), and the slot-reservation commit had widened `PEDAL_TYPE_COUNT` to
11 while the table still had 8 entries, so the gear tray was already offering
three types whose faces were value-initialized NULL strings. Fixed structurally —
the face now carries its own `type` and the lookup is keyed, so neither can recur
— without touching the delay's or the chorus's slots.

## Measured results

* THRESHOLD law: **40.00 dB** of travel (−59.05 → −19.05 dBV), monotone, worst
  |measured − stated| **0.01 dB**; at the 0.35 default a −60 dBV hiss floor stays
  shut and a 0.15 V note opens it. Rate spread 44.1–96 kHz **0.02 dB**.
* **Control-law contrast: 40.00 dB of THRESHOLD travel against 0.0000 dB of
  GAIN travel**, versus §59.4's SUSTAIN at 25.33 dB of gain against 0.28 dB of
  output. Gain when open −0.0176 dB at every knob position.
* Hysteresis gap **6.09 dB** (0.21 dB with the feedback removed).
* **Chatter: 1 open / 1 close** at THRESHOLD 0.35/0.50/0.65 on a decaying low E;
  **5/6/7** with the hysteresis removed.
* Pick attack: **0.01 dB** of 20 ms peak lost, **0.02 dB** of 20 ms rms,
  **0.00 ms** late (the compressor loses 7.72–20.00 dB of the same transient).
* Noise reduction **80.01 dB**, which is the modelled VCA off-isolation.
* DECAY −20 dB point 97.8 / 197.4 / 512.4 / 1508.5 / 4658.6 ms across the knob;
  fixed attack 0.40 ms, fixed hold 30.0 ms.
* Feed-forward vs feed-back: **−0.02 dB vs −80.02 dB**, and the feed-back build
  never opens.
* Not oversampled: floor **−176.93 dB** at 1×, bit-identical at 2/4/8×,
  **latency 0**.
* DC on signal 0.0000 % both stimuli; ragged-block invariance exactly 0.0;
  `reset()` vs fresh 0.000e+00, NaN 246/256 → 0/48000; zipper 1.00×;
  `maxAbsRestingState()` exactly 0.0 with the envelope resting at 9.0000 V and
  the VCA gain at 1.0000e−04.
* **6/6 perturbations RED, restore GREEN** (§61.9) — including P6, whose first
  version could not fail (it read `control().holdSeconds` back) and was replaced
  by a measured bar: unity survives **35.8 ms** after the comparator closes,
  **5.5 ms** with the hold removed.
* **All five goldens UNCHANGED at ±0.00 — nothing blessed, nothing written.**

## Files created / modified

New: `core/include/clipper/dsp/SidechainDetector.h`,
`core/include/clipper/dsp/GateModel.h`, `core/src/dsp/GateModel.cpp`,
`core/tests/test_gate_model.cpp`.
Modified: `CompressorEngine.h`/`.cpp` (the extraction + the corrected seam
banner), `clipper_c_api.cpp`, `core/CMakeLists.txt`, `test_denormal.cpp`,
`test_nan_guard.cpp`, `tools/render`, `tools/bench`, `scripts/build-wasm.sh`,
the three WASM artifacts, `web/worklet/clipper-processor.js`, `rig.ts`,
`Pedal.tsx`, `Board.tsx`, `pedal.css`, `assistant/prompt.ts`, `assistant/tools.ts`,
`web/tests/audio.spec.ts`, native `ClipperEngine.*`, `PluginProcessor.*`,
`PedalCard.*`, `PluginEditor.cpp`, `identical_core_test.cpp`,
`docs/DEVELOPMENT.md` §61, `CLAUDE.md`.

**ADR number needed** for "the shared detector is a component, not a config
struct — ADR 019's seam corrected by its second consumer". Numbers are assigned
centrally; ADR 019's own text is amended in this slice with a pointer to §61.2.

## Deferred to next session

* **Find the NS-2 schematic.** Every component value in `GateModel.cpp` is a
  reconstruction; §61.1 lists exactly what is sourced and what is not.
* **Unify the three envelope followers.** M13.1's detector is now shared with
  M13.6a, but the wah's (§58) is still its own. That follow-up is now smaller,
  because there is a `SidechainDetector` for it to move to.
* **A gate before AND after the dirt** — `kMaxChain` is one-instance-per-type
  natively, and a gate in two places is a normal request.
* **M13.3's slice should re-read the corrected seam banner** before assuming
  anything above the detector is reusable.

## Status

- [x] Complete
