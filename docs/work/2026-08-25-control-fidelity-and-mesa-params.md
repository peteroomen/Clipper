# Mesa live params, the amp selector, and appropriate controls

**Date:** 2026-08-25
**Branch:** claude/dual-rectifier-knobs-unresponsive-fxwjw2
**Roadmap item:** Owner field report 2026-08-25 (four reports in one pass)

## Goal

The Dual Rectifier's knobs move its sound; every amp is selectable in both front
ends; and a control that selects one of N discrete states is drawn as a switch or
a labelled selector rather than a 0-100 knob.

## The reports, and what each one measured as

1. *"my dual rectifier's knobs don't do anything, it only has one voice, unchanging"*
2. *"the cellar pedal knob is a pain, it's a drop pedal give me semitones not a 0-100 knob"*
3. *"the pitch jumped back up at 100 ... I'd expect 1 octave down"*
4. *"it's too laggy too"*
5. *"the controls aren't appropriate on the mesa either, mode is a 0-100 when I think a
   switch is what's needed. I saw some I didn't recognize"* + *"add new controls/components
   for all those things that don't benefit from a knob/pot"*
6. *"the amp selector is dogfooded, missing amps, don't line up"*

### Diagnosis (measured before any edit)

**The core is not at fault, and that was checked first.** Driving the C ABI exactly
as the worklet does (`amp_create` -> `amp_set_model(6)` -> `amp_set_param`), every
Mesa control moves the audio:

```
BASS      0->1  +8.68 dB     GAIN      0->1  +47.28 dB
MID       0->1  -0.27 dB     MASTER    0->1 +115.14 dB
TREBLE    0->1  -2.30 dB     RECTIFIER 0->1   -3.12 dB
PRESENCE  0->1  +0.00 dB     POWERMODE 0->1   -2.24 dB
MODE detents 0..4 rms: 0.1587 / 0.2001 / 0.2575 / 0.0728 / 0.1474
```

PRESENCE reading 0.00 dB is CORRECT, not a defect: the probe sits at MODE 4
(RED MODERN), where sheet `mbdr7` opens the power amp's feedback loop (docs §69).

So all four functional defects are in the FRONT ENDS, and they are the same shape
as §71 — the engine boundary was wired and the layer above it was not.

**(A) Native: the Mesa is parameterized once and never again.**
`PluginProcessor::processBlock` calls `engine_.updateParams(p)` every block — a
SEPARATE "apply only the changed ones" path from `applyParamsToModels()`, which
§71 fixed. `ClipperEngine::updateParams()` has **no Mesa branch of any kind**:
`jcmGain`/`jcmMaster` reach `jcm_` and `rockerverb_`, `jcmPresence` reaches
`jcm_`/`ac30_`/`orange_`, bass/middle/treble/reverb reach every voice EXCEPT
`mesa_`, and `mesaMode`/`mesaRectifier`/`mesaPowerMode` are never compared or
applied at all. The Mesa therefore gets its parameters exactly once, at
`prepareToPlay`, and every knob move afterwards is dropped. That is report 1
verbatim: the knobs do nothing and the voice never changes.

**(B) Web: a saved Mesa rig starts on the Clean 120.** `startEngine()`'s amp-model
chain (`web/src/audio.ts`) ends at `rockerverb` — there is no `mesa` branch — so
the engine stays on voice 0 while the UI draws a Dual Rectifier. It also never
sends `AMP_PARAM_MESA_MODE/RECTIFIER/POWERMODE`, so the three switches ignore
saved state. Live swap works (`setAmpType` -> `setAmpModel`); only the start path
is broken.

**(C) Native amp selector: five items against a seven-choice parameter.**
`PluginEditor.cpp:229` builds `ampVoiceBox_` from a hardcoded
`{"Clean 120","Eight Hundred","Twin Sixty-Five","Thirty","Overdrive"}` — Rocker
Verb and Dual Rectifier are absent, and the labels differ from the parameter's own
`kAmpModelChoices`. The `ComboBoxAttachment` maps item index to choice index, so a
5-item box against a 7-choice param is report 6 exactly: missing amps that do not
line up. §71 added `AMP_MODEL_COUNT` and a `jassert` for `kAmpModelChoices`; this
second list was never brought under it.

**(D) Web amp menu: `AMP_TYPE_LABEL` (Board.tsx) has six entries, no `mesa`,** so
the Mesa's menu item and the amp-slot button render blank.

