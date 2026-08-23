# The Mesa is unreachable in the plugin — native parameter + editor wiring

**Date:** 2026-08-19
**Branch:** fix/native-mesa-voice
**Roadmap item:** defect in M10.4 (docs §69) — found by the owner: *"I can see cellar, the drop pedal. I can't see my mesa"*

## Goal

The Mesa Dual Rectifier voice can be selected and played in the JUCE plugin, with
its three switches (MODE / RECTIFIER / POWER) reachable, exactly as on the web.

## What is actually wrong

§69 wired the Mesa into `ClipperEngine` — voice 6 exists, `mesa_` is a member,
`applyMesaSwitches()` runs, and `Params` carries `mesaMode` / `mesaRectifier` /
`mesaPowerMode`. **It stopped there.** The plugin's parameter layer and editor
were never touched, so:

1. **`kAmpModelChoices` has six entries and the Mesa would be the seventh** — the
   `ampModel` Choice parameter cannot express the value 6, so nothing can ever
   set it. This is the whole reason it is invisible.
2. **There are no APVTS parameters for the three switches**, so
   `updateParamsFromApvts` never writes them and they sit at the `Params`
   defaults forever. Even if (1) were fixed, MODE would be stuck.
3. **`updateAmpFace()` clamps `jlimit(0, 5, …)` and has no `case 6`**, so the
   panel would fall through to `default:` and silently re-label itself Clean 120
   *and reset `ampModel_` to 0*.

This is exactly the class of defect §61.10, §62 and §67.10 record on the pedal
side — a keyed table right and a switch incomplete — reappearing on the amp side.

**Scope check: the CORE is not touched, so this is not a tone change.** No golden
can move, and no WASM rebuild is needed (`core/` and `web/worklet/` are untouched).

## Approach

Mirror the web, which already ships this voice. Notably the web exposes all three
switches as **plain knobs**, not as switch widgets — MODE is a 5-position control
and `ModeSwitch` only does 2 and 3 states, so a knob is the honest widget and the
core already quantizes it. Native gets three new `NeuKnob`s on the amp superset,
which keeps the panel's whole mechanism unchanged.

