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

Built as planned, with four things worth recording.

**1. The retirement question had a concrete answer.** The plan said "free the retired
pair on the message thread AFTER the swap is observed". Because both convolver pairs are
plain members of `ClipperEngine` (not heap-owned pointers), there is nothing to `delete`
— the honest statement is that the retired pair's *state* is cleared on the message
thread (`retireCab()`) and its *heap buffers* are released and reused by the next
message-thread `prepare()` into that pair. Either way no `free()` reaches `process()`,
which is the property that matters. `retireCab()` is called by the next `prepareCab*`
(so it can never be skipped) and, for promptness, by the editor's existing 250 ms timer.

**2. The handshake needed five states, not three.** A three-state Idle/Armed/Swapped
word has a real race: the audio thread CASes Armed→Swapped and *then* flips
`activeCabPair_`, so a message thread that sees Swapped in between computes the retired
pair as the one that is about to become LIVE, and resets it. The fix is an intermediate
`Committing` state — the audio thread claims the swap, flips, then release-stores
`Swapped`, and the message thread's acquire-CAS to Idle is what makes the flip visible.
The message thread's wait on `Committing` is a bounded spin (two instructions on the
audio thread; a hard cap of 1e6 iterations so a stopped audio thread can never wedge
the UI).

**3. Built-in IRs must NOT be re-normalized.** The obvious symmetry — "normalize
whatever IR we are about to load" — would have broken bit-identity: the built-in
generators already peak-normalize (M6.6), so a second pass rescales by ~1+ε and moves
the low mantissa bits of a render `identical_core_test` compares exactly. Normalization
is applied to the CUSTOM branch only, exactly where `amp_prepare_cab_custom` applies it.

**4. One click was arming two fades.** A user's menu pick reaches `applyCabFromState`
twice — directly (so the UI is synchronous) and again through the APVTS listener's async
hop — and if the audio thread committed in between, the second pass prepared and armed a
SECOND swap, i.e. two audible declick fades on one click. Fixed with a "what did we last
ask the engine for" signature (`choice|path`). That in turn broke the *retry* case (the
file came back; re-picking Custom looked identical to the request that failed), so a
user action carries `force` and bypasses the dedup. Both behaviours are asserted in
`clipper_cab_state_test`.

**Deliberate divergence from the web, recorded:** a missing IR falls back to the Clean
2×12 with a note (web convention) but native KEEPS the path in the state tree, where the
web clears its label. A DAW session opened before an external drive is mounted is then
one click from repair.

## Measured results

**Declick / no pop** (`clipper_chain_edit_test`, new case: Clean 2×12 → Brit 4×12
landing mid-note, 220 Hz sine, amp powered, cab on, block 64 @ 48 kHz):

| | value |
| --- | --- |
| steady slew (pre / post-swap) | 0.001782 / 0.002413 |
| max step at the seam | **0.002413** |
| bound (1.25 × the larger slew) | 0.003017 |
| the same switch spliced HARD | **0.021781 (7.2× the bound)** |
| audio-thread cab commits | 1 |
| settled RMS vs the new cab rendered alone | 0.058567 vs 0.058567 |

The seam step *equals* the settled signal's own 220 Hz slope — the fade contributes
nothing measurable. **Perturbation** (scratch copy: the declick arm replaced with an
immediate `commitCabIfArmed()`, restored + `touch`ed after): seam **0.060711 against the
same 0.003017 bound, 20×** → the case goes red on both "armed the fade" and "no
discontinuity". The bound itself is derived from the LARGER of the pre- and post-swap
settled slews; a pre-only bound failed at 0.002413 vs 0.002227 on nothing but the new
cab being ~4 dB hotter, which is a measurement artefact, not a click.

**Latency** — unchanged for every cab, as the partition stays 128:
`clean212 264, brit412 264, custom 264` samples (engine test), and the same three
numbers through the whole plugin (`clipper_cab_state_test`). Arming a swap does not
move it either.

**Bit-identity** — `clipper_identical_test`, source file **untouched**: max
|plugin − ref| = **0.000e+00** on all eight cases (Clean 120 / JCM800 / Twin / AC30 and
four multi-pedal boards), reported latency matching the reference in every case. That is
the proof that the default state renders exactly as the pre-picker engine did.