**(E) Discrete parameters drawn as continuous knobs — both front ends.** The core
quantizes each of these; the UI shows a 0-100 pot. The source comments already say
"genuinely DISCRETE" / "a 9-position ROTARY, not a continuous control" — the widget
just never followed.

| Control            | States | Labels                                   | Today |
| ------------------ | ------ | ---------------------------------------- | ----- |
| Cellar AMOUNT      | 9      | -1..-7, OCT, OCT+DRY                     | knob  |
| Mesa MODE          | 5      | Clean/Vintage/Modern/Red Vint/Red Mod    | knob  |
| Mesa RECTIFIER     | 2      | Silicon / 5U4                            | knob  |
| Mesa POWER         | 2      | Bold / Spongy                            | knob  |
| Orange F.A.C.      | 6      | six-position rotary                      | knob  |
| Lumen MODE         | 2      | Compress / Limit                         | knob  |
| Swirl MODE         | 2      | Chorus / Vibrato                         | knob  |
| Ensemble MODE      | 2      | Chorus / Vibrato                         | knob  |

That is report 5, and it is also report 2 and most of report 3.

**(F) "The pitch jumped back up at 100" is the OCT+DRY position, working as
designed.** Detent 9 sums the DRY signal with the octave, so the original pitch is
audible next to it — which is what "jumped back up" describes. Detent 8 (OCT) is
the pure octave down. This is faithful to the reference (docs §70.2) and is a
LEGIBILITY failure, not a DSP one: a knob reading "100" cannot tell you that the
last click adds your dry signal back. The selector in (E) labels it. Flagged for
an owner decision rather than changed unilaterally.

**(G) Latency is real and is NOT a constant that can be turned down.** Mean
algorithmic delay is `dMin + 0.5*(1+x)*W` = 35.8 ms at the shipped 65 ms window.
The window is pinned by the SOLA search span a major triad needs (§70). Measured
this session while probing smaller windows: **the shifter does not terminate at a
20 ms window** (1 s of audio renders in 425 ms at 65 ms, 454 ms at 40 ms, and
hangs at 20 ms) — a latent robustness defect in `PitchShifter` at small windows,
found by measuring rather than by reading. So a latency cut is a real DSP slice
with its own acceptance bars, not a constant edit in this one.

## Approach

Fidelity-neutral throughout: no circuit model is touched, no golden can move.

1. **Fix (A)** by giving `updateParams` the Mesa, and — because this is the SECOND
   time a Mesa was wired at one boundary and missed at the next — make the drift
   impossible to repeat rather than just adding lines. `applyParamsToModels` and
   `updateParams` must be provably in step.
2. **Fix (B)/(D)** in the web start path and the label map, keyed off a single
   source of truth so a future voice cannot be half-added.
