# Clipper — Full Project Audit

**Date:** 2026-07-24
**Scope:** DSP core (pedal + valve-amp circuit models, modulation, shared infrastructure), WASM/AudioWorklet web layer, React UI, JUCE native plugin/editor, assistant + proxy server, Electron shell, build and test integrity. Read-only audit — no code changed.
**Method:** Direct code inspection across ~42k lines, plus a built-and-measured pass: the core was compiled and its full `ctest` suite run, the Playwright suite run, and standalone instrumented harnesses built against `libclipper_dsp` to measure latency, CPU, denormal behaviour, parameter-step discontinuities, frequency response and DC offset. Six parallel deep-dive reviews (pedals, valve amps, modulation/infra, web audio, UI/UX, app infrastructure). Every number below is measured from the code as checked out at `909754f`.

---

## Verdict

**The engineering baseline is genuinely strong.** 16/16 core test suites pass, 70/70 Playwright audio tests pass, `tsc --noEmit` and `vite build` are clean, there is exactly one compiler warning in the tree, no `-ffast-math`, dependencies are pinned to commits/tags, no secrets are tracked, and the native UI→audio handoff is race-free. The documentation is unusually honest — several findings below were already half-known and written down.

**Three things are shipping-blockers.** A single NaN parameter permanently destroys all audio with no recovery path, and the AI assistant can send one. A cab change runs 11–46 ms of allocation and FFT setup inside `process()`. And `CabConvolver` corrupts its own stream on any host block size that isn't a multiple of 128 — which the native plugin passes straight through.

**The circuit models have real architectural errors**, concentrated in the AC30 (its "sag" is a static saturator, and its tone stack has a structural ~37 dB mid notch) and in the shared phase-inverter model (which cannot express a real long tail, leaving two of three amps biased near cutoff). These are not value tweaks.

**The pattern worth naming:** the test suite is large and passes, but a recurring class of test asserts an identity, a tautology, or the implementation against a reference derived from the same code — so wrong topologies and wrong constants pass. Several findings below are things a test *named for that property* did not catch.

---

## Critical

### 1. One NaN parameter permanently bricks the rig — and the assistant can send one

**Measured:** after a single NaN parameter, every unit outputs non-finite samples forever (12672/12672 in a 1 s window, for `Jcm800Amp`, `AmpModel` and `RatModel` alike). Writing a good value afterwards never clears it — the NaN is latched in smoother/biquad/feedback state. There is no `reset` export, so the only recovery is destroy + recreate.

Root cause is precise: **`Inf` is handled correctly, NaN is not.** Every parameter clamp in the codebase is NaN-transparent, because both comparisons are false for NaN:

- `core/src/dsp/AmpModel.cpp:53` and `core/src/dsp/RatModel.cpp:154` — `clamp01(v) { v < 0 ? 0 : (v > 1 ? 1 : v) }`
- `core/src/dsp/Jcm800Preamp.cpp:221` — `std::clamp(value, 0.0, 1.0)` (also NaN-transparent)
- `web/src/assistant/tools.ts:350`, `web/src/App.tsx:401`, `web/src/components/Knob.tsx:33` — the same shape in TypeScript

The reachable path is end-to-end: `web/worklet/clipper-processor.js:479` passes `+data.value` to `_amp_set_param` with no validation, while the input-trim handler five lines away at `:386` *is* guarded with `Number.isFinite`. `tools.ts` declares `minimum: 0, maximum: 1` in the tool schema but nothing enforces it, so any non-numeric model emission (`"max"`, `null`, `{}`) becomes NaN via `Number(...)` and reaches the engine.

**Fix:** reject non-finite at the ABI boundary (`if (!std::isfinite(value)) return;` in every `*_set_param`), make the clamps NaN-safe, and export `amp_reset` / `*_reset` so the front-end has a recovery path.

### 2. Cab swap runs 11–46 ms of allocation and FFT setup inside `process()`

**Measured on the shipped WASM artifact**, against a 2.67 ms render deadline at 48 kHz/128:

| call | wall time | vs deadline |
|---|---|---|
| `amp_process_stereo(128)` | 0.025 ms | 0.9 % |
| `amp_set_cab_builtin` | **11.4 ms** | **427 %** |
| `amp_load_custom_ir(4096)` | **45.7 ms** | **1714 %** |

`_commitPending()` is called from inside the per-sample loop at `web/worklet/clipper-processor.js:641`, and it calls `_amp_set_cab_builtin` / `_amp_load_custom_ir`, which synthesise an IR, heap-copy, peak-normalize and run `CabConvolver::prepare` twice (FFT plan + 32 partition spectra per side). That is up to 17 consecutively dropped render quanta — an audible dropout on every cab change and every IR upload.

The design comment at `:425-427` justifies it as inaudible "because it runs at the output-zero of the declick". Output-zero prevents a *step discontinuity*; it does nothing about *missing the deadline*.

Same site also caches `HEAPF32` across the `malloc`: `heap` is fetched at `:627`, `_commitPending()` runs at `:641`, and `heap[...]` is read at `:660`. Every other site in the file correctly re-fetches (the comment at `:543` explains exactly why); this is the one place a `malloc` actually happens mid-block, and the one place the view isn't re-fetched. Verified that growth does detach the view.

**Fix:** build and partition the IR off the audio thread; make the audio-thread step an O(1) pointer swap into a double-buffered convolver pair, exactly as `amp_set_model` already does for the four amp voices. Re-fetch `heap` after any commit, and hoist the commit out of the sample loop.

### 3. `CabConvolver` corrupts its stream on any block size that isn't a multiple of 128

`core/src/dsp/CabConvolver.cpp:111-131`. A short tail is zero-padded to a whole partition, but the FDL and `overlap_` advance by a **full** partition and `partition_ - n` output samples are discarded. The stream is then permanently misaligned, and every subsequent call inherits the corruption.

**Measured** (impulse through the default 2×12 IR, reference = 128-aligned):

- blocks of 100: `max|ref - chunked| = 0.1636` against a reference peak of `0.1521` — **the error is larger than the signal**
- blocks of 64: `max error 0.1535`

This is live: `native/src/PluginProcessor.cpp:288,313` passes `buffer.getNumSamples()` to `engine_.process()`, which only chunks when `numFrames > maxBlock_` (`native/src/ClipperEngine.cpp:372-381`). Any host running 64-, 96-, 100-, 441- or variable-size buffers gets a broken cab.

