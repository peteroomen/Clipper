# Native visual pass 3 — shadow head-room everywhere, mode-switch geometry, dark mode

**Date:** 2026-07-31
**Branch:** claude/native-visual-pass3-6f557i
**Roadmap item:** owner screenshot review of pass 2 (2026-07-31): "power button
shadows are clipped, bright switch goes too far down and has a white line on top of
the orange. the pedals don't have shadows matching the other skeuomorphic things
like input and amps"; "the chorus/trem [mode switch] is [broken] as well"; "I wanted
light/dark mode like the web app, we've mixed them both".

## Goal

Every shadow and glow renders un-clipped; the levers and mode switch match the web
recipes exactly; pedal cards cast real shadows like the input/amp cards; and the
native editor gets the web's LIGHT/DARK theme pair (system-following with a manual
toggle), replacing the current single mixed look.

## Approach (root causes, diagnosed from the screenshot)

1. **Clipped power shadows**: the Bright/Cab/Power cluster slots are ~52 px wide
   against a 46 px rocker whose dual DropShadows need ~12 px of head-room per side
   (and the jewel glow needs glowSpread()). Fix the layout contract: widen the
   cluster (the knobs can give ground; web .amp-right is ~190 px for three
   controls) and/or add per-widget shadow margins; assert-by-screenshot after.
2. **Bright lever**: (a) the lit lever's inset light top rim reads as a white line
   on the accent — the web's `.t-lever` rim is imperceptible on the lit gradient;
   drop the rim when lit (or alpha it way down). (b) "goes too far down": verify
   the settled lit position is the web's `top: 26px` inside the 54 px slot with the
   4 px inset preserved, and CLAMP the easeOutBack overshoot so the lever never
   renders outside the slot (overshoot can carry it past bottom during the slide
   and the final frame must settle exactly at 26).
3. **Mode switch**: the active segment renders sharp-cornered and visually wider
   than the carved neighbours. Rebuild web-exact: uniform segment geometry
   (.mode-opt: same width, stacked flush with −1 px overlap, ONLY the stack's outer
   corners rounded 12 px), the active segment raised + lit with the same footprint,
   captions centred. Both 3-state (Clean 120) and 2-state (Twin) forms.
4. **Pedal card shadows**: PedalCard paints its chassis shadow inside its own
   component bounds → clipped → pedals read flat next to the input/amp cards whose
   shadows paint in the editor. Fix: draw the pedal cards' cast shadows from the
   board content's paint (parent, before children), or give cards a transparent
   shadow margin — either way the shadow must fall on the rail/bench like the web.
5. **Dark mode**: the web resolves themes via tokens (light root / dark root) with
   `.pedal` pinned dark on both. Native now has skin::Scheme — add the DARK bench
   context from tokens.css `@media (prefers-color-scheme: dark)` root values
   (ground #26292E, groundDeep #202329, well #1D2025, dark sh-* etc.), theme the
   BENCH + AMP (amp uses the dark token context in web dark mode) + board rail +
   scrollbar + edge fades + top-bar text/pills; pedals stay pinned dark. Theme
   selection: follow the OS (juce::Desktop::isDarkModeActive(), with change
   callback) plus a small manual toggle chip in the top bar (Auto/Light/Dark,
   persisted in the APVTS-adjacent state or a plain properties file — NOT an audio
   parameter). Accent readability: reuse the web's dark-theme accent values where
   tokens.css defines them.

## Steps

- [ ] Read pass 2's plan + code (ClipperLookAndFeel, PluginEditor layout), the web
      tokens.css BOTH theme roots, amp.css/pedal.css/board.css recipes
- [ ] Fixes 1–4 with the snapshot tool driving iteration (before/after per issue)
- [ ] Dark mode per approach 5; snapshot BOTH themes, all four amp voices
- [ ] Native tests (identical_core, chain_edit) + full Standalone/VST3 compile;
      snapshot tool runs headless under Xvfb
- [ ] Docs: pass-2 plan file's deferred list updated; CLAUDE.md entry; this file
- [ ] ONE commit on claude/native-visual-pass3-6f557i, feat: …, standard trailers;
      NO push, NO PR

## How this will be measured

Headless before/after screenshots per issue and per theme (the snapshot tool);
the specific screenshot defects named above visibly gone; native tests pass;
core/web untouched (git status shows native/ + docs only).

## Manual test steps

- [ ] Owner: power/lever/mode all clean at several window sizes; pedals cast
      shadows; theme follows the OS and the toggle overrides; dark mode readable
- [ ] Edge: minimum window; theme flips live without restart artifacts

## Out of scope

The cab/IR picker (its own functional slice), web changes, DSP of any kind.

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
