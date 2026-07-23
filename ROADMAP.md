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
- **M6.3 — JC-120 chorus & vibrato** — the amp's soul: speed + depth knobs,
  chorus/vibrato mode switch (chorus = dry + modulated wet; vibrato = wet
  only, true pitch wobble), and the JC's signature stereo bloom (dry L / wet
  R) — the worklet goes stereo at the amp stage. Modulated delay in core,
  offline-validated.
- **M6.4 — Pedalboard visual pass** — stackable, drag-reorderable pedals
  joined by neumorphic cables, add/remove/swap from a gear tray, amp slot at
  the end with amp swapping. Architecture for everything M7+ plugs into.

### M7 — Tuner *(S)*

Not a modeling problem — pitch detection + mute. Chromatic needle tuner
(Polytune-style *polyphonic* mode is a much harder multi-pitch problem;
deliberately out of scope).

- Detection in the web layer via the McLeod pitch method (`pitchy`, the same
  tiny MIT library Riff uses) on an analyser tap of the live input — zero
  core-DSP changes.
- Pedal-format UI in the design language: note + cents needle, green-lock LED,
  footswitch = mute (the one DSP touch: a mute flag in the worklet chain).
- Forces the rig into a proper multi-pedal chain (tuner → dirt → amp) —
  RigState grows `pedals[]`, the seam M4 left ready.
- Assistant learns `tune` awareness ("you're a quarter-tone flat on the G").

### M8 — SD-1 Super Overdrive *(M)*

The essential second dirt box, and the perfect topology contrast with the RAT:

- **Soft, asymmetric clipping in the op-amp feedback loop** (2 vs 1 diode —
  even-harmonic warmth) vs the RAT's hard shunt-to-ground clipping. New WDF
  arrangement; all M1/M2 machinery (oversampling, ADAA infra, alias
  measurement, render CLI) reuses directly.
- The signature mid-hump input shaping, tone control, level. Three knobs.
- Ground truth on the real board for A/B.
- Sets up the canonical M9 pairing: SD-1 boosting a cranked Marshall.

### M9 — First valve amp: Marshall JCM800 2204 *(L, phased)*

Chosen over alternatives (tweed Deluxe 5E3 = technically simplest; AC30 =
hardest) because: the preamp is one building block — a 12AX7 common-cathode
triode stage — repeated, so we build a single validated triode model and get
future amps mostly for free; it's exhaustively documented; and SD-1 → JCM800
is one of the most canonical pairings in rock.

1. **Triode stage module** — grid conductance + blocking distortion included
   (Koren-style model or WDF triode), validated standalone against published
   curves with the M2 measurement discipline. This module is 80% of every
   future amp.
2. **Preamp** — cascaded stages with interstage attenuation, cathode follower
   driving the classic Marshall TMB tone stack (passive — existing machinery).
3. **Power section — where "responsive" lives**: push-pull EL34 approximation,
   negative feedback + presence control, and **sag** (supply droop under pick
   attack) modeled explicitly and measured, not vibed.
4. Amp-panel UI (preamp/master volume era-correct), oversampling per stage
   budgeted like M2, A/B render harness.

### Parked (unordered)

- **BD-2 Blues Driver** (third dirt flavor), **DD-3 delay** (new DSP family:
  delay lines — easy after the above).
- **Presets & sharing** — the M4 rig-state JSON is already the format.
- **User cab IR upload** + a Marshall 4×12-style IR to pair with M9.
- **Native path** — only when latency or Logic integration actually bites:
  JUCE wrap of the identical core → VST3/AU/CLAP. A re-wrap, not a rewrite.
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