`core/tests/test_amp_model.cpp:590` (`testConvolverChunking`) is titled "convolver output depends on block segmentation" but compares a whole-buffer call against an explicit 128-block loop — both take the identical internal path, so it passes trivially and never tests a non-128-aligned segmentation, which is the only case that breaks.

**Fix:** give `CabConvolver` a sample-accurate input/output FIFO; never advance the FDL on a partial block. Also move the `float tmpIn[4096]/tmpOut[4096]` stack arrays (32 KB, indexed by the caller-supplied `partition_`) into `prepare()`-sized members.

### 4. The AC30's "GZ34 sag" is a static saturator; the power section is a brick wall above ~0.5 V

`core/src/dsp/Ac30PowerAmp.cpp:297-302`. `vSec ∝ (ipUp − ipDown)` and the gain applied to it is `1/(1 + 15·|ipUp − ipDown|)` — algebraically `y = x/(1+k|x|)`, a memoryless soft clipper, not a supply sag.

**Measured drive sweep** (1 kHz, normalized peak):

| PI grid (V) | 0.05 | 0.1 | 0.2 | 0.5 | 1.0 | 3.0 | 8.0 | 25.0 |
|---|---|---|---|---|---|---|---|---|
| peak out | 0.112 | 0.201 | 0.330 | 0.499 | 0.545 | 0.551 | 0.531 | 0.533 |

A **50× increase in drive buys 0.6 dB.** At 0.05 V the EL84 grids swing ±1.6 V against a −9.5 V bias — dead linear — yet the model already applies **2.4 dB of gain reduction**, because the reduction tracks *instantaneous* signal current rather than *average* draw above idle. So it acts as distortion at every level including clean, and above ~0.5 V it exactly cancels the output rise: no touch sensitivity, no bloom-then-squash, no pick attack.

Secondary defect from the same block: a 2.6 ms attack / 40 ms release envelope on a full-wave-rectified audio signal ripples at 2·f₀ at 80–200 Hz, amplitude-modulating the output on exactly the low notes an AC30 is played on.

The suite corroborates rather than catches it — `core/tests/test_ac30_amp.cpp:363` had to drop its TOP CUT probe from 0.5 V to 0.05 V because "0.5 V is already deep power-amp saturation, where the clipped level is drive-independent".

**Fix:** make sag a *rail* effect. Feed a slow (≥1 cycle) mean of `iSagEnv_` into `Reff` in the rail integration at `:280` and delete step 6b entirely.

### 5. The AC30 top-boost tone stack has a structural ~37 dB mid notch

`core/include/clipper/dsp/Ac30Preamp.h:47-59` + `core/src/dsp/Ac30Preamp.cpp:58-65`. **Measured stack loss alone** (source 45 kΩ, knobs at noon):

| Hz | 80 | 220 | 440 | 880 | 1100 | 2200 | 5000 |
|---|---|---|---|---|---|---|---|
| dB | −8.7 | −14.3 | −21.1 | **−36.5** | **−37.0** | −21.4 | −14.1 |

The whole preamp therefore has **unity gain at 1 kHz** (−0.3 dB at 880 Hz) where the Twin has +51 dB and the JCM +72 dB. Independently corroborated by a full-amp sweep, which showed a −14.3 dB notch at 880 Hz relative to neighbouring bands.

The mechanism is structural, not a value tweak: `Cb` (22 nF) is stamped **in parallel with** the bass rheostat straight to ground, so at 1 kHz its 7.2 kΩ pulls the slope node — the only mid-band path to the wiper — essentially to ground, while `Ct` (47 pF, 3.4 MΩ at 1 kHz) is effectively open. **There is no resistive mid-band path from input to output at any knob setting**, so the notch cannot be dialled out.

Consequence: at a −20 dBFS pluck the modelled AC30 delivers 0.82 V to the PI grid at 220 Hz (hard clipping) but 0.061 V at 1.1 kHz (clean) — a 25 dB frequency-dependent drive imbalance. The amp breaks up only on bass content.

`Ac30Amp.h:74` justifies `kInterstageScale = 0.67` from "a stack that loses ~13 dB" — that is the loss at 220 Hz only; the mid-band loss is 37 dB. The constant was calibrated from an unrepresentative measurement point.

**Fix:** put `Cb` in **series** between the slope node and the bass rheostat with a fixed resistor on the wiper, so a resistive mid path always exists. Then re-derive `kInterstageScale` and `kFullScaleSecV` from the corrected stack.

---

## High

### 6. No parameter smoothing anywhere in the valve amps

`OnePoleSmoother` is used by the pedals, the clean `AmpModel`, chorus, phaser and reverb — and by **none** of `Jcm800*`, `Twin*`, `Ac30*`, which assign knob values straight to state (`Jcm800Preamp.cpp:220-235`, `TwinPreamp.cpp:197-207`, `Ac30Preamp.cpp:183-191`).

**Measured** discontinuity from one knob step, against the signal's own steady-state slew:

| | step / steady slew |
|---|---|
| AC30 VOLUME (its primary overdrive control) | **38.9×** |
| Twin VOLUME | **29.5×** |
| JCM800 MASTER / GAIN | **10.3× / 6.8×** |
| clean120 VOLUME *(smoothed — control case)* | 2.0× |

This violates an invariant the architecture states explicitly. Both `web/worklet/clipper-processor.js:20` and `native/src/ClipperEngine.h:48` say *"Plain knob moves are NOT bracketed — the core's ~5 ms one-pole smoothing already declicks those."* True for pedals; false for exactly the three flagship amps, which sit behind up to 76 dB of gain. In the web app a knob drag pushes values at pointer rate, so it is a continuous stream of these steps.

`setKnobs()` additionally swaps the tone stack's 5×5 conductance matrix wholesale at a block boundary with no cap-state migration.

**Fix:** mirror `AmpModel::Impl` — `OnePoleSmoother` (~8 ms) on volume/gain/master applied per sample, and for the tone stacks either smooth the knobs and rebuild at a 32-sample control rate or interpolate between old and new matrices over a few ms.

### 7. The phase-inverter model cannot express a real long tail; two of three amps idle near cutoff

