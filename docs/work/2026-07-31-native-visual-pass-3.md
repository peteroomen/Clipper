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

All five, screenshot-driven throughout (`clipper_editor_snap` under Xvfb, before →
after per issue, then both themes × all four amp voices). UI only: zero DSP, zero
chain, zero core/web change, no WASM rebuild, goldens untouched by construction.

**1. The clipped power cluster was a WIDTH bug, and pass 2 only fixed the height.**
Pass 2 gave the cluster `PowerControl::preferredHeight()` but left its width at the
old `jlimit(96, 168, in.getWidth()/3)`, which on the shipped 1360-wide window hands
the power control **36 px for a 46 px rocker** — the widget was *narrower than the
thing it draws*, so the rocker, its 5px/5px/12px cast shadow and the outer ring of
the jewel's halo were all sliced off at the component's right edge. That is the hard
vertical cut in the owner's screenshot, and no amount of jewel head-room could have
fixed it. Each widget now reports what its own shadows need
(`LeverToggle::preferredWidth()` 46, `PowerControl::preferredWidth()` 70,
`ModeSwitch::preferredWidth()` 94) and the cluster asks for their sum, scaling the
slots *together* if the card cannot give it all — so a squeeze can no longer land
entirely on one widget.

**2. The lever's white line was the token being right and the material being wrong.**
`.t-lever`'s `inset 0 1px 1px var(--sh-light)` is faithful — but `--sh-light` is
`rgba(255,255,255,.95)`, and a 95 %-white hairline across a saturated accent reads
as a white line *painted on the orange*, which is exactly what the owner reported.
The lit lever now takes a rim of its own accent, brightened (same cue, right
material); the unlit cap keeps the web rim verbatim. "Goes too far down" was the
easeOutBack spring: it peaks at **1.0987**, i.e. 2.17 px past the slot's 4 px inset,
so the lever really did leave its slot at both ends of the travel. `leverPos_` is now
clamped to [0,1] — the timing is unchanged, the lever just has a physical stop, and
the settled lit position is the web's `top: 26px` with the 4 px inset preserved by
construction (`travel = slotH − 2·inset − leverH = 22`).

**3. The mode switch was two bugs wearing one coat.** The inset shading was drawn as
FULL rounded rects (`roundedRectPath(seg, 12)`) rather than the segment's own
corner-specific path, so every carved neighbour drew a capsule outline *inside*
itself — while the active middle segment, which correctly has no rounded corners at
all, drew none. That is the "sharp-cornered wider red block": the neighbours looked
inset and rounded, the active one looked full-bleed and square. Second, the stack ran
edge-to-edge in the component, so the active segment's `2px 3px 6px` cast shadow was
clipped on both sides and piled up as apparent width. Rebuilt to the CSS: one
footprint for every segment, the `margin-top: -1px` overlap, only the stack's outer
corners rounded (12 px), inset shading stroked along the segment's OWN path, the
active segment drawn LAST (it is the raised element, so its shadow belongs on top of
its neighbours) inside `kModeShadowPad` of head-room. Verified on both the 3-state
(Clean 120) and 2-state (Twin) forms, in both themes.

**4. The pedal shadows were a JUCE clipping rule, not a missing recipe.**
`PedalCard::paint` called `drawChassisCard`, which draws the cast shadow — but a
component's paint is clipped to its own bounds, so the entire shadow landed
underneath the card. The input and amp cards had shadows only because the EDITOR
paints them. `drawChassisCard` is now split into `drawIslandCastShadow` +
`drawChassisBody`; the cards paint the body, and `paintBoardContent` (the parent)
paints their shadows onto the rail before the children draw.

**5. Dark mode, and the finding that made it cheap.** The dark token root's control
values are **byte-for-byte** the values `pedal.css` pins `.pedal` to on every theme —
so `darkIsland()` already WAS the dark bench scheme, no third scheme was needed, and
the pedals are theme-invariant for free (which is the web behaviour). Only the bench
values (`--ground`, `--ground-deep`, the bench ink triplet, the cast shadow, the
cable palette) and the ACCENTS differ, and the accents are not pinned by pedal.css —
they resolve at the root, so a RAT LED really is a different red in dark theme.
`PedalFace::accent` therefore became a token (`skin::AccentId`) rather than a value.
Themed: bench gradient, amp panel + all its controls, board rail + mat, scrollbar,
edge fades, top bar, cables, input card's cast shadow. Pedals stay pinned dark.
Theme selection follows the OS via `juce::DarkModeSettingListener` with a 3-way
Auto/Light/Dark chip in the top bar, persisted in a `juce::PropertiesFile` — NOT an
APVTS parameter, so it never reaches host automation or a session. The store is
opened and closed per call rather than kept in a global, because a `PropertiesFile`
(a Timer + ChangeBroadcaster) outliving the message manager is a shutdown assert
waiting to happen.

