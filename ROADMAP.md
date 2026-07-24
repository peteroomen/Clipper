# Clipper — Roadmap

*A web-first guitar rig simulator that does real audio modeling, paired with a
conversational AI assistant that helps you dial in tones by reasoning with you.*

This roadmap turns the project handoff into an ordered build plan. The guiding
rule, inherited from the handoff: **prove the modeling pipeline first; the
assistant layers on top of working audio.**

---

## Decisions (locked)

| Decision | Choice | Rationale |
|---|---|---|
| DSP core language | **C++** | `chowdsp_wdf` provides working wave-digital-filter building blocks (including diode clippers); the reference literature (Chowdhury, Werner, Yeh) is C++-adjacent; Emscripten→WASM is mature. Learning budget goes to DSP, not porting. |
| First circuit method | **WDF** (via `chowdsp_wdf`) | Maps directly onto component-level thinking, which matches the builder's analog circuit background. Nodal DK stays available for later circuits where WDF gets awkward (multiple interacting nonlinearities). |
| First pedal | **RAT-style diode clipper** (Mooer Black Secret) | One dominant nonlinearity, well-documented, ground truth on the builder's pedalboard. Proves the entire pipeline. |
| MVP amp | **Clean solid-state amp, modeled linearly** — JC-120-style tone stack + cab IR | A tube amp can't honestly be modeled as linear; the Roland JC-120 (mostly) can. Iconic clean pedal platform, so drive character comes from the pedal — exactly how the real rig works. ~15% of the effort of a nonlinear amp model. |
| Cabs | **Impulse response + convolution** | Don't model speakers from physics. |
| AI backend | **Deferred** | Decided at Milestone 6, when the assistant is actually built. Options on the table: thin serverless proxy vs. bring-your-own-key client-side. |

## MVP definition

The first *shippable* thing (end of Milestone 6):

1. **One pedal** — RAT-style distortion, three knobs (gain / filter / level), true-feeling bypass.
2. **One amp** — JC-120-style clean amp: linear tone stack (bass/mid/treble, volume) into a cab IR.
3. **AI chat** — conversational assistant that can read the rig state, change parameters via tool calls, explain *why*, and iterate ("still too saturated" → suggests rolling guitar volume back before touching a knob).
4. **Guitar profile** — user enters their guitar (e.g. "Strat, SSS") and the assistant's advice accounts for it. Nearly free to build; disproportionate payoff in how personal the advice feels.

Everything else — more pedals, nonlinear amp, presets, native — is post-MVP.

---

## Milestones

Ordered so that each one produces something runnable and de-risks the next.
Sizes are relative (S/M/L), not calendar promises.

### M0 — Walking skeleton *(S)*

Prove the whole toolchain before writing any real DSP.

- Repo layout: `core/` (portable C++, zero platform deps), `web/` (React/TS app), `web/worklet/` (AudioWorklet glue).
- CMake + Emscripten build producing a WASM module with **SIMD enabled** (needed later for oversampled nonlinear stages).
- An `AudioWorkletProcessor` (never ScriptProcessorNode) loading the WASM and processing audio on the audio thread.
- **Done when:** a sine wave (or looped guitar clip) passes through a trivial C++ gain function and comes out of the browser speakers, controlled by one slider.
- De-risks: build system, WASM/worklet plumbing, the C++↔JS parameter bridge — all the boring stuff that would otherwise contaminate the DSP milestones.

### M1 — Diode clipper core *(M)*

The first real model, developed *offline* — no browser in the loop.