`core/include/clipper/dsp/Jcm800PowerAmp.h:171-198`. `LtpInverter::Config` is `{bPlus, Ra1, Ra2, Rtail}` — the tail returns to **ground**, so standing current is set entirely by `Rtail`. The code admits the limitation (`Ac30PowerAmp.cpp:50-58`).

**Measured** DC points and small-signal leg gains against the project's own target (`docs/DEVELOPMENT.md:4153`: 0.5–0.9 mA/triode, plates at 70–85 % of B+, ×25–35 per leg):

| amp | Rtail | Va1q | % B+ | Ip/triode | leg1 | leg2 | ratio |
|---|---|---|---|---|---|---|---|
| JCM800 | 10k | 322.1 | **94.7 %** | **0.179 mA** | −15.4 | +9.4 | **0.607** |
| Twin | 22k | 386.8 | **94.3 %** | **0.232 mA** | −7.4 | +7.5 | 1.010 |
| AC30 | 2.2k | 247.1 | 82.4 % | 0.529 mA | −31.8 | +17.5 | 0.550 |

Only the AC30 meets the target, and only because `Rtail` was cut to 2.2 kΩ — which **destroys the long-tail property** (7:1 common-mode rejection, leg ratio degraded to 0.55). The Twin and JCM remain parked at 94 % of B+, near cutoff — exactly the condition the "starved phase inverter" amendment identified as the bug. The fix addressed the AC30 symptom, not the shared cause.

`TwinPowerAmp.cpp:40-46` also contradicts its own header: the header documents "balanced 100k/100k plate loads, 10k shared tail" and claims a measured ratio of 1.007; the code is `Ra1=100k, Ra2=142k, Rtail=22k`. With the *documented* values the measured ratio is 0.668 — the "±1 % balance" is an artifact of a 142 kΩ plate resistor that exists in no AB763.

**Fix:** add `double tailRef = 0.0` to `LtpInverter::Config` and change the tail term from `Vk*gTail` to `(Vk − tailRef)*gTail` in `prepare()` (`:91`) and `processSample()` (`:137`). Then keep `Rtail = 10k` for all three and set `tailRef` to land 0.5–0.9 mA. One change fixes all three PIs and restores the high tail impedance.

### 8. JCM800 `Ra2 = 82 kΩ` moves the imbalance the wrong way; the claimed even-harmonic cancellation is broken

`Jcm800PowerAmp.h:176`, with the header at `:30-37` claiming "the 100k/82k imbalance is deliberate — it evens the two legs' large-signal gain". In an LTP with a finite tail the *driven* leg always has more gain, so the compensating resistor must be **larger** on the undriven side.

**Measured**, varying only `Ra2` through the full power chain:

| Ra2 | leg ratio | H2 (dBc) | H1 @2 V |
|---|---|---|---|
| **82k (shipped)** | 0.607 | **−26.6** | 0.348 |
| 100k | 0.724 | −33.9 | 0.376 |
| 120k | 0.848 | **−41.4** | 0.406 |
| 150k | 1.022 | −27.7 | 0.451 |

`Ra2 ≈ 120 kΩ` gives **15 dB less 2nd harmonic and 1.4 dB more output** than shipped. Full-amp: the JCM leaks −23.1 dBc H2 at 1 V drive where the balanced Twin leaks −42.8 dBc, against a header claim that "EVEN harmonics cancel — the push-pull signature". The pair is matched; its *drive* is not.

`core/tests/test_jcm800_power.cpp:126-165` cannot catch this: part (a) builds `f(V+v) − f(V−v)` from the device law and asserts it is odd — true by construction, testing nothing about the amp. Part (b) is the only real assertion and its bar is 6 dB, for a claim of cancellation. No test asserts the PI leg ratio anywhere.

### 9. The push-pull plate load line is single-ended

`Jcm800PowerAmp.cpp:261`, `TwinPowerAmp.cpp:128`, `Ac30PowerAmp.cpp:184` all use `f = Vp − (rail − (i − iqTube_) * kRppReflected)`. For a centre-tapped primary the correct relation is `Vp_up = rail − (Raa/4)·(i_up − i_down)` — each plate depends on the **differential** current. The code uses each tube's own deviation from idle with `Rpp = Raa/4` and solves the two tubes independently; the conventional single-ended shortcut for this equation uses `Raa/2`, not `Raa/4`.

**Measured** (EL34, rail 467 V): the shipped model needs **530 mA from one EL34** before the plate reaches the knee — more than twice an EL34's real peak cathode current. So plate-load saturation, which `Jcm800PowerAmp.h:72-75` names as the power-stage clipping mechanism, arrives only at unphysical currents; in practice clipping comes entirely from grid cutoff and conduction. `:331` separately labels `(ipUp − ipDown)·kRppReflected` "differential primary voltage" where plate-to-plate is `2·(Raa/4)·(i_up − i_down)` — a factor of 2 low. Both errors are absorbed by `kFullScaleSecV`, so two different physical quantities share one calibration constant.

**Fix:** solve the pair jointly (2×2 Newton with `Vp_up + Vp_down = 2·rail`), use `Raa/2·(i_up − i_down)` for plate-to-plate, then recalibrate `kFullScaleSecV`.

### 10. The EL84 and 6L6 screen-current fits are 3–4× too high; the AC30 screen exceeds its rating at idle

Because `Ig2 = base·kg1/kg2` and `Ip = base·atan(Vp/kvb)`, the screen/plate ratio is a fixed constant:

| tube | kg1/kg2 | Ig2/Ip | idle Ig2 | screen dissipation | limit |
|---|---|---|---|---|---|
| EL34 (JCM) | 0.155 | 0.102 | 3.88 mA | 1.8 W | 8 W |
| 6L6GC (Twin) | 0.324 | **0.210** | 6.07 mA | 2.7 W | 5 W |
| EL84 (AC30) | 0.542 | **0.363** | **12.67 mA** | **3.6 W** | **2 W** |

At full grid drive the EL84 screen draws 32.5 mA → 9.3 W, **4.6× its limit**; real EL84 idle screen current at 35 mA plate is 3–5 mA. `Ac30PowerAmp.h:49-50` says `kg1/kg2` were "trimmed to land the ~35 mA/tube idle" — the trim hit the plate target by wrecking the screen. Since the screen node drives sag and total supply draw, the AC30 sags ~3.6× too deeply.