Two notes for the next session. On Linux `Desktop::isDarkModeActive()` reports false,
so **Auto resolves light there** and the manual override is how you get dark — that
is what the dark screenshots use. And `PropertiesFile::Options::folderName` would put
the file at `~/Clipper/` on Linux; it is set to `.config/Clipper` there so the XDG
path is used (verified: `~/.config/Clipper/Clipper.settings`).

## Measured results

Geometry, measured from the built editor (light theme, Clean 120):

| window | amp card | bright | cab | power | mode |
| ------ | -------- | ------ | --- | ----- | ---- |
| 1360×640 | 390 | 46 | 46 | **70** | **94** |
| 1040×560 (minimum) | 356 | 44 | 44 | **70** | **94** |

Before, from the previous layout expression at the same card widths: cluster
`jlimit(96,168,350/3)` = 116 → slots 38 → **power 36 px** at 1360 and **31 px** at
1040, against a 46 px rocker — clipped by 10 px and 15 px respectively, plus the
whole of its cast shadow and the halo's outer ring.

Lever: easeOutBack peak **1.0987** → clamped to 1.0; settled lit position
`slotY + 4 + 1.0 × 22 = slotY + 26` = the web's `top: 26px`, bottom inset 4 px.

Screenshots (session scratchpad `vp3/`): `before/` and `after/` hold the full
21-scene set; `fix1_2_cluster_{before,after}.png`, `fix3_mode_{before,after}.png`,
`fix4_shadow_{before,after}.png` are the per-fix crops. Both themes × four voices:
`after/native_theme_{light,dark}_{clean120,eight_hundred,twin,thirty}.png`, plus
`after/native_theme_{light,dark}_board.png` (six-pedal board mid-scroll). Visible in
them: the halo renders round with the rocker's shadow complete on all four sides; the
lit lever carries no white line and sits inside its slot; the mode segments are one
footprint with only the stack's outer corners rounded; the pedal cards cast real
shadows onto the rail; dark mode reads across the whole bench with the pedals
unchanged.

Theme persistence round-trip, measured with a temporary probe (reverted before
commit): `dark → dark`, `light → light`, `auto → auto`, all OK, file written to
`~/.config/Clipper/Clipper.settings`.

Gates: `clipper_identical_core` + `clipper_chain_edit` **pass**; full
Standalone + VST3 + shared-code + snapshot-tool build clean with
warnings-as-errors; `git status` shows only `native/` + `docs/`.

## Files created / modified

- `native/src/ClipperLookAndFeel.h/.cpp` — `skin::ThemeMode` + resolved theme state,
  the persisted store, theme-resolved bench colours (`ground/groundDeep/benchWell/
  benchInk/benchInkDim/benchFaint/castShadow/cable*`), `benchScheme()`,
  `AccentId` + `accent()`, `drawIslandCastShadow` / `drawChassisBody` split,
  themed rail/scrollbar/jack/cable, `LeverToggle` clamp + lit-rim material +
  `preferredWidth`, `PowerControl::preferredWidth`, the `ModeSwitch` rebuild
- `native/src/PedalCard.h/.cpp` — `PedalFace::accent` is a token; `applyTheme()`;
  body-only chassis paint
- `native/src/PluginEditor.h/.cpp` — dark-mode listener, `setThemeMode`,
  `applyTheme`, the top-bar theme chip, cluster sized from the widgets'
  preferred widths, mode-switch width, pedal cast shadows in `paintBoardContent`,
  theme-resolved bench/veil/caption colours
- `native/tools/editor_snapshot.cpp` — both themes × four voices + a board shot per
  theme, with `persist=false` so photographing never edits the user's setting
- `docs/work/2026-07-31-native-visual-pass-2.md` (deferred list), `CLAUDE.md`,
  this file

## Deferred to next session

- **Auto is light on Linux** — `Desktop::isDarkModeActive()` has no X11
  implementation in JUCE 8.0.4. macOS/Windows follow the OS properly; a Linux
  reader (XSETTINGS / `gtk-application-prefer-dark-theme`) would be its own slice.
- The combo pills, popup menu and chip buttons still paint on the pinned-dark
  chassis tokens in both themes (they read as small dark islands, deliberately) —
  if the owner wants them bench-toned in light theme that is a separate decision.
- The dark-theme scrollbar thumb is the brightest thing on the bench at rest; it
  is legible but could take a quieter alpha.
- Everything pass 2 deferred and this pass did not take: sfx in hosted wrappers,
  the web's per-amp jewel hot-spot colour triples, the native tuner, duplicate
  pedal instances.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