**State round trip** (`clipper_cab_state_test`, 33 checks, all PASS):
built-in choice survives save+reload; a custom IR's path survives and is re-read from
disk; the path is verified to live in a `<cab customIr="…"/>` state node and NOT in any
`<PARAM>`; a stereo 44.1 kHz IR loads (mono-ised + resampled); a missing IR falls back to
the Clean 2×12 with the note *"IR not found — using the Clean 2×12."*; a non-audio file is
refused with *"Could not load that IR (unreadable audio file)."* and the previous cab
keeps playing; re-selecting Custom once the file is back repairs the session.

**Build**: full `cmake --build native/build -j8` clean — `Clipper_Standalone` and
`Clipper_VST3` both link. **AU could not be compiled here**: `native/CMakeLists.txt`
adds AU to `CLIPPER_FORMATS` only `if(APPLE)` and this container is Linux. Nothing
platform-specific was added (JUCE's `FileChooser::launchAsync`, `AudioFormatManager`,
`LagrangeInterpolator` are all cross-platform, and the AU wrapper compiles the same
shared `Clipper` target that VST3 and Standalone do), but that is reasoning, not a
measurement — the AU build has to be run on the Mac before tonight.

**Screenshots** (`clipper_editor_snap` under Xvfb): `native_cab_clean212_{light,dark}`,
`native_cab_brit412_{light,dark}`, `native_cab_custom_{light,dark}`,
`native_cab_missing_{light,dark}`, `native_cab_small_window`.

## Files created / modified

- `native/src/ClipperEngine.{h,cpp}` — the double-buffered cab pair, the five-state
  handshake, `prepareCabBuiltin` / `prepareCabCustom` / `commitCabNow` / `retireCab` /
  `commitCabIfArmed`, the widened cab-swap zero hold, `CabChoice`.
- `native/src/CabIrFile.{h,cpp}` — NEW. Decode → mono-ise → resample → cap+fade, to
  `web/src/cab.ts`'s conventions.
- `native/src/PluginProcessor.{h,cpp}` — the `cabModel` choice parameter, the `cab`
  state node holding `customIr`, `applyCabFromState`, the audio-thread-safe parameter
  listener (`AsyncUpdater`), the missing-file fallback, label/note/version for the UI.
- `native/src/PluginEditor.{h,cpp}` — the CAB IR chip, its popup, the async
  `FileChooser`, the caption + fallback note, layout under the Cab lever in both themes,
  the cab-version watch and the message-thread `retireCab()` poll.
- `native/tests/chain_edit_test.cpp` — the mid-render cab-swap no-pop case + the cab
  invariants block.
- `native/tests/cab_state_test.cpp` — NEW.
- `native/tools/editor_snapshot.cpp` — the `native_cab_*` scenes.
- `native/CMakeLists.txt` — `CabIrFile.cpp`, the `clipper_cab_state_test` target + test.
- `docs/DEVELOPMENT.md` — §30 amendment. `CLAUDE.md` — Current State.

## Deferred to next session

- **`.github/workflows/ci.yml`**: the native job filters
  `ctest -R 'clipper_identical_core|clipper_chain_edit'`, which does not match
  `clipper_cab_state`. It runs locally but not in CI until that regex gains it. Out of
  this slice's scope (native/ + docs only) — a one-line change.
- **AU compile on macOS.** Cannot be built on Linux; verify before the session.
- **Web parity in the other direction:** the web caps IRs at 4096 and stores the SAMPLES
  in localStorage, native stores a PATH. Neither is wrong, but a rig JSON exported from
  one does not carry a custom cab to the other.
- **An IR browser / recently-used list.** Today the popup offers exactly one remembered
  custom IR (the last one loaded), which is what the web offers too.
- The engine's `prepare()` still rebuilds a custom IR from the samples the shell handed
  it at the OLD engine rate when the shell does not re-load the file; the plugin always
  does re-load (`prepareToPlay`), so this only shows up for a direct `ClipperEngine`
  user that changes rate without re-supplying the IR.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