Also worth ledgering: the Koren pentode screen law has no `Vp` dependence, so `Ig2` never surges when `Vp` falls below `Vg2` — the dominant source of real screen sag and power-amp squish at clipping.

### 11. Denormal traps: two pedals cost ~2× more CPU on silence than on signal

**Measured**, and confirmed as denormals by toggling hardware FTZ (WASM has no FTZ, so there is no escape there):

| | signal | silence | with hw FTZ |
|---|---|---|---|
| RAT | 301 ms | 584 ms (**1.94×**) | 133 ms |
| GOLD | 293 ms | 563 ms (**1.92×**) | 128 ms |

GOLD also emits **392342/480000 subnormal output samples** downstream. Causes: `dcY1_` is the one recursive state in these loops without `flushDenormal` (`OverdriveEngine.cpp:178-181`, `GoldModel.cpp:388`), and the `chowdsp` WDF capacitor state (double) is unguarded — `Denormal.h:57` provides a `double` overload for exactly this case and the WDF path never calls it.

Separately, **none of the seven valve-amp source files include `Denormal.h`** while eight pedal/modulation files do. Unguarded states: `otHpS_`, `otLpS_`, `presLpS_`, `fbDelay_`, `topCutS1/2_`, `iSagEnv_`, the tone stacks' `vT_/iT_/vB_/iB_/vM_/iM_`, `TwinPreamp::brightS_`, `TriodeStage::vCc_/vCk_/vCo_`.

And `core/src/Processor.cpp:40-51` hand-rolls the smoother recurrence without the guard the codebase exists to enforce: measured `finalGain = 1.68e-43` (subnormal), stuck, with 395/400 blocks in subnormal state.

**Note on false confidence:** the project's own `denormal_bench` reports a clean 1.00× cliff ratio — I reproduced that — but it only covers `Biquad` and `OnePoleSmoother`. It says nothing about the paths above.

### 12. The Muff costs 3.2× more CPU when you are *not* playing

**Measured:** 59 % of one core on silence vs 20 % while playing (natively; worse in WASM). It is **not** denormals (hardware FTZ makes no difference) and **not** iteration count (flat at 7).

Root cause, `core/src/dsp/BjtStage.cpp:184-189`: the damped-Newton line search accepts a step only on a *strict* residual decrease (`infNorm3(rt) < cur * (1.0 - 1e-4*lam)`), and the only loop exit is on step size — there is no residual-based early-out. At the quiescent point the residual is already at machine precision, so no trial can decrease it and the loop burns all **30 backtracks × 7 iterations**, each a full Ebers-Moll evaluation with 4 `exp` calls.

**Fix:** add a residual-norm early-out at the top of the loop. Fidelity-neutral — the solution is already converged — and it cuts idle cost ~3×.

### 13. Native doesn't declick amp power / cab / voice changes; the web does

`ClipperEngine::chainEditPending()` (`native/src/ClipperEngine.cpp:171-184`) checks only chain length, order and per-pedal engaged flags — not `ampOn`, `cab`, or `ampModel`. The worklet stages all five edit classes at the fade zero (`_pendingCab`, `_pendingAmpModel`, `_pendingAmpBypass`, `clipper-processor.js:430-471`).

