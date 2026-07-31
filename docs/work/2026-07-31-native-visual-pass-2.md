# Native visual pass 2 — amp switch draw order, web-oracle skeuomorphism, UI sfx

**Date:** 2026-07-31
**Branch:** claude/native-visual-pass-6f557i
**Roadmap item:** owner field report 2026-07-31 ("still draw order issues, on amp on/off
switches. there aren't sfx matching the web either. the web designs have nicer/more
detailed skeuomorphic design on the switches, knobs, and buttons — use the web version
as the golden oracle. keep the horizontal aspect of the native version though")

## Goal

The native editor's amp controls match the web's design (the golden oracle) in
detail and in draw order, the pedal LED / power jewel halos render as smooth round
glows instead of clipped squares and banded discs, and the web's two UI sounds
(knob tick, footswitch thunk) exist in the native Standalone. Layout stays
horizontal.

## What the before-screenshots show (headless snapshot tool, Xvfb)

- **The amp POWER jewel's glow is clipped to a hard square** — the jewel sits at
  the top edge of its component and the glow (~3× the jewel radius) is cut by the
  component bounds on three sides. This is the reported "draw order issue".
- **The rocker below it is squished** — the Bright/Cab/Power cluster gets
  `max(cellH, 84)` px of height; the web rocker alone is 64 px, plus jewel band
  and caption ≈ 130 px. The rocker renders as a sliver.
- **Pedal LED halos band** — `drawJewel` stacks four alpha-0.16 discs, which
  compounds to visible concentric steps rather than a smooth falloff.
- **The native amp is painted on the DARK chassis, but the web amp is a LIGHT
  bench-style panel.** The web's dark-token pinning is `.pedal`-scoped; the amp's
  knob/lever/rocker/mode recipes resolve to the LIGHT theme tokens
  (`--cap: #EFEDE8→#D9D6CF` etc.). This is the root of "the web is more
  detailed": porcelain neumorphism carries far more visible sculpting than the
  same recipe on charcoal. Web oracle screenshots captured (Playwright, served
  dist) confirm it.
- **No native UI sfx** — the web plays `tick()` (2.1 kHz, 15 ms, on 0.04 knob
  detents) and `thunk(down/up)` (95/120→45 Hz sine, 60–90 ms) from
  `web/src/ui-sound.ts` on every knob detent and switch press.

## Approach

UI-only slice: zero DSP change, zero chain change, goldens untouched, no WASM
rebuild (core/ and web/worklet/ untouched).

1. **Token schemes.** `skin::Scheme` carries the token set (cap, cap-edge, well,
   ink triplet, sh triplet, arc track, panel gradient); two instances translated
   verbatim from tokens.css — `darkIsland` (the pedal pinning, today's values)
   and `lightBench` (the `:root` light values). Recipes become scheme-aware.
2. **The amp goes light** (web parity): the amp card paints with the web `.raised`
   recipe (`--panel-grad` + 10/10/24 dark + −10/−10/22 light + inset top light),
   and amp knobs/levers/rocker/mode switch paint in the light scheme per
   amp.css. Pedals stay dark (they match the web already). Input card stays dark
   (native-only element, chain identity).
3. **drawJewel rewritten**: two-layer smooth radial glow (the web's
   `0 0 16px + 0 0 5px` accent-glow, alpha .5) via radial gradients — no banding,
   and callers pass bounds with glow head-room. Hot spot at 35 %/30 % toward
   white, dark rim (the web's per-amp radial recipe, derived from the accent).
   Off state gets the web's inset shadow.
4. **PowerControl re-laid-out**: glow padding inside its bounds, full 46×64
   rocker with the web's half-inset shading, caption below; the cluster in
   `layoutAmpCard` gets the height this actually needs (~140 px) instead of
   `max(cellH, 84)`.
5. **LeverToggle**: lever cast shadow + inset light rim per `.t-lever`, and the
   web's 160 ms overshoot slide (`cubic-bezier(.34,1.56,.64,1)`) via a Timer.
6. **ModeSwitch**: per-segment carved wells (each `.mode-opt` is its own inset
   well; only the stack's outer corners round), active segment RAISED and lit
   (cast shadow + accent gradient), exactly the web geometry (78 px wide
   segments).
7. **Knobs**: the missing light counter-shadow (`-5px -5px 12px sh-light`), the
   cap's own cast shadow onto the skirt (`2px 3px 6px sh-darker`), scheme-aware
   colors. NeuKnob gains `setScheme()`.
8. **Round stomp**: the light counter-shadow (`-7 -7 16 sh-light`) — subtle on
   the dark chassis, per the oracle.
9. **UI sfx**: `native/src/UiSound.{h,cpp}` — the two ui-sound.ts voices
   synthesized sample-exactly (square 2100 Hz ×0.015 exp-15 ms; sine 95/120→45 Hz
   ×0.12 exp-90 ms), played through a lazily-opened output-only
   `AudioDeviceManager`. **Standalone only** (wrapperType check): a plugin must
   not open a second audio device inside a DAW. Wired: NeuKnob detent crossings
   (0.04, the web's constant), Footswitch/LeverToggle/PowerControl/ModeSwitch/
   ChipButton press (down) + release (up).

## How this will be measured

The headless snapshot tool (already in-tree: `clipper_editor_snap` under Xvfb),
before/after per amp voice: the glow renders round and unclipped, the rocker
renders at full height, the amp face reads as the web's light panel. Side-by-side
against the web oracle screenshots captured from the served dist. Sfx: measured
by ear in the Standalone (container has no audio device; the synthesis constants
are ported verbatim and unit-visible in the code).

## Manual test steps

- [ ] Standalone: amp face is the light panel; knobs porcelain with accent arcs;
      BRIGHT/CAB levers slide with a spring; POWER shows a full rocker, jewel
      glows round; mode switch segments carve/light correctly on Clean 120
      (3-state) and Twin (2-state)
- [ ] Pedal LEDs: smooth round halos on every pedal type
- [ ] Sfx: knob drag ticks at detents; every switch/stomp thunks (press + release)
- [ ] Edge: amp off dims knobs/arcs on the light panel legibly; window at minimum
      size (1040×560) — cluster and knobs still fit; plugin build (VST3) silent
      on sfx by construction
- [ ] Regression: pedals unchanged vs before-screenshots except LED halos

## Out of scope for this session

Native dark theme (still light-bench only), the tuner, per-pedal drag sfx
variations, web changes of any kind, DSP of any kind.

---

## What actually happened

As planned, with one finding that reframed the "more detailed" half of the report:
the web amp's controls are not merely more detailed — they resolve a DIFFERENT
token context. The dark pinning is `.pedal`-scoped, so the web amp is a light
porcelain panel with light knobs/rocker/levers/mode segments, while the native
amp had been translated onto the dark chassis where the same sculpting recipes
barely read. The fix is structural: `skin::Scheme` (light/dark token sets,
translated verbatim from tokens.css) and the amp painting entirely in
`lightBench()` — knobs (`NeuKnob::setScheme`), the card (`drawBenchCard`, the
web `.raised` recipe), levers, rocker, jewel, mode switch, divider, captions.
Pedals stay `darkIsland()` (they matched the web already).

The clipped-glow bug was two bugs: the jewel drawn at its component's top edge
(JUCE clips paint to bounds → square halo) and the cluster given `max(cellH,84)`
px when the web anatomy needs ~132 (jewel band + 9 px gap + 46×64 rocker +
caption) — the rocker rendered as a sliver. `PowerControl::preferredHeight()`
now feeds the layout, `skin::glowSpread()` documents the head-room contract, and
`drawJewel` is rewritten as two smooth radial-gradient layers (the web's
`0 0 16px + 0 0 5px`) replacing four stacked alpha discs that compounded into
banded steps — this also fixed the pedal LEDs' halos everywhere. First glow
attempt was too hot and still square at the bounds (screenshot-driven fix:
spread 1.1×→0.8× diameter, falloff from the jewel edge, alphas matched to the
oracle).

The web sfx exist natively: `UiSound` synthesizes ui-sound.ts's two voices with
the constants carried verbatim, through its own lazily-opened output-only device
— Standalone only (`JUCEApplicationBase::isStandaloneApp()`), silent no-ops in
any hosted wrapper, self-disabling when no audio device exists. Wired: knob
detent ticks (0.04, only while the user is dragging — host/preset updates stay
silent), press+release thunks on Footswitch/LeverToggle/PowerControl/ModeSwitch/
ChipButton. The Bright/Cab levers also gained the web's 160 ms overshoot slide
(easeOutBack ≈ cubic-bezier(.34,1.56,.64,1)); the first APVTS sync snaps so a
window opening on a saved session doesn't play the animation.

## Measured results

Headless before/after screenshots (`clipper_editor_snap` under Xvfb, all four amp
voices + the small/large parity shots), before kept beside after in the session
scratchpad and embedded in the PR: the POWER jewel renders a round halo (was a
square-clipped block), the rocker renders its full 46×64 with both sculpted
halves (was a sliver), the amp face is the web's porcelain panel with accent
arcs, mode segments carve individually with the active one raised+lit, pedal LED
halos are smooth (banding gone). Minimum-window (1040×560) layout verified: knob
rows wrap and the cluster fits. Native tests: `clipper_identical_core` +
`clipper_chain_edit` pass; full plugin + Standalone + snapshot tool compile with
zero warnings-as-errors. Core/web untouched (git status: native/ + docs only) —
no WASM rebuild required, goldens untouched by construction.

## Files created / modified

- `native/src/ClipperLookAndFeel.h/.cpp` — skin::Scheme (darkIsland/lightBench),
  drawBenchCard, drawWell(scheme), glowSpread + drawJewel rewrite, scheme-aware
  knob with dual body shadow + cap cast shadow, Footswitch light counter-shadow +
  thunks, LeverToggle slide + light recipe, PowerControl full web anatomy +
  preferredHeight, ModeSwitch per-segment wells + raised active, ChipButton thunks
- `native/src/UiSound.h/.cpp` (new) — the web ui-sound.ts port, Standalone-only
- `native/src/PluginEditor.cpp` — amp card → drawBenchCard, amp knobs → light
  scheme, cluster sized from PowerControl::preferredHeight, lever tops aligned to
  the jewel, light-scheme divider/captions/wordmark (Clean 120 wordmark = ink,
  the accent voices keep their wink, per web .amp-name)
- `native/CMakeLists.txt` — UiSound.cpp added to the shared target
- this plan file

## Deferred to next session

- Native dark theme (still light-bench only; the Scheme struct now makes it a
  data problem rather than a rewrite)
- Sfx in hosted wrappers (deliberately disabled — would need a host-safe output
  strategy, and DAW users may not want UI sounds at all)
- The web's per-amp jewel hot-spot colour triples (#FFF0C8 etc.) are approximated
  by accent-derived mixing; exact triples if the owner sees a difference
- The tuner (still web-only), duplicate pedal instances (unchanged)

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