3. **Fix (C)** by building the editor's combo from `kAmpModelChoices` itself.
4. **Fix (E)** with two NEW shared components per front end:
   - a **2-state switch** (reuse the existing `ModeSwitch` natively / `.mode-switch`
     on the web — the silverface Twin's widget the owner named), and
   - a **detented rotary selector** with a LABELLED readout for 5/6/9 positions,
     which is new: `ModeSwitch` does two and three, and nine segments is not a
     switch.
   Every discrete control in the table above moves onto one of them, in both fronts.
5. **Report (F) and (G)** with numbers and a recommendation; change neither
   without a decision.

## How this will be measured

- **(A)** A native test that renders through the PLUGIN's own per-block path with a
  knob moved mid-stream, and asserts the output changes. Today's `amp_voice_test`
  proves the parameter EXISTS; nothing proves a live move reaches the model — the
  exact gap §71 named as its follow-up. Perturbation: revert the `updateParams`
  lines, test goes red.
- **(B)** A web spec that starts the engine with a persisted `mesa` rig and asserts
  the rendered voice is the Mesa, not the Clean 120.
- **(C)/(D)** Assert the selector's item count equals `AMP_MODEL_COUNT` / the label
  map covers every `AVAILABLE_AMP_TYPES` entry — a build error or a red test if a
  voice is added to one and not the other.
- **(E)** Each discrete control's UI must emit only the quantized detent values, and
  its readout must be the position NAME. Asserted per control.
- **(G)** Report the measured latency table and the small-window hang; no bar.

## Manual test steps

- [ ] Plugin: load, select Dual Rectifier from the top-bar selector (it is now
      listed), turn GAIN / MASTER / BASS / MID / TREBLE / PRESENCE — each audibly
      changes the sound while playing.
- [ ] Plugin: move MODE through all five positions — five different voices, each
      named on the panel.
- [ ] Plugin: RECT and POWER read Silicon/5U4 and Bold/Spongy, not 0-100.
- [ ] Web: select the Mesa, reload the page, press start — it is still a Mesa.
- [ ] Cellar: the control reads -1 ... -7, OCT, OCT+DRY; nine clicks, no in-between.
- [ ] Edge: a saved rig holding an out-of-range or non-finite discrete value still
      loads and clamps to a real position.
- [ ] Edge: host automation sweeping Mesa MODE continuously never lands between
      detents.

## Out of scope for this session

- Reducing the Cellar's ~36 ms latency (its own slice; see (G)).
- The `PitchShifter` small-window hang (found here, reported, not fixed).
- Changing the OCT+DRY position's behaviour (owner decision pending).
- Any circuit/model change; any golden re-bless.

---

<!-- Fill in below during/after the session -->

## What actually happened

**The core was never at fault, and checking that first is what made the rest quick.**
Driving the C ABI exactly as the worklet does showed every Mesa control moving the
audio. So all four functional defects were front-end wiring, and all four are the
same shape as §71: the engine boundary was wired and the layer above it was not.

**(A) The native defect was a SECOND parameter path nobody had noticed.**
`PluginProcessor::processBlock` calls `engine_.updateParams(p)` every block — an
"apply only the changed ones" body entirely separate from the
`applyParamsToModels()` that §71 fixed. It had no Mesa branch of any kind. The Mesa
took its parameters once at `prepareToPlay` and dropped every move after that.

Rather than add the missing lines, the two bodies were MERGED into one
`applyAmpRouting(p, old)` — `old == nullptr` applies every row (setup), otherwise
only changed rows (live). A voice can no longer be added to one path and missed by
the other, because there is only one path.

**(B) The web start path had the mirror defect.** `startEngine`'s amp-voice
if/else chain stopped at `rockerverb`, so a persisted Mesa rig started the engine
on voice 0 while the UI drew a Dual Rectifier — the tone knobs did something (they
reach the Clean 120) and GAIN/MASTER/PRESENCE/MODE/RECT/POWER did nothing at all.
Replaced by the total map `engine.setAmpModel(opts.ampType)`. It also never sent
the three Mesa switch ids, so those ignored saved state; now sent.

**(C) and (D), the amp selector.** `PluginEditor.cpp` built its top-bar combo from
its OWN five-entry list against a seven-choice parameter — two voices unlistable
and, because a `ComboBoxAttachment` maps item i to choice i, everything after AC30
misaligned. Now built from `ClipperAudioProcessor::ampModelChoices()`, the
parameter's own array. On the web, `AMP_TYPE_LABEL` was `Record<string, string>`
and missing `mesa`, so the menu item rendered BLANK; it is now `Record<AmpType,
string>`, which makes a missing voice a build error.

**(E) The controls.** Eight discrete parameters were drawn as 0-100 pots in both
front ends, although the models quantize them and the source comments already said
"genuinely DISCRETE". Two new web components (`SegmentSwitch` for 2-3 states,
`RotarySelector` for 4+, both emitting only detent centres) and one new native
facility (`NeuKnob::setPositions`, which sets the slider's interval so the value
CANNOT land between detents and prints the position name). The Mesa's RECT and
POWER became real `ModeSwitch` segmented switches natively, as asked.

**Three things the screenshots caught that a build could not.** The first native
switch layout truncated "Silicon" to "Silic…", collided with the CAB IR chip, and
captioned BOTH switches "MODE" (`ModeSwitch` hardcoded that string). Fixed by
giving `ModeSwitch` a caption, moving the switches to their own row at their own
preferred width, and counting that row in the card's height arithmetic. Found by
building the headless snapshot tool under Xvfb and looking — which also revealed
that the snapshot tool's own voice list was stale at FOUR voices, so no visual pass
had ever photographed the Overdrive, the Rocker Verb or the Dual Rectifier.

## Measured results

**The defect, reproduced and then fixed.** Perturbation P1 (remove the Mesa from
the live path, i.e. the shipped state) — all NINE controls read `max|d| = 0.000e+00`
against a render that never moved the knob, which is the owner's report exactly.
After the fix, on the same probe:

| control   | move        | max\|d\|    | level     |
| --------- | ----------- | ---------- | --------- |
| GAIN      | 0.20 → 0.90 | 5.847e-01  | +20.15 dB |
| MASTER    | 0.15 → 0.80 | 3.815e-01  | +11.69 dB |
| PRESENCE  | 0.00 → 1.00 | 9.225e-02  |  +0.31 dB |
| BASS      | 0.10 → 0.90 | 1.570e-01  |  +1.19 dB |
| MID       | 0.10 → 0.90 | 6.966e-02  |  −0.08 dB |
| TREBLE    | 0.10 → 0.90 | 7.562e-02  |  −1.81 dB |
| MODE      | 0.00 → 0.50 | 2.373e-01  |  +7.04 dB |
| RECTIFIER | 0.00 → 1.00 | 1.100e-01  |  −2.87 dB |
| POWER     | 0.00 → 1.00 | 8.223e-02  |  −2.00 dB |

PRESENCE had to be probed at MODE 0.25 (Orange NORMAL): at the DEFAULT mode it
reads exactly 0.0000 dB and that is CORRECT — §69's open loop. The first version of
the test asserted at the default and failed for that reason, which is a test bug
worth recording: a wiring bar must not be written at an operating point where the
CIRCUIT makes the control inert.

**Core C ABI probe (the check that cleared the core):** BASS +8.68 dB, MID −0.27,
TREBLE −2.30, GAIN +47.28, MASTER +115.14, RECT −3.12, POWER −2.24 dB; MODE detents
0..4 tail rms 0.1587 / 0.2001 / 0.2575 / 0.0728 / 0.1474.

**Suites.** Core ctest **38/38** (5 xfail ledgers Skipped) — unchanged, and it
cannot have changed: no file under `core/` was touched, so **no golden can move and
no WASM rebuild is needed**. Native **4/4** with the CI filter, `identical_core`
included at its usual bar. Playwright **85 → 87** (two new specs). node 15/10/12;
electron 20.

**Cellar latency, measured and NOT changed.** Mean algorithmic delay is
`dMin + 0.5*(1+x)*W` = **35.8 ms** at the shipped 65 ms window, which the SOLA span
a triad needs is what pins. Probing smaller windows found a latent defect worth
recording: **`PitchShifter` does not terminate at a 20 ms window** (1 s of audio
renders in 425 ms at 65 ms, 454 ms at 40 ms, and hangs at 20 ms). So the window is
not a constant that can simply be turned down, and a latency cut is its own slice
with its own acceptance bars.

## Files created / modified

- `native/src/ClipperEngine.{h,cpp}` — `applyAmpRouting`, one body for both paths
- `native/src/PluginProcessor.{h,cpp}` — `ampModelChoices()` accessor
- `native/src/PluginEditor.{h,cpp}` — selector from the parameter's list; Mesa MODE
  as a detented rotary; RECT/POWER as `ModeSwitch`; the switch row layout
- `native/src/ClipperLookAndFeel.{h,cpp}` — `NeuKnob::setPositions`,
  `ModeSwitch::setCaption`
- `native/src/PedalCard.{h,cpp}` — per-knob detent labels
- `native/tests/amp_voice_test.cpp` — the live-edit test + the selector coverage test
- `native/tools/editor_snapshot.cpp` — all seven voices; a discrete-controls scene
- `web/src/components/Selector.tsx` — NEW: `SegmentSwitch` + `RotarySelector`
- `web/src/{params,audio,App}.ts(x)`, `web/src/components/{Amp,Pedal,Board}.tsx`,
  `web/src/styles/amp.css`, `web/tests/amp.spec.ts`

## Deferred to next session

1. **The Cellar's ~36 ms latency** — its own slice. §70 already names a
   frequency-domain shifter as the fix for its XFAIL; latency is a second axis.
2. **`PitchShifter` hangs at a ~20 ms window** — found here, reported, not fixed.
   It is not a shipped configuration, but it is a real robustness defect.
3. **OCT+DRY** — kept faithful and now NAMED. If the owner would rather the last
   click were a bare octave, that is a departure from the reference and wants an
   ADR (the reference's own last position blends dry).
4. The web pedal MODE switches use the segmented widget; the native pedal cards use
   a two-position labelled selector rather than a segmented switch, because the card
   knob row is a fixed grid. Cosmetic parity gap, named.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