**Measured** native steps against steady slew: **amp POWER 10.2×**, **cab OFF 8.4×**, pedal engage toggle 1.0× (correctly declicked — the machinery exists, it just isn't wired to these). Amp-voice switching additionally changes reported latency by up to 360 samples (192 → 552 measured), renegotiating host latency mid-playback.

`identical_core_test` cannot catch this, because both sides it compares share the gap — and there is **no web↔native parity test at all**. The rig graph is implemented twice (JS worklet and C++ `ClipperEngine`) with parity maintained only by convention.

### 14. `prepare()` discards the parameter targets just pushed into it

`AmpModel.cpp:174-197` hardcodes tone flat, volume 0.5, **reverb 0, chorus OFF** — while `ClipperEngine::prepare` documents the opposite ordering ("push targets first, then prepare so each model snaps to them", `ClipperEngine.cpp:324`).

**Measured:** set reverb 1.0 + vibrato + volume 1.0, call `prepare(48000,128)` — reverb tail 0.000, |L−R| 0.000, impulse peak at the knob-0.5 default. Because `updateParams` only re-applies *changed* knobs, any host `prepareToPlay` (sample-rate change, buffer change, reload) silently reverts the clean amp until the player physically moves each knob. `ChorusModel.h:129`'s "settable before/after prepare" is false.

### 15. The RAT's diode ideality factor was dropped — and the ADAA reference was fitted to the bug

`core/src/dsp/RatModel.cpp:201` uses 1N4148's `Is = 2.52e-9` with `n = 1.0`; the SPICE model is `IS=2.52n N=1.752`. **Measured** clipping ceiling **~0.35–0.43 V instead of ~0.6–0.7 V (5–6 dB low)**, with a harder knee. The code documents ±0.6 V (`RatModel.h:10`).

Worse, `DiodeClipperADAA.h:13-16` was then fitted to the bug — it cites the "~0.33-0.39 V" measurement and sets `kDefaultVk = 0.35`. `BjtStage.h:97` gets ideality right (`nVt = 0.0453`, n ≈ 1.75), so the codebase is internally inconsistent. The same one-constant error (`GoldModel.cpp:157`, `kSiIdeality = 1.0`) makes GOLD's germanium-vs-silicon A/B a **1 dB** difference instead of the real ~6 dB.

### 16. The Muff has no output DC blocker and almost no bass

**Measured** up to **+0.47 V DC** on the output at high sustain (28 % of peak) — there is no high-pass anywhere in `MuffModel`, while every sibling carries `dcBlockHz = 12.0` precisely because "the asymmetric clip produces DC" (`SdModel.cpp:66`). The real pedal's 0.1 µF output cap is absent.

Separately, `BjtStage` drives each 100 nF coupling cap from an ideal source directly onto the base with **no series base resistor** (`BjtStage.cpp:127`), putting the corner at ~250 Hz per stage × 4 stages. **Measured −41 dB at the low E** relative to 1 kHz, with a ~21 dB/octave slope below 60 Hz.

Two follow-ons: `test_muff_model.cpp:307` asserts as canon "~−4.5 dB/stage at 60 Hz" where the measured figure is **−13.9 dB/stage**; and `testHumRejection` passes largely *because of* this defect — it validates a bug as a feature. The DC blind spot is systemic: `test_muff_model.cpp:389`, `test_rat_model.cpp:366`, `test_ts_model.cpp:297` and `test_gold_model.cpp:344` all check DC on **silent** input only.

### 17. The dev proxy binds `0.0.0.0`, exposing an unauthenticated key relay to the LAN

`server/index.mjs:169` calls `server.listen(PORT, cb)` with no host. **Verified reachable** from a non-loopback address; both `/api/health` and `/api/chat` respond. No auth, no rate limit, and the client controls `system`/`tools`/`messages` verbatim with no cap beyond the 5 MB body limit. The startup log prints `http://localhost:8787`, actively misleading. `electron/serve.mjs:117` binds `127.0.0.1` correctly — so this is a divergence, not a decision.

### 18. A mid-stream API error poisons the session permanently

`web/src/assistant/client.ts:111-116` handles `content_block_*` and `message_delta` but not `event: error`, which the API can emit after a 200. The turn then pushes `{role:'assistant', content: []}` into history (`:224`) with no `onNotice`, so the user sees the typing indicator stop and nothing else. That message stays in `historyRef` forever and the API rejects empty assistant content — so **every subsequent turn 400s until reload**.

Related: iteration-cap exhaustion is silent (`:222-249`), and a failed turn leaves an unanswered user message in history (`Chat.tsx:112-138`), producing two consecutive user messages.

### 19. Web UI saturates the main thread while you play

`web/src/App.tsx:477` wires the worklet's ~43 Hz peak report into App-root state, and nothing below it is memoized — `Board`, `Pedal`, `Amp`, `Tuner`, `Knob` are plain function components and `Board` takes 13 inline-arrow props. **The entire React tree re-reconciles ~43×/s, permanently, for as long as the player is plugged in.** The authors knew the pattern was dangerous — `handleTuner` (`:92-99`) is explicitly throttled to 30 Hz "so the app doesn't re-render ~90x/s" — the peak meter got no such treatment.

Compounding, per `pointermove` during a knob drag:

- a synchronous `localStorage.setItem` of the whole serialized rig (`App.tsx:114` → `rig.ts:454`), ~120/s on a trackpad
- `Board`'s `ResizeObserver` is disconnected, reconstructed and re-observed, a `setTimeout` scheduled and dropped, and `measure()` re-runs `getBoundingClientRect()` per jack then sets a fresh `{w,h}` object literal that can never `Object.is`-bail — a forced synchronous layout plus a double render (`Board.tsx:131-179`)

Also: dragging a pedal to reorder pushes a full chain re-commit per boundary crossing (`Board.tsx:203`), so the rig audibly gulps through up to four declick fades during one drag.

---

## Medium

### DSP

- **`OutputLimiter`'s sliding-max ring is one slot short.** `OutputLimiter.h:74` sizes `kLookahead + 2` but `dequePush` runs *before* `dequeExpire`, so the deque must transiently hold 66 entries in 66 slots; `dqTail_` wraps onto `dqHead_` and the structure silently resets. Real defect, one-line fix (expire before push). **But the impact is much smaller than it first appears:** measured maximum overshoot on musical above-ceiling signals is **0.16 dB above the 0.97 ceiling**, absorbed by the headroom to the clamp. The hard clamp fired only on a synthetic single-sample 3.0 impulse, for one sample.
- **The limiter's attack cannot reach target inside the lookahead.** `attackCoeff_ = exp(-1/(kLookahead/3.0))` gives exactly 3 time constants, leaving 5 % residual: output exceeds 1.0 for any input peak above ≈1.57, contradicting "by construction it should never engage" (`:20-22`). Use `kLookahead/6.0`.
- **The reverb does not achieve its documented mode-spacing stretch.** Design rationale cites ~3.3×; **measured 1.10×** (a harmonic comb is 1.00×). The dispersion cascade is only 15 % of the loop, so the constant bulk delay dominates 6:1. Chirp span measured 4.75 ms against a documented ~12 ms; RT60 1.90 s against a documented ~1.7 s. Reaching 3.3× inside a ~30 ms loop needs ~141 sections and ~380 samples of bulk, not 32/1150.
- **The JCM800 cathode follower idles in grid conduction.** Measured `Vgk = +0.26 V` (`Rk = 100 kΩ`, `Jcm800Preamp.cpp:175`) — ~133 µA of standing grid current and *zero* positive-swing headroom before conduction, where a real 2204 CF idles at −1.5…−2 V. The grid is also driven from an ideal source, so that current never loads V2A's plate resistor — the direct-coupled interaction on hard crunch is absent.
- **The composed JCM800 preamp is nearly flat.** Measured 2.7 dB mid scoop across 80 Hz–8 kHz (independently corroborated: I measured 2.6 dB on the same path). `MarshallToneStack` alone gives a correct ~10 dB scoop; it is cancelled by three `820 Ω ‖ 0.68 µF` cathode-bypass shelves worth +3.6 dB each above ~660 Hz. The arithmetic is certain; the individual component values need a schematic check.
- **The chorus mode switch is an unramped hard discontinuity.** Measured an 18× step (0.278 against a 0.016 signal slew) toggling OFF→VIBRATO. `PARAM_CHORUS_MODE` goes through the plain param path while every comparable topology change is declick-bracketed.
- **The opto tremolo's depth collapses and its level sags ~5.6 dB as SPEED rises.** A fixed 55 ms release against a 100 ms LFO period means the LDR never discharges: measured max gain 0.973 → 0.830 → **0.522** across the SPEED range, with effective depth shrinking from −71.9 dB to −27.2 dB.
- **`MuffModel`'s TONE is not smoothed** (`MuffModel.cpp:219`) while SUSTAIN and VOLUME are, and it is used as a per-sample mix coefficient.
- **Control-rate parameter sampling defeats the 5 ms smoother at DAW block sizes.** `OverdriveEngine.cpp:147`, `MuffModel.cpp:161`, `GoldModel.cpp:351` advance the smoother per sample but keep only the chunk-end value: 44 % of the distance traversed per 128-sample chunk, but **99 % at 1024** — i.e. effectively bypassed. `RatModel` does it correctly.
- **`AsymSoftClipper` has zero asymptotic slope**, so the fixed feedback resistor the header cites is missing: above the knee the stage becomes a clean pedestal and the drive knob stops affecting the clipped branch. A real TS keeps a residual `R_fixed/Zg ≈ 11×` slope, which is much of why a cranked Screamer keeps growing.
- **The Big Muff mid-scoop is 2.9 dB deep** (real circuit ≈ 8–12 dB): an in-phase weighted sum of a first-order LP and HP cannot notch, because at crossover both legs are −3 dB with ∓45° and add almost coherently.
- **No `reset()` anywhere in the valve-amp tree**, and `prepare()` is expensive — measured `Jcm800Amp::prepare` at **68.9 ms**, half of it a redundant second `settleDC()` because `prepare()` unconditionally calls `setOversampling()`.
- **The C ABI validates only `handle`.** `in_ptr`, `out_ptr` and `num_frames` are unchecked on every process export, and in WASM address 0 is valid, so a null pointer silently corrupts the low heap rather than trapping. `ReverbModel.cpp:287` does `std::copy(in, in + numFrames, out)` with no `numFrames > 0` guard — for negative input that is a multi-gigabyte `memmove`.
- **`BjtStage::prepare()` — a damped-Newton DC solve — runs on the audio thread** via `processBlock` → `updateParams` → `setPedalOversampling` → `MuffModel::repreparePerRate`, on the order of 10³ Newton solves when the user touches the oversampling combobox.
- **`prepare()`-time redundancies and unbounded rig cost.** Measured WASM: Muff@8× + JCM800 = **117 % of the render deadline**; a 4-pedal chain into the AC30 = 112 %. The UI exposes every combination with no budget model — `renderCapacity` reports underruns after the fact.

### Web / lifecycle

- **Double-start leaks an entire engine.** `App.tsx:456` guards on React state that stays `false` across the whole `await startEngine()` (worklet fetch + WASM init + `getUserMedia`), so two clicks create two AudioContexts, two mic streams and two connected worklets — and `engineRef` keeps only the second. The first is unreachable, unstoppable, and keeps the mic indicator lit.
- **Nothing is ever torn down.** `grep 'return () =>' web/src/App.tsx` returns zero matches across six effects; there is no `beforeunload`/`pagehide` handler. On unmount the AudioContext keeps running and **the microphone stays hot**. `engine.stop()` also never clears `node.port.onmessage`, so the handler closure leaks per Start/Stop cycle.
- **`postMessage` from `process()` allocates on the audio thread ~140×/s** — 47 peak posts/s plus 94 tuner posts/s × 16 KB = **1.5 MB/s of structured-clone allocation**. The tuner rate is 8× redundant by `tuner.ts`'s own reasoning (93 readings/s against a 60 fps needle, with 87.5 % frame overlap).
- **`n > 128` silently overruns the WASM scratch buffers.** Six `_malloc(RENDER_QUANTUM * 4)` allocations are indexed by an unclamped `n` (`clipper-processor.js:532`); with `renderSizeHint` shipping in Chromium a quantum can exceed 128. No test varies the render quantum.
- **WASM init blocks the audio thread ~175 ms at startup** (`amp_create` prepares all four voices up front). This is a deliberate and correct trade — it is what makes `amp_set_model` free (measured 0.000 ms) — but worth documenting.
- Unbounded pre-ready message queue if `createModule()` rejects; tuner tap ring never cleared between engagements; limiter constructed from the global `sampleRate` while everything else uses `this._sr`.

### UI/UX

- **Native: Power, Bright, Cab, Mode and every footswitch fire on `mouseDown`, from any mouse button** (`ClipperLookAndFeel.cpp:529,759,796,841`). No drag-off-to-abort, and a right-click — the gesture for the host's automation menu — silently toggles the control. Accidental mid-song Power-off is the worst live failure mode in either build.
- **Remove and Swap are instantly destructive with no undo anywhere in the project.** Swap replaces a dialled-in pedal with fresh defaults (`App.tsx:242`), and the autosave has already committed it. There is no undo stack, no named presets, and a single implicit save slot.
- **Assistant tool calls rearrange the live rig with no cancel and no undo.** `runAssistant` takes no `AbortSignal`; the input is merely `disabled`, which also drops focus to `<body>` mid-turn.
- **Accessibility.** Web popover menus have no Escape, no click-outside, no focus trap; "Upload IR…" is a `<label>` wrapping a `display:none` input, so it is mouse-only and absent from the a11y tree. Knobs are the most numerous tab stop and the only control with no focus ring, no fine-adjust (1.6 px/unit), and no Home/End/PageUp. Native: `Footswitch`/`ChipButton`/`LeverToggle`/`PowerControl`/`ModeSwitch` derive from bare `juce::Component` — mouse-only, no role, name or state — and `NeuKnob::setName` shadows the non-virtual `Component::setName`, leaving all 17+ knobs anonymous to assistive tech.
- **Contrast.** `.pedal` pins its chassis to the dark tokens on every theme but does not pin `--accent-*`, so in light theme four pedals' value readouts land at **2.2–4.0:1** on the dark chassis. The native tree already found and fixed exactly this — for one pedal, with the reasoning in the comment (`ClipperLookAndFeel.h:86-93`). The web never got it.
- **Native repaint storms.** `boardView_.onScroll` calls full-editor `repaint()`, and each `PedalCard::paint` runs two `DropShadow` blur passes plus one per knob and chip — on the order of a thousand offscreen image allocations and blurs per second during a 42 Hz edge-drag. Live drag-reorder additionally calls `resized()` + full `repaint()` + a `ValueTree` write per card crossing.
- **Tone-knob taper: half the travel is dead.** Measured, lower half vs upper half of the sweep: JCM800 BASS **+9.5 / +0.2 dB**, JCM800 MID **+5.1 / +0.2 dB**, Twin BASS **+11.0 / −0.0 dB**. Same class as the AC30 CUT taper already found and fixed once (§23: "circuit right, knob taper wrong").
- A stray patch cable renders from off-screen below 760 px — `center()` null-checks the element but not a zero `DOMRect`, so a `display:none` jack yields a truthy point (`Board.tsx:134-138`). Tuner burns a rAF loop whether engaged or not. Chat auto-scrolls unconditionally and has no `aria-live`. Native minimum editor width 1040 px.

---

## Fidelity-neutral performance wins

Ordered by payoff. All are bit-identical or bit-comparable.

1. **One oversampler per preamp instead of one per triode stage.** Each `TriodeStage` owns its own `Oversampler` (`TriodeStage.h:183`) and runs serially, so latencies stack: **measured JCM800 preamp 288 samples, full amp 360 (7.5 ms)**; Twin 216; AC30 144. The signal crosses the 129+33-tap halfband cascade **eight times** in the JCM preamp where once each would do. Hoisting one oversampler to preamp level takes the full amp to **144 samples (3.0 ms)** and cuts resampling work ~4×. Repeated band-limiting of an already band-limited signal is also a fidelity *loss*, so this improves both.
2. **Halfband resampler does an integer division per tap.** `HalfbandFilter.h:90-101` indexes `ring_[(w_ - p + M) % M]` with a runtime `M`, so the compiler emits a hardware `div` — 64 per output sample at the oversampled rate, for every oversampled pedal and amp. A doubled ring buffer removes all of them: **measured 3.3× faster, bit-identical output.** Best perf-per-effort item in the project.
3. **Muff line-search early-out** (finding 12) — ~3× on idle.
4. **Two `exp()` where one suffices, in every tube solver.** `softplus(u)` and `sigmoid(u)` are separate functions and every call site evaluates both on the same argument (`TriodeStage.cpp:79-80,100-101` and the three power-amp grid solves). A single helper returning both removes 2 of 4 `exp()` per Newton iteration — bit-identical, ~15–25 % off the solver.
5. **Per-sample divides that are constant after `prepare()`:** `TriodeStage.cpp:316-317` (`rth`, `gThev`, `gCc_/(gCc_+gRgl_)`), `:329,334` (`1.0/Ra`, two per Newton iteration), `:98-101` (three in `gridEval`), `:278` (`1/Rg`, `1/Rk` every sample in the follower), plus `ig0 = gridVgn_/gridRgk_` in all three power amps.
6. **`CabConvolver`: ~4× available.** The FFT is a full complex double transform and the FDL stores all 256 bins, but a real input has Hermitian symmetry — half the storage and half the CMACs are redundant (~2×), and packing the real input into an N/2 complex FFT gets another ~2×.
7. **Reverb recomputes `sin`+`cos` per sample** for a mix value that is bit-identical after ~10 ms — roughly a quarter of the reverb's cost. `OptoTremolo::tick` calls `std::pow` per sample for a value that changes only on knob moves. The phaser calls `std::tan` 4× per sample despite a comment claiming it is hoisted.
8. **Tone-stack matvec is 5×5 dense where 4×4 suffices** (`b[OUT]` is always 0): 25 → 16 multiply-adds per sample.
9. **Redundant `settleDC()`** — `prepare()` settles, then `setOversampling()` settles again: ~9.5 ms wasted per stage.
10. Worklet: an unnecessary 128-float `copyWithin` per quantum, and `this._limiter()` re-resolved once per *sample*.

---

## Test & process integrity

This is the systemic finding, and it explains why several defects above shipped with a green suite.

- **There is no CI.** No `.github/`, no aggregate root `test` script. Every suite that exists is good and passes — nothing runs them automatically.
- **`check-artifact.mjs` only calls `existsSync`.** It is the sole guard on WASM/worklet staleness, wired into `prebuild` and `test`, and it passes for an arbitrarily old artifact. (Both artifacts are in sync right now — verified — but by luck.) `EMSDK_VERSION` defaults to `latest`, so the committed 164 KB `clipper.js` is not reproducible either.
- **`update-goldens.sh` blesses any regression in one command** — no clean-tree check, no diff summary, no confirmation, and the goldens are `.wav` files a reviewer cannot read in a diff. The `--update-goldens` path writes then immediately re-reads the file it just wrote, so it can only ever measure 16-bit quantisation (≤0.11 dB) against a 1.5 dB gate.
- **`core/CMakeLists.txt:255` guards `-UNDEBUG` behind `if(NOT MSVC)`**, so on MSVC the entire 1129-line expectations suite compiles to a no-op `main` that prints "All M11 player-expectations tests passed" — including with **zero golden WAVs present**.
- **Tests that assert the wrong thing** (the recurring class):
  - The four "no pop / declick continuity" tests post the topology change *before* `startRendering()`, so the swap lands at the opening zero crossing with empty filter state — removing the declick entirely would still pass. `cab.spec.ts:155` is doubly vacuous: its "swap" selects `brit412`, which a sibling test asserts is *darker*, so lower slew is guaranteed.
  - Three "perf smoke" tests assert `performance.now()` delta `> 0` and never read an output sample.
  - `'board: move buttons reorder the pedal chain'` never checks the order.
  - `testConvolverChunking` compares two identical code paths (finding 3).
  - `test_jcm800_power.cpp:126` asserts an algebraic identity (finding 8).
  - Every tone-stack test compares the discrete MNA against an analytic `H(jω)` **derived from the same netlist** — validating the discretization, which is correct, but structurally unable to catch a wrong topology or component value. Findings 5 and the JCM flatness are exactly that.
  - `test_twin_amp.cpp:182` and `test_ac30_amp.cpp:195` are commented "PI: balanced anti-phase legs" but assert only a 160 V window on the quiescent plate — the exact hole the starved PI shipped through.
  - The DC assertions in all four dirt-pedal tests check silent input only (finding 16).
- **`OptoTremolo` has no test at all**; `OutputLimiter` is not in the chain the expectations suite drives; no alias-floor test; `test_phaser_model.cpp` renders ~10⁹ phaser samples per run by re-rendering 0.4 s per grid point.
- `web/tsconfig.json` excludes tests, so `npm run build` never typechecks them; `server/` and `electron/` are not typechecked at all. `playwright.config.ts:34` sets `retries: 2`, so any fault appearing in under a third of runs is retried away.

---

## Security & app layer

- **Finding 17** (proxy on `0.0.0.0`) is the largest exposure — one line.
- **Electron main window lacks navigation hardening.** The good parts are right (`contextIsolation: true`, `nodeIntegration: false`, sandbox on, narrow preload). Missing: `will-navigate` deny, `setWindowOpenHandler` (default is allow), and `session.setPermissionRequestHandler` — so any origin loaded inherits the app's granted mic permission.
- **No CSP on the app.** `key-prompt.html` has one (good instinct); the main window, which renders model-influenced text, does not.
- **`electron/config.mjs:58` `mode: 0o600` is only honoured on file *creation*** — an existing `0644` config gets the key rewritten into a world-readable file. Needs an explicit `chmodSync`.
- **The per-turn rig-JSON preamble is never trimmed from old turns**, so a 20-message window re-ships up to 10 full pretty-printed copies of rig state per request, none of it cache-eligible.
- **`electron/serve.mjs:72`** containment check is a bare prefix match (`/app/dist-evil` passes `startsWith('/app/dist')`). I could not construct an exploit — `normalize` absorbs every `..` first — but the check is wrong regardless of reachability.
- **Assistant tool validation is the best-defended surface in the audit** and worth not regressing: explicit `PEDAL_PARAM`/`AMP_PARAM`/`INPUT_PARAM` allowlists, range-clamped pedal indices, safe enum fallbacks, and independent re-validation in `App.tsx:400-438`. No third-party content enters the prompt. The only gap is NaN (finding 1) and `raw | 0` int32 wrapping on indices.
- **Model/API usage is correct and current.** `thinking: {type:'adaptive'}` sent explicitly (necessary, not optional), no `budget_tokens`/`temperature`/`top_p`, correct headers, and textbook `cache_control` placement on the stable system block with volatile rig JSON moved to the user turn. Two notes: the model is one generation behind (a drop-in string change, since `thinking` is already explicit), and `output_config.effort` is never set, so every turn runs at `high`.

---

## Verified correct — do not re-open

Recording these so the ground already covered isn't re-audited.

**Build & baseline:** 16/16 ctest, 70/70 Playwright, `tsc` clean, `vite build` clean, one compiler warning. No `-ffast-math`/`-Ofast` anywhere, so the NaN and denormal guards are not optimized out. Every test target carries `-UNDEBUG` (except under MSVC, above). `chowdsp_wdf` pinned to a SHA, JUCE to a tag. No secrets in tracked files.

**DSP correctness:** Koren triode law and its analytic derivatives; the 3×3 nodal Jacobian (all nine entries re-derived); coupling-network Thévenin and both cap companion updates; trapezoidal cap companions and RHS signs in all three tone stacks; backward-Euler rail/screen/cathode integration; NFB sign and the presence law; AC30 correctly has no global NFB; `BjtStage`'s nine Jacobian entries and six Ebers-Moll partials; the RAT stage-1 shaping algebra (exact to 6 digits); ADAA continuity and C¹-ness across `u = 0` including for asymmetric knees; `LM308Stage`'s no-guard-needed argument; halfband polyphase decomposition and stopbands (measured −79.8 dB / −78.7 dB, meeting the documented ~80 dB); `Oversampler::latencySamples()`; oversampling placed correctly relative to every nonlinearity; chorus 4-point Lagrange interpolation (exact, no wrap or off-by-one); phaser topology (4 allpasses, 2 notches, unity at DC and Nyquist); `Biquad` TDF2 numerics at low frequency/high rate; `OnePoleSmoother` and `OptoTremolo` time-constant math.

**Behaviour:** sample-rate independence verified 44.1 → 192 kHz (triode midband gain varies 0.1 %, DC points bit-identical); NaN/Inf robustness against a ±20 V Nyquist-rate slam (no non-finite output, ≤8 Newton iterations); pedals recover cleanly from a ±20 V blast; the denormal guard works where applied (`denormal_bench` cliff ratio 1.00× on all three cases); `amp_set_model` is a true O(1) flip (measured 0.000 ms); handle reuse across reorder preserves per-pedal state; valve-amp oversampling is fixed at prepare time so `settleDC` never runs on the audio thread; native parameter delivery is race-free (packed atomic, no `ValueTree` on the audio thread); IR transfer ownership is handled correctly.

**Gain structure:** the recent phase-inverter amendment achieved its stated goal — measured compression curves now order correctly, with the AC30 breaking up earliest and hottest, the Twin staying cleanest, and the JCM800 saturating hardest. (The *mechanism* is still wrong — findings 4 and 7 — but the audible ordering is right.)

---

## Claims walked back

Two reports overstated impact, and one of my own hypotheses was wrong. Recording the corrections so severity isn't miscalibrated:

- **`OutputLimiter` deque** — real latent defect, but not "hard clipping on essentially every low-note guitar signal". Measured max overshoot 0.16 dB above ceiling on musical signals; the hard clamp fired only on a synthetic single-sample impulse. Ranked medium above, not critical.
- **SD-1/TS denormal trap did not reproduce** in my configuration (1.05–1.08×, zero subnormals) — only GOLD and RAT did. The unguarded `dcY1_` remains a genuine gap next to guarded siblings, but reachability depends on knob settings.
- **Native latency reporting is correct** — it does include the amps (`ClipperEngine.cpp:497-500`). Only the header comment at `ClipperEngine.h:243-247` is stale.
- **The Twin's MID knob is fine.** I first measured 1.4 dB of authority at 880 Hz and suspected it was dead; it has **14.1 dB at 250 Hz**. My probe frequency was wrong, not the circuit.
- **Amp voice switching does apply at runtime** (RMS matches a fresh engine per voice); it is simply not declicked.

---

## Suggested order of work

1. **Findings 1, 2, 3** — the three shipping-blockers. Finding 1 is a handful of lines and currently lets one bad number brick the rig; finding 3 breaks the cab for a large fraction of DAW users.
2. **Finding 17** — bind the proxy to loopback. One line, removes the largest exposure.
3. **Stand up CI** before the circuit work, and fix the vacuous tests named in each finding. The fidelity changes below need to be *measured*, not asserted, and the current suite would pass either way.
4. **Findings 4, 5, 7** — the AC30 sag, the AC30 stack topology, and the shared PI tail reference. These three are most of the "doesn't sound like the real amp" surface, and 7 fixes all three amps from one place.
5. **Finding 6** — smoothers on the valve amps, restoring a documented invariant.
6. **Performance items 1–4** — the shared oversampler (halves latency and cuts resampling ~4×), the halfband modulo (3.3×, bit-identical), the Muff early-out, and the doubled `exp()`.
7. **Findings 18, 19** and the UI/UX cluster — the assistant history poisoning and the main-thread saturation are both small fixes with outsized effect on how the app feels.
