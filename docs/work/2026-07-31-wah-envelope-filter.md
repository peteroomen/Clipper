# Wah / envelope filter — the first FILTER pedal

**Date:** 2026-07-31
**Branch:** claude/wah-envelope-6f557i
**Roadmap item:** post-v1.1 new-gear slice — the lineup's first FILTER pedal (owner-chosen option: one resonant primitive serving BOTH a GCB-95-style treadle wah and Mu-Tron-style envelope-filter territory)

## Goal

Ship pedal type `wah`: a Dunlop GCB-95-style Cry Baby whose POSITION is a normal
automatable parameter, plus an envelope-driven AUTO sweep over the SAME filter,
wired end to end on BOTH front ends (core → C ABI → worklet → web UI → native
engine + editor → assistant), with the sweep law and the resonance height
DERIVED and MEASURED rather than tasted.

## Approach

**Deliberate tone addition** (a new pedal — no existing rig may move; all five
goldens must read UNCHANGED).

Research first, into docs §58, before a line of code: inductor value, the
Fasel/halo difference, the buffer, the BJT stage, and above all the sweep law.

The two things that must be derived, not tasted:

1. **The sweep law.** Mechanism (ElectroSmash names it verbatim): the pot tunes
   the *apparent capacitance* of the tank cap, so
   `f0(p) = f_LC / sqrt(1 + A·u(p))` with `f_LC = 1/(2π√(LC))` fixed by the
   published `L = 500 mH`, `C = 0.01 µF` and `u(p)` the pot taper. Heel is
   pinned to the published 450 Hz; the ONLY fitted parameter is the taper
   exponent, and its value must be checked against the independently documented
   audio-taper spec (10–20 % of full resistance at half rotation). Validated
   against the CCRMA/Julius-Smith digitised CryBaby (Faust `vaeffects.lib`
   `crybaby`), which was fitted to three MEASURED GCB-95 responses.
2. **Q / resonance height across the sweep.** Derived from the same mechanism:
   fixed damping resistance + fixed L + variable C ⟹ `BW ∝ f0²`, `Q ∝ 1/f0`.
   Checked against CCRMA's measured Q law. Peak height is constant across the
   sweep in this topology — assert that, don't assume it.

Structure: input buffer (unity) → derived-law resonant bandpass (TPT
state-variable — its two integrators ARE the inductor current and the cap
voltage, and it is unconditionally stable under per-sample coefficient
modulation) → the tank divider → **`BjtStage`** as the real common-emitter
output stage (MPSA18-class, collector-feedback bias with the base-to-ground leg),
inside a 4× oversampled domain. Coefficients recomputed PER SAMPLE from the
smoothed position/envelope — the phaser's precedent (docs §22).

`flushDenormal` is used in its WHOLE-STATE form (docs §56.4b: the one-liner does
not converge above first order).

Envelope follower for AUTO is written locally — a compressor slice is running in
parallel and is also building one; unification is a named follow-up, NOT a shared
edit across in-flight branches.

## Steps

- [ ] Research → docs §58 (sources + gaps) BEFORE code
- [ ] Derive the sweep law + Q law numerically; table them vs the CCRMA reference
- [ ] `core/include/clipper/dsp/WahModel.h` + `core/src/dsp/WahModel.cpp`
- [ ] C ABI `wah_*` (create/destroy/set_param/reset/set_oversampling/latency/process)
- [ ] `scripts/build-wasm.sh`: compile the new TU + export the new symbols
- [ ] `web/worklet/clipper-processor.js` dispatch
- [ ] `web/src/rig.ts`, `Pedal.tsx`, `pedal.css`, `tokens.css`
- [ ] `web/src/assistant/tools.ts` + `prompt.ts` (add_pedal + coaching)
- [ ] `native/src/ClipperEngine.{h,cpp}`, `PedalCard.cpp`, `PluginProcessor/Editor`, `ClipperLookAndFeel`
- [ ] `core/tests/test_wah_model.cpp` + `clipper_add_test_flags()` registration
- [ ] Rebuild + commit the three WASM artifacts
- [ ] Docs §58, CLAUDE.md Current State, this file's bottom sections

## How this will be measured

- **Sweep law:** rendered peak frequency at 9 POSITION points vs the derived law
  vs the CCRMA measured fit — % error table.
- **Resonance:** rendered peak height (dB re the pedal's own out-of-band
  reference) and rendered −3 dB bandwidth at each point → measured Q vs derived Q.
