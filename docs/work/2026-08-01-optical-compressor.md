# M13.3 — the optical (LA-2A-style) compressor

**Date:** 2026-08-01
**Branch:** claude/optical-comp-6f557i
**Roadmap item:** M13.3 — "Optical compressor (LA-2A style)", the second half of the
owner's "the first two types". Named consumer of ADR 019 / 021 / 023.

## Goal

Ship the lineup's SECOND dynamics pedal — an electro-optical leveling amplifier in
pedal form (T4B-style EL panel + CdS photocell) — wired end to end on both front
ends in one slice, with its **program-dependent, two-stage release** measured and
asserted as the property that makes it measurably NOT the OTA voice with different
constants.

## Approach

**This is a new voice, not a tone change to anything shipped.** The Dyna Comp must
be bit-identical (render-hash proof) and all five goldens unchanged.

### The three governing ADRs, and what this slice expects to find

ADR 019 said M13.3 is "a config of `CompressorEngine` plus one `applyGainCell()`
case". ADR 021 corrected that to "reuse the DETECTOR; expect the CONTROL side to be
your own". ADR 023 added the procedural rule: **build the substitution and measure
it** — do not compare block diagrams, and do not widen a shared component to make a
third voice fit.

Two seam questions, both to be answered by measurement, both allowed to come out
"no" as long as the number is on the table:

1. **Is the optical voice a config of `CompressorEngine`?** Prior belief: no. The
   engine's eight config structs are the CA3080 pedal's own stages (`DriveNetwork`
   is the OTA's differential input attenuator, `LoadStage` its output compliance,
   `Splitter` the Dyna Comp's phase splitter, `ControlMap` the Iabc path). The
   house precedent set one slice ago is `GateModel`: a standalone model that HOLDS
   whatever component is genuinely shared. To be reported seam by seam.
2. **Is `SidechainDetector` the optical voice's detector?** To be decided by
   building the substitution and measuring, per ADR 023, on ADR 023's own metric
   (proportional range in dB) plus this pedal's own ratio curve.

### The circuit

LA-2A idiom. The T4B is an EL panel plus a CdS photocell; the photocell is the
BOTTOM leg of a divider in the audio path, and the EL panel is driven by the
(amplified) audio, so **the panel is the rectifier and the cell is the
integrator** — there is no envelope capacitor anywhere in the control path. That
is the structural claim this slice has to model rather than approximate.

```
in ─[Cin]──[Rs]──┬──► makeup amp (GAIN) ──[Cout]──► out
                 │                    │
             R_cell (T4B)             │  MODE: COMPRESS = 100 % output (feed-back)
                 │                    │        LIMIT    = output + 1/25 of the input
                gnd                   ▼
        ▲                     PEAK REDUCTION pot → sidechain amp → step-up
        │                                                   │  (≤ 90 V peak)
        └──── R_cell = f(light, HISTORY) ◄── EL panel ◄──────┘
```

Cell dynamics: excess conductance in two branches (fast + slow) with a slow
**trap-occupancy** state that lengthens the slow branch's release. That is what
makes the release program dependent, and it is the whole voice.

### Knobs — THREE, and each must be shown to change the audio

Slot 0 PEAK REDUCTION (sidechain gain), slot 1 **MODE** (compress / limit — a
discrete two-state switch, the CE-1 precedent), slot 2 GAIN (makeup). MODE is only
shipped if it measurably changes the audio; §61 did not ship the NS-2's MODE
because it changes only what the footswitch does. Here the sourced description says
the switch changes the sidechain SOURCE, which is audio-affecting — to be measured.

## Steps

- [ ] Research pass; tabulate SOURCED vs RECONSTRUCTED before writing any constant
- [ ] `OptoCell.h` — the photocell as a COMPONENT (ADR 021's lesson), with the
      Uni-Vibe (M13.5, photocell-driven) as its named future consumer
- [ ] `OptoModel.h/.cpp` — the pedal; standalone, `GateModel`'s shape
- [ ] The `SidechainDetector` substitution, built and measured, then kept or refused
- [ ] `clipper_opto_tests` with the acceptance bar + the honest-expectations set
- [ ] C ABI `opto_*`, render CLI, bench row, worklet dispatch
- [ ] Web: rig.ts, Pedal.tsx, tokens.css, assistant tools + prompt
- [ ] Native: `PEDAL_OPTO = 11`, APVTS, `PedalCard` keyed face, identical_core_test
- [ ] Perturbation-prove every bar
- [ ] Docs §64, ADR 025, ROADMAP M13.3, CLAUDE.md Current State; close the M13.3
      follow-up in `CompressorEngine.h` / ADR 019 / 021 / 023

## How this will be measured

**THE ACCEPTANCE BAR — program dependence, which the Dyna Comp cannot have.**
Release time measured after a SHORT burst and after a LONG sustained passage that
reach the SAME gain-reduction depth, on the same model:

* optical: `t_release(long) / t_release(short)` must exceed a stated ratio;
* Dyna Comp on the identical stimulus pair: the same ratio must measure ≈ 1.0.

Both directions asserted, margin recorded not snugged, perturbation-proven.

**Absolute references** (published, so the bars are not identities): attack 10 ms;
~60 ms to 50 % release (40–80 ms window); 0.5–5 s for complete release depending on
the amount of previous reduction; ~3:1 ratio. The ratio is also PREDICTED from two
sourced device exponents (CdS γ, EL brightness ∝ V^n) and checked against the
published figure — that is this slice's independent number.

Plus the house set: static ratio curve, alias floor on a SINGLE tone across 1/2/4/8×
(a two-tone stimulus measures nothing on a compressor — §59), DC on signal, latency,
CPU as % of one 48 kHz stream, block-size invariance, reset vs fresh, rate spread,
`maxAbsRestingState()`, knob zipper, the five goldens by `--golden-report`.

## Manual test steps

- [ ] Add the pedal on the web board; PEAK REDUCTION and GAIN move the sound
- [ ] MODE flips between compress and limit and the difference is audible on picks
- [ ] Play a long chord then a short stab — the recovery is audibly longer after
      the long one (the property the whole slice exists for)
- [ ] Edge case: pedal at max PEAK REDUCTION into a clean amp — no click, no
      runaway, no denormal tail; reset/park behaves after a NaN
- [ ] Native: the pedal appears in the gear tray, its card draws its own face, its
      three parameters automate, a saved session round-trips

## Out of scope for this session

Amp work of any kind (a parallel slice owns §63/ADR 024). Tube stage modelling for
the LA-2A's 12AX7/12BH7A/6AQ5 — the amplifier stages ship as linear gains plus a
documented headroom limit. Re-tapering or re-voicing the Dyna Comp. Blessing any
golden. The stereo/meter photocell of the real T4B (we model the GR cell only).

---

<!-- Fill in below during/after the session -->

## What actually happened

**The two seam questions the plan named both came out "no", and both were
measured rather than argued** — see docs §64.3 and **ADR 025**.

1. **Not a config of `CompressorEngine`.** Of its eight config structs exactly one
   (`OutputStage`) applies to an optical leveling amplifier; `applyGainCell()`
   returns a CURRENT where an optical cell is a resistor in a divider. `OptoModel`
   is standalone, holding `OptoCell` — the shape `GateModel` already took.
2. **Not a consumer of `SidechainDetector`.** The substitution was built (a replica
   of the shipped loop validated against it to 0.05 dB, one block switchable) and
   run per ADR 023's procedure. Proportional range 2.031 dB against the
   panel-and-cell's 14.323; the ratio curve goes non-monotone to 10.40:1 with
   0.09 dB of reduction at −30 dBV; and the acceptance property goes to 1.000×.
   **This pedal has no envelope capacitor at all** — the EL panel is the rectifier
   and the photocell is the integrator — so there is no component for it to be.
   `SidechainDetector` was NOT widened.

**Three things found by measuring rather than planning:**

* **The trap-occupancy target has to SATURATE.** Written as a linear
  `ΔG/ΔG_max` — the first draft — the occupancy sits at ~1e-3 at any playable
  level and the program dependence measures **1.083×**, i.e. none. The Langmuir
  form `ΔG/(ΔG + ΔG_half)` is what trap kinetics give, needs one constant instead
  of an arbitrary normalization, and produces both the DEPTH and the DURATION
  dependence the reference is described by.
* **It has to be oversampled, and the gate's arrangement was checked first.**
  The divider is a multiply by an audio-rate signal, so the alias floor MOVES
  with the factor: −86.16 (1×) → −113.09 dB (4×). Shipped 4×, latency 72.
* **`prepare()` must not clobber a pre-prepare `setParameter`.** The first version
  set the knob defaults inside `prepare()` (copying `CompressorEngine`), which
  made `identical_core_test`'s reference chain — which writes parameters BEFORE
  prepare — disagree with the plugin by 2.4e-03. Defaults moved to the
  constructor; `OnePoleSmootherT::prepare` snaps value to TARGET, so they survive.
  The same slice adds the opto parameters to the test's plugin driver, which is
  what makes MODE (slot 1) provably plumbed end to end.

**One weak bar named rather than hidden:** `testRatioCurve`'s
measured-vs-`predictedDeepRatio()` comparison is computed from the same two
constants, so P3 (elExponent → 1.0) leaves it agreeing to 1 %. The bars that catch
P3 are the PUBLISHED bracket and the knee shape. Documented in §64.9.

## Measured results

**THE ACCEPTANCE BAR — program dependence, same stimulus on both compressors:**

| model | 150 ms burst | 6 s burst | ratio |
| --- | --- | --- | --- |
| Lumen (PEAK 0.70) | 12.17 dB GR, releases in **1.00 s** | 12.24 dB GR, releases in **2.88 s** | **2.892×** |
| Squash (SUSTAIN 0.80) | 18.93 dB, 4.11 s | 18.95 dB, 4.11 s | **1.000×** |

Bar `> 2.0` / `< 1.25`; margin **recorded, not snugged** (the cell's release
constants were pinned to the published 0.5–5 s window, not to this ratio). Depth
axis 1.04 → 3.67 s = **3.53×**. Trap occupancy 0.0686 vs 0.6163.

Ratio **2.81:1** measured against **2.875:1** predicted from two published device
exponents (published bracket 2.16–3.76, published figure ~3:1). Attack **9–10 ms
across the whole knob, spread 1.0 ms** (the OTA voice's moves 14 → 3 ms). 50 %
release **47 ms** (published 60, window 40–80). Ceiling **40.09 dB analytic,
39.64 dB rendered**. PEAK REDUCTION moves the threshold **45.44 dB**; GAIN moves
the output **32.00 dB** and the reduction **0.0000 dB**. MODE authority
**7.71 dB**. Transient **+0.93…+1.22 dB** re settled (it survives; the OTA voice
loses 7.72–20.00). Noise gain **+4.69 dB with 0.00 dB of travel** (the OTA voice's
moves 25.34). Tone within **0.12 dB** from 82 Hz up. Alias floor 1× −86.16 / 2×
−103.19 / **4× −113.09** / 8× −114.07 dB. DC on signal **0.0000 %**; block-size
invariance and `reset()` both **0.000e+00**; rate spread **0.0032 dB**; zipper
**1.00×**; `maxAbsRestingState()` exactly **0.0** (at 137 s — measured); CPU
**2.97 % of one 48 kHz stream**. Defaults measure unity (**−0.04 dB**).

**Suites:** core ctest **33/33** (32 → 33 entries), native **3/3** with the new
`Lumen → RAT → Twin` board at **0.000e+00**, Playwright **80 passed** (the new
`opto worklet` spec measures PEAK 19.99 / MODE 2.89 / GAIN 15.20 dB), node
15/10/12, electron 20, full Standalone+VST3+editor build clean. **All five goldens
UNCHANGED at ±0.00, nothing blessed.** Dyna Comp and gate **byte-identical**.
**8/8 perturbations RED, every restore GREEN.**

## Files created / modified

`core/include/clipper/dsp/OptoCell.h` (new), `core/include/clipper/dsp/OptoModel.h`
+ `core/src/dsp/OptoModel.cpp` (new), `core/tests/test_opto_model.cpp` (new),
`core/CMakeLists.txt`, `core/src/clipper_c_api.cpp`, `core/tools/render/main.cpp`,
`core/tools/bench/main.cpp`, `core/include/clipper/dsp/CompressorEngine.h` +
`SidechainDetector.h` (banner corrections), `scripts/build-wasm.sh`,
`web/worklet/clipper-processor.js`, `web/src/rig.ts`, `web/src/components/Pedal.tsx`,
`web/src/components/Board.tsx`, `web/src/styles/tokens.css` + `pedal.css`,
`web/src/assistant/tools.ts` + `prompt.ts`, `web/tests/audio.spec.ts`,
`native/src/ClipperEngine.{h,cpp}`, `PluginProcessor.{h,cpp}`, `PedalCard.cpp`,
`ClipperLookAndFeel.{h,cpp}`, `native/tests/identical_core_test.cpp`,
`web/public/generated/*` (rebuilt), `docs/DEVELOPMENT.md` §64,
`docs/decisions/025-the-optical-voice-is-not-a-config.md`, `ROADMAP.md`, `CLAUDE.md`.

## Deferred to next session

* **The biggest gap is research, not code: no schematic and no independent
  simulation.** §59's strongest asset was an outside SPICE run agreeing to 4 %,
  which caught that slice's one real bug. Nothing equivalent was reachable here.
  Every dynamic curve in §64.4 is this model's own.
* **The manual's ">20:1 above −20 dB" is not reproduced** (§64.5) and no choice
  inside the published exponent ranges reaches it. If a schematic turns up and the
  sidechain has a level-dependent gain stage, that is where it goes.
* **The tube stages are a linear gain plus one soft ceiling** — the transformer's
  low-end behaviour and the tube's asymmetry are not modelled.
* **`OptoCell`'s named future consumer is M13.5's Uni-Vibe.** Do not grow it a
  `lightSourceKind` axis; extract the lamp.
* The web face uses the `compact` CSS layout while the native card uses `Stack`
  with a `Pad` footswitch — native has four layouts and `compact` is a web CSS
  variant. A native `Compact` anatomy is a UI slice if the divergence matters.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
