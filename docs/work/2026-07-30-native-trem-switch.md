# Native trem switch — the Twin's tremolo ON/OFF reaches the JUCE editor

**Date:** 2026-07-30
**Branch:** claude/amps-pedals-fixes-6f557i
**Roadmap item:** owner field report 2026-07-30 ("no trem switch on native either" — asked twice); web/native UI parity for the docs §20-amendment switch

## Goal

The native Twin panel shows a working tremolo ON/OFF switch, wired to the same parameter the
engine already consumes, matching the web app's field-requested switch (docs §20 amendment).

## Approach

UI-only, native-only — **zero DSP change, zero chain change** (the chain parity invariant is
untouched; `ClipperEngine` already maps the shared `chorusMode` slot to
`TwinAmp::PARAM_TREMOLO_ENABLE` at ClipperEngine.cpp:137-139 and :260-262, declick-bracketed).
The gap is purely the editor: `showMode_` stays false for the Twin case, so the `ModeSwitch`
bound to `pid::chorusMode` never appears, and the widget's labels are hardcoded
{"Off","Chorus","Vibrato"}.

- `ModeSwitch` gains `setLabels()` and `setAccent()`; `setSelected` clamps to the label count
  (a state saved on Clean 120 as Vibrato = index 2 displays as "On" for the Twin — correct,
  since the engine treats every index ≥ 1 as trem-on).
- Twin case: `showMode_ = true`, labels {"Off","On"}, Twin accent. Clean 120 case: labels
  restored {"Off","Chorus","Vibrato"}, clean accent (also fixes the pre-existing quirk that the
  switch always painted the clean accent).
- The existing layout row already reserves the switch slot (`showMode_` guard), caption
  "Tremolo" already set.

## Steps

- [ ] `ClipperLookAndFeel.h/.cpp`: `ModeSwitch::setLabels/setAccent`, clamp in `setSelected`
- [ ] `PluginEditor.cpp`: Twin case shows the switch with {"Off","On"}; Clean 120 restores its
      three labels; both set the amp accent
- [ ] Build the native targets (`scripts/native.sh` or the CMake preset the repo uses); if this
      container cannot build JUCE, say so honestly in the PR and lean on CI's native job for the
      identical-core/chain tests plus a compile check of the editor if it covers it
- [ ] Manual steps below; commit, push, PR

## How this will be measured

Compile evidence (the native build), plus behavior: toggling the switch flips
`TwinAmp::PARAM_TREMOLO_ENABLE` through the existing declick-bracketed path — the engine-side
mapping is already covered by `chain_edit`/`identical_core`; no new DSP surface exists to measure.
This is a UI-parity slice; the measurement bar is "the control exists and drives the wired param",
verified by the param attachment round-trip (host automation moves the switch; clicking writes the
choice param).

## Manual test steps

- [ ] Native standalone: select the Twin, see the Tremolo row with Speed/Intensity and the new
      Off/On switch; toggle ON with intensity up — audible throb; OFF — clean, no click
- [ ] Switch to Clean 120 — the switch reads Off/Chorus/Vibrato again with the red accent;
      back to Twin — Off/On with the silver-blue accent
- [ ] Edge: automate `chorusMode` to 2 (Vibrato) from the host while on the Twin — the switch
      shows "On" and trem is audible (engine: index ≥ 1 == on)
- [ ] Edge: toggle rapidly while playing — declick path holds (no pops; pre-existing engine
      behavior)

## Out of scope for this session

The AC30/JCM panels (no tremolo in those circuits), a dedicated native trem-enable parameter
separate from `chorusMode` (would break saved sessions for no benefit), the native dark theme,
and every DSP slice of the current round.

---

<!-- Fill in below during/after the session -->

## What actually happened

As planned, plus one sync fix found while writing it: switching amp panels never fires the
param attachment, so after re-labeling (3 labels ↔ 2) the displayed index could go stale —
`applyAmpModel` now ends with `chorusModeAttach_->sendInitialUpdate()` so a Clean-120
"Vibrato" survives a Twin round-trip intact (the Twin displays it as "On", which is what the
engine does with it). The full JUCE Standalone was built in-container (Linux deps + pinned
JUCE FetchContent) to compile-verify the editor, since CI's native job only builds the two
console tests — `PluginEditor.cpp` gets zero CI compile coverage.

## Measured results

UI slice — the bar was "compiles and drives the wired param": `Clipper_Standalone` built
clean at 100 %, and `clipper_identical_core` + `clipper_chain_edit` pass 2/2 (engine mapping
unchanged, as intended). No DSP surface changed; core ctest untouched by this slice.

## Files created / modified

- `native/src/ClipperLookAndFeel.h` — `ModeSwitch::setLabels`/`setAccent`, clamped `setSelected`
- `native/src/PluginEditor.cpp` — Twin panel shows the Off/On trem switch; Clean 120 restores
  its three labels; attachment re-sync on panel change
- `docs/DEVELOPMENT.md` §20-amendment note, this plan file, `CLAUDE.md` Current State

## Deferred to next session

- macOS visual check (this container is Linux — the owner sees the switch on next build)
- The native latency item stays in the backlog; slices 3–5 of the round follow

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