- RAT-style clipper in the portable core using `chowdsp_wdf`: input gain stage → diode clipping → low-pass filter (the RAT's post-clipping filter) → level.
- A tiny native CLI harness that renders WAV in / WAV out, so iteration is fast and testable.
- Unit tests on known signals; spectrum plots to eyeball harmonic content.
- **Done when:** a DI guitar clip rendered through the model sounds recognizably like a RAT-family distortion (fizz allowed — that's M2's job).

### M2 — Antialiasing: make it musical *(M–L)*

The central technical challenge per the handoff. Budget most of the learning time here.

- Oversample the nonlinear stage 4–8× with proper decimation filters.
- Add **antiderivative antialiasing (ADAA)** on the clipping stage as the modern complement.
- Measure, don't guess: render swept sines, inspect spectrograms for foldback products, A/B against the real Black Secret.
- **Done when:** high-gain settings on high notes sound crunchy, not fizzy/inharmonic, and the aliasing products sit demonstrably below audibility in the spectrogram.

### M3 — Live in the browser *(M)*

- Wire the M2 clipper into the M0 worklet skeleton.
- Live input via `getUserMedia` (guitar → audio interface → browser), with the constraint set that disables echo cancellation / AGC / noise suppression (they destroy guitar signals).
- Parameter changes from the UI thread applied click-free (smoothing in the core).
- Test in Chrome first; note Safari quirks, don't block on them.
- **Done when:** you can plug in and play the modeled pedal live. Latency will be felt (~20–40 ms round trip) — that's acceptable and documented; native is the deferred answer, not more web engineering.

### M4 — Pedal UI *(S–M)*

- React rig board: one pedal with three knobs and a bypass footswitch.
- Rig state lives in a single serializable structure (chain + parameters). This is deliberate groundwork: it becomes the AI's tool-call surface in M6 and the preset format later.
- **Done when:** the pedal is playable and tweakable by a human, and the entire rig state round-trips through JSON.

### M5 — Clean amp + cab *(M)*

- JC-120-style amp block: input volume, bass/mid/treble tone stack modeled as linear filters (matched to the real amp's measured/published response), bright switch if it's cheap.
- Cab IR convolution (partitioned FFT convolution in the core); ship with one good open-license 2×12 IR, allow user IR upload later.
- Amp panel in the rig UI; signal chain is now guitar → pedal → amp → cab.
- **Done when:** bypassing the pedal gives a genuinely pleasant clean tone, and the pedal into the amp sounds like a rig, not a science experiment.

### M6 — The assistant *(L)* → **MVP ships**

The differentiator. Only started once audio is real, per the handoff.

- **Guitar profile:** simple form (model, pickup layout, position in use) stored client-side and injected into assistant context.
- **Tool-use schema:** the assistant reads rig state and mutates parameters through defined tools (`set_param`, `toggle_pedal`, `explain_current_chain`, …) — natural language → concrete parameter changes, never freeform.
- **Coaching behavior:** explains the *why*, iterates on feedback, and is willing to suggest non-knob answers ("roll your guitar volume back") — that's the product's soul, encoded in the system prompt.
- Circuit-level depth ("the RAT's filter is a low-pass *after* the clipping stage, so…") available when asked, never required.
- **Backend decision made here:** thin serverless proxy vs. BYO-key client-side.
- **Done when:** "give me a tighter rhythm tone, less saturated" produces sensible parameter changes *and* a sensible explanation, and iterating actually converges.

---

## Post-MVP roadmap (v1.x — ordered)

The MVP (M0–M6) shipped. Next arc: grow the board, then go valve.

### v1.0.x point releases (in flight)

Field feedback from real playing, prioritized ahead of new pedals:

- **M6.1 — RAT re-voice + gain staging** — true two-corner pre-clip response
  (≈60 Hz / ≈1.5 kHz, replacing the over-aggressive 320 Hz shelf), max gain to
  the real pedal's ≈+66 dB, rig-level input trim + peak meter (calibrate the
  interface to the model's 1 V reference), output-level pass with cab-IR
  makeup gain and a final safety limiter.
- **M6.2 — Mac app (Electron)** — desktop shell; the zero-dep proxy runs
  in-process serving the built web app, key via env or in-app prompt,
  unsigned local `.dmg` build.
- **M6.3 — JC-120 chorus & vibrato — SHIPPED.** The amp's soul: speed + depth
  knobs and an off/chorus/vibrato 3-way (chorus = dry L / modulated wet R stereo
  bloom; vibrato = modulated to both sides, true pitch wobble). The worklet goes
  **stereo at the amp stage**, with the cab IR running **per side**. Modulated
  delay in core (`ChorusModel`: 5 ms base, sine LFO ~0.15–8 Hz, up to 1.5 ms sweep,
  cubic-Lagrange fractional delay), offline-validated — vibrato pitch deviation
  measured against the depth×rate prediction, chorus dry-L/wet-R decorrelation,
  bit-exact off. Stereo chain runs at ~0.6 % of a core (44.1 k).
- **M6.4 — Pedalboard visual pass — SHIPPED.** The single pedal became a
  stackable, drag- and keyboard-reorderable **chain** (`RigState.pedals[]`,
  each instance handle-diffed in the worklet) joined by **neumorphic SVG
  catenary cables** (measured live, redraw on drag/resize), with add / remove /
  swap from a gear tray and the amp fixed at the end (amp-swap affordance).
  Chain edits are **click-free** via a raised-cosine output declick fade (swap at
  output-zero — no zipper, proven in an OfflineAudioContext test). Empty chain
  (guitar straight into the amp) works. The assistant addresses pedals by
  instance index and gained `add_pedal`/`remove_pedal`/`move_pedal`. Old
  single-`pedal` rigs migrate to a one-element chain. **No core/C-ABI change** —
  the RAT is already handle-based, so multiple instances stack safely.
  Architecture for everything M7+ plugs into.

- **M6.7 — Reverb** *(S)* — **SHIPPED** — the JC-120's missing spring. M5 shipped
  NO reverb; M6.7 adds an algorithmic **spring-flavored** reverb (`ReverbModel`,
  owned by `AmpModel`) in the authentic position (preamp → reverb → chorus split →
  per-side cabs, so the tail blooms in stereo) with a single **REVERB** MIX knob
  (`PARAM_REVERB=9`, default 0 = bit-exact dry). Compact Schroeder/Moorer network:
  10 ms predelay, 4 damped combs (fixed **RT60 ≈ 1.5 s**), 2 diffusers, a short
  "spring chirp" allpass cascade, and a 150 Hz–4.5 kHz transducer band-limit.
  Validated offline (RT60, band bounds, echo density, stability, stereo placement)
  at 44.1/48/96 k. See docs §16.
- **M6.7-2 — True dispersive spring** *(S)* — **SHIPPED** — replaced M6.7's
  Schroeder/Moorer comb bank with a Parker/Välimäki-style **dispersive waveguide**:
  two detuned feedback springs, each a **cascade of ~32 first-order allpass sections**
  (coefficient `a ≈ 0.74`) whose frequency-dependent group delay smears every echo
  into a **downward-swept chirp** ("boing") and **stretches the resonant modes** so
  they never stack into M6.7's metallic comb. Gentle in-loop damping (soft HF shelf +
  120 Hz cut) and a gentler 150 Hz–5.2 kHz transducer band replace M6.7's steep
  4.5 kHz in-loop lid (the "underwater" fix). Same **REVERB** MIX knob
  (`PARAM_REVERB=9`, bit-exact dry at 0), same position, **no UI/ABI change**.
  Validated at 44.1/48/96 k with the chirp-descent and mode-stretch assertions run
  against the OLD M6.7 as the A/B baseline (chirp **1.85× vs 1.04×**, mode stretch
  **3.3× vs 1.0×**). See docs §16.
- **M6.8 — Pedal visual identity pass — SHIPPED, doctrine revised in M6.8.1.**
  One sculpted neumorphic chassis family; the tuner goes **TC-style** (a segmented
  LED meter — recessed wells, red flat/sharp bar from center, green center lock —
  and a big **7-segment** note screen on a dark readout). The original per-pedal
  identity used **full-body enclosure tints** (amber SD-1, slate tuner); those
  were **revised out** in M6.8.1 (see below). **No core/C-ABI/worklet change.**
- **M6.8.1 — Visual doctrine revision — SHIPPED.** Review found the full-body
  tints broke the neumorphism ("real yellow breaks the neumorphism"). New recipe,
  *for every future pedal with a real-world analog:* **dark chassis for all** (one
  shared charcoal wash, the RAT's colour, which reads well on the lighter board)
  and reference the gear **subtly** via (1) a small-area **accent** colour on the
  knob value arcs + readouts + LED (RAT red, SD-1 **yellow**, tuner green), (2) one
  **morphology** cue (the SD-1's treadle — now dropped to the bottom of the body
  where it belongs, with knobs given real air), and (3) a knowing **name** (the
  RAT is "Rodent"; "Clipper" stays the *app* brand). Tuner header/rhythm spacing
  fixed (the lock LED no longer crowds the model line). Homage, never replica.
  A future pedal declares one `FACES` entry + an `--accent-*` token pair (docs
  §17). **No core/C-ABI/worklet change** — a pure web visual pass.


### M7 — Tuner *(S)* — **SHIPPED**

Not a modeling problem — pitch detection + mute. Chromatic needle tuner
(Polytune-style *polyphonic* mode is a much harder multi-pitch problem;
deliberately out of scope). No core/DSP/C-ABI change; all in `web/`. See
docs/DEVELOPMENT.md §12.

- **Detection** in the web layer via the McLeod pitch method (`pitchy`, the one
  tiny MIT dep) on a worklet tap of the **post-trim, pre-chain** raw guitar —
  zero core-DSP changes. **4096-sample frames** (~85 ms), chosen so a 7-string
  low B (B0 ≈ 31 Hz) locks reliably; measured <0.1 cents on the test tones.
  Detection runs on the main thread; frames are tapped only while a tuner is
  engaged (zero overhead otherwise). Lowest reliable note: **B0 (~31 Hz)**.
- **Pedal-format UI** in the design language: big note name, SVG cents needle
  (~60 fps), ±cents readout, **green-lock LED** (|cents| ≤ 3 held), footswitch
  = **mute** — the one DSP touch, a per-chain mute flag that reuses the M6.4
  raised-cosine declick so mute/unmute is click-free. Reference A=440.
- Joins the multi-pedal chain (tuner → dirt → amp) additively; the tuner
  instance round-trips through rig JSON (state = just `engaged`).
- Assistant learns `tune` awareness: `add_pedal type:'tuner'`, and the rig
  context reports the live note + cents so the coach can say "you're a few
  cents flat on the G."

### M8 — SD-1 Super Overdrive *(M)* — ✅ shipped

The essential second dirt box, and the perfect topology contrast with the RAT.
Details + validation numbers in `docs/DEVELOPMENT.md` §11.6.

- **Soft, asymmetric clipping in the op-amp feedback loop** (2 vs 1 diode —
  even-harmonic warmth) vs the RAT's hard shunt-to-ground clipping. Realised with
  an **asymmetric ADAA soft-clipper** (`AsymSoftClipper`) rather than WDF — the
  chowdsp diode pair is symmetric-only, and the SD-1's soft feedback limiter is
  captured cleanly by the tanh closed form + ADAA. All M1/M2 machinery
  (oversampling, ADAA infra, alias measurement, render CLI `--pedal sd1`) reuses
  directly; the 4558 op-amp reuses the M6.5 `LM308Stage` with 4558 values.
- The signature mid-hump input shaping (≈720 Hz, +46.6 dB plateau), first-order
  tone tilt, level. Three knobs (Drive / Tone / Level).
- Measured: mid-hump corner within 0.04 dB of analytic; 2nd harmonic −20.9 dBc
  (vs −152 dBc symmetric); knee 1.6× softer than the RAT; op-amp corner 14.0 kHz;
  4× alias margin −116 dB. Ground truth on the real board for A/B.
- Sets up the canonical M9 pairing: SD-1 boosting a cranked Marshall.

### M9 — First valve amp: Marshall JCM800 2204 *(L, phased)*

Chosen over alternatives (tweed Deluxe 5E3 = technically simplest; AC30 =
hardest) because: the preamp is one building block — a 12AX7 common-cathode
triode stage — repeated, so we build a single validated triode model and get
future amps mostly for free; it's exhaustively documented; and SD-1 → JCM800
is one of the most canonical pairings in rock.

1. **Triode stage module** ✅ *(phase 1 done — see docs §12, M9.1)* — a 12AX7
   common-cathode Koren-model stage with grid conductance + blocking distortion,
   per-sample nodal-Newton solve, validated standalone with the M2 measurement
   discipline (`clipper_triode_tests`): DC op point Va≈186 V / Iq≈1.34 mA vs the
   analytic load line; small-signal gain −64× bypassed / −41× unbypassed vs the
   `−gm·(RL‖rp)` formula; 2nd-harmonic-dominant asymmetric clip; blocking
   recovery ≈22 ms (Rgl·Cc); no NaN on ±10 V slam (≤8 Newton iters); cathode
   bypass shelf vs analytic; aliasing −140 dB at the shipped 4×. Rendered alone
   via `clipper-render --triode`. This module is 80 % of every future amp.
2. **Preamp** ✅ *(phase 2 done — see docs §14, M9.2)* — the full 2204 preamp:
   four 12AX7s (V1A/V1B/V2A common-cathode + a direct-coupled cathode follower V2B)
   into the passive Marshall TMB tone stack, GAIN + MASTER audio-taper pots. Built
   by composing the M9.1 TriodeStage (one additive `CathodeFollower` topology; the
   common-cathode path unchanged). Validated (`clipper_jcm800_tests`): per-stage DC
   op points vs load lines (V1B cold at 0.31 mA, follower bias solved to V2A's plate
   185.7 V, Rout 372 Ω); small-signal chain gain 19.5 dB vs the analytic product;
   the TMB vs analytic `H(jω)` within 0.04 dB with the classic 545 Hz mid notch;
   THD monotonic + asymmetric crunch with V1B driven past its cold-bias window;
   blocking recovery + ±10 V slam stability; **measured OS requirement 4×**
   (−73/−68 dB alias floor, clears the −60 dB M2 bar at max gain). Rendered via
   `clipper-render --jcm-pre`.
3. **Power section — where "responsive" lives** ✅ *(phase 3 done — see docs §18,
   M9.3)* — the 2204's 50 W push-pull EL34 power stage, composed into the full amp
   `Jcm800Amp` (preamp → power). A 12AX7 **long-tailed-pair** phase inverter (reusing
   the M9.1 Koren law, 3×3 nodal Newton, its own soft clip), a **class-AB EL34 pair**
   (Koren pentode; 38 mA/tube at a 467 V rail vs the analytic fixed point; even
   harmonics cancel vs a single-ended reference; crossover measurable at low drive),
   a linear **output transformer** v1, **global NFB** (−3.4 dB, sign-correct: closed
   < open) with **presence** (HF lift, +3.1 dB at 4 kHz), and measured **B+ sag**
   (3.4 dB depth, 10 ms bloom, recovery on the 7.5 ms supply RC). Validated
   (`clipper_jcm800_power_tests`) with an explicit NFB-inversion catcher; the power
   section clears the −60 dB M2 alias bar at 4× (composed max-gain floor ~−58 dB at
   48 k, measured, 8× no better). Rendered via `clipper-render --jcm` / `--jcm-cab`.
4. **Integration — the JCM joins the amp registry** ✅ *(phase 4 done — see docs
   §19, M9.4)* — `Jcm800Amp` wired in end-to-end as a second selectable amp voice
   alongside the Clean 120: the C ABI hosts both voices behind one `AmpChain` handle
   (realtime-safe `amp_set_model` int flip; shared cab pair; JCM mono head →
   dual-mono; explicit per-id param routing with gain/presence/master ids 10/11/12
   and shared bass/mid/treble; latency reported per voice), a declick-bracketed
   `ampModel` worklet swap, `rig.ts` additive `gain/presence/master` with migration
   + exact round-trip, the era-correct **Eight Hundred** JCM face (§17 doctrine: dark
   chassis + gold accent, PRESENCE·BASS·MIDDLE·TREBLE·MASTER·GAIN, no
   bright/chorus/reverb, brit412 hint on switch, never auto-switched), a `set_amp`
   assistant tool + SD-1-boost coaching, and the native JUCE plugin (APVTS Amp Model
   choice + JCM knobs). The **identical-core test is bit-exact on BOTH amp models**
   (0.0 diff, latency 336/624 matched); a Playwright perf smoke reports the jcm800
   WASM offline-render wall-time (~1.14× real-time, ~43× the linear clean amp in
   headless CI). ctest (7 core + native identical-core) green; 44 Playwright specs
   green; server 11 + history 10 unchanged.

**M9 COMPLETE** — the Marshall JCM800 2204 is a fully playable, selectable amp voice.

### M10 — Amp expansion (locked lineup, ordered)

The tube toolbox (TriodeStage, pentode push-pull, NFB/presence, measured sag,
MNA tone stacks, spring reverb) covers all of these. Tube order chosen so each
amp adds ONE new piece of machinery; the two solid-state amps are light,
independent slices that can interleave anywhere:

- **M10.1 — Fender Twin-style** ✅ *(shipped — docs §20)* — the clean benchmark,
  wired in end-to-end as the THIRD amp voice `twin`. New devices: the 6L6GC +
  12AT7 Koren fits and the reusable `OptoTremolo` (fast-attack/slow-release opto
  cell). `FenderToneStack` (blackface values, pre-gain, high-Z plate source, the
  deep mid scoop), 4× 6L6GC push-pull (balanced 12AT7 LTP PI, no presence), light
  sag, period-correct spring reverb, optical tremolo. Validated (`clipper_twin_tests`)
  at 44.1/48/96 k: DC op points, Fender notch vs analytic H(s), NFB sign+magnitude,
  tremolo asymmetry, reverb placement, clean-headroom + monotonic THD, sag < JCM,
  aliasing. Identical-core bit-exact across all three voices. *(Bonus this milestone:
  the JCM800 also gained a spring REVERB knob — usability over authenticity, docs §19.)*
- **M10.2 — Vox AC30-style top boost** ✅ *(shipped — docs §23)* — new power-amp
  physics, wired in end-to-end as the FOURTH amp voice `ac30`. New devices: a new
  EL84 Koren fit and CATHODE bias with real dynamics (the shared Rk∥Ck network — the
  bias cools under sustained drive → the class-A bloom, recovering on Rk·Ck), NO
  negative feedback (asserted open==closed bit-exact — the anti-NFB catcher), the
  interactive top-boost tone stack (gain loss + treble/bass interaction, MNA vs analytic
  H(s)), the post-PI TOP CUT (inverted sense, reuses the presence control slot), and a
  deep GZ34 tube-rectifier sag (ordering Twin < JCM < AC30, 4–8 dB window). Validated
  (`clipper_ac30_tests`) at 44.1/48/96 k: DC op points + cathode fixed point, top-boost
  H(jω), anti-NFB identity, cathode-bias shift + RC recovery, TOP CUT, sag ordering,
  the class-A chime (2nd harmonic > the Twin's), monotonic THD, aliasing (4× ships).
  Identical-core bit-exact across all four voices.
  *(Field fixes since: the CUT re-taper — "muddy" — and the §23 **second amendment**,
  "it breaks up less easy than the fender twin". The second one was a genuinely
  inverted voicing: the AC30's phase inverter was biased near cutoff (83 µA/triode,
  ×9.1 per leg), so the VOLUME knob — which on an AC30 IS the overdrive — never
  reached the EL84 grids and the class-A power section was never driven. Fixed in the
  gain structure (PI operating point + a passive interstage divider + the output
  normalization); breakup onset now VOLUME 0.5 vs the Twin's 0.9, 2.2× its THD at
  mid-knob, guarded permanently by `testBreakupOrdering`. ts_ac30 golden regenerated
  deliberately.)*
- **M10.3 — Orange tube-head style** *(S–M)* — heavy EL34/JCM800 machinery
  reuse with Orange voicing; quick win after 10.1–10.2.
- **M10.4 — Mesa high-gain style** *(L)* — hardest, last: 5+ cascaded stages
  with heavy interstage voicing, selectable TUBE-rectifier sag (the sag model
  parameterizes for it), loose/tight modes, graphic-EQ charm (5 biquads).
- **M10.5 — Crush-style solid-state practice amp** *(S)* — the user owns one:
  personal ground truth, like the RAT was. Op-amp clipping stages = existing
  machinery (op-amp model + ADAA). Includes its digital-reverb-and-all
  character judged against the real unit by ear.
- **M10.6 — RG100-style solid-state high gain** *(M)* — the metal SS legend
  (the "solid state can crush" existence proof). Op-amp/BJT clipping cascade,
  tight low end, famously scooped; pairs with the brit412 and a future
  straight-cab IR.

### M11 — Player Expectations Suite ✅ *(shipped — docs §26)*

The missing test layer between "the circuit math is right" and "a player plugs
in and it sounds right". Four field bugs (RAT "no balls", cab fizz, Muff Pi hum
blowout, AC30 "muddy") shipped while every circuit metric was green — M11 pins
what the PLAYER hears, permanently:

- **Universal gear invariants** (`clipper_player_expectations_tests`, ctest #15)
  over every dirt pedal and amp voice at the app's opening knobs: min-knob
  usability (audible floor −70 dBFS), the hum-torture standard (60 Hz hum
  ≥ 28 dB below the note at min AND default gain — a floor for all future gear),
  knob-monotonicity spot-checks (gain→THD, level→RMS, tone→spectral direction,
  incl. the inverted RAT FILTER / AC30 CUT), and per-gear default-level windows.
- **Live-convention testing**: every C-ABI entry point rendered in-place at
  128-frame blocks (the worklet's exact convention) vs separate-buffer
  big-block — bit-identical today, pinned forever (the in-place convolver-bug
  catcher).
- **Golden "first five minutes" renders** (five default rigs, 944 KB of 16-bit
  48 k WAVs) gated per third-octave band at ±1.5 dB — lossless refactors pass,
  voicing drift fails; regeneration only via `scripts/update-goldens.sh`.
- **Web sim** (`web/tests/expectations.spec.ts`): each pedal added + stomped at
  defaults, all four amp voices cycled — audible, NaN-free, level-sane,
  click-free. **Found + fixed a real latent bug:** the worklet stomp (pedal
  bypass / amp power) was the one topology change not declick-bracketed — it
  popped audibly; stomps now ride the shared raised-cosine declick.

### Pre-commercialization checklist (if that day comes)

Circuit modeling itself is legally safe (topologies aren't copyrightable;
the vintage patents are long expired — the whole amp-sim industry rests on
this). The real items, from an IP review of the current app:

- **Trademark scrub of user-facing surfaces**: the amp menu still says
  "JC-120 style"; "Super Drive" is uncomfortably close to Boss's "SUPER
  OverDrive" mark — rename both. Docs/code references to real gear are fine
  (descriptive); product UI and marketing must use our fantasy names only,
  plus the standard "trademarks of their respective owners" disclaimer.
- **Trade dress**: keep the M6.8.1 doctrine (homage never replica) — it is
  the correct legal posture, not just the correct aesthetic one.
- **JUCE commercial license** — the native shell dual-licenses; closed-source
  commercial distribution requires a paid JUCE tier. Budget for it.
- **Steinberg VST3 agreement** (free, but must be signed) for VST3 binaries.
- Everything else ships permissive: chowdsp_wdf (BSD), pitchy (MIT), dr_wav
  (public domain), React/Electron (MIT); IRs are synthesized in-house.
- Get an actual IP lawyer for an hour before launch. This list is homework,
  not counsel.

Note: VST3 + AU already ship from native/ (Logic hosts AU, not VST3 — the AU
is the Logic path and exists today).

### v1.1 gear candidates (validated, ordered by reuse)

External suggestions (Gemini) validated against the codebase; effort reflects
how much existing machinery each reuses:

1. **TS808 "Screamer"** *(XS)* — ✅ **shipped** (see docs §21). The SD-1 IS the
   Tube-Screamer topology, so the Screamer ships as a SEPARATE pedal (`ts`) that
   reuses ALL the SD-1 machinery via a shared, config-parameterized
   `OverdriveEngine` — the SD-1 refactored onto it byte-for-byte (M8 suite passes
   unchanged). The two differences are the config: SYMMETRIC 0.60 V diodes (2nd
   harmonic −159.6 dBc, ~absent, vs the SD-1's −20.9 dBc asymmetric warmth — the
   mirror) and a 500 kΩ drive pot (+40.6 dB max plateau, analytic, vs the SD-1's
   +46.6 dB). Green `slim`-face box; the canonical mid-forward clean-boost stacker.
2. **CE-2 chorus pedal** *(S)* — mono pedal re-voicing of the JC-120 BBD
   chorus machinery we already validated.
3. ~~**Phase 90** *(S–M)* — 4 JFET allpass stages + LFO + feedback; the spring
   work built deep allpass fluency. Script-logo voicing.~~ ✅ *(shipped as the
   'phaser' pedal "Ninety" — see docs §20: 4 first-order allpasses swept by one
   rounded-triangle LFO, 50/50 dry+wet → 2 moving notches, ONE SPEED knob
   (0.06–8 Hz), fixed depth, NO feedback (SCRIPT voicing; block-logo feedback
   noted as future variant). Linear time-varying (no oversampling), per-sample
   coefficients (no zipper). Notch positions match the analytic 4-stage response
   to 0.00%, depth 48–94 dB, HF floor −130 dB. Single-knob orange face, assistant
   add_pedal + placement/speed coaching.)*
4. ~~**Big Muff Pi** *(M)* — first BJT (Ebers-Moll) device model: 4-stage
   cascade, 2 clipping stages, mid-scoop stack. Iconic fuzz contrast.~~ ✅
   *(shipped as the 'muff' pedal "Pi" — see docs §24. The REAL product is the
   reusable **BjtStage**: an Ebers-Moll NPN (2N5088-class, β 400) common-emitter
   gain stage with collector-feedback bias, solved per-sample with damped nodal
   Newton (analytic Jacobian + backtracking line search, slam-proof) — the BJT
   sibling of the M9.1 TriodeStage that the Fuzz Face / RG100 will inherit.
   MuffModel cascades 4× BjtStage (Q1 boost → SUSTAIN → 2 diode-clip stages →
   mid-scoop tone stack → Q4 recovery → VOLUME). Per-stage DC ops match the
   analytic bias to <0.1%; scoop notch ~980 Hz matches the analytic H(s) to
   0.1 dB; THD ~80% (a FUZZ, no clean setting); wall-of-sustain compresses a
   20 dB input sweep into ~1.6 dB out; 4× oversampling holds aliasing to −80 dB.
   Wide violet "Pi" face (triangle knobs), assistant add_pedal + fuzz-vs-drive /
   into-a-clean-Twin coaching.)*
5. **Fuzz Face (silicon)** *(M)* — 2-transistor minimalism, but its soul is
   PICKUP LOADING (cleans up with guitar volume): requires adding a source-
   impedance model to the chain input — design that first or don't bother.
6. **Klon Centaur** *(M)* — our addition (the famous omission): parallel
   clean/dirt blend, germanium clippers; the most-cloned pedal alive.
7. **Deluxe Memory Man** *(L)* — BBD delay + compander + degradation;
   replaces the parked DD-3 as the delay milestone (analog > digital DSP-wise).

### Parked (unordered)

- **BD-2 Blues Driver** (third dirt flavor), **DD-3 delay** (new DSP family:
  delay lines — easy after the above).
- **Presets & sharing** — the M4 rig-state JSON is already the format.
- ~~**User cab IR upload** + a Marshall 4×12-style IR to pair with M9.~~ ✅
  *(shipped — see docs §15: a modal-synthesis rebuild of both cabs that root-caused
  the "fizzy with the cab on" noise-tail hash, a `brit412` Marshall-style 4×12, and
  `.wav` IR upload with in-core peak-normalization. Also fixed a latent in-place
  convolver overlap bug.)*
- **Native path — phase 1 SHIPPED** (plugin + standalone). A JUCE (CMake,
  FetchContent-pinned to JUCE 8.0.4) wrap of the **identical** core in `native/`:
  Standalone + VST3 + AU (AU mac-only), mirroring the web rig's signal chain
  (input trim → RAT → SD-1 → Clean 120 amp + stereo chorus + cab → OutputLimiter),
  driven by an AudioProcessorValueTreeState with ids/ranges/defaults matching
  `web/src/rig.ts`. It is a re-wrap, not a rewrite: a console test renders the
  M2-style 220 Hz sine + pluck through the real PluginProcessor and through the
  core classes directly and asserts **bit-exact** (0.0) output on both channels,
  plus matching host latency (all four amp voices). Verified on Linux (Standalone
  + VST3); AU + Logic are the mac follow-up. The native **neumorphic UI is now
  DONE** ✅ — the editor was rebuilt as the light-bench sibling of the web UI
  (dark-chassis island cards, sculpted value-arc knobs, per-gear accents, a
  face-switching amp card that mirrors the web's per-voice control visibility),
  via a `ClipperLookAndFeel` + widget kit that translates the web CSS recipes to
  JUCE; a dev-only `xvfb` snapshot target renders one PNG per amp voice as proof.
  See `docs/DEVELOPMENT.md` → "Native app (JUCE)" → **Editor (neumorphic visual
  pass)**. Remaining phase-2 native work (drag-reorderable chain, a **dark theme**
  for the native editor, CLAP) stays parked here.
- **Riff integration** — Clipper's rig as Riff's practice-tone engine; the
  assistant patterns already converge (both grew an "applied chip" chat UI).

## Known risks

- **Aliasing is a craft, not a checkbox.** M2 may take several passes; the milestone has explicit measurement criteria so "sounds fine on my laptop speakers" doesn't pass.
- **Browser audio input is the flakiest layer** (device permissions, sample-rate mismatches, Safari). Contained by keeping the core offline-testable (M1) so audio bugs are always attributable to the shell, not the model.
- **Latency expectations.** ~20–40 ms is playable but felt. Set expectations in the UI; the answer is the deferred native shell, not heroic web hacks.
- **Assistant scope creep.** The tool-use surface stays small and typed; the assistant gets smarter through prompting and rig-state context, not through more tools.

## Key resources

- **Jatin Chowdhury** — `chowdsp_wdf`, writing on real-time WDF and antialiasing, browser/WASM pedal demos.
- **David Yeh, CCRMA thesis** — *Digital Implementation of Musical Distortion Circuits* (nodal DK method, real pedals).
- **Kurt Werner's thesis** — canonical WDF reference, incl. multiple/interacting nonlinearities.
- **Duncan's Tone Stack Calculator** — tone-stack transfer functions.