- **Bandpass shape:** rendered magnitude vs the model's OWN analytic `H(jω)`
  (validates the discretisation only — stated as such, per "Measure, don't assert").
- **AUTO tracking (player-observable):** a real plucked note, envelope on —
  measure the instantaneous peak frequency over time; it must rise after the
  pluck and fall back as the note decays, by a stated number of octaves, with a
  stated attack/release. Not a tautology on the follower's own output.
- **No zipper:** a fast POSITION sweep on a steady tone → far-field spectral floor.
- **Alias floor** at 1×/2×/4×/8× (the stage is nonlinear).
- **DC offset ON SIGNAL** (`support/DcOffset.h`).
- **`reset()`** returns to exact digital silence; NaN in → finite out.
- **Goldens:** `--golden-report`, all five UNCHANGED. No `--update-goldens`, ever.
- **Perturbation proof** on the key bars (`touch` after BOTH patch and restore).

## Manual test steps

- [ ] Web: add a Wah, sweep POSITION 0→1 on a chord — the vowel moves smoothly, no zipper
- [ ] Web: SENSE up, POSITION low, pluck hard — the filter opens and closes with the note
- [ ] Web: VOICE min→max — the peak gets broader/narrower, the centre does not move
- [ ] Native: same pedal, same order, same latency reading
- [ ] Edge: SENSE = 0 is exactly the manual pedal (envelope contributes nothing)
- [ ] Edge: NaN into a param is rejected at the ABI; `reset()` recovers from a poisoned engine
- [ ] Edge: POSITION slammed 0↔1 every block — no click, no blow-up

## Out of scope for this session

- Native tuner-style dedicated wah UI beyond the standard `PedalCard`
- An expression-pedal / MIDI input path (POSITION is already automatable)
- Unifying the envelope follower with the parallel compressor slice's (named follow-up)
- Any Orange-amp or compressor file (parallel slices)
- Re-blessing any golden

---

<!-- Fill in below during/after the session -->

## What actually happened

Research first: **every primary reference was 403 from this session's proxy** —
electrosmash.com and its archive mirror, geofex.com, dafx.de, ccrma.stanford.edu,
guitarscience.net, web.archive.org, en.wikipedia.org, grokipedia.com, blogspot,
github.io. What worked was web-search summaries (which quote those pages
verbatim) plus `github.com` clones. The single most valuable artefact came from
the latter: `grame-cncm/faustlibraries` -> `vaeffects.lib` -> `crybaby`, the
**CCRMA / Julius Smith digitised CryBaby fitted to three MEASURED GCB-95
responses**. That gave an independent reference curve nothing in this repo
produced, which is what the sweep-law and Q bars are measured against.

The derivation came out better than the plan hoped. `f_LC = 1/(2*pi*sqrt(L*C))`
from the published `L = 500 mH` and `C = 0.01 uF` is **2250.79 Hz**, and the
CCRMA MEASURED toe is **2216.06 Hz** — 1.57 % apart, from two completely
independent routes. That confirmed the apparent-capacitance mechanism, so the
whole law reduced to one free parameter (the pot taper), and its fitted value
puts the wiper at **17.09 % at half rotation**, inside the published audio-taper
window. The one fitted number came out being a pot taper.

Q was decided by the measurement rather than by intuition: the parallel-RLC
topology predicts `BW ~ f0^2` (exponent exactly 2.000) and the reference measures
1.870, while the series-LCR alternative predicts constant absolute bandwidth and
is refuted outright. So the resonance is SHARPEST at the heel, which is
counter-intuitive and correct.

Three things had to be corrected mid-slice, all found by measuring:

1. A **9.6 dB peak-height tilt** across the sweep, traced to `BjtStage`'s `Cin` at
   10 nF putting a ~1.1 kHz coupling corner in the MIDDLE of the sweep. 1 uF fixed
   it; spread is now 0.010 dB.
2. This slice's **own hypothesis about the fast-sweep noise floor was refuted by
   its own measurement** — per-block, per-8-sample and per-sample control all
   floor identically, so it is the discrete resonator, not the control staircase.
   The 2-pole smoother was kept for a different, measured reason.
3. The **perturbation run found two bars that could not fail** (a peak-height
   assert written against the model's own accessor, and nothing pinning the 4x
   default). Both fixed, both now red under perturbation.

## Measured results

Full tables in docs §58.6. Headlines:

- **Sweep law** rendered vs the independent CCRMA measurement: worst **1.48 %**,
  rms **0.59 %**, over 2.322 octaves (450.0 -> 2250.8 Hz). Against the model's own
  law (the discretisation bar): worst **0.17 %**.
- **Shape**: halves **1.147 / 1.176** octaves vs a linear-taper counterfactual's
  **0.472 / 1.851** — the "all the wah in the last inch" failure avoided, and
  measured.
- **Resonance**: peak boost **17.90-17.91 dB** (published 18.0), **spread
  0.010 dB across the whole travel**. Q rendered vs derived worst **2.6 %**, vs
  the independent measurement worst **9.8 %**.
- **VOICE**: Q **2.253 -> 5.292** with the centre at 996.40 Hz and the height at
  17.91 dB on all three settings — width only.
- **AUTO**: a real pluck sweeps the peak **0.381 / 0.762 / 1.146 / 1.534
  octaves** at SENSE 0.25/0.5/0.75/1.0, peaking 82.7 ms after the pick and back
  within 10 % in ~840 ms. **SENSE = 0 measures exactly 0.000 octaves and is
  bit-identical (0.000e+00)** to a model that never had the feature.
- **Alias floor**: 1x **-73.0** -> 4x **-118.8** -> 8x **-156.0** dB
  (45.8 dB of improvement with the factor). Latency 72 samples.
- **No zipper**: static **-322.8 dB**, per-block slam **-104.3 dB**, fast sweep a
  skirt that DECAYS -68.8 -> -81.3 dB across 3-20 kHz.
- **DC on signal**: worst **0.054 %** of peak (1 % bar).
- **Level**: a plucked low E measures **-6.6 dB RMS** at every input level —
  a bandpass is a cut on real material, not a level bomb.
- **CPU**: **14.25x realtime / 7.0 %** of one 48 k stream.
- **GOLDENS: all five UNCHANGED at +/-0.00.** Nothing blessed, nothing written.
- **Core ctest 26/26** (25 -> 26 entries). Native 3/3. Web 69 passed. Node 15/10/12,
  electron 20.

## Files created / modified

- `core/include/clipper/dsp/WahModel.h`, `core/src/dsp/WahModel.cpp` (new)
- `core/src/clipper_c_api.cpp` (`wah_*` exports), `core/CMakeLists.txt`
- `core/tests/test_wah_model.cpp` (new), registered with `clipper_add_test_flags()`
- `scripts/build-wasm.sh` (new TU + the six exports + `_wah_reset`)
- `web/worklet/clipper-processor.js`, `web/src/rig.ts`,
  `web/src/components/Pedal.tsx` (new `rocker` face), `web/src/components/Board.tsx`,
  `web/src/styles/pedal.css`, `web/src/styles/tokens.css`
- `web/src/assistant/tools.ts`, `web/src/assistant/prompt.ts`
- `web/tests/audio.spec.ts` (new `wah worklet` spec)
- `native/src/ClipperEngine.{h,cpp}`, `native/src/PluginProcessor.{h,cpp}`,
  `native/src/PedalCard.cpp`, `native/src/ClipperLookAndFeel.{h,cpp}`
- `web/public/generated/{clipper.js,clipper-processor.js,.build-stamp.json}`
- `docs/DEVELOPMENT.md` §58, `docs/decisions/NNN-wah-filter-and-gain-stage-split.md`,
  this file, `CLAUDE.md`

## Deferred to next session

1. **The full GCB-95 netlist.** With it, `kTaperBeta` should be DERIVABLE from the
   divider rather than fitted to published frequencies — and checkable against
   23.537247.
2. **The Q exponent gap** (-1.000 derived vs -0.870 measured, +-11 % at the ends).
   Reported, not fitted. Closing it means finding what else in the loading tracks
   the pot.
3. **The coupled filter/gain solve** (the ADR's named cost): the tank does not see
   the clipped output, so the resonance does not damp under a slammed stage.
4. **Inductor core saturation** — the measured Fasel/halo difference is mostly
   core behaviour, not inductance.
5. **Unify the two envelope followers** (this one and the parallel compressor
   slice's) into one primitive, once both have landed.
6. **Duplicate instances**: `kMaxChain` is one-per-type natively, and a wah before
   AND after the dirt is the first genuinely normal request for two of a kind.
7. **`.github/workflows/ci.yml`'s native job filter** does not match
   `clipper_cab_state` (pre-existing, noted by the cab-picker slice) — and it does
   not need changing for this slice, which adds no native test target.

## Status

- [x] Complete
