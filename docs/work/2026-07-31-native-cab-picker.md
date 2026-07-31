# Native cab/IR picker — parity with the web's cab select + custom IR

**Date:** 2026-07-31
**Branch:** claude/native-cab-picker-6f557i
**Roadmap item:** owner 2026-07-31: "where do I pick my cab/ir? … spin up one more
slice in parallel, the ir, I want that for recording." The native engine is
hard-wired to `generateDefaultCab2x12IR`; the web has had Clean 2×12 / Brit 4×12 /
user-IR upload since v1.1. This is a chain-parity catch-up, native side.

## Goal

The native editor picks the cab — Clean 2×12 / Brit 4×12 / Custom IR (a .wav the
user chooses) — with the swap declicked mid-audio exactly like the web, persisted
in the plugin state, working tonight in Logic.

## Approach

Mirror the worklet's ADR-003 discipline in `ClipperEngine`: the cab pair becomes
double-buffered (active + spare `CabConvolver` pair). The MESSAGE thread prepares
the spare pair (allocation is legal there: decode/resample the IR, `prepare()`
both sides), then hands an atomic commit request to the audio thread, which swaps
at the declick zero (the engine's existing declick machinery — the same bracket
chain edits use). The audio thread never allocates, never blocks, never prepares.
Free the retired pair on the message thread AFTER the swap is observed (no `free()`
on the audio thread — the worklet's `_destroyPedal` free is a documented bug, not
a precedent).

- **Cab model** = a new APVTS choice parameter (Clean 2×12 / Brit 4×12 / Custom),
  automatable; built-ins generate at the engine rate from the core generators.
- **Custom IR** = JUCE `FileChooser` (async, message thread) → `AudioFormatManager`
  WAV read → mono-ise per the web's convention (check the worklet/web upload path
  and match it) → `CabConvolver::prepare(engineRate, ir, len, fileRate, 128)`
  (the convolver resamples internally per its API — verify, don't assume). The
  file PATH persists in the APVTS state tree as a property (NOT a parameter);
  restore re-loads it, missing file falls back to the selected built-in (the web's
  fallback convention, see its cab.spec).
- **UI**: a cab selector pill next to/below the CAB lever on the amp card (the
  editor's existing ComboBox LnF); "Custom IR…" opens the chooser; the current
  custom file's name shows as the selected label. Both themes.
- **Parity note for the PR body**: web already behaves this way; this slice brings
  native level — the "change both fronts" rule satisfied by catch-up.

## Steps

- [ ] Read web/worklet cab-swap path (`_amp_prepare_cab_*`, the 'hold' declick
      phase, the removed-list free discipline), web cab.spec.ts (fallback +
      mono-ise conventions), ClipperEngine's declick machinery + prepare path,
      identical_core_test + chain_edit_test (they must stay green: default state
      = Clean 2×12 must render bit-identically to today)
- [ ] Engine: double-buffered cab pair + message-thread prepare + audio-thread
      declicked commit + message-thread retirement; latency unchanged (assert)
- [ ] Processor: cab-model choice param + custom-path state property + host
      save/restore round-trip
- [ ] Editor: selector UI (both themes), FileChooser, error surface for an
      unreadable file (fall back + caption, no crash)
- [ ] Tests: chain_edit_test gains a mid-render cab-swap no-pop case (the
      ctx-suspend pattern from the core amp-swap tests, adapted); identical-core
      stays green proving the default path is untouched
- [ ] Native full build (Standalone + VST3 + AU compile), snapshot tool screenshots
      of the selector in both themes
- [ ] Docs: CLAUDE.md entry + this file's bottom sections; §20-amendment-style
      parity note in DEVELOPMENT.md if conventions demand
- [ ] ONE commit on claude/native-cab-picker-6f557i, feat: …, standard trailers;
      NO push, NO PR

## How this will be measured

The new chain_edit no-pop case (mid-render swap inside the declick envelope);
identical-core green (default bit-identical); screenshots of the selector; a
save/reload round-trip of a custom path; unreadable-file fallback exercised.

## Manual test steps

- [ ] Owner (Logic, tonight): pick Brit 4×12 on the JCM — the missing pairing;
      load a custom .wav IR; toggle CAB off/on; save/reopen the Logic project —
      selection + custom path survive
- [ ] Edge: missing/unreadable IR file on restore → falls back to built-in,
      caption says so, no crash; swap mid-note → no pop

## Out of scope

Web changes (it already has all of this), core changes (generators + convolver
suffice), IR trimming/normalization beyond the web's convention, cab param
automation smoothing beyond the declick.

---

## What actually happened

(fill in)

## Measured results

(fill in)

## Files created / modified

(fill in)

## Deferred to next session

(fill in)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
