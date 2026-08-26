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
- **M10.3 — Orange OR120 "Overdrive"** *(S–M)* — heavy EL34/JCM800 machinery
  reuse with Orange voicing. The early-70s picture-graphics head: no master
  volume, thick midrange-forward voicing, breakup from the power section
  (Sabbath / Sleep / Mastodon). Ships with a **synthesised PPC412-style cab**
  in the docs §15 modal-synthesis house style — do NOT commit a captured
  third-party IR (provenance + licensing, and it breaks the pattern that every
  cab in this project is generated). Owner: *"I'm an orange man."*
  *(SHIPPED 2026-07-31 as the OR120 "Overdrive", amp voice 4 + an `orange412`
  cab — docs §57. The power machinery IS reused; the voice is the CIRCUIT
  differences: a CATHODYNE phase inverter DC-coupled to its driver (leg balance
  1.000000 by topology, against the LTP's fitted 0.988), a JAMES/passive-Baxandall
  BASS+TREBLE stack, the six-position F.A.C. rotary, global NFB into the driver's
  CATHODE with HF DRIVE in the loop, and NO MASTER VOLUME. The acceptance bar was
  "measurably not a re-skinned JCM800": mid-notch metric at noon **+2.32 dB vs the
  FMV's −6.03**, an 8.35 dB network contrast and 6.24 dB on the composed amps.
  All five goldens unchanged; nothing blessed. Research honesty note: no schematic
  was reachable from the build container — see §57.1 for what is sourced and what
  is reconstruction.)*
- ~~**M10.4 — Mesa Dual Rectifier**~~ — **SHIPPED 2026-08-17 (docs §69)** as amp
  voice **6** (`mesa`), and it is **the first amp voice here TRANSCRIBED from a
  complete factory drawing set** rather than reconstructed: the owner supplied
  the `mbdr` sheets (preamp, power amp, supply, switching matrix and a TRUTH
  TABLE), and they carry the manufacturer's own **marked DC node voltages** — an
  absolute external reference, which no previous amp slice had. Model vs sheet:
  V2B plate **0.01 %**, V1A 1.69 %, V2A 1.25 %, V3A 3.98 %, idle rail 0.14 %.
  **This entry's own framing was partly wrong and the slice corrected it.** The
  cost estimate was right ("close to a parameter change on the §55 Thévenin
  supply" — it was), but "loose/tight modes" understates what the drawing says:
  the two **MODERN** modes switch the power amp's **global negative feedback OFF
  ENTIRELY** (LDR19 and LDR20 both open), where every other amp in the lineup has
  a permanently wired loop. Measured loop depth **OR CLN 9.27 / OR NORM 6.19 /
  RED VINT 6.19 / OR MOD 0.00 / RED MOD 0.00 dB**. That is why a Recto's low end
  is loose, and it is topology rather than an EQ curve. Also settled by the
  sheet: **SPONGY/BOLD is a SEPARATE mains-primary-side switch, not the rectifier
  selector** — the two are routinely conflated. Rectifier droop ratio **2.24×**
  (5U4 vs silicon). The graphic EQ is NOT part of this amp and was not built
  (it is M13.6's MXR 10-band). **Open and reported, not fitted: the amp makes
  ~48 W against a rated 100** — the obvious suspect, the transcribed 1 k screen
  resistors, was perturbation-tested and REFUTED (ideal screens reach only 56 W).
- **M10.7 — Orange Rockerverb 100 MkIII** *(M)* — the modern Orange, and the
  second half of the owner's "OR120 and Rockerverb" ask. Deliberately NOT
  bundled with M10.3: it adds footswitchable clean/dirty channel structure, a
  master volume and its own reverb, so it is a real slice rather than a
  re-voice. Do it directly after M10.3, while the Orange research is warm.
  *(SHIPPED 2026-08-01 as amp voice 5 `rockerverb` — docs §63. The DIRTY channel
  only, and that is a research decision: a real netlist was found for it
  (LiveSPICE's `Orange Rockerverb 50 Preamp.schx`, parsed node by node) and none
  for the clean channel, so the clean channel and its footswitch are a NAMED
  follow-up rather than an invention — §57's "find the schematic" rule applies to
  inventing a whole channel too. The acceptance bar was "measurably NOT a
  re-skinned OR120" and it has two independent halves, both hard asserts:
  **(a) the tone network is a Marshall-lineage FMV** — mid-notch metric at noon
  **−6.01 dB (a SCOOP) against the OR120 James's +0.75 (a BUMP), 6.76 dB of
  contrast** on §57.4's own scale, and **3.50 dB on the COMPOSED amps through the
  same cab**; **(b) the MASTER VOLUME decouples drive from level** — level-matched
  at −20 dBFS the Rockerverb delivers **15.76× the OR120's THD**, and across
  VOLUME 0.01→0.10 it moves **19.89 dB of level for a THD ratio of 0.971**, which
  a no-master amp cannot do at any setting. Bars shipped 5.0 dB / 3.0 dB / 5×;
  margins recorded, not snugged. Third structural difference, reported not
  asserted: a LONG-TAILED PAIR (balance 0.972, a calibration) where the OR120 has
  a cathodyne (0.999965, topological). No new cab — it reuses `orange412` — and
  **no new param id**: GAIN and its post-stack VOLUME ride the JCM's gain/master
  slots because the FUNCTION matches. All five goldens unchanged; nothing blessed.
  One XFAIL, shared with the OR120: the 44.1 kHz alias floor.)*
- **M10.8 — Marshall "Bluesbreaker" 1962 combo** *(M)* — the JTM45 combo, the
  Clapton *Beano* tone. **Note the name collides:** the Bluesbreaker *pedal* is
  a transparent low-gain OD that overlaps the Gold/Myth territory and is NOT
  wanted; the *combo* is a genuine hole. KT66/5881 power, no master volume,
  the 5F6-A Bassman lineage.
- **M10.9 — Marshall 1959 Superlead 100W plexi** *(M)* — the non-master-volume
  ancestor the JCM800 descends from (Hendrix / Page / early Van Halen). The
  **cheapest amp on this list per unit of tone**: mostly a re-voice of machinery
  M9 already built, minus the master volume, plus the shared cathode / jumpered
  channels.
- ~~**M10.10 — Fender Champ (5F1)**~~ — **SHIPPED 2026-08-25 (docs §72)** as amp
  voice **7** (`champ`), wordmark "Cadet", plus a synthesised **`tweed8`** 1×8
  open-back cab. **This entry's own text was WRONG and the slice corrected it:**
  it said "ONE tone control", and a tweed 5F1 has **NO tone control at all** —
  one knob, VOLUME. Fender did not put one on a Champ until the 1964 blackface
  AA764. So this is the first amp voice here with no tone stack whatsoever, and
  its face has two knobs (the second is the §19 reverb convenience).
  The "new machinery" estimate was right: it is **the first SINGLE-ENDED output
  stage in the project** — every other power section is a push-pull pair behind a
  phase inverter — and three properties fall out of that, each a hard assert:
  **(a) nothing cancels the even harmonics** (h2 **−14.84 dBc** against the
  balanced Twin's **−39.72** on the identical stimulus, 24.88 dB of contrast);
  **(b) audit finding 9 from the other side** — `Vp = rail − (i − iq)·Rload` is a
  single-ended relation, exactly correct here and approximate in the push-pull
  amps, so the plate reaches the knee at **88.4 mA** where finding 9 measured
  **530 mA from ONE EL34**; **(c) there is no negative feedback at all**
  (open- vs closed-loop renders bit-identical).
  **The 6V6 device card is DERIVED and that closes audit finding 10 on a third
  tube:** two published Koren fits are reachable and disagree, both put the screen
  **2.1–2.2×** over the datasheet, and at this amp's idle the published fit sits
  at **2.53 W against the 6V6GT's 2.75 W rating** — the AC30's exact "exceeds its
  rating at idle" pathology. `kg2` derived (4500 → 10148.2) → **1.12 W**.
  Validated against **TWO absolute external references**: the RCA/TAD datasheet at
  two operating points, and **Fender's own published 5F1 node voltages** — only
  ONE constant is fitted to the latter, and cathode/Ip/Ik/Vpk all fall out within
  **0.5–2 %**. Power **reported, not chased** (§57.3): the power section's own
  sine ceiling is 5.17 W against a rated ~5 W; the composed cranked amp makes
  3.89 W because the preamp hands it a blocking-limited waveform.
  All five goldens unchanged; nothing blessed; zero XFAILs.
- **M10.11 — Soldano SLO-100** *(M–L)* — the origin of modern high gain and the
  ancestor of nearly every boutique lead channel. Best done AFTER the Mesa, so
  the cascaded-preamp machinery already exists to reuse.
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
  catcher). *Amended 2026-07-25 (docs §30): the tolerance was `2e-5` where the
  real answer is `0`, and 128 is the units' own internal chunk size, so the
  comparison could not fail. Now bit-identical at 128 AND run at a **ragged 100
  frames** — the only segmentation that can catch a block-size bug, and the one
  that found the dirt pedals' control-rate parameter sampling.*
- **Golden "first five minutes" renders** (five default rigs, 944 KB of 16-bit
  48 k WAVs) gated per third-octave band at ±1.5 dB — lossless refactors pass,
  voicing drift fails; regeneration only via `scripts/update-goldens.sh`.
- **Web sim** (`web/tests/expectations.spec.ts`): each pedal added + stomped at
  defaults, all four amp voices cycled — audible, NaN-free, level-sane,
  click-free. **Found + fixed a real latent bug:** the worklet stomp (pedal
  bypass / amp power) was the one topology change not declick-bracketed — it
  popped audibly; stomps now ride the shared raised-cosine declick.

### M12 — Tests that assert real properties ✅ *(shipped 2026-07-25 — docs §30)*

The 2026-07-24 audit's **systemic** finding, and the gate on everything after it:
"a recurring class of test asserts an identity, a tautology, or the
implementation against a reference derived from the same code — so wrong
topologies and wrong constants pass." Deliberately sequenced BEFORE the circuit
fixes (findings 4, 5, 7), because those changes will be judged by this suite and
re-blessing a golden against an unverified change is how a regression becomes
canon. Tests + CMake + docs only; zero DSP change; goldens untouched.

- **`-UNDEBUG` was behind `if(NOT MSVC)`** — so on MSVC the entire 1129-line
  expectations suite compiled to a no-op `main` that printed its success banner
  with zero goldens present. Fixed on every platform, and
  `core/tests/support/AssertsLive.h` makes it a **build error** if `NDEBUG` ever
  reaches a test TU again.
- **The XFAIL ratchet** (`core/tests/support/Xfail.h`): a known-bad property is
  measured, its real number printed, the finding named — and **an XPASS is a hard
  failure**, so an XFAIL cannot outlive its defect. Open XFAILs show as
  `<target>_xfail_ledger ... ***Skipped` in a plain `ctest` run.
- **The phase inverter measured properly for the first time**: plate as a
  *fraction* of B+, standing current from *Ohm's law on the plate load*, and the
  *leg-gain ratio* — which nothing checked anywhere. The old assertion was a
  160 V window on a 410 V rail, admitting both a healthy PI and the starved one
  that actually ships.
- DC offset asserted **on signal** (with a DC-offset-input case, because deleting
  a coupling cap changes nothing on a clean input); the JCM800 push-pull
  **algebraic identity** deleted; block B **bit-identical + a ragged block size**;
  `OptoTremolo` gets its first test; three amp-swap "no pop" tests land the swap
  **mid-render**; three perf-smoke tautologies now assert real audio; a finiteness
  guard over every render in `audio.spec.ts`.
- **Every rewritten test was perturbed and confirmed red**, then reverted — the
  table is in docs §30.

Leaves **11 XFAILs** covering findings 7, 8, 16 and two Medium/DSP items, each
naming the slice that owns the fix. Still open by choice: the tone-stack class
(discrete MNA vs an analytic `H(jω)` from the same netlist) needs published
response curves per amp.

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
   chorus machinery we already validated. **Superseded 2026-07-31 by M13.7**
   (the CE-1, which is literally the JC-120 circuit in a box).
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
6. ~~**Klon Centaur** *(M)* — our addition (the famous omission): parallel
   clean/dirt blend, germanium clippers; the most-cloned pedal alive.~~ ✅
   *(shipped as the 'gold' pedal "Myth" — see docs §27. The parallel clean/dirt
   blend is modelled literally: a dual-ganged GAIN pot cross-fades a
   full-bandwidth CLEAN path against a germanium-clipped one (`clean = 1−0.55g`
   vs `clip = g`, drive amp `1 + g·100k/1.5k` = up to 67.7×), with the lows
   high-passed at 106 Hz BEFORE the clipper and a 1N34A-class chowdsp_wdf diode
   pair (Is 200 nA, n 1.3, knee 0.29 V) as the only thing in the box that clips —
   the charge-pump ±9 V rails are modelled as headroom and never engage.
   Measured: flat within **0.138 dB** and **0.0000 % THD** at GAIN 0 (unity at
   OUTPUT noon), THD 0.00→11.66→20.60→26.31→31.73 % with the clipped share rising
   0.00→0.85 across the knob, germanium THD ×23 over a 20 dB sweep vs silicon's
   ×736 (the soft knee, as a number), 4× aliasing −93.3 dB, hum −30.1 dB at min
   gain (the physical ceiling for a linear pedal). Gold `plate` face with an
   engraved "MYTH" nameplate — no Klon/Centaur text, no centaur figure; assistant
   add_pedal + always-on / push-a-breaking-up-amp / stacking coaching.)*
7. **Deluxe Memory Man** *(L)* — BBD delay + compander + degradation;
   replaces the parked DD-3 as the delay milestone (analog > digital DSP-wise).

### M13 — Effects families: the "all kinds" expansion *(planned 2026-07-31)*

The lineup has **five flavours of dirt and almost nothing else**. Every family
below is entirely absent, and each absence makes whole genres unplayable: funk
needs wah + comp + envelope filter, country needs comp + delay, ambient needs
delay + reverb, modern metal needs gate + EQ. Ordered by the project's own
principle — machinery reuse first.

**The three primitives that unlock most of it.** Deliberately called out because
each one is shared by several pedals, and building the primitive first is what
keeps these slices small:

| Primitive | Unlocks |
| --- | --- |
| Resonant swept bandpass | wah **and** envelope filter (foot-driven vs envelope-driven) |
| Envelope detector + gain element | compressor **and** noise gate (same detector, inverted) |
| Delay line (partly exists — the JC-120 chorus is BBD) | delay, flanger, tape echo |

1. **M13.1 — Cry Baby wah + envelope filter** *(M)* — GCB-95-style inductor
   bandpass. A screen has no treadle, so **POSITION is a normal automatable
   parameter** (drive it from a DAW expression pedal / automation lane) **plus an
   AUTO mode** where an envelope follower sweeps the same filter — one primitive,
   Cry Baby and Mu-Tron III territory together. Owner-chosen 2026-07-31.
   *(✅ SHIPPED 2026-07-31 as the "Weeper" — docs §58, ADR 018. The sweep law is
   DERIVED from component values and cross-checked against CCRMA's fit to three
   MEASURED GCB-95 units: 2250.79 Hz computed vs 2216.06 measured, 1.57 % apart by
   two independent routes, leaving the pot taper as the single fitted parameter.
   2.32 octaves; peak 17.90–17.91 dB with 0.010 dB of spread across the whole
   travel. SENSE 0 is bit-identical to a build that never had the envelope mode.)*
   *(Follow-up CLOSED 2026-08-01 — docs §58.8, **ADR 023**: its envelope follower
   is deliberately NOT unified with M13.1/M13.6a's shared `SidechainDetector`.
   That component is a THRESHOLD detector — 1.95 dB of open-loop proportional
   range on the compressor's own values against 19.09 dB for the wah's one-pole —
   and a wah, having no gain to reduce, is feed-forward by necessity. The
   substitution was built and every §58.6 AUTO number regressed. Refusal
   perturbation-proven by `testFollowerLevelLaw`.)*
2. **M13.2 — Dyna Comp / Ross compressor** *(M)* — CA3080 OTA, two knobs
   (SUSTAIN + LEVEL). The squishy pedal compressor: country chicken-pickin',
   funk, the always-on sustain trick. Its envelope detector is the noise gate's.
   *(✅ SHIPPED 2026-07-31 as "Squash" — docs §59, ADR 019. FEED-BACK topology
   settled from the netlist and then proved at 216:1 against feed-forward's 3.3:1
   on identical code; control current 199.85 µA idle / 16.54 settled against an
   independent SPICE run's ~192 / ~16. SUSTAIN moves 25.33 dB of gain against
   0.28 dB of output — "not a threshold", as a number. Noise, transient loss and
   phase inversion are measured and asserted in the direction that KEEPS them.)*
3. ~~**M13.3 — Optical compressor (LA-2A style)**~~ — **SHIPPED 2026-08-01**
   (docs §64, ADR 025) as the "Lumen", pedal type `opto`, slot 11. Electro-optical
   attenuator with a **program-dependent release**, which is its acceptance bar
   and is asserted against M13.1 on the identical stimulus: the optical voice
   releases **2.892x** more slowly after a long passage than after a stab at the
   same depth, the OTA voice **1.000x**. Ratio **2.81:1** measured against a
   **2.875:1** prediction from two published device exponents; attack 9–10 ms
   across the whole knob (the OTA voice's moves 14 → 3 ms). **The SD-1 / TS
   precedent did NOT hold and that is a finding, not a shortcut:** of
   `CompressorEngine`'s eight config structs exactly one applies to an optical
   leveling amplifier, and `SidechainDetector` was refused by measurement too
   (there is no envelope capacitor in the reference circuit at all). `OptoModel`
   is standalone and holds a new shared component, `OptoCell` — whose named
   future consumer is **M13.5's photocell-driven Uni-Vibe**. See ADR 025.
4. **M13.4 — Delay: Deluxe Memory Man / Echoplex EP-3** *(L)* — the single
   biggest hole in the lineup. BBD + compander + degradation. Note the EP-3's
   *preamp* is a tone in its own right (Page, EVH) and is worth exposing even
   with the delay off.
5. ~~**M13.5 — Uni-Vibe**~~ — **SHIPPED 2026-08-10 (docs §67, ADR 026)** as
   pedal type `vibe`, slot 12, wordmark "Swirl". **This entry's own framing was
   wrong and the slice corrected it:** "four **mismatched** phase stages (not the
   Ninety's matched four)" is not the distinction, because `PhaserModel` already
   carries a deterministic ±1.5 % per-stage detune so its four corners do not
   stack into one null. The distinction is the MAGNITUDE — the sourced staggered
   caps are 0.22 µF against 470 pF, i.e. **468:1 (8.87 octaves) against 0.043
   octaves** — and the player-facing consequence is the SPREAD OF THE NOTCH PAIR:
   measured **5.74 octaves against the Ninety's 2.49**, from the identical
   stimulus. The lamp thermal lag is real and is the second, independent bar
   (the sweep rises faster than it falls; the Ninety measures 1.00 on the same
   metric). `OptoCell` was reused **unwidened** — the lamp came out as its own
   component, `LampDrive.h`, exactly as that header instructed.
6. **M13.6 — Utility + modulation batch** *(M, splittable)* — **Boss NS-2-style
   noise gate** (mandatory companion to M10.4; reuses 13.2's detector),
   **MXR 10-band graphic EQ** (mandatory for metal; 10 biquads, trivial DSP),
   **Electric Mistress flanger** (cheap once the delay line exists),
   **Mu-Tron III envelope filter** (falls out of 13.1 if that slice builds it).
7. **M13.7 — CE-1 Chorus Ensemble** *(S — "almost free")* — worth knowing: the
   CE-1 **is** the Roland JC-120's chorus circuit put in a box, which is its
   literal design origin. So this is a re-voicing of machinery already validated,
   not a new model. Supersedes the older CE-2 line item below.
8. **M13.8 — Standalone reverb pedal** *(M)* — spring exists but only *inside*
   amps. Wanted: hall / plate / room as a pedal.
9. ~~**M13.10 — Drop-tune (DigiTech Drop-style polyphonic pitch shifter)**~~ —
   **SHIPPED 2026-08-19 (docs §70)** as pedal type `drop`, slot 13, wordmark
   "Cellar", GRAPHITE-CYAN. The owner's "grunge unlock" ask of 2026-08-17,
   shipped as the second half of that sentence (§69 is the Mesa). **A whole new
   DSP family, and the FIRST voice on this board with no schematic to find** — a
   Drop is a DSP box, so §57's rule has no target and the acceptance shape is
   cents accuracy / latency / a measured artifact floor instead. It is
   polyphonic BY CONSTRUCTION rather than by tuning: the mechanism is
   resampling, which scales every frequency present and never forms an opinion
   about pitch. **Measured 0.00 cents on single notes at all nine detents and on
   power chords**; one XFAIL (`drop-triad-spread-at-minus-2`, a 2.0000-cent
   spread against this slice's own 2-cent target, with the 5-cent perceptual bar
   met by 3.75). The reusable deliverable is
   `core/include/clipper/dsp/PitchShifter.h`, `DelayLine`'s second consumer —
   a future harmoniser or detune holds one without inheriting a drop pedal.
   Shipped with ONE knob and NO MIX, faithful to the reference by owner
   decision. **Named follow-up: a frequency-domain shifter owns the XFAIL.**
10. **M13.9 — Octavia** *(S–M)* — **cheaper than it looks**: octave-UP needs no
   pitch tracking at all (full-wave rectifier → transformer → fuzz). Octave-DOWN
   is the hard one and is not in this item. *(Not selected in the 2026-07-31
   planning pass — recorded because the cost is widely over-estimated.)*

**Not wanted (decided 2026-07-31):** the Bluesbreaker *pedal* — it overlaps the
Gold/Myth transparent-OD territory already shipped. The *combo* is M10.8.

**Standing risk this expansion sharpens:** there is still **no undo path** for
removing a pedal, and no preset system. At six pedals that is an annoyance; at
fifteen it is a liability. (An earlier revision of this line claimed
`feat/undo-ring` already existed as a branch — it does not, on origin or
locally. The work is unstarted.) Land it before the board gets much longer.

### M14 — The assistant on native *(L, phased — planned 2026-08-10)*

**The gap:** the tone assistant has never existed in the plugin. It is not a
deferred phase-2 item, it was never recorded at all — `native/` contains **zero**
references to the assistant, the proxy, or Anthropic. Today the feature is
web + Electron only: ~2,200 lines of TypeScript in `web/src/assistant/`
(`client.ts` / `prompt.ts` / `tools.ts` / `history.ts`) plus the zero-dependency
Node proxy in `server/`. So the plugin — the thing you actually record through —
is the one surface with no tone coach.

**The decision that shapes the slice (owner, 2026-08-10): the plugin talks to
`api.anthropic.com` DIRECTLY with a BYO key.** The alternative — requiring the
desktop proxy on `127.0.0.1` — was rejected because a VST3 running inside a DAW
cannot assume the Electron app is also running, which would make the assistant
absent exactly where it is most wanted. **This needs an ADR**, because it
contradicts the standing rule in CLAUDE.md's env table (`ANTHROPIC_API_KEY` is
"**server-side only**, never shipped to the browser"). That rule was written for
a browser context and does not automatically transfer to a native binary the
user installed themselves — but the amendment must be explicit, not assumed.
Key storage follows the precedent visual pass 3 set for the theme: a
`juce::PropertiesFile` outside the APVTS at mode `0600`, so a key can never
reach host automation or a saved session.

**What is shareable and what is not.** The shape is more favourable than the
line count suggests: `SYSTEM_PROMPT` is a **single** template literal and `TOOLS`
is a **declarative** array of seven tools, so both can be extracted to data files
that the web and the plugin embed from one source — which is what keeps the
coaching identical on both fronts, the same way the DSP core is shared. What
cannot be shared is `executeTool`'s allowlist / clamp / dispatch layer: it is
written against a `RigController` interface over the web rig, and native needs
its own implementation against the APVTS. **That re-implementation is the risk
item** — the tool surface is the best-defended boundary in the codebase
(CLAUDE.md: "keep it that way") and a second executor is a second place for a
clamp to be missed. It gets the same param-allowlist + range-clamp + reject-
non-finite treatment, tested directly.

Phase it, because this is an L and the chat UI is independent of the transport:

1. **M14.1 — Shared prompt/tool data + the C++ client** *(M)* — extract
   `SYSTEM_PROMPT` and the `TOOLS` schema to shared data consumed by both front
   ends (web keeps its current behaviour **byte-identically** — that is the
   acceptance bar, and the existing `history` / `server` node suites are the
   guard). Add an HTTPS + SSE client on a background thread, the tool-use loop,
   and the APVTS-backed executor with its own clamp tests. **No UI.** Proven by a
   console target in the `clipper_*_test` house style, run against a real key:
   "give me a tighter rhythm tone, less saturated" must produce tool calls that
   land on the plugin's parameters and leave every value in range.
2. **M14.2 — The chat UI in the editor + key storage** *(M)* — where the
   conversation lives in a fixed-size skeuomorphic board editor is a genuine
   design question, not a detail; the web's "applied chip" pattern is the
   reference. Plus the key-entry affordance and the `PropertiesFile` store.

**Explicitly NOT in this milestone:** replacing the Node proxy (Electron keeps
it), streaming assistant edits into host automation lanes, and any growth of the
tool surface — M6's standing rule holds, the assistant gets smarter through
prompting and rig-state context, not more tools.

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
  pass)**. **PEDAL-BOARD PARITY is now DONE too** ✅ — the fixed RAT → SD-1 pair
  became the web app's board: all six AUDIO pedal types (RAT, SD-1, TS, Muff,
  Phaser, **Gold** — the v1.1 item 6 "Myth" transparent overdrive, whose native
  card translates the web 'plate' face: an engraved milled nameplate, gold accent,
  round stomp), any order, add / remove / swap / drag-reorder, each edit bracketed
  by the worklet's declick fade, with the board saved as non-automatable APVTS state
  (every existing param id untouched; a pre-parity session migrates back onto its
  old pair). The board keeps the left-to-right layout and gained patch cables, the
  web's four footswitch morphologies, and visible LEDs; the identical-core test now
  covers multi-pedal chains (including a gold-box board) and a chain-edit test bounds
  the seam. The board **SCROLLS** ✅ — a horizontal viewport with the input card and
  amp face pinned outside it, a skeuomorphic rail (a channel milled into the bench
  carrying a ribbed mat) under the pedals, drag-to-edge auto-scroll, and boundary
  cables that track the scroll and clamp to a grommet at the viewport edge. This
  **supersedes** the parity pass's grow-the-window rule: the window minimum is a flat
  1040 px again however long the chain gets. The TUNER stays web-only (display-only
  pedal: it needs a pitch tap + needle widget, and a fake one would be worse than
  none), and **duplicate instances of one pedal type** stay web-only for now — the
  native engine is one-instance-per-type; the options and a recommendation are
  written up in `docs/DEVELOPMENT.md` → **Duplicate pedal instances**. See
  `docs/DEVELOPMENT.md` → **Native pedal-board parity**. Remaining phase-2 native
  work (the **tuner**, **duplicate instances**, CLAP) stays parked here. The
  **dark theme** came off this list — it shipped in visual pass 3 (2026-07-31),
  leaving only a Linux OS-theme reader, since JUCE 8.0.4's
  `Desktop::isDarkModeActive()` has no X11 implementation. **The assistant is
  NOT parked here** — it was missing from this list entirely and is now
  **M14**, above.
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
