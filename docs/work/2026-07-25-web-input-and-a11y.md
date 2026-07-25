# Web input & accessibility cluster (audit UI/UX)

**Date:** 2026-07-25
**Branch:** fix/web-input-and-a11y
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` → **UI/UX** section (the web half of the
Accessibility / Contrast bullets plus the three loose ends in the final bullet). Docs section **§38**;
ADR **010** if one is needed.

## Goal

Every web control the audit named as mouse-only, invisible to assistive tech, unreadable, or burning
work it doesn't need is fixed and has a Playwright test that would fail if it regressed — with the
contrast and rAF claims carried by measured numbers, not adjectives.

## Approach

Seven findings, grouped by the file they live in. All of it is `web/src/**` + `web/tests/**`; **nothing
under `core/`, `web/worklet/`, `native/` or `web/public/generated/` is touched**, so no WASM rebuild is
required and the artifact stamp stays valid. Two sibling slices own `web/src/App.tsx`, `web/src/rig.ts`
and `web/src/assistant/` — this slice stays out of all three.

1. **Knob** (`web/src/components/Knob.tsx`, `web/src/styles/pedal.css`)
   - Focus ring: `.knob:focus-visible` with `var(--ai)`, matching the `.fsw` / `.toggle` / `.power`
     pattern already in the tree (2 px, offset 4 px). No new colour.
   - Fine adjust: hold **Shift** during a drag → 5× the travel per unit (1.6 px/unit → 8 px/unit).
     Implemented as an **incremental** drag (accumulate `dy` since the last move) rather than an
     anchored one, so pressing/releasing Shift mid-drag doesn't make the value jump. Shift also
     applies to the wheel and the arrow keys (fine step 0.01).
   - Keyboard: `Home` → 0, `End` → 1, `PageUp`/`PageDown` → ±0.2 (the ARIA slider convention: a
     larger step than the arrows' 0.05).
   - `role="slider"` + live `aria-valuenow`/`aria-valuetext` are *claimed* by CLAUDE.md — verify by
     test that they are actually present and actually update, rather than assuming.
2. **Popover menus** (`web/src/components/Menu.tsx` — new; `Board.tsx` call sites)
   One shared `<Menu>` that owns the trigger button and the popover, and provides Escape-to-close
   (focus restored to the trigger), pointer-outside-to-close, focus-into-the-menu on open, and a
   Tab/Shift+Tab **focus trap**. Three call sites in `Board.tsx`: pedal swap, gear tray, amp/cab menu.
   Keeps every existing `data-testid` and `aria-*` attribute so the current suites still pass.
3. **"Upload IR…"** (`Board.tsx`) — the `<label>` wrapping a `display:none` input becomes a real
   `<button type="button" role="menuitem">` that forwards to a hidden input rendered **outside** the
   conditional menu (so the pending file dialog can't be unmounted). The input keeps
   `data-testid="cab-upload-input"` because `cab.spec.ts` drives it with `setInputFiles`.
4. **Contrast** (`web/src/styles/tokens.css`, `web/src/styles/pedal.css`)
   `.pedal` pins its interior token context to the dark-theme values but leaves `--accent-*` (and
   `--chassis-tint`) on the page theme. Fix the same way the native tree did for GOLD
   (`native/src/ClipperLookAndFeel.h:86-93`): the accents that paint on a pinned-dark chassis take the
   **dark-theme token values on every theme**. Mechanically: tokens.css grows one theme-invariant
   `--accent-*-on-dark` / `--led-*-on-dark` / `--seg-green-*-on-dark` set (the dark values, declared
   once and referenced by the dark-theme blocks so there is no duplicated literal), and `.pedal` pins
   `--accent-*` to it. **No hardcoded colour outside tokens.css.**
   *Expected to be insufficient on its own for the RAT* — pre-measured at 4.11:1 even with the dark
   accent, because the light theme's `--chassis-tint` also isn't pinned, making the "dark island"
   measurably lighter in light theme than in dark. So pin `--chassis-tint` too, which makes the
   chassis byte-identical across themes — which is what the doctrine in pedal.css already claims.
5. **Chat** (`web/src/components/Chat.tsx`) — `role="log" aria-live="polite"` on the transcript, and
   auto-scroll only when the view is already pinned to the bottom. Pinned-ness is tracked in a **ref**
   updated from `onScroll` (never React state at scroll rate).
6. **Tuner** (`web/src/components/Tuner.tsx`) — gate the rAF loop on `engaged`; when it stops, run one
   final settle pass so the meter parks dark instead of freezing mid-sweep.
7. **`Board.tsx` `center()`** — a `display:none` jack yields an all-zero `DOMRect`, which the current
   null check accepts. Reject a rect with zero width *and* zero height.

## Steps

- [ ] Plan file + branch off `origin/main`
- [ ] `web/src/styles/tokens.css`: add the theme-invariant `*-on-dark` accent set; point the two
      dark-theme blocks at it (no behaviour change in dark theme — same literals, one declaration)
- [ ] `web/src/styles/pedal.css`: `.pedal` pins `--chassis-tint`, `--led*`, `--seg-green*` and every
      `--accent-*` to the on-dark set; add `.knob:focus-visible`
- [ ] `web/src/components/Knob.tsx`: focus ring markup (none needed — CSS only), Shift fine adjust,
      Home/End/PageUp/PageDown, incremental drag
- [ ] `web/src/components/Menu.tsx`: new shared popover (Escape / outside / trap / focus restore)
- [ ] `web/src/components/Board.tsx`: adopt `<Menu>` at three sites, real Upload-IR button, zero-rect
      guard in `center()`
- [ ] `web/src/components/Chat.tsx`: `role="log"` + `aria-live`, pinned-only auto-scroll
- [ ] `web/src/components/Tuner.tsx`: engaged-gated rAF
- [ ] `web/tests/a11y.spec.ts`: new spec (see below)
- [ ] Docs: `docs/DEVELOPMENT.md` §38, `CLAUDE.md` Current State, this file's post-session sections

## How this will be measured

- **Contrast** — computed, not eyeballed, and computed from the **shipped stylesheet** rather than from
  my arithmetic: the new spec reads `getComputedStyle(kval).color` and
  `getComputedStyle(pedal).backgroundImage` in the real page, alpha-composites the
  `--chassis-tint` layer over the `--panel-grad` layer exactly as the browser paints it, and computes
  the WCAG 2.x relative-luminance contrast ratio. Reported per readout, per theme, before → after,
  against the **4.5:1** bar for normal text (`.k-val` is 11 px/600 — not large text).
- **Tuner rAF** — `requestAnimationFrame` is counted in-page over a fixed 1 s wall-clock window with a
  tuner on the board, disengaged then engaged. Reported as callbacks/second before → after.
- **Knob fine adjust** — the value delta produced by an identical 40 px drag with and without Shift,
  reported as px-per-unit before → after.
- Everything else is a pass/fail behavioural assertion in the new spec (below).

## Manual test steps

- [ ] Happy path: Tab to a knob — a visible ring appears. Arrow keys move it 5 %, PageUp/PageDown
      20 %, Home/End slam to 0/100. Drag it: coarse. Hold Shift and drag: fine.
- [ ] Open the amp menu with the keyboard, press Escape — it closes and focus is back on "Change amp".
- [ ] Open the gear tray, click on the board background — it closes.
- [ ] Tab through an open menu — focus stays inside and wraps.
- [ ] Tab to "Upload IR…" and press Enter — the file picker opens (it was mouse-only before).
- [ ] Flip to light theme with an SD-1, Screamer, Pi fuzz, phaser and GOLD on the board — every value
      readout is legible on the dark chassis.
- [ ] Scroll the chat transcript up and send a message — the view is **not** yanked to the bottom.
      Then scroll to the bottom and send another — it does follow.
- [ ] Edge case: shrink the window below 760 px with pedals on the board — no cable runs off-screen.
- [ ] Edge case: engage the tuner mid-drag of a knob (drag still tracks; no rAF starvation).
- [ ] Edge case: hold Shift, drag a knob to the very top of its range, release Shift, keep dragging up
      — the value must not jump when the modifier changes.

## Out of scope for this session

- Undo for Remove / Swap, and named presets (the audit's other UI/UX bullets — their own slice).
- `AbortSignal` for assistant turns (touches `web/src/assistant/`, owned by a sibling slice).
- Every native-side UI/UX finding (`ClipperLookAndFeel`, footswitch mouse-down, repaint storms,
  `NeuKnob::setName` shadowing) — `native/` is off-limits here.
- The tone-knob taper finding (that is DSP, `core/`).
- `web/playwright.config.ts`'s `retries: 2` (deliberately not changed unilaterally — docs §29).

---

## What actually happened

Implemented as planned, with four deviations and two things found that the audit did not name.

**Deviation 1 — the contrast fix needed the chassis pinned, not just the accents.** Pinning
`--accent-*` to their dark-theme values (the native tree's fix for GOLD, generalised) clears five of
the six affected pedals but leaves the RAT readout at **4.11:1**, still under the 4.5:1 bar. The cause
is a *second* unpinned token: `--chassis-tint` is `rgba(46,52,64,.16)` in light theme and
`rgba(6,8,12,.36)` in dark, so the "dark island" composites to `#2A2D34` on the light bench and
`#1C1F24` in dark theme — the chassis the doctrine calls identical is measurably 6 RGB steps lighter
in light theme. Pinning `--chassis-tint` as well makes the pedal chassis byte-identical on both
themes (which is what `pedal.css`'s own comment already claims) and takes the RAT to **4.93:1**.

**Deviation 2 — a third unpinned token, `--ai`.** The focus ring the audit asked for reads `--ai`,
which is also not pinned by `.pedal`, so the ring itself would have landed at **2.83:1** on the dark
chassis (the 3:1 non-text bar). Pinned to the on-dark blue it measures **6.69:1**. Found by measuring
the ring rather than by assuming that adding one was sufficient.

**A thing I expected to be a problem and it was not.** I planned for the per-type
`.pedal[data-pedal-type=…] { --pedal-accent: var(--accent-sd) }` declarations needing to move below
the pinning block, on the theory that `var()` substitution would already have resolved against
`:root`. It resolves against **the element that declares the property**, and those selectors match
the *same element* as `.pedal`, so the pin reaches them and stylesheet order is irrelevant. Worth
recording because the reasoning would have been correct had `--pedal-accent` been declared on an
ancestor (docs §38.1).

**Deviation 3 — the drag had to become incremental.** The anchored form
(`startV + (startY - clientY)/RANGE`) cannot express a mid-drag sensitivity change: the instant Shift
goes down, the same pointer offset means a different value and the knob jumps. Accumulating
`dy / range` per move (with `lastY` advanced each time) makes the modifier a live sensitivity control
with no discontinuity, at the cost of clamping at the ends being "sticky-return" rather than
"remember the virtual overshoot" — the standard trade, and the behaviour most hardware-style knobs use.

**Deviation 4 — no ADR.** Nothing here is an architectural decision in the ADR sense; the contrast fix
is an application of an existing, already-recorded decision (the dark-island chassis) to the tokens it
had missed, and the rest is straight defect repair. **ADR 010 is left unused** and is still free for
whoever needs it.

**Found, not in the audit (1) — the knob focus ring the audit asked for is not sufficient on its own.**
`.knob` is a `display:flex; flex-direction:column` box with no border radius, so a plain
`outline`/`outline-offset` paints a hard rectangle straight through the neighbouring knobs' arcs at the
board's 8 px gaps. Fixed with a `border-radius` on the focus ring and a 3 px offset so the ring hugs
its own knob.

**Found, not in the audit (2) — the amp/cab menu's file input was inside the conditional menu.** Any
state change that closed the menu while the OS file dialog was open would unmount the `<input>` mid-
dialog and silently drop the selection. Moving it out of the `{ampMenuOpen && …}` block was required
for the keyboard-reachable button anyway, and it removes that race as a side effect.

**Found, not in the audit (3) — the light theme's pedal chassis was not the dark theme's.** See
Deviation 1: `--chassis-tint` differs per theme, so the "dark island" composited six RGB steps lighter
on the light bench than in dark theme, contradicting `pedal.css`'s own comment. Now identical.

**Found, not in the audit (4) — `tuner.css:175-180` hardcodes two colours** (`#d8863a` flat,
`#3e8fd6` sharp) in direct violation of the no-hardcoded-colour rule. They are theme-independent and
land on the pinned-dark chassis, so they are not broken, just not tokens. Deliberately left alone:
naming two new tokens is a palette decision, not a defect fix.

**A note on one claim I could not reproduce as stated:** the audit says the stray patch cable renders
"from off-screen **below** 760 px". The zero-`DOMRect` bug is real and reproduces exactly, but the
stray cable renders from *above and left* of the board — `center()` on a `display:none` element
returns `{-brect.left, -brect.top}`, i.e. the viewport origin expressed in board coordinates, which is
above the board, not below it. 760 px is the breakpoint, not the direction. The defect stands; the
direction in the audit text does not.

**Not caused by this slice, but observed:** `origin/main`'s Playwright suite is currently **red** on
`audio.spec.ts` `muff worklet` and `gold worklet` (and `TS worklet` intermittently). Confirmed
pre-existing by stashing this slice's `web/src` and re-running on a clean tree, and confirmed **not**
caused by the Muff Newton early-out (#12) by rebuilding against the pre-#12 committed
`clipper.js`/stamp — both still fail. The Muff renders an RMS of exactly **0** for the −30 dBFS leg of
its dynamics test (`hotRms/softRms = Infinity`), and the GOLD failure is a level-ratio assertion, so
this looks like the documented Chromium `OfflineAudioContext` silence behaviour rather than a DSP
regression — but it is not a flake in the "passes on retry" sense: it failed all three attempts.
Flagged for whoever owns the audio suite.

## Measured results

### 1. Contrast — pedal value readout (`.k-val`, 11 px/600 ⇒ WCAG AA normal text, 4.5:1)

Measured by `web/tests/a11y.spec.ts` from the shipped stylesheet in a real page: `color` from
`getComputedStyle`, background alpha-composited from the two paint layers the browser reports.

**Effective LIGHT theme** — chassis before `rgb(42,45,52)`→`rgb(36,39,45)`, after
`rgb(28,31,36)`→`rgb(24,27,31)`:

| pedal readout | before | accents pinned only | **after (accents + chassis)** | AA 4.5:1 |
|---|---|---|---|---|
| RAT | **3.51:1** | 4.11:1 ✗ | **4.93:1** | pass |
| SD-1 | **4.28:1** | 8.98:1 | **10.77:1** | pass |
| Screamer (ts) | **3.99:1** | 6.83:1 | **8.19:1** | pass |
| Pi fuzz (muff) | **2.15:1** | 5.27:1 | **6.32:1** | pass |
| Phaser | **3.32:1** | 5.93:1 | **7.12:1** | pass |
| GOLD | **2.78:1** | 6.95:1 | **8.34:1** | pass |
| Tuner | 5.88:1 | 7.88:1 | **9.46:1** | pass |

Six of seven readouts were below AA; the worst (Pi fuzz) was at **2.15:1**, less than half the bar.

**Effective DARK theme** — unchanged to the digit, as intended (the pinned values *are* the dark-theme
values, so this is a no-op there):

| pedal readout | before | after |
|---|---|---|
| RAT | 4.93:1 | 4.93:1 |
| SD-1 | 10.77:1 | 10.77:1 |
| Screamer | 8.19:1 | 8.19:1 |
| Pi fuzz | 6.32:1 | 6.32:1 |
| Phaser | 7.12:1 | 7.12:1 |
| GOLD | 8.34:1 | 8.34:1 |
| Tuner | 9.46:1 | 9.46:1 |

Also measured, and the reason a focus ring alone was not enough:

| element | bar | before | after |
|---|---|---|---|
| knob focus ring (`--ai`) on the chassis, light theme | 3:1 | **2.83:1** | **6.69:1** |

### 2. Tuner rAF — wasted frames while disengaged

Counted in-page over a 1000 ms window with a tuner on the board:

| tuner state | before | after |
|---|---|---|
| **disengaged** | **61 callbacks/s** | **0 callbacks/s** |
| engaged | 59 callbacks/s | 57 callbacks/s |

### 3. Knob fine adjust

| gesture | before | after |
|---|---|---|
| drag, no modifier | 1.6 px per 1 % (160 px full sweep) | 1.6 px per 1 % (unchanged) |
| drag, **Shift** held | 1.6 px per 1 % (no fine mode existed) | **8.0 px per 1 %** (800 px full sweep) |
| arrow key | 5 % | 5 % |
| **Shift** + arrow | 5 % | **1 %** |
| PageUp / PageDown | *no handler* | **±20 %** |
| Home / End | *no handler* | **0 % / 100 %** |

A 40 px drag therefore moves the knob 25 % coarse and 5 % fine — the same gesture now resolves a
5× finer setting.

### 4. Chat auto-scroll

Reading back at `scrollTop 0` with the bottom at 1732:

| | before | after |
|---|---|---|
| `scrollTop` after a new assistant turn arrives | **1732** (yanked to the bottom) | **0** (stayed put) |
| `scrollTop` when already at the bottom | bottom | bottom (still follows) |

### 5. Behavioural properties newly pinned by tests, and their teeth

`web/tests/a11y.spec.ts`, **15 tests, all green** on this branch. Run against the pre-fix source
(`git stash push -- web/src`, spec retained, rebuild, rerun): **14 of the 15 failed.** The single
pass is the *dark-theme* contrast test, which is the correct outcome — the contrast defect was
light-theme only, so a test that also failed in dark theme would have been measuring the wrong thing.

Full local gate: `cd web && npm run build` clean (includes `tsc --noEmit`); the artifact staleness
check reports "in step with 65 hashed inputs" (nothing under `core/` or `web/worklet/` was touched, so
no WASM rebuild); the whole Playwright suite run — the only failures are the three pre-existing
`audio.spec.ts` DSP tests described above, which fail identically on a clean `origin/main`.

## Files created / modified

- `web/src/components/Knob.tsx` — Shift fine adjust (drag + wheel + arrows), Home/End/PageUp/PageDown,
  incremental drag accumulator
- `web/src/components/Menu.tsx` — **new**: the shared popover (Escape, outside-pointer, focus trap,
  focus restore)
- `web/src/components/Board.tsx` — three menus adopt `<Menu>`; Upload IR becomes a real button with a
  hoisted file input; `center()` rejects a zero `DOMRect`
- `web/src/components/Chat.tsx` — `role="log" aria-live="polite"`; pinned-only auto-scroll via a ref
- `web/src/components/Tuner.tsx` — rAF loop gated on `engaged`, with a settle pass on disengage
- `web/src/styles/tokens.css` — theme-invariant `*-on-dark` set (accents, `--led*`, `--ai`,
  `--chassis-tint`, the whole tuner `--seg-*` group); both dark-theme blocks now reference it, so each
  colour has exactly one literal in the tree
- `web/src/styles/pedal.css` — `.pedal` pins `--chassis-tint` / `--ai` / `--led*` / `--seg-*` /
  `--accent-*` to the on-dark set; `.knob:focus-visible`
- `web/src/styles/board.css` — `.vh-input` (the hoisted, clipped file input)
- `web/tests/a11y.spec.ts` — **new**: 15 tests (contrast per pedal per theme + the focus ring, knob
  keyboard contract / fine adjust / no-jump-on-modifier-change, Escape / outside-pointer / focus trap
  on the popovers, Upload IR reachable-named-and-actually-wired, chat live region + scroll pinning,
  tuner rAF, zero-rect cable)
- `docs/DEVELOPMENT.md` — §38
- `CLAUDE.md` — Current State
- `docs/work/2026-07-25-web-input-and-a11y.md` — this file

## Deferred to next session

- **Undo for Remove / Swap** (audit UI/UX, still open). Everything in this slice is reversible;
  Remove/Swap are not, and the autosave has already committed by the time the player notices.
- **`runAssistant` has no `AbortSignal`** — cancelling a turn mid-flight, and not dropping focus to
  `<body>` while `disabled`. Lives in `web/src/assistant/`, owned by a sibling slice this session.
- **Menu items are Tab-navigated, not arrow-navigated.** `role="menu"` canonically wants arrow-key
  roving focus with a single tab stop. The trap makes Tab correct and safe, which is what the audit
  asked for; full roving focus is a follow-up.
- **The native half of every finding here** — `Footswitch`/`ChipButton`/`LeverToggle`/`PowerControl`/
  `ModeSwitch` are bare `juce::Component`s, and `NeuKnob::setName` shadows the non-virtual
  `Component::setName` so all 17+ native knobs are anonymous to assistive tech.
- **`.k-name` and `.pedal-model` / `.fsw-label` were not measured.** They read `--ink-dim` /
  `--ink-faint`, which `.pedal` already pins, so they are theme-stable — but nobody has computed their
  ratio. `--ink-faint` (#6a6f77) on the pinned chassis is around 3:1 and it is label text, not
  decoration; worth a deliberate look rather than a silent pass.
- **The two hardcoded tuner colours** (`tuner.css:175-180`) — needs a palette decision, not a fix.
- **`origin/main`'s `muff worklet` / `gold worklet` Playwright failures** — reproduced on a clean
  tree and against the pre-#12 engine, so neither this slice nor the Muff early-out. Needs an owner.

## Status

- [x] Complete