The three switches take NEW param ids rather than reusing an amp slot: no other
voice has a 5-position mode or a rectifier select, and reusing a slot would make a
saved rig silently mean something else (§57's rule, the same one that gave the
OR120's F.A.C. its own id).

`AccentId::Mesa` is added from `tokens.css`'s own `--accent-mesa` values, light
and dark, verbatim — the same way every other accent crossed over.

## Steps

- [ ] `AccentId::Mesa` + its two colours from tokens.css
- [ ] `kAmpModelChoices` += "Dual Rectifier" (APPEND ONLY — index 6)
- [ ] Three `pid::` ids + three APVTS knob parameters, defaults matching
      `Params`' own and the web's `AMP_KNOB_DEFAULTS`
- [ ] Read them in `updateParamsFromApvts`
- [ ] Three `NeuKnob`s + attachments on the amp superset
- [ ] `jlimit(0, 6)` and a `case 6` in `updateAmpFace()`
- [ ] A native test that FAILS on the pre-fix state

## How this will be measured

The bug is "a voice is unreachable", so the measurement is a REACHABILITY test,
not a tone one — and it must fail on the pre-fix code, which a render comparison
would not (the pre-fix editor cannot produce voice 6 to compare).

`native/tests/amp_voice_test.cpp` (new ctest entry), asserting:
1. **Every engine voice is offered by the `ampModel` Choice parameter.** The
   parameter's choice count must equal the engine's voice count — this is the
   assertion that goes red on the pre-fix tree, and it goes red for ANY future
   voice added to the engine and not to the plugin, which is the real value.
2. **Setting the choice to the Mesa index actually renders the Mesa**: the
   plugin's output at index 6 matches a hand-built `MesaAmp` reference, and does
   NOT match the Clean 120 (which is what the `default:` fall-through would give).
3. **Each of the three switch parameters reaches the engine**: MODE moves the
   output measurably, and specifically the two MODERN modes must measure ZERO
   feedback loop depth (§69's own bar) where the others do not.

Plus `clipper_identical_core` extended with a Mesa board case, closing the gap
§70 named ("the Mesa is still absent from that test's REFERENCE driver").

## Manual test steps

- [ ] Build Standalone, open it, click the amp voice selector → "Dual Rectifier"
      is in the list and selecting it repaints the panel crimson with the
      Recto wordmark and nine knobs
- [ ] MODE knob through its five detents audibly changes the voice; PRESENCE does
      nothing in the two MODERN positions (§69: the loop is open there — this is
      correct, and the assistant coaches it)
- [ ] RECT to 5U4 + POWER to Spongy softens the attack on a low chord
- [ ] Edge case: save a session on Dual Rectifier, close, reopen → still the Mesa
      with the same MODE, not silently Clean 120
- [ ] Edge case: a session saved BEFORE this fix (ampModel 0-5) opens unchanged

## Out of scope for this session

- Any core/DSP change. The engine is correct; only the plugin's parameter and UI
  layers are missing.
- The Mesa's own cab IR (it reuses `brit412` — §69's named follow-up).
- The effects loop and the EL34 bias option (§69's other named follow-ups).
- The web app, which already ships this voice correctly.

---

<!-- Fill in below during/after the session -->

## What actually happened

Diagnosis held exactly as written — all three gaps were real and the core was
never involved. Two things came out of the build that the plan did not anticipate.

**1. The voice ids were a COMMENT, so nothing could compare engine to plugin.**
`ClipperEngine.h` documented "0 = Clean 120 … 6 = Mesa" in prose. That is why the
omission was expressible at all. Replaced by `enum AmpModel` with
`AMP_MODEL_COUNT`, now read by a `jassert` in `createParameterLayout()`, by
`updateAmpFace()`'s clamp (which was a hardcoded `5`) and by the new test. **That
is the actual deliverable** — the Mesa is four lines of it.

**2. A bar that looked sufficient was not, and the perturbation proved it.**
"Selecting the Mesa renders something other than the Clean 120" stayed **GREEN**
with the choice list reverted: a request for index 6 against six choices clamps to
**5, the Rockerverb**, which is a real and different amp. So that bar catches a
`default:` fall-through and nothing else. The bars that caught the shipped defect
are the COUNT bar and the MODE bar. Recorded in §71.4 rather than quietly kept.

Also closed §70's named gap in the same slice, since it was the same missing
reference: `identical_core_test`'s driver now builds a `MesaAmp`, and the drop
pedal's board case moved from the JCM800 to `Cellar -> RAT -> Mesa` with all three
Recto switches off-default.

## Measured results

**Reachability (`clipper_amp_voice`, new):**

| Bar | Measured |
| --- | --- |
| Choice parameter offers `AMP_MODEL_COUNT` voices | engine 7, parameter 7 (was 6) |
| "Dual Rectifier" specifically offered | present |
| Mesa render vs Clean 120 | tail rms **0.1637** vs 0.0414, max\|diff\| **0.6760** |
| MODE reaches the engine | the two modes differ |
| PRESENCE inert in a MODERN mode (§69's open loop) | **0.0000 dB** |
| PRESENCE authority with the loop closed | **0.3109 dB** |

**Perturbation:** revert `kAmpModelChoices` to six entries → **4 of 6 bars RED**
(count, name, MODE-changes-output, PRESENCE-authority). Restore → GREEN. The
render bar stayed green under the perturbation, for the reason above.

**`identical_core_test`:** the new `Cellar -> RAT -> Mesa` case measures
**max |plugin − ref| = 0.000e+00** on both channels, as do all nine other boards.

**Suites:** native **4/4** (`clipper_identical_core` 179 s, `clipper_chain_edit`
77 s, `clipper_cab_state` 28 s, `clipper_amp_voice` 26 s). Standalone and the
plugin shared code both compile clean. **Core, web and the WASM artifact are
untouched by construction — no golden can move and no rebuild is needed.**

## Files created / modified

- `native/src/ClipperEngine.h` — `enum AmpModel` + `AMP_MODEL_COUNT`; the voice
  list stops being a comment.
- `native/src/PluginProcessor.h/.cpp` — "Dual Rectifier" appended to
  `kAmpModelChoices`, the count `jassert`, three new `pid::` ids, three APVTS knob
  parameters, and the three reads in `updateParamsFromApvts`.
- `native/src/PluginEditor.h/.cpp` — three `NeuKnob`s + attachments, the clamp
  against `AMP_MODEL_COUNT`, and `case AMP_MESA` (nine knobs, crimson, no reverb,
  no bright).
- `native/src/ClipperLookAndFeel.h/.cpp` — `AccentId::Mesa`, both theme values
  verbatim from `tokens.css`.
- `native/tests/amp_voice_test.cpp` — NEW.
- `native/tests/identical_core_test.cpp` — the Mesa reference case; the drop board
  case re-pointed at it with all three switches off-default.
- `native/CMakeLists.txt` — the new target + `add_test`.
- `.github/workflows/ci.yml` — `clipper_amp_voice` added to BOTH the build list
  and the ctest filter (this file's own comment records `clipper_cab_state`
  landing red in PR #35 for exactly the mismatch).
- `docs/DEVELOPMENT.md` §71.

## Deferred to next session

- **The editor still has no automated behaviour test.** CI compiles
  `PluginEditor.cpp`; nothing exercises it. The reachability test proves the
  parameter and the engine agree — which is what made the amp invisible — but a
  panel drawing the wrong knobs would still ship green. That is the remaining hole
  of this class, and it is now the only one.
- §69's own open items are untouched: the Mesa's own cab IR (it reuses
  `brit412`), the effects loop, the EL34 bias option, and the ~48 W power
  shortfall.

## Status

- [x] Complete
