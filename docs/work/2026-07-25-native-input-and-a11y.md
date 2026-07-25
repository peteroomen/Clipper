# Native editor: input safety, accessibility, and the repaint storm

**Date:** 2026-07-25
**Branch:** fix/native-input-and-a11y
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` → **Medium → UI/UX**, the native
input + accessibility + repaint cluster (bullets 1, "Accessibility" (native half),
"Native repaint storms", and "Native minimum editor width 1040 px").
**Docs section:** §40. **ADR:** 012 (both centrally assigned).

## Goal

The native editor's controls must be safe to touch on stage (act on mouse-**up**, left
button only, drag-off aborts, right-click reaches the host), reachable from the keyboard
and visible to assistive tech, and must stop repainting the whole bench — with measured
before → after counts for the repaint storm and the `ValueTree` write rate.

## Approach

Five findings, one editor. No `core/`, no `web/`, no `native/src/ClipperEngine.*` — this
slice is the **editor** only, so it requires no WASM rebuild and cannot touch the audio
thread. `native/tests/identical_core_test.cpp` must stay green (it is a blocking CI job).

**1. Input safety.** JUCE's `juce::Button` already fires on mouse-**up inside** with
drag-off-to-abort (`triggerOnMouseDown` defaults false, and `mouseUp` checks `wasOver`),
but it does **not** filter the mouse button — a right-click on a plain `juce::Button`
still clicks it. So the kit gets one shared base, `BenchButton : juce::Button`, that
ignores any press that is not a primary (left, non-popup) click and routes the secondary
click to the host's parameter menu instead. `juce::Slider` has the same defect from the
other direction: with `menuEnabled` false (the default) a **right-drag turns the knob**,
so `NeuKnob` gets the matching `BenchSlider : juce::Slider`. Not in the audit; same bug.

**2. Re-base the widget kit.** `Footswitch`, `ChipButton`, `LeverToggle`, `PowerControl`
derive from `BenchButton` (hence `juce::Button`) so keyboard operability, focus and the
accessibility role/name/state come for free. `ModeSwitch` is a 3-way switch, which is not
a button — it becomes a container of three radio-grouped `BenchButton` segments, so each
position is its own tab stop with its own name and checked state. Their existing
`std::function<void()> onClick` members are **deleted** in favour of `Button::onClick`,
which has the same signature, so no call site changes.

**3. `NeuKnob::setName` shadowing.** Rename to `setKnobName`, which sets the caption label
*and* `Component::setName` *and* the inner slider's accessible title — and `= delete` the
shadowing `setName` on `NeuKnob` so the mistake cannot be made again (it becomes a compile
error, not a silent no-op). Every call site is updated; a harness assertion walks the
editor and fails if any knob's slider has an empty accessible title.

**4. Repaint storm.** Two independent fixes:
   * **Repaint only what moved.** `boardView_.onScroll` and `movePedal` call full-editor
     `repaint()` (and `movePedal` a full `resized()`); the only editor-drawn thing that
     actually moves is the pair of **boundary cables**. Repaint just those two bands, and
     relayout only the board content.
   * **Cache the shadows.** Every `juce::DropShadow::drawForPath` allocates an offscreen
     `Image` and box-blurs it. A `PedalCard` paint does 11 of them (2 chassis + 1/knob +
     1/chip + 1 stomp) and they are a pure function of (shape, size, corner, spec). Route
     every shadow in the kit through one `skin::drawShadows()` that keys a bounded LRU
     image cache on exactly that, and blit. Rendered at the graphics context's physical
     pixel scale so a hidpi window is not softened.
   * **Stop writing the `ValueTree` per crossing.** `movePedal` → `setChainOrder` writes
     the board node on every boundary the pointer crosses. Split into
     `setChainOrderLive()` (publishes the packed atomic the audio thread reads, so the
     live reorder still sounds, still declicked) and `commitChainOrder()` at drag end,
     which writes the tree once.

**5. Minimum width.** Do not guess: add a **layout audit** to the harness that walks the
editor's whole component tree and fails on (a) any visible component escaping its parent,
(b) any two visible mouse-intercepting siblings overlapping, (c) any visible amp control
escaping the painted amp card, (d) the input knob escaping the input card, (e) a board
viewport narrower than one pedal card + its rail padding. Then sweep the width downward
and take the smallest value that passes for all four amp voices with a six-pedal board.
If nothing below 1040 passes, say so and leave it.

This is a UI slice, so it is fidelity-neutral by construction: no file under `core/` and
no audio-thread code is touched.

## Steps

- [ ] Instrument first, so "before" is the real code: `skin::drawShadows()` (uncached,
      counting) replaces every direct `DropShadow` site; counters for blur passes,
      cache hits/images and `ValueTree` writes behind `CLIPPER_PAINT_METRICS`
- [ ] New dev-only harness `native/tools/paint_bench.cpp` behind
      `CLIPPER_BUILD_PAINT_BENCH` (default OFF, like the snapshot tool): (A) sustained
      42 Hz edge-drag scroll, (B) a full drag-reorder across a six-pedal board,
      (C) the layout audit + accessible-name audit
- [ ] Measure BEFORE (A, B) and record it
- [ ] `BenchButton` / `BenchSlider`, `showHostParameterMenu()`
- [ ] Re-base `Footswitch` / `ChipButton` / `LeverToggle` / `PowerControl`; rebuild
      `ModeSwitch` as three radio segments; wire the host menu per parameter
- [ ] `NeuKnob::setKnobName` + delete the shadowing `setName`; update all call sites
- [ ] Narrow the scroll + reorder repaints; add the shadow cache
- [ ] `setChainOrderLive` / `commitChainOrder`; drag end commits
- [ ] Measure AFTER (A, B); sweep the minimum width (C)
- [ ] Docs §40, ADR 012, CLAUDE.md Current State, plan file fill-in

## How this will be measured

`native/tools/paint_bench.cpp`, built with `-DCLIPPER_BUILD_PAINT_BENCH=ON` and run under
`xvfb-run` against a real desktop peer (so JUCE's real dirty-region plumbing decides what
repaints — not a synthetic `paintEntireComponent`):

| number | how |
|---|---|
| `DropShadow` blur passes + offscreen image allocations **per second** during a sustained 42 Hz edge-drag scroll | counter in `skin::drawShadows`, over a 2 s pumped message loop |
| `ValueTree` writes **per drag-reorder** | counter in `ClipperAudioProcessor::setChainOrder`, over one grip drag that crosses every card |
| blur passes per drag-reorder | same run |
| the abort path | the harness synthesises a press + drag-off + release, and a right-button press + release, on Power / Bright / Cab / a footswitch / a mode segment, and asserts the bound parameter **did not change**; a plain left click on the same control asserts it *did* (so the test can fail) |
| accessible names | the harness asserts every visible `NeuKnob`'s slider has a non-empty accessible title and that every kit widget exposes a button/radio role |
| minimum editor width | the layout audit above, swept downward |

`ctest --test-dir native/build -R 'clipper_identical_core|clipper_chain_edit'` must stay
green, and the plugin targets must still build.

## Manual test steps

- [ ] Standalone: press and hold Power, drag the pointer off it, release → the amp stays on
- [ ] Right-click Power / Bright / Cab / a stomp / a mode segment → nothing toggles
- [ ] Tab through the editor: every stomp, chip, lever, Power and each mode segment takes
      focus and responds to Space/Return
- [ ] Drag a pedal's ⠿ grip from one end of the board to the other, crossing every card:
      the reorder follows the pointer, the audio follows it declicked, and the board does
      not flicker
- [ ] Edge case: drag a grip to the viewport edge and hold there so the auto-scroll pump
      runs for several seconds — the board scrolls smoothly and the amp card does not
      redraw
- [ ] Edge case: resize the window to the new minimum with a six-pedal board on each of
      the four amp voices — no control leaves its card, nothing overlaps

## Out of scope for this session

- The web half of the accessibility finding (popover Escape/focus trap, "Upload IR…",
  knob focus ring / fine adjust / Home-End) — `web/` is another slice's.
- Undo for Remove/Swap, and the assistant's cancel path (both need a project-wide undo
  ring — its own slice).
- The web contrast finding, the tone-knob taper, and every DSP item.
- A native dark theme (explicitly deferred).
- `native/src/ClipperEngine.*` and the native block-size chunking (finding 3's native half).

---

<!-- Fill in below during/after the session -->

## What actually happened

(filled in below)

## Measured results

(filled in below)

## Files created / modified

(filled in below)

## Deferred to next session

(filled in below)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
