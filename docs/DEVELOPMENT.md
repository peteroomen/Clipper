# Clipper — Development

Milestone 0 (walking skeleton). This document lists the exact, tested commands
to set up the toolchain, build the portable core, compile it to WASM, and run
the web app plus its automated audio verification.

All commands assume the repository root unless noted. On this machine the repo
lives at `/home/user/Clipper`.

## Repository layout

```
core/                     Portable C++17 DSP core — ZERO platform/OS/browser deps
  include/clipper/        Public headers (Processor.h)
  src/                    Processor.cpp + clipper_c_api.cpp (C ABI for WASM/FFI)
  tests/                  Plain-assert native tests (no framework)
  CMakeLists.txt
web/                      Vite + React + TS app
  src/                    UI (App.tsx), audio engine (audio.ts), params
  worklet/                AudioWorklet processor source (plain JS)
  public/generated/       WASM build output (git-ignored) — clipper.js + worklet copy
  tests/                  Playwright OfflineAudioContext audio test
  scripts/check-artifact.mjs
scripts/
  setup-emsdk.sh          Idempotent Emscripten SDK install
  build-wasm.sh           core -> web/public/generated/clipper.js (WASM ES module)
docs/DEVELOPMENT.md
```

## Prerequisites

- CMake >= 3.16 and a C++17 compiler (clang++ or g++) for the native core.
- Node 18+ and npm for the web app.
- Emscripten SDK for the WASM build (installed by `scripts/setup-emsdk.sh`).

## 1. Native core: build and test

The core has no external dependencies.

```bash
cd core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/clipper_tests
```

Expected: `All tests passed.` The tests cover gain applied after smoothing
settles, absence of NaNs/infinities, and that the one-pole gain smoothing ramps
gradually across a block (no zipper noise on parameter jumps).

## 2. Install Emscripten (once)

```bash
bash scripts/setup-emsdk.sh
```

Idempotent: it is a no-op if `emcc` is already on `PATH`, and skips the download
if `/home/user/emsdk` already contains an activated SDK. Override the location
with `EMSDK_DIR=/some/path bash scripts/setup-emsdk.sh`.

Installed version on this machine: **emsdk 6.0.3** (emcc 6.0.3).

## 3. Build the WASM artifact

```bash
bash scripts/build-wasm.sh
```

This compiles the core with `-O3 -msimd128` (SIMD enabled, as required for later
oversampled nonlinear stages) and emits a single self-contained ES module to
`web/public/generated/clipper.js`, then copies the worklet
(`web/worklet/clipper-processor.js`) alongside it.

The script sources emsdk automatically if `emcc` is not already on `PATH`.

### WASM loading approach (and why)

Built with **`MODULARIZE=1 EXPORT_ES6=1 SINGLE_FILE=1`**:

- **`SINGLE_FILE`** embeds the `.wasm` as a base64 data URI inside the emitted
  `.js`. AudioWorkletGlobalScope has no `fetch`/`XHR`, so embedding the binary is
  the robust way to get it into the worklet — no network fetch at all.
- **`EXPORT_ES6` + `MODULARIZE`** produce an ES module whose default export is a
  factory. The worklet statically `import`s it (Chromium supports static ES
  imports in AudioWorklet module scripts).
- This avoids `SharedArrayBuffer` / COOP-COEP headers entirely.

Both the WASM module and the worklet are placed under `web/public/`, so they are
served verbatim by Vite in **dev, preview, and production** and are never run
through the bundler. The worklet's `import './clipper.js'` therefore resolves
identically everywhere as a sibling file — sidestepping the known fragility of
bundling AudioWorklet modules. The four exported C functions
(`clipper_create/destroy/set_param/process`) plus `_malloc`/`_free` and
`HEAPF32` are used by the worklet to run each 128-frame render quantum through
the C++ core.

Since **M3** the module also compiles the RAT model (`src/dsp/RatModel.cpp`) and
exports the `rat_*` C ABI (`rat_create/destroy/set_param/set_oversampling/`
`latency_samples/process`) alongside the original `clipper_*` gain exports (kept
so the M0 path still links). The RAT model needs the header-only `chowdsp_wdf`
includes; `build-wasm.sh` locates them under `core/build/_deps/chowdsp_wdf-src/`
`include` (the FetchContent checkout) and, if they are missing, runs a `cmake`
configure of `core/` to populate them, failing with an actionable message rather
than a raw include error. Configure the native core once (§6) before the first
WASM build so the dependency is present offline.

The web build/test will fail with a clear message
(`web/scripts/check-artifact.mjs`) if this artifact is missing.

## 4. Web app: install, build, run

```bash
cd web
npm install
npm run build      # type-checks (tsc --noEmit) then vite build; checks artifact first
npm run dev        # dev server at http://localhost:5173
```

Open http://localhost:5173. Click **Start audio** (an AudioContext needs a user
gesture to start). Since **M3** the app is the RAT pedal: pick a **source** (test
tone or live input), then dial the **Distortion / Filter / Level** sliders,
toggle **Bypass**, and pick an **Oversampling** factor; status text shows context
state, sample rate, and latency. The graph is `source (220 Hz oscillator or
getUserMedia) -> AudioWorkletNode (WASM RatModel) -> destination`. You cannot
hear it inside the container; the audio proof is the Playwright suite (§8). (The
M0 gain graph description is superseded — the gain export still ships in the WASM
module but the app no longer wires it.)

## 5. Automated browser verification

```bash
cd web
npm test
```

This builds the app, serves it with `vite preview` on port 4173, and runs the
Playwright suite headless in Chromium:

- **Audio test:** renders the 220 Hz sine through the WASM worklet in an
  `OfflineAudioContext` at gain 1.0 and 0.5, asserting the output is non-silent
  and that the steady-state RMS ratio is ~2x (tolerance 1.8–2.2 for the smoothing
  ramp). This proves audio actually flows through the WASM worklet without
  speakers or a microphone.
- **UI test:** asserts the Start audio button and the 0..2 gain slider render.

### Browser note

The container ships a preinstalled Chromium under `PLAYWRIGHT_BROWSERS_PATH`
(`/opt/pw-browsers`). Do **not** run `playwright install`. `playwright.config.ts`
auto-discovers the `chromium-<build>/chrome-linux/chrome` binary and launches it
with `--no-sandbox` (required when running as root).

## 6. M1 — RAT diode-clipper model (offline)

M1 adds the first real DSP model — a RAT-style diode-clipper distortion — as
**new code beside** the M0 gain core. It is developed and tested entirely
offline (no browser). Wiring it into the worklet is deferred to M3, so nothing
in `web/` or `scripts/build-wasm.sh` changed.

New source:

```
core/include/clipper/dsp/OnePoleSmoother.h   one-pole param smoother (extracted from M0 gain smoothing)
core/include/clipper/dsp/RatModel.h          public API (pimpl; hides the WDF template tree)
core/src/dsp/RatModel.cpp                     the 3-stage model + all circuit values/mappings
core/tests/test_rat_model.cpp                 plain-assert tests (Goertzel, no FFT/framework)
core/tools/render/main.cpp                    clipper-render CLI (native only)
core/tools/third_party/dr_wav.h              vendored WAV I/O (tool-only; never in the DSP core)
```

### Dependency: chowdsp_wdf

The WDF diode stage uses [`chowdsp_wdf`](https://github.com/Chowdhury-DSP/chowdsp_wdf),
pulled by CMake `FetchContent` and **pinned to release v1.0.0**
(commit `36b5775555af21f0f417d2bc866ba7b4b2788614`). It is header-only; the
checkout is cached under `core/build/_deps`, so re-configuring an
already-populated build does not re-download. Its headers are included as
`SYSTEM` so they do not trip our `-Wall -Wextra`. The library's own
tests/benchmarks are force-disabled (no transitive CPM downloads).

`dr_wav` (public domain, `mackron/dr_libs`, v0.14.6, commit
`34a89ffe6bfc4d78db6888fef76cd408dba18185`) is vendored verbatim and included
**only** by the render tool.

### Build and test

Same flow as §1 — the M1 targets are added to the existing `core` build:

```bash
cd core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/clipper_tests       # M0 gain tests: "All tests passed."
./build/clipper_rat_tests   # M1 model tests: "All RatModel tests passed."
```

The first configure clones `chowdsp_wdf` (needs network); later builds are
offline. `clipper_rat_tests` is compiled with `-UNDEBUG` so `assert()` stays
live even in a Release build.

### Render harness (`clipper-render`)

WAV in → WAV out, or a generated test signal (no input file needed). Output is
**32-bit float mono**; input may be mono/stereo 16/24-bit PCM or 32-bit float
(stereo is down-mixed to mono). Params are knob positions in `[0,1]`; defaults
are distortion 0.7, filter 0.4, level 0.8. Run from `core/`:

```bash
# Generated 220 Hz sine, 2 s, three distortion settings, dumping spectra:
./build/clipper-render --gen sine:220:2.0 /tmp/s220_d30.wav --distortion 0.30 --filter 0.3 --level 0.8 --spectrum /tmp/s220_d30.csv
./build/clipper-render --gen sine:220:2.0 /tmp/s220_d70.wav --distortion 0.70 --filter 0.3 --level 0.8 --spectrum /tmp/s220_d70.csv
./build/clipper-render --gen sine:220:2.0 /tmp/s220_d95.wav --distortion 0.95 --filter 0.3 --level 0.8 --spectrum /tmp/s220_d95.csv

# Log sweep 20 Hz -> 20 kHz over 4 s:
./build/clipper-render --gen sweep:20:20000:4.0 /tmp/sweep.wav --distortion 0.8 --filter 0.4 --level 0.8

# Process a real WAV file:
./build/clipper-render in.wav out.wav --distortion 0.5 --filter 0.6 --level 0.7
```

Other flags: `--sr SR` (sample rate for `--gen`, default 48000), `--amp A`
(generated amplitude in "volts", default 0.3), `--spectrum file.csv` (magnitude
spectrum of the last second, `freq,magnitude_db` rows at ~1 Hz resolution up to
20 kHz — harmonics of a periodic tone land on bins).

Sanity check: the odd harmonics of a 220 Hz tone grow with distortion while even
harmonics stay near the noise floor (symmetric clipping), e.g. from the CSVs
above the 660 Hz / 1100 Hz bins rise roughly `-21.7 / -28.8 dB` (dist 0.30) →
`-17.7 / -22.7 dB` (dist 0.95), while 440 Hz sits below `-190 dB`.

### Parameter mapping

All three params are normalized knob positions `[0,1]` (`clipper::dsp::RatModel::ParamId`),
mapped internally and one-pole smoothed (~5 ms, the M0 philosophy):

| Param (id) | Knob 0 | Knob 1 | Mapping | Notes |
|---|---|---|---|---|
| `PARAM_DISTORTION` (0) | 0 dB | +66 dB | linear-in-dB pre-clip gain | plus the two-corner RAT feedback voicing (see below; M6.1 re-voice — was +54 dB + a single 320 Hz shelf) |
| `PARAM_FILTER` (1) | 20 kHz (bright) | 500 Hz (dark) | log-swept one-pole LP cutoff | RAT convention: clockwise = darker |
| `PARAM_LEVEL` (2) | 0.0 | 1.0 | identity linear gain | audio-taper law is a future refinement |

### Circuit model & assumptions (circuit-informed, NOT SPICE-accurate)

Reference level: input float `1.0f == 1.0 V` at the diode stage (a hot humbucker
DI peaks ~0.3 V), so pre-gain must lift the signal past the diode knee to clip —
as the real LM308 stage does.

1. **Gain / shaping (LM308 non-inverting amp).** Variable pre-gain 0…**+66 dB**
   (M6.1: was +54 dB). The real RAT non-inverting gain is `1 + P1/(R1‖R2)` with
   P1 = 100 k Distortion pot; at max the two ground legs short to R1‖R2 ≈ 43 Ω
   → +67.3 dB HF plateau, so +66 dB is essentially that plateau. **Pre-clip
   voicing (M6.1 re-voice against the real circuit).** The RAT feedback network
   has TWO series-RC legs from the inverting input to ground —
   **560 Ω + 4.7 µF** (corner ≈ 60.5 Hz) and **47 Ω + 2.2 µF** (corner ≈ 1539 Hz)
   — so the stage gain **rises with frequency in two steps** and falls toward
   unity below ~60 Hz (NOT a fixed −10.5 dB shelf below 320 Hz, which the old M1
   single shelf used). Normalized to the HF plateau (= the knob's dB), the
   response reduces exactly to `shaped = x − g1·LP₆₀(x) − g2·LP₁₅₃₉(x)`
   (g1 = 0.0774, g2 = 0.9222 — two one-pole low-passes at the leg corners; see
   `RatModel.cpp`). *Validation* (core test `testPreClipVoicing`, small-signal
   shape vs the analytical `A(f)` at Rf = 100 k, dB relative to the 3 kHz plateau,
   measured within **±1.5 dB** — worst 0.67 dB at 44.1 k): 82 Hz **−18.2 dB**,
   320 Hz **−11.5 dB**, 1539 Hz **−1.8 dB**, 5 kHz **≈ 0 dB** (analytical targets
   −18.9 / −11.8 / −2.1 / −0.4 dB). *Assumption:* the shape is fixed at Rf = 100 k
   (the real pot-dependent shape flattens at low DISTORTION). The op-amp's
   bandwidth + slew limits (the LM308) are modeled as of **§11.4 M6.5** — placed
   at the op-amp output inside oversampling, before the diode clamp.
2. **Clipper (WDF, `chowdsp_wdf`).** Antiparallel silicon diode pair to ground
   (the 1N4148/1N914 SPICE card: Is = 2.52 nA **with its ideality factor
   n = 1.752**, Vt = 25.85 mV, one diode/side → a soft ±0.6–0.7 V knee; the
   library carries `n` as its `nDiodes` argument, `Vt_eff = n·Vt = 45.29 mV`).
   **The ideality factor was missing from M1 until 2026-07-25** and the clipper
   actually topped out at 0.32–0.43 V, 5–6 dB below what this bullet claimed —
   audit finding 15, fixed in **§36** (ADR 008), which carries the measured
   before/after curve, the refit of `DiodeClipperADAA::kDefaultVk` (0.35 → 0.67)
   and the level/THD consequence at realistic input. Built
   exactly like the library's RC diode-clipper example: a resistive voltage
   source (series Rs = 1 kΩ) in **parallel** with a shunt capacitor (Cp = 10 nF),
   feeding a `DiodePairT` root (Werner et al. "Best" model). Output is the
   voltage across the shunt cap (= the clipping-node voltage). The shunt cap is
   from the library example — it aids stability and adds a gentle ~16 kHz HF
   corner. *Assumptions:* Rs and Cp are modeling choices, not measured RAT values.
3. **Tone / output (RAT "Filter" + "Volume").** One-pole passive low-pass whose
   cutoff the FILTER knob log-sweeps (bright→dark), then LEVEL as clean linear
   gain.

**No oversampling/antialiasing in M1** (that is M2): high-gain settings alias
("fizz") on purpose. The model runs at the host sample rate; the WDF stage runs
in `double` for numerical stability.

## 7. M2 — Antialiasing (oversampling + ADAA)

M2 kills the aliasing ("fizz") that M1 left in on purpose. Only the **nonlinear**
stage-2 clipper is antialiased; stages 1 (gain/shaping) and 3 (tone LP) are
linear and stay at the base rate. Everything here is measurement-driven — the
tests assert on measured spectra, not vibes.

New source (all header-only DSP, platform-free C++17):

```
core/include/clipper/dsp/HalfbandFilter.h    Kaiser halfband taps + polyphase 2x interp/decim
core/include/clipper/dsp/Oversampler.h       1x/2x/4x/8x cascade of 2x halfband stages
core/include/clipper/dsp/DiodeClipperADAA.h  memoryless tanh clipper w/ 1st-order ADAA (experimental)
core/tools/measure/AliasMetric.h             shared aliasing metric (Goertzel) for tests + CLI
```

`RatModel.cpp` gains `setOversampling`, `setStage2Mode`, `latencySamples`, and an
internal `processChunk` (see below). No `web/` or `scripts/build-wasm.sh` change:
the WASM build still ships only the M0 gain core; the RAT model is wired to the
browser in M3.

### New RatModel API

| Method | Effect |
|---|---|
| `setOversampling(int factor)` | Select 1 / 2 / 4 / 8× for the nonlinear stage (other values snap **down** to the nearest valid power of two). **Default 4×.** Takes effect **immediately**, resetting the oversampling filter state and re-preparing the WDF cap at the oversampled rate (a click is possible on a live change — set it before playing). |
| `oversampling()` | Current factor. |
| `latencySamples()` | Round-trip group delay of the OS filters, in **base-rate** samples (0 at 1×). |
| `setStage2Mode(mode)` | `STAGE2_WDF` (production default) or `STAGE2_ADAA` (experimental memoryless comparison path). |

The public 3-param knob API is unchanged. **Factor 1 reproduces the M1 signal
path bit-for-bit** (verified: `max |os1 − M1 golden| = 0.0`), so it is the
regression guard.

`process()` now **chunks** the input into ≤ `maxBlockSize` sub-blocks internally
(the render tool and tests hand it the whole signal at once), so the fixed
oversampling scratch — allocated once in `prepare()` for the 8× worst case — is
never overrun and `process()` performs **no heap allocation**.

### Oversampling filter design

A hand-rolled **polyphase halfband cascade**: each 2× stage is a Kaiser-windowed
halfband FIR (cutoff π/2; every even tap zero except the centre = 0.5). 4× = two
stages, 8× = three. Up = zero-stuff + halfband LP (the even output phase is a
delayed copy of the input via the centre tap, the odd phase is the odd polyphase
branch); down = halfband LP + decimate (sparse FIR, one output per two inputs).
Coefficients are generated at `prepare()` (`makeHalfband(M, beta)`), never in
`process()`.

The **first** (lowest-rate) 2× stage has the tightest transition: at a 44.1 kHz
base the 20 kHz audio edge sits at 0.2268 of the 88.2 kHz stage rate, just below
the π/2 (0.25) cutoff. Every later stage runs at ≥ 176.4 kHz where 20 kHz is a
small fraction of the rate, so a much shorter halfband clears the stopband target
with margin.

| Stage | Taps (L = 2M+1) | Kaiser β | Passband ripple (≤20 kHz @ 44.1k) | Stopband |
|---|---|---|---|---|
| First 2× (tight) | **129** (M=64) | 7.857 | 0.0013 dB | −81 dB |
| Later 2× (relaxed) | **33** (M=16) | 7.857 | ≤ 0.0006 dB | −88 dB |

Cascaded passband ripple ≪ ±0.1 dB (measured 1 kHz through-gain shift 1× → 4× is
**0.0007 dB**). Group delay (base-rate samples), from `latencySamples()`:

| Factor | Stages | Latency (base samples @ 44.1k) |
|---|---|---|
| 1× | 0 | 0 |
| 2× | 1 | 64 |
| 4× | 2 | 72 |
| 8× | 3 | 76 |

The WDF capacitor is `prepare()`d at the **oversampled** rate (`sampleRate ×
factor`) so its ~16 kHz HF corner lands correctly; the diode impedance
propagates from the cap.

### ADAA comparison path (experimental)

`DiodeClipperADAA` is a **memoryless** stand-in for the WDF diode stage, used only
to compare first-order antiderivative antialiasing against oversampling — it is
**not** the production clipper.

- **Transfer curve match:** `f(x) = Vk·tanh(x/Vk)`, **`Vk = 0.67`**. The WDF diode
  node's *static* curve (measured by settling the WDF at DC) has small-signal
  slope ≈ 1 (diodes off) and a soft knee whose output saturates around
  **0.55–0.74 V** under realistic overdrive (it keeps rising ~logarithmically
  rather than hard-limiting). `tanh(x/Vk)·Vk` matches the unity origin slope
  exactly and limits at ±Vk; Vk is **fitted to the WDF by least squares in dB**
  over 41 log-spaced drive levels from 0.5 V to 100 V (optimum 0.6659, rms error
  0.605 dB; the shipped 0.67 costs 0.607 dB). The tanh flattens where the diode
  keeps creeping up — a documented approximation; the comparison is about
  aliasing, not an exact curve.
  **`Vk` was 0.35 until 2026-07-25**, quoting a "0.33–0.39 V" WDF measurement that
  was really a measurement of the missing diode ideality factor — the reference had
  been fitted to the bug. See **§36** (audit finding 15, ADR 008) for the refit and
  for `testAdaaTracksWdf`, the new test that ties the two constants together so this
  cannot silently recur. The table below predates the fix; its *relative* ADAA-vs-WDF
  verdict is unaffected (both paths moved together) but its absolute dB are stale.
- **ADAA:** first-order (Parker/Esqueda/Bilbao/Välimäki 2016)
  `y[n] = (F1(x[n]) − F1(x[n−1]))/(x[n] − x[n−1])`, `F1(x) = Vk²·ln(cosh(x/Vk))`
  (evaluated stably), with the divided-difference fall-back
  `f((x[n]+x[n−1])/2)` when successive inputs are within 1e−6.

**Findings** (f0 = 4186 Hz, dist 0.9, fs = 44.1 kHz, worst-alias dB rel.
fundamental; CPU = ms to process 1 s of audio in 128-frame blocks, native
`-O3`):

| Path | worst-alias | sum-alias | CPU (ms/s) |
|---|---|---|---|
| WDF 1× (M1 baseline) | −18.4 | −13.2 | 2.9 |
| WDF 2× | −26.7 | −21.0 | 27.9 |
| **WDF 4× (shipped default)** | **−86.6** | **−80.3** | **45.3** |
| WDF 8× | −90.6 | −88.2 | 81.0 |
| ADAA 1× | −25.3 | −24.2 | 1.6 |
| ADAA 2× | −38.5 | −36.7 | 25.3 |
| ADAA 4× | −115.0 | −107.8 | 40.0 |

**Verdict.** ADAA@1× (−25.3 dB, 1.6 ms) beats WDF@1× (−18.4 dB) for *less* CPU,
and ADAA@2× (−38.5 dB) beats WDF@2× by ~12 dB — first-order ADAA is a cheap,
genuine win at low oversampling. But it **plateaus**: it does not on its own reach
the ≥60-dB-below-fundamental "inaudible alias" bar. Oversampling does — WDF@4×
hits −86.6 dB, limited by the ~81 dB halfband stopband floor (which is why 8× only
adds ~4 dB over 4× here: 4× already sits near the filter floor). At high factors
the halfband filtering dominates CPU, so WDF and ADAA converge (81 vs 70 ms @ 8×).
**Shipped default: WDF + 4× oversampling** — inaudible aliasing at ~0.045× real
time natively, with generous headroom. ADAA stays available (`--stage2 adaa`) for
measurement and as a future cheap low-OS option.

On a first-order ADAA note: its benefit grows with input frequency, so the
in-test demonstration uses f0 = 12 kHz (odd harmonics fold hard), where ADAA beats
the naive memoryless clipper by **15.6 dB** (worst) / 16.5 dB (sum). Avoid
f0 = fs/4 for these measurements — every odd harmonic folds onto the fundamental
(degenerate).

### CLI additions (`clipper-render`)

```bash
# Aliasing metric table for os=1/2/4/8 (renders a 4186 Hz tone at high drive):
./build/clipper-render --alias-report --sr 44100 --distortion 0.9
./build/clipper-render --alias-report --sr 44100 --distortion 0.9 --stage2 adaa

# Render with an explicit oversampling factor / stage-2 mode:
./build/clipper-render --gen sine:4186:1.0 out.wav --distortion 0.9 --os 8
./build/clipper-render in.wav out.wav --os 4 --stage2 wdf

# Sweep A/B: alias foldover in the last-second spectrum, 1x vs 8x (high drive):
./build/clipper-render --gen sweep:20:20000:4.0 s1.wav --distortion 0.9 --filter 0.0 --level 0.9 --sr 44100 --os 1 --spectrum s1.csv
./build/clipper-render --gen sweep:20:20000:4.0 s8.wav --distortion 0.9 --filter 0.0 --level 0.9 --sr 44100 --os 8 --spectrum s8.csv
```

The `--alias-report` table prints worst-alias, sum-alias, fundamental amplitude,
and latency per factor — the expected monotonic story at 44.1 kHz:

```
  os  worst-alias(dB)  sum-alias(dB)  fund-amp   latency(smp)
   1         -18.4          -13.2    0.44726   0
   2         -26.7          -21.0    0.45470   64
   4         -86.6          -80.3    0.46202   72
   8         -90.6          -88.2    0.46580   76
```

For the sweep A/B: the last-second spectrum's **sub-2 kHz** band (no legitimate
signal there — the fundamental is 3.5–20 kHz over that second) carries the alias
foldover. Summing that band's energy from the two CSVs gives **1×  −24.4 dB vs
8×  −45.2 dB = 20.7 dB less aliasing**, while full-band energy is unchanged
(0.2 dB) — oversampling removes aliases without altering the tone.

### Build and test (M2)

Same flow as §6; the M2 tests are added to `clipper_rat_tests`:

```bash
cd core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/clipper_rat_tests   # M1 + M2 tests: "All RatModel tests passed."
```

M2 test coverage (all Goertzel-based, no FFT): aliasing improves monotonically
1×→2×→4×→8× (strict at 44.1 kHz; weaker floor-limited checks at 96 kHz), 4× ≥ 20 dB
better than 1×, 8× worst-alias < −60 dB; passband integrity (1 kHz within 0.2 dB,
3rd/5th harmonic ratios within 3 dB of 1×); factor-1 bit-regression vs an M1
golden; ADAA ≥ 12 dB better than the naive memoryless clipper; and an 8× perf
sanity bound (1 s in ≪ 500 ms; measured ~60–75 ms).

## 8. M3 — Live in the browser

M3 wires the M2 RAT model into the AudioWorklet skeleton and makes it playable
live: **guitar → audio interface → `getUserMedia` → worklet (RatModel) →
speakers**, plus a built-in **test tone** so everything works with no instrument
connected.

What changed:

- **C ABI** (`core/src/clipper_c_api.cpp`): added `rat_create(sr)`,
  `rat_destroy`, `rat_set_param(h,id,v)`, `rat_set_oversampling(h,factor)`,
  `rat_latency_samples(h)`, `rat_process(h,in,out,n)` beside the gain exports
  (opaque handle, `EMSCRIPTEN_KEEPALIVE`). `prepare` uses maxBlockSize 128.
  `clipper_c_api` now links `clipper_dsp` so this compiles/links natively too.
- **`scripts/build-wasm.sh`**: compiles `dsp/RatModel.cpp`, adds the chowdsp_wdf
  include dir, exports the `rat_*` functions. SIMD + SINGLE_FILE + EXPORT_ES6
  unchanged.
- **Worklet** (`web/worklet/clipper-processor.js`): runs the RAT model. Messages:
  `{type:'param', id, value}` (the 3 knobs, 0..1), `{type:'oversampling',
  factor}` (1/2/4/8), `{type:'bypass', on}` (bypass is done **in the worklet** —
  input passed through untouched, so it works even before WASM is ready). Ready
  handshake + pending-queue kept; the `ready` (and post-oversampling `latency`)
  message carries `latencySamples`.
- **App** (`web/src/{audio.ts,App.tsx,params.ts}`): source select (test tone /
  live input), optional input-device selector, Distortion/Filter/Level sliders
  (0..100 display, 0..1 to the core), Bypass toggle, Oversampling select (default
  4×), Start/Stop, a latency/status readout, and a persistent headphone/feedback
  hint when live input is selected.

### Plugging in (real hardware: interface → browser)

1. Connect the guitar to the audio interface (e.g. an **Alesis MultiMix 8** into
   a Mac over USB) and select that interface as the system audio input.
2. Open the app, choose **Live input** as the source, press **Start audio**, and
   grant the microphone permission when the browser prompts. (Chrome first;
   Safari has known getUserMedia/worklet quirks — not blocking per the roadmap.)
3. If several inputs exist, a **device selector** appears (populated by
   `enumerateDevices` after permission — labels are only exposed once permission
   has been granted, a browser privacy rule).

### Constraint rationale (why we disable browser DSP)

Live input requests `getUserMedia({ audio: { echoCancellation:false,
noiseSuppression:false, autoGainControl:false, channelCount:1 } })`. Those three
processors are tuned for **speech on laptop mics** and destroy a guitar DI:
echo cancellation comb-filters and gates, noise suppression chews sustain and
pick attack, AGC pumps the level out from under the distortion. We want the raw,
unprocessed signal — the pedal model is the only thing allowed to shape it. Mono
(`channelCount:1`) matches the model's mono in/out.

### Latency expectations

Two contributions, both shown in the status readout:

- **Model latency** — the oversampling filters' round-trip group delay
  (`rat_latency_samples`): **72 base-rate samples at the default 4×** ≈ **1.6 ms
  @ 44.1 kHz** (0 at 1×, 64 at 2×, 76 at 8×). Small.
- **I/O latency** — `AudioContext.baseLatency + outputLatency`, dominated by the
  interface + OS audio buffer, not our code.

Total felt round trip is the roadmap's **~20–40 ms** — playable but noticeable.
(The headless CI context reports ~40 ms I/O for its fake device, so the UI shows
~41.6 ms total there; real Mac + MultiMix numbers will differ and depend on the
interface's buffer size.) Lower latency is the deferred **native** answer (JUCE
wrap of the identical core), not more web engineering.

### Headphones / feedback

When **live input** is selected the app shows a persistent hint recommending
**headphones**: guitar into a live mic/interface with the modeled output on
**speakers** can feed back (mic → speaker → mic loop), and at high distortion the
model's gain makes that worse. Headphones break the loop.

### Commands (verified)

```bash
# 1. Native core + tests (also fetches chowdsp_wdf on first configure):
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/clipper_tests        # "All tests passed."
./build/clipper_rat_tests    # "All RatModel tests passed."

# 2. Build the WASM artifact (now includes the RAT model):
cd .. && bash scripts/build-wasm.sh

# 3. Web build + headless Playwright suite:
cd web && npm install && npm run build && npm test   # 4 passed

# 4. Dev server:
npm run dev   # http://localhost:5173
```

### M3 test coverage (`web/tests/audio.spec.ts`)

- **UI:** Start button, Source select, Distortion/Filter/Level sliders (0..100),
  Bypass checkbox, Oversampling select all render.
- **Harmonics + LEVEL:** 220 Hz sine at high distortion through the worklet
  (bright filter, default 4×) — an in-page Goertzel confirms the **3rd harmonic
  (660 Hz) is strong**, the **2nd (440 Hz) sits near the floor** (symmetric diode
  clipping = odd harmonics), and **LEVEL 1.0 vs 0.5 halves RMS** (clean linear
  gain).
- **Bypass:** with bypass on, output ≈ the raw oscillator (RMS ~0.71, no added
  3rd harmonic); with it off the processed path clearly adds one.
- **Live input (smoke):** using Chromium's fake-media flags
  (`--use-fake-device-for-media-stream`, `--use-fake-ui-for-media-stream`, set in
  `playwright.config.ts`), select Live input, Start, and assert the AudioContext
  reaches `running` with **no console errors**. Reliable here; safe to skip on a
  runner where the fake device misbehaves, since the offline tests already cover
  the DSP.

## 9. M4 — Pedal UI

M4 replaces the placeholder sliders with the approved visual design
("Design Direction 01 — soft-touch neumorphism + liquid glass") and makes the
whole rig a single serializable structure. **No audio-path changes**: the worklet
(`web/worklet/clipper-processor.js`), the C ABI, and `scripts/build-wasm.sh` are
untouched. The UI still talks to the engine through the same message path
(`param` / `oversampling` / `bypass`) from M3.

### Design system (`web/src/styles/`, plain CSS custom properties)

The tokens and neumorphic recipes are ported **verbatim** from the approved
artifact — no framework, no preprocessor.

```
web/src/styles/
  tokens.css   @font-face (Anton, base64 woff2 — copied as-is) + all design
               tokens. Base :root = light "porcelain"; @media
               (prefers-color-scheme: dark) = "graphite"; both forced via
               :root[data-theme="light"|"dark"] (the theme toggle stamps
               data-theme on <html>, which wins over the media query).
  base.css     reset, body, typography helpers (.display = Anton, .mono, .wrap)
               and the two neu recipes used this milestone: .raised / .well.
  pedal.css    the pedal face, knob anatomy (.k-arc/.k-body/.k-knurl/.k-cap/
               .k-ptr), LED, and footswitch — verbatim from the artifact.
  app.css      page chrome + the restyled secondary M3 controls (selects in
               carved wells, raised transport buttons, status readout, theme
               toggle). Written for this app; uses the same tokens/recipes.
```

**Token map to the artifact.** `tokens.css` is a direct extract of the artifact's
`:root` blocks; `pedal.css` is its `.pedal`/`.knob`/`.fsw` rules (the one
`.amp:not(.on)` selector was dropped — no amp until M5). Knob anatomy: red value
arc is a `conic-gradient` from `225deg` spanning `270deg`, driven by a `--deg`
custom property (`value * 270`) set on `.k-stack` and inherited by the arc and
pointer; the arc dims to `opacity: .3` under `.pedal:not(.on)`. The LED lights
via `.pedal.on .led`. **Glass is intentionally unused this milestone** — it is
reserved for the M6 AI assistant (its tokens still ship in `tokens.css`).

### Components (`web/src/components/`, plain React + CSS)

- **`Knob.tsx`** — controlled 0..1 value with a label and 0..100 readout, exposed
  as an accessible `role="slider"` (`aria-valuemin/max/now`, `aria-label`) so it
  is both screen-reader- and test-addressable. Interactions ported from the
  artifact: vertical **drag** with pointer capture, **wheel**, **double-click**
  reset to a per-knob default (`KNOB_DEFAULTS`), **ArrowUp/Down/Left/Right**. A
  very quiet **tick** plays as the value crosses a detent.
- **`Pedal.tsx`** — the CLIPPER face: model line `DIRT Nº1 · RAT-TYPE`, Anton
  logo, LED, three knobs (Dist/Filter/Level), and a footswitch (`role="switch"`,
  `aria-checked` = engaged) that toggles bypass with a low **thunk**.

The tick/thunk sounds (`web/src/ui-sound.ts`, ported from the artifact) run on
their **own** lazily-created `AudioContext`, created on first interaction (a user
gesture). This context is **completely separate** from the processing graph in
`audio.ts` and must never be injected into the pedal signal path.

### Rig state (`web/src/rig.ts`) — the AI tool-call surface

The whole rig is one typed, serializable structure. It is the single source of
truth in `App` state; the knobs, footswitch, and selects all read/write it, and
every change both propagates to the worklet and persists to `localStorage`
(restored on load, defaults on any parse failure). **This JSON is the surface the
M6 assistant reads and mutates**, and the future preset format.

```jsonc
{
  "pedal": {
    "type": "rat",
    "engaged": true,           // false = bypassed (LED dark, arcs dimmed)
    "params": {                // all normalized knob positions, 0..1
      "distortion": 0.7,
      "filter": 0.4,
      "level": 0.8
    }
  },
  "oversampling": 4,           // 1 | 2 | 4 | 8
  "source": "test"            // "test" | "live"
}
```

`serializeRig()` / `deserializeRig()` round-trip a valid rig exactly;
`deserializeRig` runs `normalizeRig`, which coerces unknown/partial input back to
a valid rig (per-field fallback to `DEFAULT_RIG`), so widening the schema later
(e.g. `pedal` → a `pedals[]` chain for the M5 amp/cab) stays backward-compatible.
`engaged` is the inverse of the worklet's `bypass` flag (`bypass = !engaged`).

`App` mirrors the live rig (and the last worklet param/bypass message) onto
`window.__CLIPPER_TEST__` — a small, stable hook used by the Playwright tests and,
later, a convenient read seam for the assistant.

### Test coverage (`web/tests/audio.spec.ts`)

The two offline-render DSP proofs from M3 are unchanged (they post messages to
the worklet directly) and now run **first**, before any test creates a live
`AudioContext` — many live contexts in one browser process can starve later
`OfflineAudioContext` renders. New UI/state tests: knobs are found by
`role="slider"` (name = param); a **knob interaction** test drives Distortion by
keyboard and asserts both the readout and the worklet param message; a
**footswitch** test asserts the engaged/LED state flip and the bypass message; a
**rig round-trip** test checks exact JSON serialize→deserialize equality and
`localStorage` restore across a reload. Both themes were screenshotted via
Playwright (forcing `data-theme`) to `web/test-results/pedal-{light,dark}.png`
(+ `-bypassed` variants) — molded surfaces, NW light, and red value arcs are
visible and correct in light and dark; bypassed shows the dark LED and dimmed
arcs.

## 10. M5 — Clean amp + cab

M5 adds a JC-120-inspired **clean amp** and a **cab IR convolver**, making the
signal chain `input → RAT pedal → amp (volume + tone stack + bright) → cab IR →
out`. The amp is modeled **LINEAR** (the roadmap's deliberate scope choice: a
solid-state clean platform is honest to model linearly; drive comes from the
pedal). The pedal (`rat_*`) and amp (`amp_*`) are **separate WASM instances**
driven in sequence by the worklet; the cab is part of the amp instance.

New source (all platform-free C++17, `-Wall -Wextra` clean, no allocation in
`process()`):

```
core/include/clipper/dsp/Biquad.h        TDF2 biquad + RBJ cookbook designers (shelf/peak/HP/LP)
core/include/clipper/dsp/FFT.h           hand-rolled radix-2 complex FFT (no deps)
core/include/clipper/dsp/AmpModel.h      public API (pimpl)
core/include/clipper/dsp/CabConvolver.h  partitioned FFT convolver API
core/include/clipper/dsp/CabIR.h         default cab IR generator declaration
core/src/dsp/AmpModel.cpp                linear tone stack + volume + bright
core/src/dsp/CabConvolver.cpp            uniform-partitioned overlap-save convolution
core/src/dsp/CabIR.cpp                   procedural 2x12 IR (seeded, deterministic)
core/tests/test_amp_model.cpp            plain-assert amp+cab tests (44.1k + 96k)
```

### Amp model — tone-stack assumptions

JC-120-informed, **not** a measured transfer function — a musically-sensible
linear approximation (same spirit as the RAT model's circuit-informed comments).
All params are normalized knob positions in `[0,1]`; the three tone controls are
flat at `0.5`.

| Param (id) | Type / center | Range | Notes |
|---|---|---|---|
| `PARAM_VOLUME` (0) | linear-in-dB | −40 … +6 dB | knob 0 = **true silence** (fades out below 3% knob); `db = −40 + 46·knob` above that |
| `PARAM_BASS` (1) | low-shelf @ **100 Hz** | ±12 dB (S=0.8) | knob 0 = −12, 0.5 = 0, 1 = +12 |
| `PARAM_MIDDLE` (2) | peaking @ **650 Hz** | ±9 dB (Q=0.7) | broad mid, JC-voiced |
| `PARAM_TREBLE` (3) | high-shelf @ **3.5 kHz** | ±12 dB (S=0.8) | |
| `PARAM_BRIGHT` (4) | high-shelf @ **3 kHz** | +5 dB (0/1 toggle) | fixed shelf; real bright switches scale with volume — simplified for M5 |
| `AMP_PARAM_CAB` (5) | cab on/off (0/1) | — | **chain-level** id handled by the C ABI wrapper (bypasses the convolver for A/B), not by `AmpModel` |

Signal order: bass → middle → treble → bright → volume. The stack is flat within
~±0.5 dB at all knobs = 0.5 (tested). **Smoothing:** the tone gains (dB) and the
volume (linear) are one-pole smoothed (~8 ms); the four biquads are recomputed
from the smoothed dB values every **32 samples** (a control rate). Because the dB
inputs move only a hair per control tick, the coefficient steps are tiny and
there is no zipper noise on a knob sweep (verified: max per-sample delta stays
below 1.5× the signal's own slope through an abrupt volume jump).

### Cab IR generator — rationale + measured response

`generateDefaultCab2x12IR(sampleRate)` builds a plausible closed-back 2×12 IR
deterministically (a seeded LCG feeds a small diffuse tail — **no**
`random_device`/Date, so tests rely on it). It is a documented **placeholder**
until real IR upload lands. Recipe (see `CabIR.cpp`):

- **1024 samples** @ 48 kHz (length scales with sample rate);
- a direct hit + **two small early reflections** (amplitudes 0.10 / 0.06 — kept
  low so their comb ripple stays < ~1 dB and the smooth filter response
  dominates) + a **tiny seeded exponential-decay tail** (0.02);
- **spectral shaping:** 2nd-order high-pass @ 95 Hz (low cut) → **four cascaded**
  2nd-order low-passes @ 5 kHz (≈ 48 dB/oct speaker rolloff) → peaking +3.5 dB @
  2.5 kHz (presence);
- **normalized to unity passband** (`|H(1 kHz)| = 1`), *not* unit time-domain
  peak — so the cab colors the tone rather than shoving the level ~+14 dB.

Measured magnitude response, **raw DTFT of the IR** (dB relative to 1 kHz; the
right tool for an IR — a Hann-windowed sinusoid-amplitude Goertzel gives
meaningless low-frequency numbers here). The test asserts this shape at 44.1 k
and 96 k:

| Freq | 44.1 kHz | 96 kHz | Expectation |
|---|---|---|---|
| 60 Hz | −8.6 dB | −9.5 dB | sub-bass cut |
| 100 Hz | −2.8 dB | −2.6 dB | low cut easing in |
| 1 kHz | 0 dB | 0 dB | reference / passband |
| 2.5 kHz | +2.5 dB | +6.2 dB | presence bump, not collapsed |
| 5 kHz | −10.9 dB | −9.3 dB | into the speaker rolloff |
| 8 kHz | −37.6 dB | −32.6 dB | far down the steep rolloff |

### Convolver design (partition size, FFT, latency)

Uniform-partitioned **overlap-save** convolution, "one partition behind":

- **Partition P = 128** (matches the worklet render quantum), **FFT N = 2P =
  256** (hand-rolled radix-2, `FFT.h`).
- The IR is split into `K = ceil(irLen/P)` partitions, each forward-transformed
  once at `prepare()`. Per block: FFT the `[prev P, current P]` window, push into
  a frequency-domain delay line, `Z = Σ FDL[head−k]·H_k`, inverse-FFT, take the
  last P samples — the un-delayed linear convolution of the positions that block
  covers.
- **Latency = exactly one partition = 128 samples.** An impulse comes out as the
  IR delayed by 128 samples (`latencySamples()` matches the measured impulse
  delay; the impulse reproduces the IR to ~5e-18 — the FFT partitioning is
  exact). If the IR sample rate ≠ engine rate it is linearly resampled at load
  (fine for the smooth synthetic IR; a documented compromise for future real
  IRs).
- **The latency lives in the output FIFO's seed zeros, not in the FDL indexing.**
  See "Block-size independence" below. Historically the deferral was an FDL
  offset (`FDL[head−1−k]`, i.e. the newest block was not consumed until the next
  cycle); the sum is now un-deferred and the output FIFO is seeded with P zeros
  instead. Same complex products in the same order, so the emitted stream is
  bit-identical — this is a refactor of *where* the delay is stored, not a change
  to the delay.
- `process()` allocates nothing (all FFT scratch, the FDL, the FIFOs, and the IR
  spectra are sized in `prepare()`). The amp+cab chain processes 1 s of audio in
  ~6 ms (44.1 k) / ~12 ms (96 k) — far under real time.

### Block-size independence (2026-07-25, audit finding 3)

`process()` accepts **any** `numFrames` — smaller than, larger than or coprime
with the partition, a different value every call, or `<= 0` (a no-op) — and emits
the **same stream** in every case, bit-identically. Input accumulates in a
one-partition FIFO; `processBlock()` runs only on a genuinely full partition;
output drains from a `2P` ring that `reset()` seeds with P zeros.

Why the FIFO costs no extra latency: an un-deferred `Σ FDL[head−k]·H_k` consumes
input block *m* as soon as it lands, and produces exactly what the output stream
needs one partition later. The 128 samples of latency are therefore a whole
partition of *slack*, and the FIFO spends it. No underflow is possible — after
*K* input samples the convolver has produced `P + floor(K/P)·P` and emitted `K`,
so `available = P − (K mod P) ≥ 1`. Segmenting each call at `P − inFill` keeps the
ring bounded at `2P` and runs at most one block per segment, so nothing allocates.

**The bug this replaced (shipping-blocker).** A block shorter than a partition was
zero-padded to a whole partition and run anyway: the FDL and `overlap_` advanced a
FULL partition for a partial partition of real input, and `P − n` computed output
samples were discarded. The stream was permanently misaligned from that point and
every later call inherited it. This was live in the plugin —
`native/src/PluginProcessor.cpp` passes `buffer.getNumSamples()` straight through
and `ClipperEngine` only chunks when `numFrames > maxBlock_` — so any DAW on 64-,
96-, 100-, 441- or variable-size buffers got a broken cab. Measured
`max |128-aligned reference − chunked|` on the default 2×12 IR, before → after:

| block | impulse (44.1 k) | pluck (44.1 k) | impulse (48 k) | pluck (48 k) | after (all) |
| ----- | ---------------- | -------------- | -------------- | ------------ | ----------- |
| 1     | 0.1521           | 0.3253         | 0.1423         | 0.3256       | 0.0         |
| 64    | 0.1535           | 0.4826         | 0.1442         | 0.5331       | 0.0         |
| 96    | 0.1609           | 0.4241         | 0.1519         | 0.4151       | 0.0         |
| 100   | 0.1636           | 0.4140         | 0.1533         | 0.4365       | 0.0         |
| 128   | 0.0              | 0.0            | 0.0            | 0.0          | 0.0         |
| 441   | 0.0              | 0.5517         | 0.0            | 0.4206       | 0.0         |
| vary  | 0.1556           | 0.6048         | 0.1464         | 0.5013       | 0.0         |

Reference peaks are 0.1521 (impulse) and 0.3260 (pluck) at 44.1 k — i.e. before
the fix **the error exceeded the signal**. Note 128 was already exact, which is
why the goldens and `identical_core_test` never saw this.

The same slice removed the `float tmpIn[4096]/tmpOut[4096]` locals from that
padding path — 32 KB of stack indexed by the *caller-supplied* `partition_`, so
`partitionSize > 4096` was a stack overflow.

`testConvolverChunking` in `core/tests/test_amp_model.cpp` pins the property. It
used to compare a whole-buffer call against an explicit 128-block loop — two
identical internal paths, so it passed trivially and never tested a non-aligned
segmentation. It now renders an impulse and a decaying pluck at block sizes
1/64/96/100/128/441/4096 plus a randomly-varying size, in place and out of place,
with zero- and negative-length calls interleaved, and asserts every one is
**exactly** equal to the 128-aligned reference.

### Total chain latency (reported in the UI)

`Latency · model+cab` in the status readout = **pedal oversampling latency + cab
partition**. At the defaults (4× pedal, cab on): **72 + 128 = 200 base-rate
samples** ≈ **4.5 ms @ 44.1 kHz** (~4.2 ms @ 48 k). Bypassing the cab (Cab off)
or powering the amp off removes the 128; the worklet re-reports latency on those
changes. This matches theory (M2's 72-sample 4× figure plus one 128-sample cab
partition).

### C ABI (`core/src/clipper_c_api.cpp`)

Added beside the `rat_*` exports: `amp_create(sr)` / `amp_destroy` /
`amp_set_param(h,id,v)` / `amp_latency_samples(h)` / `amp_process(h,in,out,n)`.
The amp handle is a small `AmpChain { AmpModel amp; CabConvolver cab; bool cabOn; }`
— `amp_process` runs amp → cab (in-place); `AMP_PARAM_CAB` (id 5) toggles the cab;
`amp_latency_samples` returns the cab partition when engaged, else 0. The default
IR is generated at the engine rate (no resampling).

### Worklet + rig message shapes

The worklet (`web/worklet/clipper-processor.js`) now owns **two** instances and
runs `input → rat → amp+cab → out` with per-unit worklet-local bypass. Messages
gained a `unit` field (back-compatible — a missing `unit` targets the pedal):

```jsonc
{ "type": "param",  "unit": "pedal"|"amp", "id": <int>, "value": <0..1> }
{ "type": "bypass", "unit": "pedal"|"amp", "on": <bool> }   // pedal skip / amp power-off
{ "type": "oversampling", "factor": 1|2|4|8 }                // pedal only
```

The worklet posts a `{type:'latency', latencySamples}` echo on oversampling,
cab-toggle, and amp-bypass changes (the offline tests await this echo as a
delivery/flush barrier before `startRendering`, since a synchronous offline
render can otherwise finish before a just-posted message reaches the processor).

### Rig state (`web/src/rig.ts`)

`RigState` gained an `amp` block; `normalizeRig` fills it from defaults, so an
**M4-shaped saved rig (no `amp`) migrates cleanly** (tested):

```jsonc
{
  "pedal": { "type": "rat", "engaged": true, "params": { "distortion": 0.7, "filter": 0.4, "level": 0.8 } },
  "amp": {
    "type": "clean120",
    "engaged": true,                 // false = amp+cab bypassed (jewel dark)
    "params": {                      // 0..1; tone flat at 0.5; bright/cab are 0/1
      "volume": 0.4, "bass": 0.5, "middle": 0.5, "treble": 0.6, "bright": 0, "cab": 1
    }
  },
  "oversampling": 4,
  "source": "test"
}
```

### UI (`web/src/components/Amp.tsx`, `web/src/styles/amp.css`)

The **CLEAN 120** panel per the approved design: Anton name, four small knobs
(Vol/Bass/Mid/Treble, reusing `Knob`), a **Bright** lever, a **Cab** lever, and a
**Power** rocker with a jewel lamp (lit when engaged; dark + arcs dimmed when
off). `amp.css` is the artifact's `.amp` block (deliberately omitted in M4).
Layout: pedal + amp side by side (`.rig`, wraps on narrow), control desk below.

### Build and test (M5)

```bash
# Native core + all suites (M0/M1-M2/M5), at 44.1k and 96k where specified:
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/clipper_tests        # "All tests passed."
./build/clipper_rat_tests    # "All RatModel tests passed."
./build/clipper_amp_tests    # "All AmpModel + CabConvolver tests passed."
ctest --test-dir build       # 3/3 pass

# WASM (now compiles AmpModel/CabConvolver/CabIR, exports amp_*):
cd .. && bash scripts/build-wasm.sh

# Web build + headless Playwright suite (10 tests):
cd web && npm install && npm run build && npm test   # 10 passed
```

## 11. M6 — The assistant (MVP ships)

M6 adds the conversational **tone coach** — the product's differentiator. A user
says "give me the rhythm tone from The Bends," and the assistant reasons about
their guitar + current rig, changes parameters via tool calls (the knobs
visibly move), explains *why* in terms of what the ear hears, and iterates on
feedback — including non-rig advice ("roll your guitar volume back to 8")
alongside knob moves. **No audio-path changes**: the worklet, C ABI, `core/`,
and `scripts/` are all untouched — the assistant drives the same `RigState`
setters the knobs use.

### Architecture

```
browser (web/)                          server/ (zero-dep Node proxy)
  Chat.tsx ── POST /api/chat ─────────▶  index.mjs ── x-api-key ──▶ api.anthropic.com
   │  (system, tools, messages)              (injects model, max_tokens,
   │                                          stream, thinking; pipes SSE back)
   ├─ assistant/client.ts   SSE parse + tool-use loop (cap 6 iterations)
   ├─ assistant/tools.ts    executes tool_use against the live rig (RigController)
   ├─ assistant/prompt.ts   coaching system prompt + per-turn context preamble
   └─ guitar.ts             guitar profile (localStorage, separate key)
```

- **Proxy (`server/`)** — a minimal Node 22 HTTP server, **zero npm
  dependencies** (built-in `http` + global `fetch`). `server/index.mjs` is the
  http glue (routing, CORS, body reading, SSE piping); `server/handler.mjs`
  holds the request-shaping core and upstream call, written injectable so the
  handler could be **lifted into a serverless function** (see the Vercel note
  below). The API key lives only in the server's environment — never sent to the
  client, never logged; message contents are never logged.
  - `POST /api/chat` — accepts `{messages, tools, system}`, forwards to
    `https://api.anthropic.com/v1/messages` with streaming, and pipes the SSE
    response back verbatim. The proxy **injects** `model`, `max_tokens`,
    `stream: true`, and `thinking: {type: "adaptive"}` server-side — a
    client-supplied `model` is ignored, so the client can never choose the model.
  - `GET /api/health` — `{ok: true, hasKey: boolean}` (advertises key presence
    without leaking it). The chat surfaces `hasKey: false` as a clear notice.
  - With **no key**: `/api/health` reports `hasKey:false` and `/api/chat` returns
    a clear **500** with fix instructions; the UI shows that state gracefully.

- **Client tool-use loop (`web/src/assistant/client.ts`)** — parses the SSE
  stream (`message_start` / `content_block_start|delta|stop` /`message_delta` /
  `message_stop`), accumulating content blocks: `text_delta` streams to the UI
  live, `input_json_delta` accumulates each tool's input JSON. On
  `stop_reason == "tool_use"` it executes **all** tool_use blocks locally, then
  appends the assistant message (**full content array, including thinking blocks
  with their signatures**) + **one** user message containing **all**
  `tool_result` blocks, and continues. Loops until `end_turn` (capped at 6).
  `refusal` and `max_tokens` are handled gracefully (in-chat notice, no crash).

### Anthropic API facts (current — do not rely on training priors)

- Model **`claude-opus-4-8`** (env `MODEL`), `max_tokens` **8192** (env
  `MAX_TOKENS`), **streaming always** (`stream: true`).
- **Thinking**: `thinking: {type: "adaptive"}` is sent explicitly — on Opus 4.8
  omitting it runs *without* thinking. `budget_tokens` / `temperature` / `top_p`
  / `top_k` are **never** sent (they 400 on Opus 4.8). Thinking blocks stream
  with empty text (display omitted) — fine, but their signatures are preserved
  when echoed back in the tool loop.
- **System prompt** is an array with one stable text block carrying
  `cache_control: {type: "ephemeral"}`; the big coaching prompt stays there.
  **Volatile context (rig JSON + guitar profile) goes in the USER turn**, not
  the system prompt, so caching works.

### Tool schema (the AI's hands — small and typed)

| Tool | Input | Effect |
|---|---|---|
| `set_param` | `{unit: "pedal"\|"amp", param, value 0..1}` | Sets a knob (clamped). Pedal: `dist`/`filter`/`level`; amp: `volume`/`bass`/`middle`/`treble`. |
| `set_engaged` | `{unit: "pedal"\|"amp", engaged: boolean}` | Pedal bypass / amp power. |
| `set_switch` | `{name: "bright"\|"cab", on: boolean}` | Amp bright / cab toggle. |

Tool executor (`web/src/assistant/tools.ts`) operates through a `RigController`
implemented by `App` over its existing setters, so the AI's changes move the
knobs, reach the worklet, and persist. Each `tool_result` returns a short applied
JSON (e.g. `{"applied":{"unit":"pedal","param":"dist","value":0.6}}`), and the
change renders as a chip in the chat flow (e.g. `Dist 70 → 55`).

### Guitar profile

A small form in a settings well inside the chat panel (`Guitar` toggle):
model (free text), pickup type (SSS/HSS/HH/P90/other), pickup position (free
text or 1-5). Stored in localStorage under `clipper.guitar.v1` (separate from
the rig's `clipper.rig.v1`), editable anytime, and injected into the assistant's
per-turn context so advice is instrument-specific.

### Chat UI (liquid glass)

`web/src/components/Chat.tsx` + `web/src/styles/chat.css` port the approved glass
design (backdrop blur, 1px specular edge, top sheen — `--glass-*` / `--ai`
tokens): assistant bubbles on glass, user bubbles as inset wells, a typing
indicator, applied tool calls as chips, and a pill input with a send button.
Placed as a right-side panel next to the rig on wide screens; stacks below on
narrow. Conversation is in memory (not persisted across reloads — fine for MVP).
Error states (proxy down, no key) render as a clear in-chat notice with fix
instructions.

### How to run

```bash
# 1. The proxy needs a real Anthropic API key (this is the one path the test
#    suite cannot cover — it must be verified live):
export ANTHROPIC_API_KEY=sk-ant-...
npm run server            # http://localhost:8787  (root package.json)

# 2. The web app (separate terminal). Vite proxies /api -> :8787, so the app
#    calls same-origin /api/chat and the key stays server-side.
cd web && npm run dev     # http://localhost:5173
```

Model / env config (all read by `server/index.mjs`):

| Env | Default | Meaning |
|---|---|---|
| `ANTHROPIC_API_KEY` | — (required for `/api/chat`) | Anthropic key; server-side only. |
| `MODEL` | `claude-opus-4-8` | Injected server-side; client can't override. |
| `MAX_TOKENS` | `8192` | Injected server-side. |
| `PORT` | `8787` | Proxy port (matches the Vite dev proxy target). |
| `MOCK` | `0` | `MOCK=1` **and no key** → `/api/chat` streams a canned `[mock]` SSE (dev, no spend). Ignored when a key is present. |

### Security notes

- The API key is read from the server's environment, used only as the
  `x-api-key` header, and is **never** sent to the client, logged, or included in
  any error body. Message contents are never logged. It does **not** appear in
  the client bundle (the browser only ever talks to same-origin `/api`).
- The tool surface is small and typed; the assistant never sends freeform
  parameter values — only the three tools above, all clamped/validated.
- **The proxy binds loopback only (`127.0.0.1`).** This process is an
  **unauthenticated** relay to your Anthropic key: anything that can reach the
  port can spend it. Until 2026-07-25 it called `server.listen(PORT, cb)` with no
  host, which binds `0.0.0.0`/`::` — the 2026-07-24 audit (finding 17) verified
  both `/api/health` and `/api/chat` answering from a non-loopback address, while
  the startup banner claimed `http://localhost:8787`. Now:
  - `HOST` (default `127.0.0.1`) selects the bind address. `HOST=0.0.0.0` opts
    into LAN exposure deliberately and the banner prints a warning saying what
    that means. A `HOST` that cannot be resolved **fails the bind loudly** with a
    non-zero exit — it never falls back to every interface.
  - The banner reports the address **actually bound**, read back from
    `server.address()`, never a hardcoded `localhost`.
  - `PORT=0` now means "ephemeral port" instead of silently becoming 8787
    (`Number('0') || 8787` was truthy-testing a valid port).
  - There is still **no auth and no rate limit** — that is a separate slice, and
    it is why `HOST=0.0.0.0` is only ever safe on a network you fully trust.
- **A body over the 5 MB limit destroys the request.** `readJsonBody` used to
  throw and let the handler reply 400 while the client kept uploading into a
  socket nobody drained, holding the connection and its buffers open for as long
  as the sender liked. It now calls `req.destroy()` at the throw site.
- `server/index.mjs` exports `resolveHost` / `resolvePort` / `createProxyServer` /
  `startProxy` / `startupBanner` and only self-`listen`s when it is the process
  entry point, so `server/index.test.mjs` can bind it on an ephemeral port without
  a stray listener appearing. `npm run server` is unchanged.

### Vercel-deploy note (handler is liftable)

`server/handler.mjs` is dependency-free and split from the http glue on purpose:
`buildUpstreamPayload` / `proxyChat` take an injected `fetch` and touch no Node
globals. To deploy on Vercel, add `api/chat.js` and `api/health.js` (Vercel
functions) that import from `handler.mjs`, read `process.env.ANTHROPIC_API_KEY` /
`MODEL` / `MAX_TOKENS`, call `proxyChat`, and stream the upstream body back
(`Response`/`ReadableStream`). Set the same env vars in the Vercel project;
same-origin `/api/*` needs no CORS. `server/index.mjs` remains the local dev
entry point.

### Post-M6 hardening: history trimming, error categories, mock mode

Three patterns ported (in spirit) from the sibling project *Riff* and adapted to
Clipper's hand-rolled, dependency-free client + proxy.

**1. Conversation-history trimming (`web/src/assistant/history.ts`).** A pure
function, `trimHistory(messages)`, applied to the outgoing copy before **every**
`/api/chat` request (in `client.ts`; the local `messages` array is left intact,
so the live tool-use loop still echoes full thinking + tool_results). What it
does and why:

- **Caps** the window to the most recent `MAX_MESSAGES` (default **20**) — an
  unbounded history grows cost + latency every turn.
- For **older (completed) turns only**, it replaces each `tool_result`'s payload
  with a tiny marker (`TRIMMED_MARKER = "[trimmed]"`) — the model still sees a
  tool *ran*, without re-processing stale bytes — and **drops `thinking` /
  `redacted_thinking` blocks** (they're only needed while a turn is live).
- The **current in-flight turn is never touched**: thinking blocks (with their
  signatures) and full tool_results in the active loop are echoed back verbatim,
  as the API requires. The protected region is `min(current-turn-start,
  last-KEEP_FULL)` (default `KEEP_FULL = 5`).
- **Correctness invariants** (unit-tested): never exceeds the cap; the window
  always starts on a genuine **user** turn (never a leading assistant turn or an
  orphaned `tool_result` whose `tool_use` was capped away); `tool_use`/
  `tool_result` pairing is preserved (a surviving `tool_use` keeps its block and
  its matching `tool_result` survives with at least the marker); pure (no input
  mutation). Constants are exported for testing.

**2. Error classification (`web/src/assistant/errors.ts`).** A failed
`/api/chat` is classified by HTTP status (or a network failure) into a
`ChatErrorCode` with distinct, actionable copy (`CHAT_ERROR_COPY`), wired into
the chat notice via `client.ts`:

| Category | Trigger | Copy gist |
|---|---|---|
| `proxy_unreachable` | `fetch` itself throws (server down) | run `npm run server` |
| `missing_key` | proxy **500** (also 401/403) | the proxy's own body (exact `export` command) is shown |
| `rate_limited` | upstream **429** | wait a few seconds and retry |
| `overloaded` | upstream **5xx** (529/503/502) | Anthropic briefly down, retry |
| `unknown` | anything else | check the server logs |

`refusal` (stop_reason) is still handled in the tool-use loop, unchanged. The
proxy already **passes upstream status codes through faithfully** (`index.mjs`
echoes `upstream.status`); a server test asserts 429/529/503/401 are not
swallowed.

**3. Keyless dev MOCK mode (`server/handler.mjs` `buildMockSse`).** When the
server runs **without** `ANTHROPIC_API_KEY` **and** `MOCK=1`, `/api/chat` streams
a canned SSE response instead of the 500, so the whole chat UX (streamed text +
the tool-use loop + applied chips + a knob actually moving) works with **no key
and no spend**. It is deterministic and clearly labeled `[mock]` in the text:

- First request → a short `[mock]` line **+ one `set_param` tool_use** (pedal
  `dist` → 0.40), so the loop runs and a knob visibly moves.
- Follow-up request (now carries a `tool_result`) → a text-only `[mock]`
  wrap-up.
- `/api/health` reports `mock: true` (only when there's no key), so the chat
  shows a friendly `[mock]` notice instead of the "no key" error.

The **default keyless behavior is unchanged**: with no key and no `MOCK`,
`/api/chat` still returns the clear 500 with fix instructions.

```bash
# Run the chat with no Anthropic key (canned, deterministic):
MOCK=1 npm run server          # http://localhost:8787  hasKey=false, mock on
cd web && npm run dev          # message -> streamed [mock] text + a knob moves
```

### Build and test (M6 + post-M6)

```bash
# Server unit tests (request shaping, status passthrough, mock SSE; fetch stubbed):
npm run test:server            # 11 passed  (root package.json)

# History-trimming unit tests (pure fn; Node 22 native TS type-stripping):
npm run test:history           # 10 passed  (root package.json)

# Web build + headless Playwright suite (15 tests = 10 audio/UI + 5 assistant):
cd web && npm run build && npm test   # 15 passed
```

M6 web test coverage (`web/tests/assistant.spec.ts`, proxy MOCKED via
`page.route` — **no live Anthropic call**): (a) streamed text renders in the
chat; (b) a canned `set_param dist 0.55` tool_use visibly updates the knob
readout + rig state and the follow-up request carries a `tool_result` block
(asserted by inspecting the second request body); (c) proxy-down shows the error
notice; (d) a 500 (no key) surfaces the server's error message; (e) a 429
surfaces the friendly rate-limit copy (and the raw upstream text does **not**
leak). The existing 10 audio/UI tests stay green. **The one thing the suite
cannot cover is the live-key path** — a real end-to-end request to Anthropic
requires a valid `ANTHROPIC_API_KEY` and must be verified by hand. The keyless
`MOCK=1` path is verified against the real local server (curl / manual).

## 11.1 M6.1 — RAT re-voice + input calibration + output level

A bug-fix pass driven by a real-guitar report ("the RAT has no balls — even
cranked it only gives a small amount of gain/saturation") plus a follow-up
("it needs more volume — the whole rig is too quiet"). Root causes, in order of
impact, and what changed.

**Cause 1 (primary): input level.** A guitar through an audio interface arrives
far below the model's `1.0f == 1 V` diode reference (DIs often peak 0.01–0.05),
so at real-world knob settings the signal barely reaches the ±0.6 V diode knee —
it stays clean/thin no matter how high DISTORTION goes. There was no calibration
control. **Fix:** a rig-level **input trim** (−12…+24 dB, default 0 dB) applied in
the worklet *before* the pedal (a sample multiply — no core change), plus a
**peak meter** (the worklet reports the post-trim block peak; the UI meter marks
the good zone ~−12…−3 dBFS). A/B (low-E pluck, `dist 0.5`, real level `amp 0.03`,
tail RMS): **0.028** (no trim) → **0.077** (+12 dB) → **0.142** (+24 dB, into
sustained clipping). This is the real "balls" fix.

**Cause 2: over-aggressive / wrong pre-clip voicing.** M1 approximated the RAT
op-amp EQ as a single low shelf cutting everything below 320 Hz to a flat
−10.5 dB. **Fix:** re-voiced against the actual ProCo RAT LM308 non-inverting
stage — `A(s) = 1 + Rf/Zg`, `Zg = (560 Ω+4.7 µF) ‖ (47 Ω+2.2 µF)` to ground,
Rf = 100 k — a two-corner rise (≈60.5 Hz, ≈1539 Hz) that falls toward unity at DC
rather than a fixed shelf floor. Implemented as `x − g1·LP₆₀ − g2·LP₁₅₃₉` (see §6
bullet 1). Validated in `testPreClipVoicing` within **±1.5 dB** of the analytical
target (worst 0.67 dB). *(ElectroSmash's ProCo Rat Analysis was requested for
cross-check but the host is blocked by this environment's egress policy; the
network is fully specified by the component values, so the response is derived
analytically.)*

**Cause 3: max gain too low.** `+54 dB` → **`+66 dB`** (`RAT_GAIN_DB_MAX`, i.e.
`kDistMaxDb`) — the real RAT's ≈+66–67 dB HF plateau, so a cranked knob has the
headroom to slam the diodes.

**Cause 4 (task-1 live-path audit): no bug found.** Traced dist-knob →
`setParam` → worklet `{param, unit:'pedal', id:0, value}` → `rat_set_param` →
`RatModel::setParameter`: the 0..1 reaches the core **unscaled**; oversampling is
a unity-passband cascade (the existing factor-1 bit-regression + 1 kHz
0.0003 dB passband tests prove no gain is dropped); the worklet feeds the pedal
the **raw** input (no hidden attenuation). Verified, no change.

**Output level (task 4b).** The rig came out ~20 dB too quiet: the amp VOLUME
default knob (0.4) sat at −21.6 dB under the M5 linear `−40…+6 dB` map, so a
default render peaked at **−26.2 dBFS**. The cab IR is already ~unity-RMS
(measured −1.1 dB for a guitar signal — no makeup needed), so the loss was
entirely the volume taper. **Fix:** a loud-biased **audio taper**
`db(knob) = +6 − 46·(1−knob)⁴`, so the design's default 0.4 == **unity** and the
bottom third stays a usable quiet range (fades to silence at 0). A default render
(0.1 input, 220 Hz) now peaks at **−2.7 dBFS** (+23.5 dB). A **soft limiter** on
the worklet output (transparent below ±0.9, tanh knee to ±1.0) guarantees the
louder staging never emits raw overs; a full-scale bypass passthrough picks up
only ~0.6% 3rd harmonic. Validated: `testChainGain` (amp+cab ≈ unity at default),
`testVolume` (audio taper: 0.4 = unity, +6 dB top, quiet bottom), and a Playwright
full-chain render (healthy peak + never overs).

**Assistant.** `set_param` now also accepts `unit:"input"`, `param:"trim"`
(0..1 → −12…+24 dB); the per-turn rig context carries the input trim (dB) and the
live post-trim peak (dBFS) so the coach can diagnose "no balls" as an input-level
problem and raise the trim.

**A/B evidence.** `bash core/scripts/ab_render.sh` builds the OLD model
(HEAD `RatModel`, +54 dB + 320 Hz shelf) beside the NEW one and renders matched
pluck/sine/sweep signals to an untracked `core/.ab-scratch/` (peak/RMS + spectra).
Output-level before/after: default rig cab peak **−26.2 → −2.7 dBFS**.

**Files changed.** Core: `src/dsp/RatModel.cpp` (voicing + +66 dB),
`src/dsp/AmpModel.cpp` (+ `.h` comment; volume taper), `tools/render/main.cpp`
(`pluck` generator), `tests/test_rat_model.cpp` (+`testPreClipVoicing`,
regenerated factor-1 golden), `tests/test_amp_model.cpp` (+`testChainGain`,
rewritten `testVolume`, updated smoothing bound), `scripts/ab_render.sh` (new).
Web: `worklet/clipper-processor.js` (input trim + peak + limiter),
`src/params.ts`, `src/rig.ts` (`input` section), `src/audio.ts`, `src/App.tsx`,
`src/components/InputStage.tsx` (new), `src/styles/app.css`,
`src/assistant/{tools,prompt}.ts`, `src/components/Chat.tsx`, and the two test
specs (+ `playwright.config.ts` retries for the known WebAudio flake).

## 11.2 M6.3 — JC-120 chorus & vibrato (the amp goes stereo)

The Roland JC-120's soul is its stereo chorus/vibrato. This milestone models it
and takes the rig **stereo from the amp stage on**.

### Circuit rationale (what the real amp does)

After the preamp and spring reverb, the JC-120's signal **forks into two
independent power-amp + speaker paths**: one **dry**, one through a
**bucket-brigade (BBD) delay** whose clock is swept by an LFO. The knobs are
**Speed** and **Depth**; a 3-way selects the character:

- **Chorus** — dry to the LEFT speaker, modulated-wet to the RIGHT. The famous
  "chorus" is not a wet/dry electrical mix — it is the **acoustic sum of the two
  speakers in the room** (or the listener's two ears): a ~5 ms delay difference
  plus movement, heard as width and shimmer with comb-filter motion.
- **Vibrato** — the modulated signal to **both** speakers (no dry reference), so
  you hear true **pitch wobble** rather than chorus.

We model the fork faithfully, not the room: **preamp/tone → chorus split →
per-side cab**. There is no reverb block in Clipper yet, so the split sits right
after the tone stack + volume.

### DSP (`core/src/dsp/ChorusModel.{h,cpp}`)

A single modulated delay line that splits mono → stereo, owned by `AmpModel`
(which routes its `PARAM_CHORUS_*` here).

- **Interpolation — 4-point Lagrange (cubic), not all-pass.** The read tap sweeps
  continuously and **reverses direction twice per LFO cycle**. All-pass
  interpolation is recursive (stateful), so a fast reversal rings its internal
  filter and smears transients; Lagrange is **stateless** — every sample is
  interpolated from scratch with flat-ish group delay and no reversal artifact.
  Cubic (vs linear) keeps the moving-tap HF loss inaudible.
- **LFO — sine, not triangle.** The BBD clock sweeps triangle-ish, but a
  **triangle delay sweep makes the pitch deviation a square wave** — it snaps
  between +Δ and −Δ cents with an audible chirp at each reversal. A **sine** delay
  sweep gives a smooth cosine pitch deviation: musical, and what the ear reads as
  "the JC chorus." (Documented tradeoff — authenticity of the clock waveform vs.
  the artifact-free result; we chose the result.)
- **Numbers (tuned in `ChorusModel.cpp`):**
  - base delay **5.0 ms** — decorrelates the wet side for the stereo bloom;
    long enough to widen, short enough not to read as slapback.
  - depth `0..1` → sine sweep **0 .. 3.5 ms peak** (squared taper, `A = depth²·3.5 ms`;
    widened from 1.5 ms linear on field feedback that the effect was too subtle).
    Full depth swings the wet delay 5 ± 3.5 ms (1.5..8.5 ms) — always well inside the buffer and
    far above the interpolator floor.
  - speed `0..1` → LFO rate **~0.15 .. 8 Hz**, **log** mapped
    (`rate = 0.15·(8/0.15)^speed`) so the musical 0.5–3 Hz range fills most of the
    knob.
- **Peak pitch deviation.** For `delay(t) = D0 + A·sin(ωt)` the instantaneous
  fractional pitch shift is `−d(delay)/dt = −A·ω·cos(ωt)`, so the **peak** is
  `A·2π·f`, i.e. in cents `≈ (1200/ln2)·A·2π·f ≈ 38.1 · depth² · f_Hz`
  (with `A = depth²·3.5 ms`). Examples: **depth 1 @ 2 Hz ≈ 76 ¢**, depth 1 @ 5 Hz
  ≈ 82 ¢, depth 0.5 @ 2 Hz ≈ 16 ¢. Deviation grows with **both** depth and rate
  — the physical truth of a fixed-excursion swept delay; the 1.5 ms cap keeps a
  full-depth mid-rate vibrato lush (~30–40 ¢) rather than seasick.
- **OFF is bit-exact.** Mode 0 copies the input to both sides untouched
  (`L == R`, and `== AmpModel::process()`'s mono voice, bit-for-bit).
- Depth (as sweep-in-samples) and rate (Hz) are one-pole smoothed (~8 ms); the
  LFO phase is continuous so a rate change never clicks. Mode is a **hard switch**
  (a deliberate footswitch action, like the pedal/amp bypass elsewhere).

### Stereo architecture & CPU

`AmpModel::processStereo(in, outL, outR, n)` runs the tone stack + volume (the
same mono voice as `process()`) into `outL` as scratch, then splits via the
`ChorusModel`. The **cab IR runs per side** — two `CabConvolver` instances in the
C ABI's `AmpChain` (`cabL`, `cabR`, same IR). This is required, not optional: in
chorus mode L and R are genuinely different signals, so mono-summing before a
single cab would collapse the bloom. The stereo entry points are additive:
`amp_process_stereo` (C ABI) and the worklet's stereo output path; the old mono
`amp_process` + single `cabL` stay for compatibility.

**CPU (from `testChorusPerf`, amp + chorus + BOTH cabs, 1 s of audio):**

| Sample rate | Time for 1 s | Fraction of one core |
|---|---|---|
| 44.1 kHz | ~6.3 ms | ~0.6 % |
| 96 kHz | ~14.2 ms | ~1.4 % |

The second convolution roughly doubles the cab cost (single cab was ~2.3 ms at
44.1 k) but the whole stereo chain is still **>150× faster than real time** — the
worklet can afford two proper per-side cabs comfortably, so we never compromise
the chorus by summing to mono.

### Test method — how the pitch deviation is measured

`testChorusVibrato` (in `core/tests/test_amp_model.cpp`) renders a steady 1 kHz
probe through vibrato at a **known** rate (the speed knob is inverted from the log
map to hit exactly 3 Hz) and depth 0.7. It finds **positive-going zero crossings**
with linear interpolation, converts each crossing-to-crossing period to an
instantaneous frequency (`f = fs/period`), and takes the **peak** `|1200·log2(f/f0)|`
over the tail. That measured peak is asserted within a broad band (½..1½×) of the
`16.3·depth·f` prediction — deterministic, FFT-free, and it exercises the whole
`AmpModel::processStereo` path. (Zero-crossing spacing averages over a signal
period, so it reads slightly under the instantaneous peak; the band accounts for
that.) Companion tests: `testChorusOff` (L==R bit-exact and == mono voice),
`testChorusChorus` (L is the bit-exact dry voice; R's noise cross-correlation
peaks at the ~5 ms base lag and is decorrelated at zero lag), `testChorusPerf`.

### Web plumbing

- **Worklet** (`web/worklet/clipper-processor.js`) allocates `outL`/`outR` heap
  buffers and calls `amp_process_stereo`; the M6.1 soft limiter runs **per
  channel**. Amp power-off copies the mono pedal signal to both sides. A
  1-channel output (a mono `OfflineAudioContext`) takes the LEFT side only, so the
  pre-M6.3 mono audio tests still read the same signal. The peak meter still taps
  the **post-trim input** (mono, pre-pedal) — unchanged.
- **Node** (`web/src/audio.ts`) is created with `outputChannelCount: [2]`.
- **RigState** (`web/src/rig.ts`) `amp.params` gains `speed`, `depth`, and
  `chorusMode` (0/1/2). `normalizeRig` fills them from defaults, so a pre-M6.3
  saved rig loads with **chorus off** (migration tested).
- **UI** (`web/src/components/Amp.tsx`, `styles/amp.css`) adds a second facia row:
  **Speed** + **Depth** knobs (shared `Knob`) and an **Off / Chorus / Vibrato**
  3-way selector (carved neu segments, active one lit like the bright/cab levers).
- **Assistant** (`web/src/assistant/{tools,prompt}.ts`): `set_param` gains
  `speed`/`depth` (unit `amp`); the 3-way mode reuses **`set_switch`** with the
  name enum extended to `chorus`/`vibrato` (mutually exclusive — turning one on
  selects it, off returns to mode 0), the minimal-diff option consistent with how
  `bright`/`cab` already work. The per-turn rig context already carries the full
  amp params, so the coach sees the chorus state for free.

### Build and test (M6.3)

```bash
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build          # 3/3 (clipper_amp_tests gains 4 chorus tests)
cd .. && bash scripts/build-wasm.sh   # now compiles ChorusModel, exports amp_process_stereo
cd web && npm run build && npm test   # 19 Playwright (17 + stereo chorus + assistant chorus)
```

## 11.3 M6.4 — Pedalboard visual pass (stackable, reorderable chain)

M6.4 turns the single pedal into a **stackable, drag-reorderable chain** joined by
neumorphic cables, with add / remove / swap from a gear tray and the amp fixed at
the end (with an amp-swap affordance). This is the architecture every later pedal
(M7 tuner, M8 SD-1) and amp (M9 JCM800) plugs into. **No core / C-ABI / `core/` /
`scripts/` change** — the RAT model is already handle-based (pimpl, per-instance,
only `constexpr`/pure helpers in its anonymous namespace), so **multiple RAT
instances are independent and stack safely with no DSP changes** (verified). All
the new work is in `web/`.

### RigState — the chain (`web/src/rig.ts`)

The single `pedal` became an **ordered chain** `pedals: PedalInstance[]`. Each
instance has a stable `id` (used by the worklet to reuse its DSP handle across
reorders, and by the UI as the React key / drag id), a `type` (`'rat'`), `engaged`,
and `params`. The amp already carried `type: 'clean120'`.

```jsonc
{
  "input": { "trim": 0.333 },
  "pedals": [
    { "id": "rat-1-x9f2a", "type": "rat", "engaged": true,
      "params": { "distortion": 0.7, "filter": 0.4, "level": 0.8 } }
    // ...more instances; may be EMPTY (guitar straight into the amp)
  ],
  "amp": { "type": "clean120", "engaged": true, "params": { /* … M6.3 … */ } },
  "oversampling": 4,
  "source": "test"
}
```

**Migration (tested).** `normalizeRig` accepts three shapes, in priority order:
(1) a `pedals` array (normalize each; an **empty** array is valid); (2) a legacy
single `pedal` object (M4..M6.3) — **wrapped into a one-element chain**; (3)
neither — the default one-RAT chain. So old saved rigs and preset JSON keep
loading. `AVAILABLE_PEDAL_TYPES` / `AVAILABLE_AMP_TYPES` and `makePedal(type)` /
`newPedalId(type)` are the seams the gear tray and future gear use. The JSON stays
the round-trip preset format (a valid multi-pedal rig round-trips byte-for-byte).

### Worklet — dynamic chain + click-free switching (`web/worklet/clipper-processor.js`)

The worklet now owns an **ordered array** of pedal nodes `{ id, handle, engaged }`
(created at construction with one default RAT so the legacy offline tests, which
never send a `chain` message and address `unit:'pedal'`, keep working) plus the one
amp instance. Per block it **ping-pongs** the mono signal through each *engaged*
pedal (bypassed pedals are skipped), then the stereo amp stage. Latency now **sums
the engaged pedals'** oversampling group delays + the cab partition.

**Message protocol (additions).** A new `chain` message
`{ type:'chain', pedals:[{id,type,engaged,params}] }` sets the whole topology;
`param` / `bypass` gained an optional `pedalId` (missing = the first pedal, for
back-compat). `oversampling` applies globally to every pedal handle.

**Click-free chain edits — a declick output fade (documented choice).** Chain
edits (add / remove / reorder / swap) can step the output waveform (a suddenly
inserted distortion, a reordered nonlinear stage). Rather than crossfade two
parallel chains (which would double-advance the handles that a reorder *reuses*),
the worklet brackets every chain swap with a short **raised-cosine output fade**
(`DECLICK_SECONDS = 6 ms` each way): on a `chain` message it prepares the new node
list in the message handler (**reusing handles by id**, creating new ones,
deferring destruction — *no allocation inside `process()`*), ramps the output to
**zero**, performs the topology swap **exactly at that zero** (a cheap reference
swap), then ramps back up. Because the discontinuity always lands at output-zero
there is **no step/pop and no zipper**. A plain knob change is *not* bracketed —
the core's ~5 ms one-pole smoothing already declicks those, so knob moves during
play use the light `param` message (no fade). Verified click-free by an
`OfflineAudioContext` render (the reorder test asserts same-order determinism —
bit-identical — and that order A vs B differ).

### UI — the board (`web/src/components/Board.tsx`, `styles/board.css`)

`Board` replaces the old `pedal + amp` `.rig` row. Layout: a **guitar-in jack →
pedal instances → amp** left-to-right chain that **wraps on overflow** (the app's
`.wrap` caps content at 1120 px, so 2+ pedals wrap — expected). Each unit is a
positioning wrapper carrying two **side jacks** (carved sockets) and a floating
**control rack** (drag grip, ◀ ▶ move, ⇄ swap, ✕ remove, position number).

**Neumorphic cables.** An absolute SVG overlay *behind* the units draws one
**catenary path** per hop (`source.out → pedal0.in → … → amp.in`): a cubic Bézier
with both control points pulled **down** (gravity droop, sag ∝ span). Each cable is
three layered strokes — a `--cable` rubber body, a `--cable-hi` specular top edge
(nudged up 1.4 px), and `--cable-plug` end plugs — under a `drop-shadow(var(--sh-
dark))` cast shadow, so it reads as a rubber patch cable lying on the surface. Jack
centers are **measured from the live DOM** (`getBoundingClientRect`) and the paths
**redraw on** mount, chain edit, `ResizeObserver`, window resize, and every
`requestAnimationFrame` during a drag. New tokens `--cable` / `--cable-hi` /
`--cable-plug` were added to all four theme blocks in `tokens.css`
(porcelain/graphite) — everything stays inside the token system.

**Drag reorder — hand-rolled, no @dnd-kit (justified).** Reorder is pointer-event
based with pointer capture and **live reorder** (the dragged unit keeps its
`key={id}`, so its DOM node — and the capture — survive the array change): the same
proven pattern as `Knob.tsx`. We deliberately **did not** add `@dnd-kit`: this is a
single horizontal list of a few items, the repo has **zero UI dependencies**, and
avoiding the dep keeps the offline build hermetic. **Keyboard accessibility** is
covered by explicit ◀ ▶ move buttons (focusable, `aria-label`ed) on each pedal.

**Gear tray + amp slot.** An "Add pedal" tray (popover listing
`AVAILABLE_PEDAL_TYPES` = RAT today, with a "more coming" note) sits before the
amp; **swap** = a per-pedal ⇄ menu (remove+add in place — plumbing ready for when
more types exist); the **amp slot** has a "Clean 120 (JC-120 style) ▾" select with
the same pattern ("more amps coming"). **Empty chain** shows a "guitar straight into
the amp" note and still runs.

### Assistant — chain awareness (`web/src/assistant/{tools,prompt}.ts`)

`set_param` / `set_engaged` gained an optional **`pedal`** field (0-based instance
index, default 0) so the coach can address a specific instance; chips tag the
position (`Dist #2 70 → 30`) when there is more than one pedal. Three small typed
chain ops were added — **`add_pedal`** `{type, position?}`, **`remove_pedal`**
`{index}`, **`move_pedal`** `{from, to}` — keeping the surface minimal and typed.
The per-turn rig context already dumps the full `pedals` array (ids + order), so the
coach sees the chain for free; the system prompt now teaches that the chain is an
ordered, editable list (order matters for nonlinear dirt) addressed by index.

### Build and test (M6.4)

```bash
# Core is UNCHANGED (verified multi-instance needs no C ABI change):
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build          # 3/3
cd .. && bash scripts/build-wasm.sh   # WASM binary unchanged; re-copies the new worklet
cd web && npm run build && npm test   # 25 Playwright (19 + 6 new)
```

New Playwright coverage (`tests/audio.spec.ts`, `tests/assistant.spec.ts`):
reorder of two RATs changes the render (order A vs B differ) and same order is
bit-deterministic; adding a pedal changes the sound and an **empty chain** passes
through clean; the board draws SVG cables between units and the gear tray
adds/removes pedals (empty-chain note appears, cable count tracks the chain); the
move buttons reorder the chain in rig state + worklet; an old single-`pedal` rig
migrates to a one-element chain; the multi-pedal rig round-trips through JSON; and
the assistant can address a pedal **instance by index** (`set_param` `pedal:1`
moves the second pedal only). Both themes were screenshotted at the 2-pedal state
(board with cables, gear tray, amp swap) for the visual eyeball.

## 11.4 M6.5 — Fizz fixes: LM308 op-amp model + clean-path re-staging

Two independent fixes for a user report that the rig sounds **fizzy** — "the audio
model is updated but it's fizzy," and, critically, "it's fizzy with it [the RAT]
off too." The second clause pointed at the **clean, pedal-bypassed** path, which
turned out to be the *dominant* fizz source. Both are reported below with numbers.

### Fix A (dominant): the clean path was soft-clipping every cycle

**What was missing / wrong.** With the RAT bypassed the chain is `input(trim) →
amp (linear) → cab (linear) → output soft-limiter`. The only nonlinearity is the
M6.1 output limiter (transparent below ±0.9, tanh knee to ±1.0). M6.1 also made
the amp **loud**: a quartic volume taper with a **+6 dB ceiling** whose default
knob (0.4) sat at **unity**. At realistic input levels (the trim's −12…−3 dBFS
target zone) the amp+cab output ran **past full scale** — so the limiter
soft-clipped **on every cycle**. Soft-clipping a clean amp = fizz.

Measured *before* (default rig, pedal bypassed, 44.1 k, sine at a −3 dBFS input
peak, tail THD):

| f0 | amp+cab out-peak (no limiter) | THD with 0.9 limiter |
|---|---|---|
| 220 Hz | 1.67 | **−35.7 dB** (audibly fizzy) |
| 2.5 kHz | 1.77 | **−33.3 dB** |

**Fix — one coherent level plan.** The amp is a **clean platform**, so its
ceiling is **unity (clean full scale)**, not +6 dB, and the safety limiter is
raised and given real headroom:

```
guitar/DI ──▶ input trim ──▶ [pedal chain] ──▶ amp volume taper ──▶ cab ──▶ output limiter ──▶ out
             target post-trim   (RAT, bypassable)   quartic, UNITY ceiling   ~unity      SAFETY only,
             peak −12…−3 dBFS                        default knob 0.4≈−6 dB   passband    transparent <0.97
```

- **Volume taper** (`AmpModel.cpp`): `db(knob) = kVolMaxDb − 46·(1−knob)⁴` with
  **`kVolMaxDb = 0`** (was +6). Same *shape/feel* (span unchanged, so the tone/
  volume-sweep tests, which assert on spans, are untouched); the ceiling is pulled
  to unity. Default 0.4 now sits at **−6 dB** — deliberate headroom, still ~+16 dB
  louder than the original M5 default (−21.6 dB), so the M6.1 "more volume" win is
  kept and the knob still offers ~+6 dB of clean boost above the default.
- **Output limiter** (`OutputLimiter.h`, mirrored in the worklet as `LIM_THRESH`):
  threshold **0.9 → 0.97**, a narrow tanh safety knee. It now only catches genuine
  transient overs.

Measured *after* (default rig, pedal bypassed): the amp+cab output peaks at a
−3 dBFS input are **0.72 (220 Hz) / 0.48 (1 kHz) / 0.84 (96 k 220 Hz) / 0.63
(low-E pluck)** — all **below 0.97**, so the limiter is **bit-transparent** (never
engages) → the clean path adds **zero** distortion. A/B render (pedal-bypassed
`--chain clean`, 220 Hz sine at a −3 dBFS input): **OLD staging peaks 1.000**
(constant limiting) → **NEW staging peaks 0.856** (dormant).

`testCleanPathTHD` (both sample rates, sines + a low-E pluck) asserts the exact
fix: amp+cab out-peak **< 0.97** and the limiter **bit-transparent**. We do *not*
assert an absolute THD bar because the M5 cab convolver has pre-existing
discrete-bin float-FFT artifacts (present with or without the limiter, at both
rates for some frequencies) that would confound it; the limiter-dormancy pair is
exact, SR-independent, and *is* the fix. `testChainGain` was updated for the new
staging (amp 0.4 ≈ −6 dB, amp+cab in a headroom band below unity).

### Fix B: the LM308 op-amp — the classic digital-RAT fizz

**What was missing.** M1..M6.1 used an **ideal** op-amp in the RAT gain stage:
infinite bandwidth and slew rate passed razor edges straight to the diode clamp.
The real ProCo RAT's **LM308** has two limits the model now reproduces
(`LM308Stage.h`), placed at the **op-amp output node** — after the
frequency-dependent gain, before the shunt-diode clamp, **inside** oversampling:

1. **Gain-tracking closed-loop bandwidth.** One-pole low-pass with corner
   `f_c = GBW / A_noise`, where `A_noise` is the DISTORTION-knob plateau (noise)
   gain, refreshed per chunk from the smoothed pre-gain (so it glides click-free).
   **`GBW = 1.0 MHz`** — the LM308's documented unity-gain bandwidth with its
   ~30 pF compensation (the "0.5–1 MHz" range). At the +66 dB plateau (A ≈ 1995)
   the corner **collapses to ~500 Hz** (thick, not fizzy); at unity gain it is
   1 MHz (clamped to the oversampled Nyquist — transparent).
2. **Slew-rate limiter.** A hard per-sample dV clamp at **`SR = 0.3 V/µs`**
   (LM308 datasheet-typical with standard compensation; referred to our
   `1.0f == 1 V`, i.e. 0.3e6 V/s). Rounds the steep edges. It is a genuine
   nonlinearity — hence inside oversampling; ADAA is not trivially applicable to a
   slew clamp, so **measurement decides** (below).

It is the pedal's fixed identity (no user knob). `setIdealOpAmp(true)` bypasses it
for measurement only (like `setStage2Mode`); the render tool exposes it as
`--ideal-opamp`.

**Closed-loop corner tracks GBW / A** (`testClosedLoopBandwidth`, measured by
ratioing the small-signal response at high gain against DISTORTION=0 so the
shaping / shunt-cap / FILTER / diode-slope all cancel):

| DISTORTION | plateau gain A | analytic f_c = GBW/A | measured f_c (44.1 k / 96 k) |
|---|---|---|---|
| 0.0 | ×1 | 1.00 MHz | (clamped to Nyquist — transparent) |
| 0.5 | ×44.7 | 22.4 kHz | (above audio) |
| 0.7 | ×204 | **4898 Hz** | **4975 / 4956 Hz** |
| 0.85 | ×638 | 1567 Hz | — |
| 1.0 (+66 dB) | ×1995 | **501 Hz** | **512 / 512 Hz** |

**Slew limiter** (`testSlewRate`, LM308Stage unit-tested with a big step + a 1 kHz
square): measured max |dV/dt| = **0.3000 V/µs** at both 44.1 k and 96 k (exact —
the clamp caps every per-sample delta at SR·dt). Verified it does not itself alias
badly at the shipped 4× (below); at **2×** the slew nonlinearity *is* a few dB
worse than 1× (documented tradeoff — 2× is not the shipped factor).

**Aliasing re-measured at dist = 1.0 (+66 dB)** — 12 dB hotter into the clipper
than M2's dist-0.9 bar (`testAliasingAtMaxGain`, f0 = 4186 Hz, worst-alias rel.
fundamental, shipped 4×, LM308 on):

| base rate | 4× worst-alias (LM308 on) | M2 audibility bar | margin |
|---|---|---|---|
| 44.1 kHz | **−88.5 dB** | −60 dB | 28.5 dB |
| 96 kHz | **−104.4 dB** | −60 dB | 44.4 dB |

So 4× **passes the M2 bar with wide margin at +66 dB** — no OS increase needed;
the BW/slew band-limiting actually *helps* at 4× (−88.5 vs −80.9 dB ideal-op-amp).
Because the slew nonlinearity aliases at low OS, the M2 monotonic regression
(`testAliasingMonotonic`) now measures the **oversampled clipper in isolation**
(`setIdealOpAmp(true)`), reproducing the exact pre-M6.5 numbers (1×=−18.6 →
8×=−90.5 at 44.1 k); the shipped LM308 path's max-gain aliasing is the table
above. The factor-1 golden regression was regenerated (the LM308 is in the path at
every factor).

**Perceptual (HF-energy) note — an honest, modest result.** For a low-E+high-E
dyad at dist 1.0 / filter 0.3, the LM308 drops the output **spectral centroid
1082 → 950 Hz** (darker — "thick not fizzy"), but the absolute >5 kHz output
energy barely moves (~0.5–0.9 dB): in *this* model the post-clip **Filter** knob
and the hard diode clamp already govern output brightness, so the LM308's LP
(which sits *before* the clamp) mainly shapes what the clipper *sees* — its larger
quantified wins are the aliasing headroom above and the edge/transient rounding.
The user's *dominant* fizz was Fix A (the clean path). Listen via the A/B renders.

### A/B renders + verification (M6.5)

`bash core/scripts/ab_render.sh` now also emits (untracked `core/.ab-scratch/`):

- **Fizz A/B (RAT):** high-E pluck + high-E sine, dist 1.0, `--ideal-opamp` (LM308
  **off** = fizzy) vs on (thick), with spectra.
- **Clean-path A/B:** pedal-bypassed `--chain clean`, **OLD** binary
  (`--limiter-thresh 0.9`, HEAD +6 dB taper) vs **NEW** (`0.97`, unity taper), at a
  −3 dBFS input — OLD peaks 1.000 (limiting), NEW ~0.86 (dormant).

New render-tool flags: `--ideal-opamp`, `--chain rat|clean`, `--limiter-thresh T`.

```bash
# Core (all suites green, 44.1k + 96k): M0 + RAT(+LM308 corner/slew/max-gain) + amp(+clean-path)
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build   # 3/3
# WASM (LM308 in RatModel; worklet LIM_THRESH 0.97) — COMMITTED artifacts update:
cd .. && bash scripts/build-wasm.sh
# Web (no UI change — the LM308 is not a knob) + full suite:
cd web && npm run build && npm test        # 25 Playwright
cd .. && npm run test:history              # 10
node --test server/handler.test.mjs        # 11
# A/B evidence:
bash core/scripts/ab_render.sh
```

**Files changed (M6.5).** Core: `include/clipper/dsp/LM308Stage.h` (new),
`include/clipper/dsp/OutputLimiter.h` (new), `src/dsp/RatModel.{h,cpp}` (LM308 +
`setIdealOpAmp`), `src/dsp/AmpModel.cpp` (unity volume ceiling),
`tools/render/main.cpp` (`--ideal-opamp` / `--chain` / `--limiter-thresh`, clean
chain), `tests/test_rat_model.cpp` (closed-loop BW + slew + max-gain aliasing tests,
monotonic isolates the clipper, regenerated golden), `tests/test_amp_model.cpp`
(clean-path THD test, updated chain-gain), `scripts/ab_render.sh` (fizz + clean
A/B). Web: `worklet/clipper-processor.js` (`LIM_THRESH` 0.97),
`public/generated/clipper.js` + `clipper-processor.js` (rebuilt, committed). No UI
changes (no new knobs — the LM308 is the pedal's identity, not user-adjustable).

## 11.5 M6.6 — Clean-path fizz, root-caused: gain-riding limiter + peak-normalized cab IR

Field report after M6.5: still "fizzy — only with the cab on; the RAT is fixed."
Two root causes, both structural:

1. **The cab IR could BOOST.** The IR was normalized to unity at 1 kHz, leaving
   its +3 dB presence bump (2.5 kHz) above unity — engaging the cab pushed
   presence-band peaks INTO the output limiter. Fix: the IR is now normalized to
   its SPECTRAL PEAK (max |H(f)| = 1 over 40 Hz–16 kHz): a cab may color, never
   boost. 1 kHz now sits ≈ −3 dB; the documented relative shape is unchanged;
   default chain gain moved from ≈ −7 dB to ≈ −10..−12 dB (testChainGain band
   updated; the Playwright loudness floor updated accordingly).
2. **The limiter was a waveshaper.** Any tanh knee bends every sample above
   threshold into harmonics — riding into it at ALL means fizz. Replaced with a
   **lookahead gain-rider** (`OutputLimiter`, mirrored as `LookaheadLimiter` in
   the worklet): 64-sample lookahead delay + monotonic-deque sliding maximum →
   target gain = ceiling/peak; attack completes inside the window, **50 ms
   hold** (prevents inter-peak gain ripple = AM sidebands), 40 ms release,
   double-precision gain accumulator (a float32 gain stalls below the
   snap-to-unity threshold near 1.0 — observed at 96 kHz). Gain scaling adds
   ZERO harmonics; when no over is in the window the gain is exactly 1.0 and the
   output is bit-identical to the delayed input. Hard clamp at ±1 remains as a
   never-engaged backstop. Latency +64 samples (~1.3–1.5 ms).

Measured (`testLimiterGainRiding`, 44.1 k & 96 k): steady 220 Hz at +1 dB over →
output pinned at the 0.97 ceiling with THD < −70 dB (the tanh measured ~−35 dB
here — that WAS the fizz); after the over ends the gain recovers to EXACT unity
(bit-transparent) within ~0.4 s. `testCleanPathTHD` transparency is now
delayed-bit-identity through the real stateful limiter.

Render-tool contrast: `--limiter-thresh` now sets the gain-rider ceiling; the
legacy tanh `softLimit()` remains in the tool ONLY for OLD-tree A/B builds.

## 11.6 M8 — Boss SD-1 Super Overdrive: the soft, asymmetric contrast to the RAT

The second dirt box, and a deliberate topology contrast. Where the RAT clamps
hard and symmetrically to ground (odd harmonics, aggressive), the SD-1 clips
**softly and asymmetrically inside the op-amp feedback loop** — a clean signal
component always passes, the knee is gradual, and 2-vs-1 diodes add even-harmonic
warmth. Files: `core/include/clipper/dsp/{SdModel.h,AsymSoftClipper.h}`,
`core/src/dsp/SdModel.cpp`, `core/tests/test_sd_model.cpp`; C ABI `sd_*` in
`clipper_c_api.cpp`; `--pedal sd1` in the render CLI; worklet `sd1` dispatch;
`rig.ts` / Pedal / gear-tray / assistant wiring.

### The circuit → the model (analytic targets, all derived from the values)

- **Non-inverting gain with feedback clip.** `V_out = V_in + f(K·HP720(V_in))`,
  where `f` is the asymmetric soft clipper. Feedback `Zf` = 1 MΩ DRIVE pot;
  to-ground leg `Zg` = 4.7 kΩ + 0.047 µF. The non-inverting gain
  `A(s) = 1 + Zf/Zg = 1 + K·HP(s)` is a **mid-hump**: unity at DC, rising through
  the corner `f_mid = 1/(2π·4.7k·0.047µF) = 720.5 Hz` to the HF plateau `1+K`.
  `K = Zf/R_g`; at max DRIVE `K = 1e6/4.7e3 = 212.8` → plateau **+46.6 dB**. The
  clean `+V_in` pedestal is the Tube-Screamer trait: bass below ~720 Hz stays
  comparatively clean while mids/highs are slammed.
- **DRIVE** maps linear-in-dB over plateau `[+12, +46.6] dB` (K = 4 .. 214). Min
  is NOT unity — a hot input still clears the ~0.5 V knee, so there is no fully
  clean setting (measured light clip, THD ≈ 1 % at 0.30 V, DRIVE 0).
- **Asymmetric soft clip** (`AsymSoftClipper`, ADAA): `f(u) = Vc·tanh(u/Vc)` with
  `Vc = Vp = 0.95 V` (2 diodes) for `u ≥ 0`, `Vn = 0.50 V` (1 diode) for `u < 0`.
  Continuous C1 antiderivative → first-order ADAA applies across the sign
  boundary. **Why ADAA not WDF:** chowdsp_wdf ships a *symmetric* diode pair only;
  the SD-1's soft feedback limiter is well-captured by the tanh closed form and
  ADAA antialiases it cleanly, so ADAA is the SD-1's PRODUCTION nonlinearity
  (RAT stays WDF). A naive path is kept for the aliasing A/B.
- **4558 op-amp** (`LM308Stage` reused, 4558 values GBW = 3 MHz, slew = 1.7 V/µs
  — much faster than the RAT's LM308). Closed-loop corner `GBW/A_noise`; at the
  +46.6 dB max (A = 214) it sits at **14.0 kHz** — high enough that the mid-hump
  voicing is unaffected in-band, only a gentle top-octave softening at max drive.
  *Approximation (as M6.5):* the band-limit/slew act on the amplified feedback
  drive `u`, not the unity-gain clean pedestal (whose own corner is GBW/1 = 3 MHz).
- **Tone control** — a first-order treble TILT about a ~1 kHz pivot, ±12 dB as
  TONE sweeps 0..1, **transparent at noon** (0.5). This matches the published
  SD-1 tone response SHAPE (progressive treble cut/boost, bass ~fixed) without
  modelling the exact 10k-pot/0.018µF/0.027µF network — a documented
  approximation. A ~12 Hz output DC-blocker (the coupling cap) removes the DC the
  asymmetric clip produces. LEVEL is a clean linear gain (identity, as the RAT).
- **M2 reused directly:** only the feedback clip runs oversampled (default 4×);
  the op-amp model and ADAA both live at the oversampled rate; the pedestal is
  upsampled alongside so it stays sample-aligned (no separate delay line).

### Validation (ctest `clipper_sd_tests`, 44.1 k + 96 k, assert-backed)

- **Mid-hump corner ≈ 720 Hz:** small-signal shelf matches the analytic
  `1 + K·HP720` within **0.04 dB worst** (44.1 k) / 0.07 dB (96 k), well inside
  the ±1.5 dB bar; 720 Hz measures **−2.87 dB** below the plateau (the −3 dB
  corner), 82 Hz **−18.5 dB** (bass shelved unity-ward).
- **Asymmetry → even harmonics:** 220 Hz at moderate drive → 2nd harmonic
  **−20.9 dBc** (asymmetric) vs **−152.6 dBc** with the diodes forced symmetric —
  a **131.7 dB** contrast (the even harmonic is entirely asymmetry-driven).
- **Soft knee vs the RAT's hard clamp:** compression-knee width (input ratio over
  which the fundamental compresses −0.9 → −6 dB) = **3.95×** for the SD-1 vs
  **2.47×** for the RAT — **1.60× softer**; THD rises gradually
  4.3 % → 16.9 % → 24.5 % across input 0.05/0.15/0.30.
- **4558 op-amp corner:** extracted (real vs ideal-op-amp ratio) at max DRIVE =
  **14 308 Hz** (44.1 k) / 14 096 Hz (96 k) vs analytic GBW/A = **14 032 Hz**
  (≈2 %).
- **Aliasing (M2 bar):** shipped 4× ADAA at max DRIVE worst-alias
  **−116.5 dB** (44.1 k) / −126.1 dB (96 k), far below the −60 dB bar; ADAA beats
  naive by ~8 dB at 1×.

### A/B render commands

```
# SD-1 (soft, asymmetric) vs RAT (hard, symmetric), same knobs, plucked low A:
clipper-render --gen pluck:110:2.0 sd1.wav --pedal sd1 --distortion 0.6 --filter 0.5 --level 0.8 --sr 48000
clipper-render --gen pluck:110:2.0 rat.wav --pedal rat --distortion 0.6 --filter 0.5 --level 0.8 --sr 48000
#   -> SD-1 peak 1.22 / rms 0.20 (clean pedestal + soft clip keeps rising);
#      RAT  peak 0.30 / rms 0.14 (hard shunt clamp pins the output near the knee).
# Even-harmonic (asymmetry) A/B on a 220 Hz sine, with spectra:
clipper-render --gen sine:220:1.0 sd1_220.wav --pedal sd1 --distortion 0.6 --sr 48000 --spectrum sd1_220.csv
# Op-amp / ideal A/B at max drive (14 kHz top-octave softening):
clipper-render --gen sweep:20:20000:4.0 sd1_real.wav  --pedal sd1 --distortion 1.0 --sr 96000
clipper-render --gen sweep:20:20000:4.0 sd1_ideal.wav --pedal sd1 --distortion 1.0 --sr 96000 --ideal-opamp
```

### Integration notes

- **Shared param shape.** Both dirt pedals keep `PedalParams {distortion, filter,
  level}` (the chain/worklet/serializer ABI, ids 0/1/2). For an SD-1 those slots
  READ as **Drive / Tone / Level** — the Pedal component relabels, tokens add an
  amber LED/arc accent (`--led-sd`, both themes), and the assistant accepts
  `drive`/`tone` aliases (→ distortion/filter). One shape keeps everything pedal-
  agnostic and the change to shared files additive. The worklet dispatches
  `sd_*`/`rat_*` per node `type`.
- Core suites all green (M0 + RAT + **SD-1** + amp, 44.1 k + 96 k); web tsc +
  vite build; Playwright 27 (25 + `SD-1 worklet: adds harmonics…` and
  `assistant: set_param drive dials an SD-1 instance`); server 11; history 10.

## 12. M9.1 — 12AX7 triode stage (the amp building block)

M9 (JCM800 2204) is built bottom-up: its preamp is one 12AX7 common-cathode gain
stage repeated 3-4× plus a cathode follower, so **phase 1 builds and validates
that single stage standalone**, offline, with the M2 measurement discipline. No
user-facing feature yet — **module fidelity IS the milestone**. Every future
valve amp reuses it.

New source (all portable, platform-free C++17; zero web/server/electron touch):

```
core/include/clipper/dsp/TriodeStage.h    Koren 12AX7 stage, nodal-Newton solver, config
core/src/dsp/TriodeStage.cpp              device law + derivatives + per-sample solve
core/tests/test_triode_stage.cpp          measurement suite (clipper_triode_tests)
```

CLI: `clipper-render --triode` renders a single stage alone (below). CMake adds
`TriodeStage.cpp` to `clipper_dsp` and registers the `clipper_triode_tests` ctest.

### Device model — Koren 12AX7

Norman Koren's triode approximation ("Improved SPICE models for vacuum-tube
amplifiers", 1996), the canonical law in the modelling literature. Published
12AX7 parameters `mu=100, ex=1.4, kg1=1060, kp=600, kvb=300`:

```
E1 = (Va/kp)·ln(1 + exp(kp·(1/mu + Vgk/√(kvb + Va²))))
Ip = (E1^ex / kg1)·(1 + sign(E1))          # sign() rectifies to cutoff
```

`Va` = plate-cathode V, `Vgk` = grid-cathode V. The `(1+sign(E1))` factor is part
of the fit (with these constants a −2 V / 250 V bias gives ≈0.95 mA, matching the
datasheet). `korenPlateCurrent()` is exposed `static` so the tests derive the
analytic operating point / small-signal `rp, gm, mu` independently. Overflow-safe
`softplus`/`sigmoid` guard the `exp` for large arguments.

### Circuit — JCM800 first-stage-style (parameterizable `Config`)

`B+ = 320 V`, `Ra = 100 k`, `Rk = 820 Ω` (self-bias) with optional bypass `Ck`
(**0.68 µF** first-stage voicing, or **22 µF** fully bypassed), grid stopper
`Rg = 68 k`, and the interstage **coupling cap `Cc = 22 nF` + next-stage grid
leak `Rgl = 1 M`** (τ = `Rgl·Cc` = **22 ms** — the blocking-distortion RC). The
stage is a faithful cascade element: an **input** coupling (the driving stage's
output coupling / this grid's DC block) **and** an **output** coupling that both
DC-blocks the output and **loads the plate with the next 1 M grid leak**, so the
mid-band gain is `−gm·(Ra‖Rgl‖rp)`, not `−gm·(Ra‖rp)`. Every stage is identical,
so both couplings share `Cc`/`Rgl`. Blocking distortion is testable on a single
stage because it is *this* grid conducting into *this* input coupling cap that
shifts the bias.

Two large-signal behaviours the ideal transfer curve lacks:
- **Grid conduction** — for `Vgk ≳ 0` the grid draws current (soft clamp,
  `Igk = (Vgn/Rgk)·softplus((Vgk−Vgt)/Vgn)`, ≈2 kΩ conduction resistance, 0.1 V
  knee so idle leakage at `Vgk≈−1.1 V` is negligible). Fed back through the 68 k
  grid stopper it squashes positive grid peaks — the source of even-harmonic
  (2nd) dominance / one-sided soft clip.
- **Blocking distortion** — that same grid current charges `Cc`; it recovers
  through `Rgl` with τ = 22 ms, so a hard burst shifts the bias toward cutoff and
  recovers over ~one RC.

### Solver — per-sample nodal Newton (3 unknowns: Va, Vg, Vk)

Each sample solves the KCL system for the plate, grid and cathode nodes. The
reactive elements (`Cc`, `Ck`) are **backward-Euler companions**: the input
coupling network collapses to a Thévenin source into the grid; the cathode is
`Rk‖Ck`; the output coupling is a series-RC companion loading the plate. Residuals
`r1` (plate), `r2` (cathode), `r3` (grid) with an **analytic 3×3 Jacobian** from
the Koren + grid-current derivatives; solved by Cramer's rule.

**Convergence (measured):** warm-started from the previous sample's solution (the
RC constants are ms, the step is µs), it converges in **2-4 iterations** in
normal use and **≤8 even under a ±10 V slam** — cap `kMaxNewtonIter = 50`, never
approached. Fallback: singular Jacobian → keep the current iterate; per-iteration
steps are **damped** (`|ΔVa|≤60`, `|ΔVg|,|ΔVk|≤20 V`) to stay out of `exp`
overflow. An **unbypassed** `Rk` is instantaneous local feedback (degeneration).
At `prepare()`/OS change the stage **settles** silent samples to the exact
discrete zero-input fixed point (no turn-on thump; silence→silence from sample 0).

The whole (nonlinear + reactive) stage runs **oversampled** (the shared M2
`Oversampler`); `setOversampling(1/2/4/8)`, default **4×**.

### Validation — `clipper_triode_tests` (deterministic, 44.1 k & 96 k)

All numbers below are asserted against analytic targets **derived in the test**
(load-line bisection, central-difference small-signal params, complex cathode
shelf) — the suite pins the solver against the physics, not against itself.

1. **DC operating point** vs the analytic load line (`B+ = Va + Ra·Ip`,
   self-bias `Vgk = −Ip·Rk`): **Va = 185.7 V** (analytic 185.6, ±5 %), **Iq =
   1.34 mA**, `Vk = 1.10 V`. *Note:* the modelled JCM800 first stage runs a hot
   bias — 1.34 mA sits just above the nominal 1.0-1.2 mA "textbook" figure, an
   honest consequence of the published Koren fit + the 820 Ω self-bias; band
   asserted 1.0-1.6 mA, Va 170-200 V.
2. **Small-signal gain** at 10 mV / 1 kHz vs `−gm·(RL‖rp)` (bypassed) and
   `−mu·RL/(RL+rp+(mu+1)·Rk)` (unbypassed), `RL = Ra‖Rgl = 90.9 k`, with
   `gm = 2.20 mS, rp = 43.3 k, mu = 95.3` from the Koren linearisation:
   **bypassed −64.4× (36.2 dB)**, **unbypassed −40.8× (32.2 dB)** — both within
   **0.02 dB** of analytic (tol ±1.5 dB). The −64× lands in the JCM800
   first-stage window; unbypassed is reduced exactly by the feedback term.
3. **Transfer shape** (1 kHz, 0.68 µF): asymmetric soft clip — **2nd harmonic
   dominant** (−43.9 dBc vs 3rd −71.6 dBc at 0.2 Vpk; window −30..−55 dBc),
   **monotonic THD** 0.64 → 1.45 → 2.78 % across 0.2/0.5/1.0 Vpk, and
   **27.8 % peak asymmetry** at 3 Vpk (cutoff vs saturation+conduction).
4. **Blocking distortion**: a 2 Vpk / 200 Hz burst shifts the coupling-cap bias
   **0.67 V**; after the burst it recovers to 1/e in **20.5 ms** vs the
   `Rgl·Cc = 22 ms` RC (tol ±25 % — the tail discharges slightly faster while
   residual conduction lingers early).
5. **Stability**: white noise + DC steps + **±10 V slam** at 44.1 k/96 k, 4×/8×
   — all finite, output bounded (~160 V plate scale), **max Newton iters 8**
   (cap 50).
6. **Cathode bypass** (0.68 µF): the gain shelf vs the analytic complex transfer
   `|A(f)| = mu·RL/|RL+rp+(mu+1)·Zk|`, `Zk = Rk/(1+jω·Rk·Ck)`, shelf zero at
   `1/(2π·Rk·Ck) = 285 Hz` — measured **40.3×(50 Hz) → 63.9×(3 kHz)**, worst
   deviation **0.15 dB** (tol ±1.5 dB).

### Aliasing & the oversampling requirement

Same M2 sweep method (a hard-driven 4186 Hz tone, worst folded-alias vs
fundamental). The smooth triode transfer + reactive band-limiting alias far less
than a hard clipper:

| base | 1× | 2× | 4× | 8× |
|---|---|---|---|---|
| 44.1 kHz | −54.4 dB | −89.2 dB | **−140.0 dB** | −137.7 dB |
| 96 kHz   | −74.7 dB | −152.0 dB | −155.1 dB | −155.0 dB |

**Required OS factor: 4× at a 44.1 kHz base** (matches the M2 pedal budget). 2×
already clears −89 dB; 4× reaches the numerical floor; 8× buys nothing audible.
The stage ships at **4×**.

### Render harness (`clipper-render --triode`)

```bash
# A single 12AX7 stage, driven pluck (grid = input × drive), JCM800-voiced cathode:
./build/clipper-render --gen pluck:110:2.0 --amp 0.3 \
    --triode --triode-drive 4 --triode-cathode 0.68 out.wav
# Sine + spectrum (even-harmonic signature); fully-bypassed cathode, 8×:
./build/clipper-render --gen sine:220:2.0 --amp 0.3 --triode --triode-drive 5 \
    --triode-cathode 22 --os 8 --spectrum spec.csv out.wav
```

Grid drive is the input × `--triode-drive` (a bare 0.3 V DI barely moves a 12AX7;
~1-3 V grid is where it distorts). Output is the plate AC (next-grid) voltage in
the tens of volts, peak-normalized to 0.9 for the WAV (raw plate peak reported).
`--triode-cathode` sets the bypass µF (0 = unbypassed), `--os` the factor.

## 14. M9.2 — JCM800 2204 preamp (the cascade + tone stack)

M9 phase 2 composes the validated M9.1 TriodeStage into the **full 2204 preamp**:
four 12AX7 triodes (three common-cathode gain stages + a direct-coupled cathode
follower) driving the passive Marshall TMB tone stack, with GAIN and MASTER pots.
Still no user-facing feature — **module fidelity is the milestone**; the power amp
+ sag (phase 3) and the UI/integration (phase 4) come later.

New source (portable, platform-free C++17; zero web/server/electron touch):

```
core/include/clipper/dsp/Jcm800Preamp.h   4x TriodeStage + MarshallToneStack, knobs
core/src/dsp/Jcm800Preamp.cpp             composition, follower bias, tone-stack MNA
core/tests/test_jcm800_preamp.cpp         measurement suite (clipper_jcm800_tests)
```

TriodeStage gained ONE additive feature: a **`CathodeFollower` topology** (+ a
`gridBias` field) for V2B — plate tied to B+, output at the cathode, grid DC-coupled
to the driving stage's plate. The M9.1 `CommonCathode` path is byte-for-byte
unchanged and `clipper_triode_tests` passes untouched.

### Topology (canonical 2204 values, B+ ≈ 320 V throughout)

```
guitar → V1A → 0.022µF + 470k/1M GAIN pot → V1B → 0.022µF → V2A → V2B → TMB → MASTER
```

| stage | role | Ra | Rk | Ck | grid-leak Rgl |
|---|---|---|---|---|---|
| **V1A** | bright input CC | 100 k | 820 Ω | 0.68 µF (bypassed) | 1.47 M (= 470 k + 1 M GAIN pot) |
| **V1B** | **cold** 2nd stage | 82 k | 10 k | — (UNBYPASSED) | 470 k |
| **V2A** | bright CC → follower | 100 k | 820 Ω | 0.68 µF | 1 M |
| **V2B** | cathode follower | — (plate = B+) | 100 k | — | direct-coupled |

- **GAIN** (the drive knob) is the 1 M preamp-volume pot after the 470 k series
  resistor: interstage scale = `0.68 · taper(gain)`, where `0.68 = 1M/(1M+470k)`
  is the series divider and `taper(·)` the pot's audio law (below).
- **V1B is the crunch source**: a 10 k *unbypassed* cathode biases it cold
  (Vgk ≈ −3.1 V, Iq ≈ 0.31 mA) and degenerates its gain to ~5.6× — so it's the
  first stage to run out of linear grid window as GAIN comes up (white-box test 4).
- **V2A → V2B is DIRECT-COUPLED** (no cap): the follower's grid DC bias is *solved*
  to V2A's quiescent plate voltage (185.7 V) at `prepare()`; the AC rides on it.
  V2A's `Rgl = 1 M` is the honest compromise for TriodeStage's single shared Rgl
  (its own input grid-leak, so blocking recovery τ = Rgl·Cc = 22 ms; the follower
  grid is high-Z so 1 M barely loads the plate).
- **Why the follower**: the passive TMB is lossy and impedance-sensitive; the
  follower's low output impedance (**≈ 1/gm ‖ Rk = 372 Ω**, measured) drives the
  stack so its response matches the (high-Z-source) analytic transfer.

### Audio taper law (GAIN, MASTER)

`taper(x) = (e^{4·x} − 1)/(e^4 − 1)` — a musical log/audio pot (~12 % at noon,
0 at 0, 1 at 1). Documented and reproduced in the tests.

### Marshall TMB tone stack (FMV) — passive, nodal MNA

`MarshallToneStack` implements the FMV network by **modified nodal analysis**:
trapezoidal (bilinear) capacitor companions, a 5×5 node system whose inverse is
cached per knob change (per-sample cost = one 5×5 mat-vec). Netlist (nodes
IN/N2/N3/N4/OUT): treble cap `Ct` IN–N2, slope `R1` IN–N3, treble pot
`RT` split N2–OUT–N3 (wiper = output), bass pot `RB` (rheostat) + bass cap `Cb`
N3–N4, mid pot `RM` (rheostat) + mid cap `Cm` N4–GND.

Component values: slope **33 k**, treble pot **250 k**, bass **1 M**, mid **25 k**;
caps **Ct = 470 pF, Cb = 22 nF, Cm = 22 nF**. *The spec's "0.47" treble cap is
0.47 **nF** = 470 pF; a 0.47 µF treble cap would put the treble corner at ~1 Hz
(unphysical) and destroy the mid notch — so it is read as nanofarads.* The test
derives the analytic `H(jω)` from the **same** netlist via a complex nodal solve
(caps = jωC) — independent of the runtime discretization.

### Oversampling requirement — MEASURED (M2 sweep, 4186 Hz, max gain)

Each TriodeStage antialiases itself (shared M2 `Oversampler`); the tone stack and
interstage networks are linear (base rate). The **cascade** is the nonlinearity —
measured worst folded-alias vs the fundamental at MAX gain:

| base | 1× | 2× | 4× | 8× |
|---|---|---|---|---|
| 44.1 kHz | −21.9 dB | −31.8 dB | **−73.3 dB** | −73.8 dB |
| 96 kHz   | −26.2 dB | −68.3 dB | **−68.3 dB** | −68.2 dB |

**Required OS factor: 4×.** 4× reaches the cascade's compound alias **floor** (8×
buys ~0 dB — the residual is inter-stage band-limiting, not per-stage aliasing) and
clears the **M2 audibility bar (−60 dB)** with margin even at max gain. Ships at 4×
(the M2 pedal budget), same as M9.1.

### Validation — `clipper_jcm800_tests` (deterministic, 44.1 k & 96 k)

Every number is asserted against an analytic target **derived in the test** (load-
line bisection per stage, the follower's 1-D cathode solve, central-difference
small-signal params, the complex cathode-bypass transfer, and the complex nodal
tone-stack `H(jω)`).

1. **Per-stage DC operating points** vs the load line (±5 %): **V1A/V2A**
   Va = 185.7 V, Iq = 1.34 mA, Vk = 1.10 V; **V1B (cold)** Va = 294.6 V,
   Iq = 0.309 mA, Vk = 3.09 V; **V2B follower** gridBias = 185.7 V (= V2A plate),
   Vk_out = 185.9 V (analytic 186.0), Iq = 1.86 mA, Rout = 372 Ω. All within 0.1 %.
2. **Small-signal chain gain** at low GAIN (0.03, linear region) vs the product
   `A_V1A · gainScale · A_V1B · A_V2A · A_fol · |H_TMB(1 kHz)| · master`:
   **measured 19.5 dB vs analytic 19.7 dB** (tol ±2 dB) [V1A 61.8×, V1B 5.6×,
   V2A 61.1×, follower 0.996, gainScale 0.0016, H_TMB −11.0 dB].
3. **Tone stack** vs analytic `H(s)` at 100/650/3000 Hz, scooped (1,0,1) and flat
   (.5,.5,.5): **within 0.04 dB** (tol ±1.5 dB); the classic **Marshall mid notch
   at 545 Hz, −16 dB** (asserted in the 300–800 Hz band).
4. **Gain character**: THD **monotonic** 2.1 → 9.3 → 54.1 → 90.7 % across GAIN
   0.1/0.3/0.6/1.0; **asymmetric clip** 53 % at max (crunch); and (white-box) V1B's
   grid drive **4.27 V exceeds its 3.09 V cold-bias window** at high gain while
   staying inside it at low gain.
5. **Blocking / stability**: a 1.5 V / 180 Hz burst with ±10 V slams then silence —
   all finite, bounded (peak ~19 V), and the coupling RCs recover (a decaying
   low-frequency blocking wobble settles from rms 10.9 to 0.003) at 44.1 k/96 k,
   4×/8×.
6. **Aliasing**: the table above; shipped 4× clears the −60 dB M2 bar at max gain.
7. The **five existing ctest suites still pass**, incl. `clipper_triode_tests`
   **unchanged**.

### Render harness (`clipper-render --jcm-pre`)

```bash
# Clean edge (low GAIN, sparkle):
./build/clipper-render --gen pluck:110:2.0 --amp 0.2 --jcm-pre --jcm-drive 1.0 \
    --jcm-gain 0.25 --jcm-master 0.6 --jcm-bass 0.5 --jcm-mid 0.6 --jcm-treble 0.6 clean.wav
# Crunch (mid GAIN, scooped-ish):
./build/clipper-render --gen pluck:110:2.0 --amp 0.3 --jcm-pre --jcm-drive 2.0 \
    --jcm-gain 0.6 --jcm-master 0.5 --jcm-bass 0.6 --jcm-mid 0.4 --jcm-treble 0.7 crunch.wav
# Full send (GAIN maxed, low E):
./build/clipper-render --gen pluck:82:2.0 --amp 0.3 --jcm-pre --jcm-drive 3.0 \
    --jcm-gain 1.0 --jcm-master 0.5 --jcm-bass 0.7 --jcm-mid 0.5 --jcm-treble 0.8 fullsend.wav
```

Grid drive = input × `--jcm-drive`; `--jcm-gain/-master/-bass/-mid/-treble` are the
0..1 knobs; `--os` sets the per-stage oversampling (default 4). Output is the preamp
voltage (tens of volts at high gain), peak-normalized to 0.9 for the WAV.

## 18. M9.3 — JCM800 2204 power section + the full amp

The 2204's 50 W push-pull EL34 **power section** (`core/src/dsp/Jcm800PowerAmp.{h,cpp}`)
and the composed **full amp** `Jcm800Amp` (preamp → power). This is where a cranked
Marshall's "responsive" character lives: the phase-inverter clip, the class-AB
push-pull, the output transformer, global negative feedback + presence, and the B+
supply **sag**. All from circuit physics and MEASURED, not vibed. Convention: real
circuit VOLTS internally; `process()` output is normalized so **1.0 == full scale**.

### The model (every simplification documented in the header)

- **Phase inverter** — a 12AX7 **long-tailed pair** (`LtpInverter`), reusing the M9.1
  Koren 12AX7 device law (no new fit). Shared 10 k tail, asymmetric 100 k/82 k plate
  loads (large-signal balance). Solved per oversampled sample as a **3×3 nodal Newton**
  (Va1, Va2, Vk_tail) with the analytic Koren Jacobian. At high drive one triode is
  steered to cutoff → the PI's own asymmetric **soft clip**, part of the cranked sound.
  PI grid-current blocking is deferred (the dominant PI clip is the tail-steering
  cutoff, which the LTP solve already captures).
- **EL34 push-pull, class AB** — **Koren pentode** law (Norman Koren, "Improved SPICE
  models for vacuum-tube amplifiers", 1996). A widely-circulated EL34 fit (mu 11,
  ex 1.35, kg1 650, kp 60, kvb 24, kg2 4200; pentode fits are looser than triodes →
  the ±10 % validation band). Fixed bias −43 V (what THIS fit needs for the 2204's
  operating point). Both tubes idle at ~38 mA → a measurable **crossover** at low
  drive; the matched difference `Ip(+Vac) − Ip(−Vac)` is odd → **even harmonics
  cancel**. EL34 grid conduction charges the PI→grid coupling caps → **blocking**
  (τ = Rg·Cc) on overdrive. Plate-load saturation via a per-tube 1-D Newton
  (Vp = rail − (Ip−Iq)·Raa/4).
- **Output transformer** — LINEAR v1: Raa = 3.4 k reflected, turns ratio √(Raa/8) ≈
  20.6, two documented one-pole corners (LF ~75 Hz magnetizing, HF ~12 kHz leakage).
  Core saturation explicitly **deferred** (the nonlinearity lives in the tubes).
- **NFB + presence** — global feedback from the OT secondary into the PI's V3B grid,
  injected as **−β·V_secondary**. V3B's forward path to the secondary is NON-inverting
  (V3A is the inverting input), so the loop **opposes** the output → true negative
  feedback: **closed-loop gain is LOWER than open-loop**. β = the 2204 divider
  5k/(5k+47k) = 0.0962; since β acts on the real secondary VOLTS the loop gain is
  β·A_real (A_real ≈ 5), giving a **meaningful ~3.4 dB** of feedback. **Presence** (0..1)
  low-passes the feedback (one-pole, ~1.5 kHz corner) so raising it REMOVES HF feedback
  → **HF response RISES** (a shelf lift). *(This fixes the interrupted M9.3 run's
  reported sign inversion — the shipped code and the `testFeedbackAndPresence`
  inversion-catcher assert BOTH `gain(closed) < gain(open)` AND
  `response(presence=1) > response(presence=0)` at 4 kHz, so a sign flip in either
  cannot pass.)*
- **Sag** — the B+ rail is a Thévenin source (Vsupply 480 V behind Rsupply 150 Ω) with
  a 50 µF reservoir, plus a slower screen node (1 k / 22 µF). Both integrated backward-
  Euler per oversampled sample from the total current draw; tube gain/headroom follow
  the rail **and** (strongly) the screen. A loud burst blooms then compresses over one
  reservoir RC (τ = 7.5 ms), recovering with the same RC — a deliberately **modest**
  sag (the 2204 is a tight, solid-state-rectified amp).

### Full-scale calibration

`kFullScaleSecV = 26 V` maps the OT secondary to 1.0 full scale. Calibrated against the
COMPOSED amp: **fully cranked** (GAIN 1, MASTER 1, hot input) a power sine peaks **~0.90**
(measured 0.895–0.899 across 44.1/48/96 k), a normal full-send (MASTER 0.7) ~0.78 —
headroom to 1.0 for transients. The NFB tap uses the real secondary volts, not the
normalized output, so this constant is independent of the loop gain.

### Validation — `clipper_jcm800_power_tests` (deterministic, 44.1 k / 48 k / 96 k)

Every number is asserted against an analytic target **derived in the test** (the EL34
bias fixed point from the Koren law, the matched-pair even-harmonic cancellation, the
NFB reduction `1/(1+β·A_real)`, the presence one-pole shelf, the sag depth/RC).

1. **EL34 quiescent** vs the analytic Koren fixed point (±10 %): **38.07 mA/tube**
   (analytic 38.07), rail **467.4 V**, screen 459.7 V, plate dissipation 17.8 W (< 25 W
   max). In the 33–42 mA / 460–472 V design window.
2. **Push-pull even-harmonic suppression**: device-law reference — single-ended 2nd
   harmonic **−16.5 dB**, matched push-pull 2nd **−240 dB** (a machine zero: even
   harmonics cancel). Real amp 2nd **−26.8 dB** (well below the single tube; the LTP's
   slight imbalance leaves the residual). Class-AB **crossover** THD present and
   monotonic at low drive (0.42 / 0.84 / 1.66 %).
3. **NFB sign + magnitude + presence (the inversion catcher)**: open-loop −14.2 dB →
   **closed-loop −17.6 dB** = **−3.41 dB** of feedback (analytic −3.45, A_real 5.07) —
   gain goes DOWN. Presence 0→1 **lifts** 4 kHz by **+3.1 dB** (analytic one-pole shelf
   +2.1 dB, within the ±1.5 dB band) — highs go UP. A sign flip in either fails an assert.
4. **Sag**: depth **3.4 dB** (in the 2–6 dB window), **bloom 10 ms** (5–20 ms), rail
   467→432 V under the burst, **recovery 7.3–8.0 ms** vs the supply τ = 7.5 ms (±25 %).
5. **Power compression** monotonic (output RMS rises, incremental gain falls, no
   fold-back) and **±10 V slam** finite/bounded at both 4× and 8×, all three rates.
6. **Aliasing** (max drive, M2 sweep): the **power section** clears the −60 dB bar by a
   huge margin — 44.1/48/96 k **4× = −117 / −125 / −122 dB**, and 8× buys nothing (4× is
   the requirement). The **composed full amp at max gain** is measured separately: its
   power stage is fed the preamp's harmonically-dense output, so it hits a **compound
   alias/IMD floor that 8× does NOT improve** — **4× = −74 / −58 / −69 dB** at 44.1/48/96 k.
   It clears −60 dB at 44.1 k and 96 k and sits at the compound floor (~−58 dB) at 48 k,
   at/near the audibility bar; raising OS does not help (measured), so **4× ships**.
7. The **six existing ctest suites still pass**, incl. `clipper_triode_tests` (M9.1) and
   `clipper_jcm800_tests` (M9.2) **unchanged**.

**CPU** (single core, −O2, 4× at 48 k, cranked): the composed full amp runs ~**1.0×
realtime** and the power section alone ~**2.0×** in the CI sandbox (which is CPU-throttled
— the M9.2 preamp alone measures ~2× here too, so representative hardware is several ×
faster). The per-sample nodal Newtons (LTP 3×3 + two grid + two plate solves × 4× OS) are
the cost; an analytic plate-solve Jacobian is the obvious future optimization.

### Render harness (`clipper-render --jcm` / `--jcm-cab`)

```bash
# Clean edge (low GAIN) through the full amp + brit412 4x12 cab:
./build/clipper-render --gen pluck:110:2.5 --amp 0.22 --sr 48000 --jcm --jcm-cab \
    --jcm-gain 0.3 --jcm-master 0.6 --jcm-treble 0.6 --jcm-presence 0.5 clean.wav
# Full send (low E, cranked):
./build/clipper-render --gen pluck:82:2.5 --amp 0.3 --sr 48000 --jcm --jcm-cab \
    --jcm-drive 2.5 --jcm-gain 1.0 --jcm-master 0.7 --jcm-presence 0.6 fullsend.wav
```

`--jcm` renders the FULL 2204 (preamp → power); output is already normalized (1.0 ==
full scale, NOT re-normalized). `--jcm-presence` is the power-amp presence knob;
`--jcm-cab` runs it through the brit412 4×12 (shipped output limiter guarding the
ceiling). Reuses `--jcm-drive/-gain/-master/-bass/-mid/-treble` and `--os`. `--jcm-pre`
(M9.2 preamp alone) still works unchanged.

## 19. M9.4 — JCM800 joins the amp registry (integration end-to-end)

M9.1–9.3 built and validated the JCM800 2204 DSP (`Jcm800Amp` = preamp cascade →
power section). **M9.4 wires it into the app as a second selectable amp voice**
alongside the Clean 120, end-to-end: C ABI, worklet, rig serializer, React face,
assistant, and the native JUCE plugin. No DSP was changed — this milestone is pure
integration, and the load-bearing proof is that the **identical-core test is now
bit-exact on BOTH amp models**.

### One handle, two voices (the C ABI)

`AmpChain` (`core/src/clipper_c_api.cpp`) now owns **both** `AmpModel amp` (Clean 120)
and `Jcm800Amp jcm`, plus an `int model`. Both are **created and prepared up front**
in `amp_create` (the JCM at its fixed **4× internal oversampling** — docs §18: 4×
ships), so the new `amp_set_model(handle, which)` export is a **realtime-safe int
flip**, never an allocation on the audio thread. The cab pair is **shared**: whichever
voice is active feeds the same per-side `CabConvolver`s.

- **Routing.** `amp_set_param` keeps **both** voices current at all times (a knob
  moved just before a switch must already be reflected in the incoming voice). The two
  models use **different id spaces** (`AmpModel::PARAM_VOLUME==0` vs
  `Jcm800Amp::PARAM_GAIN==0`), so ids are **translated explicitly**, not forwarded
  blindly: `BASS/MID/TREBLE` (ids 1/2/3) are **shared** → both; `volume/bright/chorus
  (6/7/8)/reverb (9)` are Clean-120-only → `AmpModel`; the new **JCM-only ids 10/11/12
  (gain/presence/master)** → `Jcm800Amp`. The JCM has **no bright/chorus/reverb** (a
  real 2204 has none), so those simply never reach it.
- **Mono head → dual-mono.** The JCM is a **mono valve head**; `amp_process_stereo`
  renders it once into `out_l` and **mirrors to `out_r`** before the identical cab
  pair (any stereo width there would be fake). The Clean 120 keeps its true stereo
  chorus split.
- **Latency.** `amp_latency_samples` adds the JCM's own oversampling group delay
  (`jcm.latencySamples()` = preamp four serial halfband stages + the power-section
  round trip) when it is the active voice; the linear Clean 120 adds nothing. Cab
  (128) + limiter (64) as before. Measured: Clean 120 path **336**, JCM path **624**.

### Click-free amp swap (the worklet)

`web/worklet/clipper-processor.js` gained a `{ type: 'ampModel', model }` message and a
`_pendingAmpModel`. Exactly like a cab swap, it is staged in the message handler and
applied at the **declick fade-out zero** in `_commitPending` (which calls
`_amp_set_model` then re-publishes latency, since the JCM path is longer). So switching
amps mid-signal is a **smooth raised-cosine bracketed swap — no pop** (proven in
`web/tests/amp.spec.ts`).

### Rig, params, UI, assistant

- **`rig.ts`.** `AmpType` becomes `'clean120' | 'jcm800'`; `AmpParams` gains **additive**
  `gain/presence/master` (defaults 0.5/0.5/0.4). Migration is a seam: an unknown/absent
  type coerces to `clean120`, and pre-M9.4 rigs load the JCM defaults — old saved rigs
  round-trip **unchanged**. The pinned exact-JSON round-trip test literal in
  `audio.spec.ts` was extended per the house pattern.
- **`params.ts` / `audio.ts`.** New ids `AMP_PARAM_JCM_GAIN/PRESENCE/MASTER = 10/11/12`
  and an `AMP_MODEL_INDEX` map; `setAmpModel(type)` posts the `ampModel` message. The
  JCM knobs are sent on every start so both voices stay current.
- **`Amp.tsx` — the JCM FACE (per §17 doctrine).** Same dark neumorphic chassis; identity
  is a small-area **GOLD/BRASS accent** (`--accent-jcm`) on the knob arcs/readouts + the
  "**Eight Hundred**" wordmark (model line **HEAD Nº2 · BRIT-TYPE**) — homage, never
  replica. The era-correct 2204 control row is **PRESENCE · BASS · MIDDLE · TREBLE ·
  MASTER · GAIN** (real front-panel order); **bright/chorus/reverb are hidden**; the Cab
  lever + Power rocker stay. The two faces are separate components behind one `Amp`
  wrapper switching on `amp.type`.
- **`Board.tsx` / `App.tsx`.** The amp-slot menu lists both amps; `setAmpType` updates the
  rig + engine click-free and, **when switching to the JCM with the Clean 2×12 still
  loaded, drops a one-line hint suggesting the Brit 4×12 — but never auto-switches the
  cab** (the player's choice stays theirs).
- **Assistant.** New `set_amp` tool (`clipper120` | `jcm800`), the `gain/presence/master`
  params, and coaching for the JCM voice — including the canonical **SD-1 boost into a
  cranked JCM** move and the presence-vs-treble distinction (power-amp HF lift *after* the
  distortion vs. the preamp tone stack *before* it).

### Native (JUCE)

`ClipperEngine`/`Params` gained `ampModel` + `jcmGain/jcmMaster/jcmPresence`; the APVTS
adds an **Amp Model** choice (0 = Clean 120, 1 = JCM800) and the three JCM knobs.
`bass/middle/treble` are updated on **both** tone stacks so the inactive voice is correct
at a live switch; the process path mirrors the C ABI's mono-head dual-mono routing.

### The proof — identical-core, both models

`native/tests/identical_core_test.cpp` was refactored to parametrize `renderReference` /
`renderPlugin` by `Params` and now runs **two cases**: the Clean 120 (linear stereo
chorus + reverb) and the JCM800 (an SD-1 boost into a cranked head, dual-mono into the
cab pair). Both are **bit-exact** (max |plugin − reference| = **0.0**, engine cross-check
0.0, latency 336 / 624 matched) — the plugin and raw engine wrap the identical core for
both voices.

### Perf smoke

`web/tests/amp.spec.ts` includes a perf smoke that times the **JCM800 WASM offline
render** vs the Clean 120 and reports the ratio. Headless-CI steady state: clean120 ≈ 53
ms, **jcm800 ≈ 2.27 s for 2 s of audio (~1.14× real-time, ~43× the linear clean amp)** —
the expected cost of the per-sample tube Newton solves at 4× OS. The test asserts only a
**generous** bound (a guard against pathological regressions like a lost fast-path or an
accidental 8× OS), not a tight budget; an analytic plate-solve Jacobian remains the
obvious future optimization (docs §18).

### M10.1 addendum — the JCM800 gets a spring reverb (usability > authenticity)

The real 2204 has **no reverb**. M10.1 nonetheless gives the JCM a **spring REVERB knob**
— the deliberate house call that **authenticity yields to usability** here: the user wants
the normal amp conveniences even where the original hardware lacked them (this is the ONE
place we knowingly break the replica). It reuses `ReverbModel` (mono), placed **after the
power amp, before the C-ABI dual-mono split** — the same placement logic as the Twin
below. `Jcm800Amp::PARAM_REVERB` defaults to mix 0, which is a **bit-exact passthrough**, so
every M9.3 power-section number is unchanged and the suite stays green. The face gains a
Reverb knob; the identical-core JCM case now renders with reverb engaged and stays
bit-exact.

## 20. M10.1 — TwinAmp (the Fender blackface "Twin-style" clean benchmark)

M10 opens with the **clean-headroom king**: a Fender blackface AB763-style **vibrato
channel** — the glassy, high-headroom counterpoint to the JCM's crunch and the JC-120's
solid-state clean. It joins the registry as the **third amp voice, `twin`**, end-to-end
(C ABI, worklet, rig, React face, assistant, native). Built bottom-up from the proven valve
toolbox (`TriodeStage`, the `LtpInverter`, the Koren pentode law, `ReverbModel`) plus one
new reusable block (`OptoTremolo`), and MEASURED against analytic targets, same discipline
as M9.

### The model (canon values; every simplification in the ledger)

New portable core (platform-free C++17):

```
core/include/clipper/dsp/OptoTremolo.h    reusable AB763 optical tremolo (LFO + LDR)
core/include/clipper/dsp/TwinPreamp.h     2x 12AX7 + pre-gain Fender TMB stack (+FenderToneStack)
core/include/clipper/dsp/TwinPowerAmp.h   12AT7 LTP PI (reused) + 6L6GC quad + OT + NFB + light sag
core/include/clipper/dsp/TwinAmp.h        composed: preamp -> reverb -> tremolo -> power
core/src/dsp/{OptoTremolo,TwinPreamp,TwinPowerAmp,TwinAmp}.cpp
core/tests/test_twin_amp.cpp              measurement suite (clipper_twin_tests)
```

**Signal order (authentic AB763):** `guitar → TwinPreamp → [spring REVERB] → [optical
TREMOLO] → (interstage trim) → TwinPowerAmp`. Reverb is blended AFTER the recovery stage /
volume and BEFORE the tremolo and the PI; the tremolo modulates the blended signal; both run
mono at the base rate.

- **Preamp** (`TwinPreamp`): V1 12AX7 common-cathode (Ra 100k, Rk 1.5k ∥ 25 µF, B+ 410 V) →
  **Fender TMB stack in the PRE-GAIN position** → recovery 12AX7 (same warm biasing) → VOLUME
  (audio taper) + a BRIGHT treble-bleed. Contrast with the Marshall preamp: only two gain
  stages, both **warmly** biased (no cold cathode, no follower), and the tone stack sits
  BEFORE the recovery/volume. The two triode stages each antialias themselves (4× OS).
- **Fender tone stack** (`FenderToneStack`): the FMV network — SAME MNA topology as the
  Marshall stack — with **blackface values** (treble cap 250 pF, mid 0.047 µF, bass 0.1 µF;
  treble pot 250k, bass 250k, mid 10k; **slope 100k** — Fender's, the deeper scoop), driven
  from V1's **plate** (a HIGH source impedance ≈ Ra‖rp ≈ 32 kΩ — no cathode follower, so it
  LOADS differently than the follower-driven Marshall stack). The signature **deep mid scoop**
  exists even at "flat".
- **Reverb**: reuses `ReverbModel` (the dispersive spring IS period-correct Fender-style),
  mono here, mix 0 = bit-exact passthrough.
- **Tremolo — the famous "vibrato" misnomer** (`OptoTremolo`): the AB763 optical tremolo. An
  LFO (SPEED knob **log map ~1–10 Hz**) drives a neon lamp; an LDR "roach" follows the lamp
  with **ASYMMETRIC lag — fast attack ≈ 5 ms, slow release ≈ 55 ms** (photocells darken fast,
  recover slow), so the gain envelope is a soft-throb, NOT a pure sine. INTENSITY = modulation
  depth. Built as a small reusable class (future amps want it).
- **Power amp** (`TwinPowerAmp`): a 12AT7 **long-tailed-pair PI** — REUSES the M9.3
  `LtpInverter` with a 12AT7 device fit (a **widely-circulated Koren 12AT7 set**: mu 60,
  ex 1.35, kg1 460, kp 300, kvb 300 — a lower-mu, higher-current triode than the JCM's 12AX7
  PI). The legs are **balanced to ~1 %** (a long 22k tail + a 100k/142k asymmetric plate pair,
  measured swing ratio 1.007) — this cancellation is what keeps the push-pull's **even
  harmonics down**, the key to a clean stage. **4× 6L6GC** push-pull via the **Koren pentode**
  law (a widely-circulated 6L6GC fit: mu 8.7, ex 1.35, kg1 1460, kp 48, kvb 12, kg2 4500;
  modelled as a PP pair of paralleled super-tubes, `kTubesPerSide = 2`). Fixed bias
  **−50.5 V** (nominal Twin is ≈ −52 V; this Koren fit needs −50.5 V for the ~29 mA/tube
  operating point — derived, not asserted against a datasheet), B+ ≈ 459 V loaded. OT linear
  v1: **Raa ≈ 2 k** (4 tubes), n = √(Raa/8) ≈ 15.8, corners LF 40 Hz / HF 14 kHz. **Global
  NFB (β_eff 0.16 on the modelled secondary), NO presence control** — the blackface Twin has
  none (authenticity). SS-rectifier supply → **LIGHT sag** (~1–2 dB window, and asserted
  SMALLER than the JCM's).
- **Headroom is the product**: at typical settings the amp is CLEAN; breakup arrives late and
  mostly from the PI/power stage.

### Documented simplifications (the ledger)

- The push-pull quad is modelled as a **PP pair of super-tubes** (2 paralleled 6L6 per side),
  not four independent devices — the per-tube reflected plate load is `Raa/2` and currents
  scale by `kTubesPerSide`; matched-tube assumption (no per-tube variance).
- **Fixed bias derived to the fit** (−50.5 V lands ~29 mA), not the datasheet's −52 V; the
  6L6/12AT7 Koren fits are **community parameter sets** (pentode fits looser → the ±10 % band).
- OT is **linear** (core saturation deferred; the nonlinearity lives in the tubes), same as
  M9.3.
- The **bright switch** is a one-pole treble-bleed shelf whose boost rises as volume falls (a
  faithful stand-in for the cap across the volume pot), not a full component model.
- NFB carries a **unit delay at the OS rate** (explicit-loop decoupling), same as M9.3; β is an
  **effective** divider referred to the modelled 8 Ω secondary (the real 820 Ω/100 Ω network
  taps a speaker tap).
- The tremolo runs at the **base rate** (a slow amplitude multiply — no aliasing to guard).

### Validation — `clipper_twin_tests` (deterministic, 44.1 / 48 / 96 k; MEASURED)

Every number is asserted against an analytic target derived IN THE TEST.

1. **DC operating points** vs load-line analysis: V1/V2 on the self-bias load line
   (B+ = Va + Ra·Iq, Vk = Iq·Rk) — **Va = 274 V, Iq = 1.36 mA**; 6L6 quiescent
   **28.9 mA/tube** (= the analytic Koren fixed point, ±10 %) at rail **458.8 V**, screen
   447.4 V, Pdiss 13.3 W (< 30 W); PI balanced **Va1 386.8 / Va2 381.5 V**.
2. **Fender stack** vs analytic H(jω): discrete-vs-analytic worst **< 0.07 dB** at flat and
   scooped; the **mid notch** at **339 Hz (flat) / 202 Hz (scooped)**, each below both
   shoulders — the scoop is real even "flat", asserted ±1.5 dB.
3. **NFB** sign + magnitude: open −18.7 dB → **closed −22.0 dB = −3.24 dB** (analytic −3.19),
   gain goes DOWN, **no presence** shaping (flat). A sign flip fails the assert.
4. **Tremolo**: DEPTH monotonic with INTENSITY (0 = bit-exact unity); RATE follows SPEED
   (**1.0 / 3.16 / 10.0 Hz** at knob 0 / 0.5 / 1, the log-map midpoint); gain-waveform
   **rise/fall asymmetry 2.16** (fast dip / slow recovery — NOT a sine), consistent with
   τ_attack < τ_release.
5. **Reverb placement**: mix 0 is a **bit-exact passthrough**; with reverb > 0 the wet tail
   persists in the OUTPUT after the input stops (present **pre-PI**, running through the power
   amp).
6. **The product**: clean **THD 2.96 %** at volume 0.5 / hot input (the documented clean bar
   < 4 %), **monotonic** growth to real breakup **40 %** at max volume (peak **0.89 ≈ 0.9**,
   ≥ 6× dirtier than clean); **sag Twin ≈ 2.1 dB < JCM ≈ 3.5 dB** (light + stiff), same hard
   burst; ±10 V slam finite/bounded.
7. **Aliasing** at MAX volume (M2 sweep 4186 Hz): shipped **4× = −70 / −77 / −85 dB** at
   44.1 / 48 / 96 k — the Twin is a CLEAN amp, so it clears the −60 dB M2 bar by a wide margin;
   8× buys nothing → **4× ships**.
8. All **7 existing ctest suites** still pass, incl. the M9 suites bit-exact (the JCM reverb
   defaults to passthrough).

### Integration (the M9.4 pattern, extended to three voices)

- **C ABI** (`AmpChain`): third voice `twin` (`amp_set_model` 0|1|2), created + prepared up
  front (4× OS) so the swap is a lock-free int flip. Param routing (all three voices kept
  current): BASS/MID/TREBLE → all three; VOLUME → clean120 + twin; BRIGHT → clean120 + twin;
  SPEED/DEPTH (6/7) → clean120 chorus **and** twin tremolo SPEED/INTENSITY; **REVERB (9) →
  ALL THREE**; GAIN/PRESENCE/MASTER → jcm only; CHORUS_MODE → clean only. The Twin is a
  **mono combo → dual-mono** into the shared cab pair; the app hints at the **clean212** cab
  (a real Twin is a 2×12) when switching to twin with brit412 active.
- **rig.ts / params.ts / audio.ts**: `twin` in `AmpType`/`AVAILABLE_AMP_TYPES` and
  `AMP_MODEL_INDEX` (index 2); **no new params** (reuses volume/bass/middle/treble/bright/cab/
  reverb/speed/depth). Migration coerces unknown types to clean120; old rigs round-trip.
- **Worklet**: the model index is passed opaquely (2 = Twin), declick-bracketed swap, latency
  re-published.
- **UI face** (doctrine): `TwinFace` — model line **COMBO Nº3 · BLACK-PANEL**, wordmark **"Twin
  Sixty-Five"**, a cool **silver-blue accent** (`--accent-twin`, all 4 theme blocks; the panel
  stays light/bench-style like the Eight Hundred face). Controls: VOLUME · BASS · MIDDLE ·
  TREBLE · REVERB + BRIGHT + a TREMOLO row (SPEED · INTENSITY) + cab + power; hidden
  gain/master/presence/chorus-mode. A **REVERB knob was also added to the Eight Hundred face**
  (the JCM usability add). Trademark-safe naming throughout (no "Fender/Twin Reverb").
- **Assistant**: `set_amp` gains `twin`; coaching for the clean-headroom king, the
  reverb-and-tremolo combo, and the bright switch that bites at low volume; the JCM reverb is
  documented.
- **Native**: `ClipperEngine`/APVTS gain the twin voice (Amp Model choice "Twin Sixty-Five",
  index 2); the identical-core test gains a **third bit-exact case** (Twin: spring reverb +
  tremolo) and the JCM case now exercises its reverb.

### Render harness (`clipper-render --twin` / `--jcm-reverb`)

```bash
# Clean shimmer with spring reverb, through the clean212 2x12:
./build/clipper-render --gen pluck:110:3.0 --amp 0.18 --sr 48000 --twin --twin-cab \
    --twin-volume 0.5 --twin-reverb 0.3 clean.wav
# Tremolo demo (~5 Hz, deep) with a touch of spring:
./build/clipper-render --gen pluck:147:4.0 --amp 0.2 --sr 48000 --twin --twin-cab \
    --twin-reverb 0.2 --twin-speed 0.7 --twin-intensity 0.7 trem.wav
# Pushed-hard breakup at max volume (low E):
./build/clipper-render --gen pluck:82:3.0 --amp 0.5 --sr 48000 --twin --twin-cab \
    --twin-volume 1.0 breakup.wav
```

`--twin` renders the FULL composed amp (output normalized, 1.0 == full scale); `--twin-cab`
runs the clean212 2×12 with the shipped limiter. `--jcm-reverb R` engages the JCM's new
spring reverb. Reuses `--os`.

### §20 amendment — tremolo ON/OFF (the field-requested switch)

Field report: *"the fender needs on/off for its trem."* The real amp has one (the
vibrato channel is footswitchable); we shipped SPEED/INTENSITY only, so the sole way
to kill the throb was dialing INTENSITY to zero — losing the player's setting.

The switch lives in `OptoTremolo` itself (`setEnabled`), gating the **effective
depth** through a ~10 ms **linear** enable-ramp: linear (not one-pole) so the ramp
reaches **exactly 0/1** — a settled-off tremolo multiplies every sample by exactly
1.0 and is a **bit-exact bypass**, while the ramp keeps the live toggle click-free.
The LFO + opto cell keep running while disabled, so re-enabling never jumps phase
(like the real circuit, where the footswitch grounds the oscillator's output, not
its supply). Plumbing reuses the `chorusMode` slot (the Twin has no chorus) — the
same per-voice slot-reuse pattern as presence→CUT on the AC30: C-ABI param 8 ≥ 0.5 →
`TwinAmp::PARAM_TREMOLO_ENABLE`, native `ClipperEngine` mirrors it, and the web Twin
face grows an Off/On `mode-switch` beside SPEED/INTENSITY (`trem-switch`). The
assistant gets `set_switch 'tremolo'`. **Default OFF** — old rigs serialize
`chorusMode: 0` and round-trip with the trem bypassed bit-exact.

Tests (core `test_twin_amp` + Playwright): OFF at full intensity is a bit-exact
passthrough (`out[i] == in[i]`, enable-ramp snapped to 0 by `reset()`); the mid-stream
toggle is click-free (per-sample gain step bounded by the ramp); intensity=0 remains
a unity escape hatch; the web envelope test renders switch-on (pumping, CV > 0.15)
vs switch-off at full intensity (flat, CV < 0.05); the identical-core Twin case runs
trem ON so the throb stays covered end-to-end.

**Native editor parity (2026-07-30,** `docs/work/2026-07-30-native-trem-switch.md`**):**
the paragraph above said "native `ClipperEngine` mirrors it" — true of the engine, but
the JUCE **editor** never grew the switch, so on native the only way to kill the throb
was still INTENSITY = 0 (the exact failure the amendment fixed on web). The Twin panel
now shows the shared `ModeSwitch` re-labeled Off/On (`showMode_`, labels + accent set
per amp panel; Clean 120 restores Off/Chorus/Vibrato). Same `chorusMode` param, no new
parameter, no DSP change; a Clean-120 "Vibrato" (index 2) displays as "On" on the Twin,
which matches the engine's `>= 1` mapping.

## Built DSP artifacts are committed

`web/public/generated/` (the Emscripten-built WASM engine + the worklet copy)
is **checked into git** so that `git pull` alone updates the audio engine —
no local Emscripten needed to build the web or Mac app. If you change
`core/` or `web/worklet/`, run `bash scripts/build-wasm.sh` and commit the
regenerated artifacts alongside the source change (a stale artifact means
new UI bound to an old engine — trim knobs that do nothing, etc.).

## Mac app (Electron)

**One-shot build & launch (on the Mac):** `bash scripts/mac.sh` (Electron) or
`bash scripts/native.sh` (JUCE Standalone — the LOW-LATENCY path; prefer it for
playing, especially the tube amps: WASM runs the JCM800 near the realtime
edge, native has ample headroom; `--plugins` also builds VST3 + AU for Logic) — builds the
web app, packages the arm64 .app, and opens it. `--dmg` also produces the
installer; `--dev` skips packaging for the fastest loop. Refuses to run under
Rosetta Node.

`electron/` wraps the exact same runtime as `npm run server` in a native macOS
window. **Electron on purpose:** it ships the same Chromium the whole Playwright
suite runs on, so the desktop app renders and behaves identically to the browser
build — no second engine to reason about.

### How it works (architecture)

- **No new server.** On `ready`, `electron/main.mjs` starts the proxy
  **in-process** via `electron/serve.mjs`, which **reuses `server/handler.mjs`
  verbatim** (request shaping, upstream call, `[mock]` SSE — all imported, not
  re-implemented). The only thing `serve.mjs` adds over `server/index.mjs` is
  static serving of `web/dist` and an **ephemeral** localhost port (`port 0`), so
  the window loads `http://localhost:<port>/` and the app's relative `/api/*`
  fetches "just work". Behaviorally identical to `npm run server` fronted by a
  static host — no `file://` hacks. (`server/index.mjs` isn't imported because it
  serves no statics and takes its config from `process.env` rather than by
  injection; the ~40 lines of http glue that `serve.mjs` and it share are the only
  duplication. It no longer self-`listen`s on import — that is gated on being the
  process entry point — but the static serving is still the reason for the split.)
- **API key resolution** (`electron/config.mjs`), first hit wins: (1)
  `ANTHROPIC_API_KEY` env, (2) `config.json` in the app's `userData` dir, (3)
  neither → a minimal in-app prompt to paste a key, saved to that `config.json`.
  **`MOCK=1` runs keyless** (canned `[mock]` responses, no key, no spend).
- **Key storage — v1 tradeoff:** the key is saved as **plain text** in
  `~/Library/Application Support/Clipper/config.json` (mode `0600`), **not** the
  macOS Keychain. Documented here as a known v1 limitation. The `0600` is applied
  with an explicit `chmodSync` **after** the write, because `writeFileSync(…,
  { mode })` only honours the mode when it *creates* the file — an already
  existing `0644` config would otherwise have the key rewritten into it
  world-readable (2026-07-24 audit).
- **Mic permission:** on startup the app calls
  `systemPreferences.askForMediaAccess('microphone')`; the built app declares
  `NSMicrophoneUsageDescription` (guitar input) in its Info.plist.
- **Paths are `app.isPackaged`-aware:** in dev, `web/dist` and `server/` sit next
  to the repo; when packaged, electron-builder copies them into the `.app`'s
  `Resources/` (`web-dist/` and `server/`) and `main.mjs` resolves them via
  `process.resourcesPath`.

### Shell hardening (CSP, navigation, permissions)

The main window renders **model-influenced text**, so it is treated as a hostile
rendering surface. `key-prompt.html` already carried a CSP; the main window did
not, and Electron's navigation defaults are permissive (2026-07-24 audit,
Security & app layer). What is in place now:

- **CSP as a response header on `.html` only** (`electron/serve.mjs`, exported as
  `CSP` so the test asserts the string actually served). A header rather than a
  `<meta>` in `web/index.html`, because a meta tag would also apply under `vite
  dev` where it fights HMR; the CSP for a browser-hosted deploy belongs to
  whatever host serves it. The policy:

  ```
  default-src 'self'; script-src 'self' 'wasm-unsafe-eval';
  style-src 'self' 'unsafe-inline'; connect-src 'self';
  img-src 'self' data:; font-src 'self' data:;
  object-src 'none'; base-uri 'none'; frame-ancestors 'none'
  ```

  Two relaxations beyond the audit's suggested policy, both **measured** against
  the real `vite build` output in Chromium rather than assumed:

  | Directive | Why it is required | What happens without it (measured) |
  | --- | --- | --- |
  | `script-src 'wasm-unsafe-eval'` | the engine is `WebAssembly.instantiate()` over a binary embedded in `web/public/generated/clipper.js` (Emscripten SINGLE_FILE) | `CompileError: … Refused to compile or instantiate WebAssembly module because 'unsafe-eval' is not an allowed source of script` — **every sound the app makes is dead** |
  | `font-src data:` | the single `@font-face` in `web/src/styles/tokens.css` ships its woff2 as a base64 `data:` URI | `Refused to load the font 'data:font/woff2;base64,…'`, app renders in a fallback face |

  Note what is *not* relaxed: `script-src` has no `'unsafe-inline'`, no
  `'unsafe-eval'`, and no remote origin, so assistant-authored `<script>` cannot
  run. `style-src 'unsafe-inline'` is required for React `style={{…}}` props;
  `img-src data:` for the inline data-URI favicon.
- **`will-navigate`** denies any navigation whose origin is not the served origin.
  SPA history routing fires `did-navigate-in-page`, not `will-navigate`, so this
  costs the app nothing.
- **`setWindowOpenHandler` returns `{action:'deny'}`.** The Electron default is
  *allow*, so `window.open` was spawning real `BrowserWindow`s with no policy of
  their own. `https:`/`http:`/`mailto:` targets are handed to
  `shell.openExternal` instead; everything else is dropped and logged.
  `will-attach-webview` is refused outright (no `<webview>` exists in this app).
- **`setPermissionRequestHandler` + `setPermissionCheckHandler`** grant exactly
  one permission — `media`, and only to the served origin. Without a handler, any
  origin the window ended up on inherited the microphone grant the user gave for
  guitar input. `media` is the only permission anything in `web/src` asks for
  (`audio.ts` → `getUserMedia`); nothing uses clipboard, notifications,
  geolocation or fullscreen.
- **`sandbox: true`** is pinned explicitly on the main window alongside the
  already-correct `contextIsolation: true` / `nodeIntegration: false`. It is the
  Electron 20+ default; pinning it stops the default drifting silently.
- **Static path containment** (`isContainedIn`) compares against `distDir +
  path.sep` instead of a bare `startsWith(distDir)`, which accepted any sibling
  directory sharing the prefix (`/app/dist-evil` "inside" `/app/dist`). Not
  reachable over HTTP — `path.posix.normalize` absorbs every `..` before the join
  — but the predicate was wrong regardless, so the test pins the predicate rather
  than dressing it up as an exploit that does not exist.

### Dev loop (any OS with Electron)

```bash
cd web && npm run build        # the shell serves web/dist — build it first
cd ../electron && npm install  # downloads Electron for your platform
MOCK=1 npm run dev             # keyless demo window (or set ANTHROPIC_API_KEY)
```

`npm test` (in `electron/`) runs the main-process unit suites with plain Node —
no Electron needed: `serve.test.mjs` (ephemeral port, statics, SPA fallback,
`/api` wired to the real handler, keyless-mock + no-key-500, path-traversal
guard, the CSP header, the containment predicate — 12 tests) and
`config.test.mjs` (key resolution order + the `0600` mode on both a fresh and a
pre-existing `0644` file — 8 tests).

### Build a `.dmg` (on your Mac)

macOS only — a `.dmg` cannot be produced on Linux/Windows.

```bash
cd web && npm run build                 # 1. build the web app
cd ../electron && npm install           # 2. install shell deps (downloads Electron)
npm run make-icon                        # 3. (optional) regenerate the placeholder icon
npm run dist:mac                          # 4. builds dmg + zip for Apple Silicon (arm64)
#    -> electron/dist-app/Clipper-<ver>-arm64.dmg
# Intel Macs: npm run dist:mac:intel (x64; runs under Rosetta on Apple Silicon - avoid)
```

`predist:mac` re-runs the web build for you, so step 4 alone is enough if the web
tree is current. Output lands in `electron/dist-app/`. `appId` is
`com.clipper.app`; targets are `dmg` + `zip` for `arm64` (Apple Silicon) by default; `dist:mac:intel` builds `x64`. On an Apple Silicon Mac make sure Node itself is arm64 (`node -p process.arch` should print `arm64`), otherwise `npm install` fetches the Intel Electron binary and the dev app runs under Rosetta with audio lag.

**Unsigned build — first-launch note.** The app is built **unsigned** (no Apple
Developer ID; `hardenedRuntime: false`, `identity: null`). macOS Gatekeeper will
refuse a double-click on first open. To run it: **right-click (or Control-click)
the app → Open → Open**, once. After that it launches normally. (Signing +
notarization is a later step; for local/personal use, unsigned is fine.)

**Mic prompt.** On first launch macOS asks for microphone access — required for
live guitar input. The reason string shown is the `NSMicrophoneUsageDescription`
above.

### What is verified vs. deferred to a Mac

- **Verified in CI/container:** both Node unit suites (20 tests) pass, and
  `electron-builder --dir` parses the `build` config and reaches the packaging
  step (it only stops when it cannot download the Electron binary from GitHub —
  an egress-policy restriction, not a config problem).
- **Deferred to the user's Mac:** the actual GUI launch, the mic-permission
  dialog, and producing the signed/unsigned `.dmg` — all require running the real
  Electron binary on macOS.

## 13. M7 — Tuner (chromatic needle tuner)

A chromatic needle tuner as a chain pedal (`type: 'tuner'`), Polytune-mini in role
but **chromatic only** (polyphonic multi-pitch is explicitly out of scope). It is
**not a modeling problem** — pitch detection + mute — so there is **no core/DSP/
C-ABI change**: detection runs in the WEB layer and the only worklet touch is a
per-chain mute flag + a tap point. All the work is in `web/` plus one new
dependency.

### Detection — where, how, and the frame-size decision

Detection uses the **McLeod pitch method (MPM)** via the **`pitchy`** npm package
(MIT, tiny — the ONLY new dependency) on the **MAIN THREAD**, fed by frames the
worklet taps off the **post-input-trim, pre-chain** signal (the raw guitar, before
any pedal) — the same message-port tap pattern the M6.1 peak meter uses. The
worklet keeps a ring buffer of the raw input and, **only while an engaged tuner is
in the chain** (zero cost otherwise), posts the most recent `TUNER_FRAME_SIZE`
samples every `TUNER_HOP_BLOCKS` render quanta as a `tunerFrame` message; `audio.ts`
runs a reused `PitchDetector` over each frame and converts the result to a note +
cents (`analyzePitch` in `web/src/tuner.ts`). Nothing streams into the assistant —
it reads a throttled point-in-time snapshot.

**Frame size = 4096 samples (~85 ms @ 48 k), hop = every 4th block.** MPM needs the
analysis window to hold a couple of periods of the note for the NSDF
autocorrelation peak at that lag to have enough overlap to lock. Measured on
synthetic plucked tones (fundamental + decaying harmonics, 30 trials each):

| frame | low B (B0, 30.87 Hz) | low E (82.41 Hz) | A4 (440 Hz) |
|---|---|---|---|
| **2048** (~43 ms) | **30/30 FAIL** (only ~1.3 periods) | 0.00 c, clarity 1.0 | −0.02 c |
| **4096** (~85 ms) | −0.04 c, clarity 1.0, 0 fails | −0.02 c | −0.01 c |

So 2048 (the size the peak meter would suggest) cannot see a 7-string low B at all,
while 4096 (~2.6 periods of B0) locks it to **<0.1 cents**. The larger frame costs
nothing the rest of the time because the tap only runs while the tuner is engaged.
**Lowest reliable note: B0 (~31 Hz).** The gates in `tuner.ts` reject <27.5 Hz,
>1400 Hz, or clarity <0.7. Reference pitch is fixed at **A=440** (12-TET:
`MIDI = 69 + 12·log2(f/440)`; nearest integer = note, fractional remainder = cents).
A hop of 4 blocks ≈ 93 readings/s, well above the 60 fps the needle interpolates at.
`TUNER_FRAME_SIZE` / `TUNER_HOP_BLOCKS` are mirrored in the worklet with a sync
comment (the worklet is un-bundled and can't import the module).

### DSP behavior — engaged tuner MUTES the chain (`web/worklet/clipper-processor.js`)

True tuner-pedal behavior: **engaged = muted** (stomp the tuner on, the rig goes
silent, you tune; stomp off, audio returns). The tuner is a **handle-less chain
node** — `_createPedal('…','tuner',…)` makes no WASM instance, the DSP loop skips
it (pass-through), and it contributes 0 latency. `_refreshMute()` derives
`_muteActive = any engaged tuner`, recomputed on every chain commit and bypass
toggle. A per-sample `_muteGain` ramps toward the target (0 when muted) reusing the
**same 6 ms raised-cosine step as the M6.4 declick fade**, so mute/unmute is
click-free — the mute envelope multiplies the output alongside the declick
envelope. The tap ring is written in the same input loop that computes the peak
(only when `_muteActive`), and posted (no per-block allocation — a reused scratch
buffer, structured-cloned by the postMessage) every hop.

### UI — the tuner pedal (`web/src/components/Tuner.tsx`, `styles/tuner.css`)

Pedal-format enclosure on the shared `.pedal raised` shell: model line
`TUNE Nº0 · CHROMATIC`, a **lock LED** (green when |cents| ≤ 3 held ≥ 350 ms, red
when off-pitch, dim when no signal), a **big note name + octave** (Anton display
face), an **SVG arc needle** that sweeps with the cents deviation, a **±cents
readout** (flat = amber, sharp = blue), and a footswitch labelled **Tune / Muted**.
The needle sweep and LED run off a single `requestAnimationFrame` loop that eases
toward the latest reading (~60 fps smoothing independent of the ~93/s detection
rate); App throttles the reading state to ~30/s for text while keeping a full-rate
ref for the assistant. Detection only runs while engaged, so a disengaged tuner
rests the needle at center and shows `—`.

### Integration & serialization

`'tuner'` joins the pedal-type registry **additively** (kept minimal so the
parallel SD-1 work merges cleanly): the `rig.ts` `PedalType` union, the gear-tray
`AVAILABLE_PEDAL_TYPES`, `PEDAL_TYPE_LABEL`, the worklet chain dispatch, and
`Board` rendering (a tuner instance renders `<Tuner>` instead of `<Pedal>`). A
fresh tuner (`makePedal('tuner')`) starts **DISENGAGED** so dropping it on the
board doesn't silence the rig, and carries the uniform `params` object (ignored —
its only state is `engaged`), so it **round-trips through rig JSON** like any pedal
and old rigs keep loading (`normalizePedal` coerces unknown types to the RAT).
Assistant: `add_pedal` gains `type:'tuner'`; the coach can toggle it via
`set_engaged`; the per-turn rig context adds a **`## Tuner`** section (nearest note
+ cents + flat/sharp) whenever a tuner is engaged, and the system prompt learns to
suggest checking tuning ("that sourness is tuning, not tone").

### Build and test (M7)

```bash
# No core change, but the worklet changed -> re-copy the committed artifact:
bash scripts/build-wasm.sh            # WASM binary unchanged; re-copies the worklet
cd web && npm install                 # pulls in pitchy@4.1.0 (the one new dep)
npm run build && npm test             # 28 Playwright (25 + 3 new)
```

New Playwright coverage (`tests/tuner.spec.ts`): known frequencies (low E 82.41 Hz,
A4 440 Hz, a +20-cent-sharp 445 Hz, and low B B0 ≈ 30.87 Hz) detect to the right
note + cents within ±2 through the real McLeod path; an **offline render** proves an
engaged tuner silences the chain (RMS ≈ 0) while a disengaged one passes audio; and
the gear tray adds a tuner that renders the enclosure, starts disengaged, and
engages on a footswitch stomp. Both themes were screenshotted with the tuner locked
on the 220 Hz test tone (A3). (Two pre-existing offline-render WebAudio flakes may
retry, per `playwright.config.ts`.)

## 15. Cab expansion — Brit 4×12, user IR upload, modal cab rebuild

Two roadmap items (a second built-in cab + user IR upload), plus a root-cause fix
of the long-running "fizzy only with the cab on" report and a latent convolver
bug found on the way.

### 15.1 The fizz was the IR, not the convolver — modal rebuild

Field report: fizz with the cab engaged, even pedal-bypassed, surviving every
M6.5/M6.6 fix. Root cause, measured: the generated cab IRs built their tail from
**seeded random noise** under an exponential envelope, plus a couple of discrete
early reflections. On a steady sine (a linear system) this averages out — which
is why every steady-state test passed — but on real playing each pick is an
impulse that **re-fires a ~20 ms colored-noise burst**, heard as per-note hash.
Measured on the old 2×12: tail energy after 3 ms only **−22.9 dB** below the
direct sound, a ragged **comb** magnitude response (≈2 dB steps between adjacent
log-spaced points, ~15 dB ripple 500 Hz–5 kHz), and — with a dense enough search
grid — a normalized peak of **1.06** (the old 160-point normalization grid
undershot the true peak, so the cab could still boost ~0.5 dB past unity).

The cabs are now **deterministic modal synthesis** (`core/src/dsp/CabIR.cpp`): a
direct impulse plus a small set (~10) of **exponentially-decaying resonant
sinusoids** (damped modes standing in for box/cone/breakup resonances), shaped by
the same biquad voicing cascade (low cut, presence bump, steep speaker rolloff),
gated by a short raised-cosine **tail fade**, and **peak-normalized on a dense
512-point grid**. A handful of tiny-amplitude modes gives natural body and a
short (~2–5 ms) decay with **zero noise**; because each mode is a broad, gentle
resonance the magnitude stays smooth. The coarse voicing is preserved so the amp
still sounds like the amp — we removed the hash, not the character. (Note: a
mode's spectral bump ≈ `amp·decay_samples/2`, so mode amplitudes are in the
thousandths — the *voicing* comes from the cascade, the modes only season it.)

### 15.2 Cab lineup + voicing

| | Clean 2×12 (`clean212`) | Brit 4×12 (`brit412`) |
|---|---|---|
| role | JC-120 clean platform | Marshall-style rock cab (pairs with the JCM800) |
| low cut | ~95 Hz (Q 0.6) | ~72 Hz (Q 0.6) — fuller lows |
| low-mids | flat | broad **+2 dB @ ~215 Hz** (the 4×12 "chunk") |
| presence | +3.5 dB @ 2.5 kHz | +3 dB @ 3 kHz |
| top rolloff | 4× LP @ 5 kHz | **5× LP @ 4.6 kHz** — steeper, darker |
| 5 kHz / 8 kHz (dB re 1 kHz) | −12 / −39 | **−19 / −56** (clearly darker) |

The `brit412` is audibly thicker in the low-mids and much darker up top — the
load-bearing distinction the assistant coaches and the Playwright spectrum test
measures.

### 15.3 "Sounds-not-wrong" metrics (ctest, both cabs, 44.1 / 48 / 96 kHz)

`core/tests/test_amp_model.cpp` asserts four metrics on **every generated IR** so
a regression back toward noise/comb fails the build. Before/after (2×12 @48k):

| metric | old noise IR | modal 2×12 | modal brit412 | bound |
|---|---|---|---|---|
| tail energy after 3 ms | −22.9 dB | **−33.7** | **−30.5** (−27.4 @96k) | `< −25` |
| body smoothness (500 Hz–4 kHz max step) | 1.21 dB | **0.25** | **0.38** | `< 0.5` |
| rolloff monotone (4–8 kHz) | no (comb) | yes | yes | true |
| tail flatness (modal vs noisy) | 0.35–0.53 | **0.11–0.14** | **0.04–0.07** | `< 0.25` |
| spectral peak (M6.6) | ~1.06 | **1.0000** | **1.0000** | `≈ 1` |

**Documented deviations from the original metric brief, by physics:** (1) the tail
bound is **−25 dB, not −35 dB** — a steep speaker rolloff spreads the direct
impulse (time/frequency tradeoff) and a fat 4×12 needs low-mid energy that rings a
few ms, so −35 dB at a 3 ms split isn't reachable without gutting the voicing; the
**low tail-flatness** assert (tonal, not noise) is the real anti-hash guarantee.
(2) Smoothness is measured over **500 Hz–4 kHz** (the body, where comb hash lives)
plus a separate **monotone** assert on the 4–8 kHz rolloff — a literal
500 Hz–5 kHz step bound penalizes the (smooth, legitimate) steep rolloff knee near
5 kHz, which is slope, not hash. A `clipper-render --chain clean --cab {clean212|
brit412}` render of a low-E pluck reports the **post-attack residual** (~−38 dB re
rms — clean decay, no noise burst).

### 15.4 Convolver was exonerated — and a latent in-place bug fixed

A permanent **null test** proves the partitioned-FFT `CabConvolver` equals a
direct double-precision convolution to **−152 dB** (block-processed, 44.1/48/96k)
— it was never the fizz. But building the custom-IR test surfaced a real latent
bug: `CabConvolver::processBlock` wrote `out[i]` and *then* read `in[i]` to save
the overlap-save history. The worklet runs the cab **in place**
(`cab.process(out, out, n)` in `amp_process`/`amp_process_stereo`), so the overlap
captured the *output*, not the input — corrupting the next block's window. On the
smooth default IR the error looked harmless; on a peaky IR (a user cab, or the
test comb) it **filled notches**. Fixed by saving the overlap from `in` *before*
writing `out`; a dedicated **in-place comb null test** (−152 dB) now guards it.

> **2026-07-25 follow-up.** `processBlock` is no longer reachable with caller
> buffers at all: `process()` now copies into a convolver-owned input FIFO and
> `processBlock` writes to a convolver-owned block buffer, so the in-place hazard
> is handled once, in `process()`. The overlap-before-output ordering is kept
> anyway. See **Block-size independence** under "Convolver design" — the same
> slice fixed a much worse sibling of this bug (non-128-multiple block sizes).

### 15.5 Cab selection + user IR upload — the pipeline

- **RigState** (`web/src/rig.ts`): `amp.cabModel: 'clean212' | 'brit412' |
  'custom'` (+ optional `customCabLabel`), persisted and **migrated** (old rigs →
  `clean212`). This is separate from the `cab` 0/1 *enable* lever (which bypasses
  the convolver). Presets/rig JSON **never embed IR data**.
- **C ABI** (`core/src/clipper_c_api.cpp`): `amp_set_cab_builtin(handle, which)`
  regenerates both per-side cabs; `amp_load_custom_ir(handle, ptr, len)` builds an
  IR from the samples and **peak-normalizes it in the core** (M6.6 — never trust
  the file's level: a cab must not boost) before preparing both sides. Same
  convolver, same 128-partition → latency/CPU unchanged.
- **Worklet** (`web/worklet/clipper-processor.js`): a `{type:'cab', builtin}` or
  `{type:'cab', custom: Float32Array}` message stages the swap in the message
  handler (malloc + heap copy) and applies it at the **M6.4 declick** fade-out
  zero, so the IR change is click-free (RMS-continuity Playwright test).
- **Upload pipeline** (`web/src/cab.ts`, main thread): `decodeAudioData` → mono-ize
  (average channels) → resample to the engine rate via `OfflineAudioContext` → cap
  at **4096 samples** (truncate with a short fade-out; the UI reports the original
  length) → transfer the `Float32Array` to the worklet. The IR **samples** persist
  in their own localStorage key (`clipper.customCab.v1`, base64), next to the rig.
- **Fallback**: loading a rig that references `cabModel:'custom'` with the IR data
  missing (e.g. shared to another browser) falls back to `clean212` with a UI note
  (`App.tsx`, tested).
- **Assistant** (`web/src/assistant/{tools,prompt}.ts`): the rig context carries
  `amp.cabModel`; a `set_cab` tool switches between the **built-ins only** (never
  `custom` — that needs a user upload); the coach knows 4×12 = thicker/darker Brit
  voicing for rock/JCM tones, 2×12 = the clean platform.

### 15.6 Build & test

```bash
cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest
bash scripts/build-wasm.sh            # regenerates web/public/generated/* (committed)
cd web && npm run build && npx playwright test   # 35 specs incl. 5 new cab tests
# demo (no post-attack noise burst): clean pluck through either cab
core/build/clipper-render --gen pluck:82.4:2.0 out.wav --chain clean --cab brit412
```

## 16. M6.7 / M6.7-2 — Spring reverb (the JC-120's tank)

M5 shipped the Clean 120 with **no reverb** — the real Roland JC-120 has a spring
tank, and the panel had no knob because the block did not exist. M6.7 added an
algorithmic reverb in the amp's **authentic position** with a single **REVERB** knob.
**M6.7-2 replaced the algorithm inside `ReverbModel` with a TRUE DISPERSIVE SPRING**
(same class, same one-knob interface, same position, same bit-exact-dry-at-0), which
is what this section now describes. The interface story (position, C ABI, RigState,
UI, assistant, native) is unchanged from M6.7; only the DSP core changed.

> **The metallic/underwater postmortem (why M6.7-2 exists).** M6.7 was a bank of **4
> short parallel feedback combs** + a **steep 4th-order 4.5 kHz in-loop lid**. The
> user heard its two textbook weaknesses immediately:
> * **"too metallic"** — 4 short combs ring on **harmonically-spaced** modes (each
>   comb resonates at integer multiples of `1/delay`). Evenly-spaced modes fuse into
>   a pitched, clangy "sproing". A real spring does not do this.
> * **"underwater"** — a **steep 4.5 kHz lowpass *inside* the feedback loop** plus an
>   allpass smear made a dull, phasey wash with no air on top.
>
> M6.7-2 fixes both by modelling the actual physics: a spring is a **dispersive**
> waveguide (wave speed varies with frequency), so a transient arrives smeared into a
> **downward-swept chirp** ("boing"), and — because the round-trip delay varies with
> frequency — its modes are **stretched (inharmonic)** and never stack into a metallic
> pitch. The steep in-loop lid is replaced by **gentle** damping, so the top stays
> alive.

### Position — why it lives inside `AmpModel::processStereo`

The real signal path is **preamp/tone → spring tank → the stereo chorus fork →
two power-amps + speakers**. So the reverb is a **mono** block (the split comes
after it) sitting **after the tone stack + volume and before the chorus split**:

```
in ─► tone stack + volume (mono voice) ─► ReverbModel (mono) ─► ChorusModel split
                                                              ─► per-side cab L / R
```

`ReverbModel` is **owned by `AmpModel`** (like `ChorusModel`), which routes
`PARAM_REVERB` to it. Because it is upstream of the split, the wet tail **blooms in
stereo through the chorus and both per-side cabs** — exactly like the hardware tank
feeding the stereo section. The mono legacy `AmpModel::process()` path is left
untouched (reverb is a stereo-path feature; it is the real amp's stereo voice).

### DSP (`core/src/dsp/ReverbModel.{h,cpp}`) — the dispersive spring

A mono network, in signal order:

```
in ─► transducer band-limit (2nd-order HP 150 Hz + 2nd-order LP 5.2 kHz)
   ─► [ spring 1 ]  +  [ spring 2 ]      (0.5·sum; two detuned dispersive springs)
   ─► transducer band-limit (HP 150 Hz + LP 5.2 kHz) ─► ×3.0 ─► wet
out = cos(mix·π/2)·dry + sin(mix·π/2)·wet             [equal-power mix]

each spring = FEEDBACK LOOP:
  loopIn ─► bulk delay Dₖ ─► [ N first-order dispersion allpasses ]
         ─► gentle in-loop damping (HF high-shelf + 120 Hz low-cut) ─► loopOut
  feedback = g·loopOut ;  loopIn = band(dry) + feedback
```

- **Dispersion allpass cascade** — the heart. Each section is a **first-order allpass
  in the DOWNWARD form** `H(z) = (z⁻¹ − a)/(1 − a·z⁻¹)` (pole at `+a`). Its group
  delay **decreases with frequency** (≈ `(1+a)/(1−a)` samples at DC, `(1−a)/(1+a)` at
  Nyquist), so **low frequencies are delayed more than highs** — a broadband impulse
  exits as a **high→low descending chirp**. Cascading `N` of them accumulates the
  sweep into an audible boing and makes the loop's round-trip delay
  frequency-dependent, which **stretches the mode spacing** and kills M6.7's harmonic
  (metallic) stacking. `a = 0.740 / 0.728` (in the 0.6–0.75 window); `N = 32 / 34`
  sections per spring.
- **Bulk delay** (`Dₖ = 1150 / 1219` @44.1k, scaled by `fs`) sets the round-trip
  **echo period** together with the cascade's low-frequency group delay — **≈ 30 ms**.
  This is deliberately the **short end** of the spring range: lengthening the loop
  toward the 40–56 ms of a big tank let the *constant* bulk delay dominate the
  *fixed-length* dispersion, and the modes **collapsed back to a harmonic comb**
  (measured mode-spacing stretch fell from **≈3.3× to ≈1.0×** — the exact metallic
  failure we are fixing). At ~30 ms the dispersion stays a large enough fraction of
  the loop to keep the modes stretched, and successive ~12 ms chirps overlap into a
  wash rather than discrete pings.
- **Two detuned springs** — loop lengths differ ~6% with a slight coefficient
  difference (`0.740` vs `0.728`); their sum gives the dual-tank shimmer/beat and
  de-correlates the two mode series.
- **In-loop damping — GENTLE.** A high-**shelf** (`−2.5 dB` above ~6 kHz, so the
  spring loses a little top per pass) + a `120 Hz` low-cut (no boom). This is
  deliberately **not** M6.7's steep 4.5 kHz in-loop lid — that lid was the
  "underwater" culprit.
- **Transducer band-limit** — 2nd-order HP `150 Hz` + 2nd-order LP `5.2 kHz` at input
  **and** output. Gentler (2nd- vs 4th-order) and higher (5.2 vs 4.5 kHz) than M6.7,
  so the tail keeps air (>6 kHz **down but not dead**) instead of sounding dull.
- **Decay is FIXED** (the knob is a MIX, like the real REVERB pot): loop gain
  `g = 0.90` over the ~30 ms round trip gives **RT60 ≈ 1.7 s** via
  `RT60 ≈ −3·T_rt / log₁₀ g` — springs ring a touch longer than the plate-ish 1.5 s
  M6.7 used.
- **One parameter — `reverb` (0..1), an equal-power wet MIX.** `dryGain = cos(mix·π/2)`,
  `wetGain = sin(mix·π/2)`. At `mix == 0`, `cos(0)==1` and `sin(0)==0` **exactly**
  (IEEE), and the network is **skipped entirely** (fast path), so `reverb == 0` is a
  **bit-exact dry passthrough** (asserted). Deterministic and allocation-free in
  `process()` (all delay lines / allpass state sized in `prepare()`); a `1e-20`
  anti-denormal offset injected at each spring's loop input keeps a decaying tail off
  the denormal CPU cliff (removed by the 120 Hz / 150 Hz high-passes, so no audible
  DC; >100 dB below anything audible).

**Cross-rate note.** The dispersion is defined in **samples** (`N` fixed across `fs`);
only the bulk delay scales with `fs`. So at 96 kHz the chirp sweep is a little faster
and RT60 drifts from 1.77 s → 1.63 s — well inside tolerance — while the mode-stretch
fingerprint holds (3.33× → 3.11×). Scaling `N` with `fs` would double the 96 kHz CPU
for an inaudible gain, so it is left fixed and the drift is asserted within band.

**Modulation — measured, not needed.** The brief allowed a small (<±0.3 sample) slow
delay modulation *if* the tail still showed comb clustering. It does not: the
dispersion alone already stretches the modes **3.3×** vs a comb's **1.0×**, so no
modulation is used — keeping the model fully deterministic and cheaper.

**The CPU tradeoff.** A genuine dispersion cascade is a long **sequential** allpass
chain (each section depends on the previous, per sample) — latency-bound, not
cheap like M6.7's combs. The brief targeted 100–200 sections; at `a ≈ 0.74` (upper
end) each section carries enough group delay that **~32 sections/spring** reproduce
the dispersion a lower-coefficient 150-section chain would, at ~1/4 the cost. That is
the documented **chirp-rate/CPU tradeoff**: fewer sections → a slightly shorter chirp
sweep, but the mode-stretch and boing survive (see the perf row below).

### Validation (`testReverb*` in `core/tests/test_amp_model.cpp`, 44.1/48/96 kHz)

The sound-validation house style — deterministic, framework-free, run at all three
rates. Several tests run the new model **against an embedded OLD-M6.7 reference**
(`OldSpringRef` in the test) as the A/B failing baseline, exactly as the brief asks.
Measured numbers (44.1 kHz shown; 48/96 k in parens where they differ):

| Test | Metric | NEW spring | OLD M6.7 | Bound |
|---|---|---|---|---|
| Passthrough | `reverb=0` vs dry | **bit-exact** | — | `==` |
| Decay | RT60 (Schroeder T30) | **1.77 s** (1.75 / 1.63) | 1.52 | 0.9–2.5 s (design 1.6–2.2) |
| Decay | tail energy, 100 ms windows | **monotone** ×38 | — | non-increasing (±2 %) |
| **Chirp** | echo-1 centroid early→late | **1.85×** down (1.71 / 1.82) | **1.04×** (flat) | > 1.4 **and** > 1.4× OLD |
| **Chirp** | echo period | **30 ms** | — | 20–45 ms |
| Band | tail >6 kHz re mid | **−19.7 dB** (−18.5 / −8.2) | −20.4 | in [−26, −5] (down, not dead) |
| Band | tail <100 Hz re mid | **−28.3 dB** | −26.2 | < −14 dB |
| **Anti-metallic** | mode-spacing stretch hi/lo | **3.33×** (3.33 / 3.11) | **0.97×** | > 1.8 **and** > 1.5× OLD |
| Density | mid-tail crest factor | **4.32** (4.34 / 4.77) | 6.17 | < 8 (diffuse) |
| Stability | ±1 slam + noise | no NaN, **peak 1.9** | — | bounded, < 20 |
| Placement | tail after input stops, L / R | **0.022 / 0.023** | — | both > 1e-3 (stereo bloom) |
| Placement | `reverb=0` tail, L / R | **0 / 0** | — | < 1e-5 (clean bypass) |

- **Chirp (the load-bearing test).** A single impulse → the first echo's **spectral
  centroid drops** from its first half to its second (a downward sweep). The OLD comb
  bank is essentially flat (1.04×), so the new spring must sweep **> 1.4×** *and*
  markedly more than OLD. This is precisely what M6.7 could not do.
- **Anti-metallic.** The dispersive loop's round-trip delay is frequency-dependent, so
  its resonant modes **stretch**: high-band mode spacing ÷ low-band spacing ≈ **3.3×**.
  A plain comb bank has **constant (harmonic) spacing ≈ 1.0×** — the metric is measured
  identically on the embedded OLD reference (**0.97×**) as the failing baseline. Mode
  spacing is read from the autocorrelation of the log-magnitude spectrum on a linear
  grid, in a low (300–1200 Hz) vs high (2500–4500 Hz) sub-band.
- RT60 is a **Schroeder backward energy-decay** curve (`−5 → −35 dB` T30, `RT60 =
  2·T30`). Band energy is averaged `|H(f)|²` over several probes per band. Density is
  the mid-tail **crest factor** over `[0.2, 0.6] s`. Placement drives the whole
  `AmpModel::processStereo` with chorus on and reverb up.

### CPU

From `testReverbPerf` (1 s of audio through the reverb alone, plain `-O3` release):

| Sample rate | Time for 1 s | Fraction of one core |
|---|---|---|
| 44.1 kHz | ~11.6 ms | ~1.2 % |
| 48 kHz | ~12.5 ms | ~1.3 % |
| 96 kHz | ~24.1 ms | ~2.4 % |

This is **comparable to** the chorus + 2×cab stage it feeds (`testChorusPerf`:
~10 ms @44.1k / ~26 ms @96k), not far under it — the honest price of a real
sequential dispersion cascade (M6.7's combs were ~1.7 ms). It is still a small
fraction of one core (the whole rig stays ~2–3 % of a core), i.e. comfortably
real-time with large headroom; the perf test bounds it at < 0.1× real time so a slow
CI box cannot flake. Section count was cut to ~32/spring precisely to keep it here.

### Integration

- **C ABI / worklet.** `PARAM_REVERB = 9` is appended additively to the amp ABI
  (chorus stays 6/7/8, cab stays 5). The worklet passes amp param ids straight
  through to `_amp_set_param` — **no special-casing** beyond the existing cab-toggle
  latency echo — so the reverb needs zero worklet plumbing.
- **RigState** (`web/src/rig.ts`). `amp.params.reverb` (0..1, default **0**),
  persisted and migrated: a pre-M6.7 saved rig (no `reverb` field) loads at 0 (dry).
- **UI** (`web/src/components/Amp.tsx`). A **REVERB** knob on the amp facia beside
  the tone controls (Vol / Bass / Mid / **Treble / Reverb**), reusing the shared
  `Knob` and existing tokens — where the real JC's reverb pot sits.
- **Assistant** (`web/src/assistant/{tools,prompt}.ts`). `set_param 'reverb'` (unit
  `amp`) plus a coaching line: the JC spring is surfy/ambient, keep it low (10–30)
  for clarity and note definition, higher for ballads/ambient washes; the knob is a
  mix, decay is fixed.
- **Native** (`native/`). `Params.reverb` + an APVTS `reverb` knob (default 0)
  keep the JUCE plugin chain-complete; the **identical-core test exercises the wet
  path** (`reverb = 0.5` in its param set) and still passes **bit-exact**.



- `core/` must never include platform/OS/browser/Emscripten headers. The only
  Emscripten touch point is `EMSCRIPTEN_KEEPALIVE` in `src/clipper_c_api.cpp`,
  guarded by `#if defined(__EMSCRIPTEN__)` so the file still builds natively.
- Gain smoothing (one-pole, ~5 ms) lives in the core, not in JS, so parameter
  changes are click-free regardless of the host.
- Parameter ids are mirrored and must stay in sync. The **RAT** ids
  (`PARAM_DISTORTION=0`, `PARAM_FILTER=1`, `PARAM_LEVEL=2`) live in
  `core/include/clipper/dsp/RatModel.h` (`clipper::dsp::RatModel::ParamId`),
  `web/src/params.ts`, and `web/worklet/clipper-processor.js`. The **AMP** ids
  (`PARAM_VOLUME=0`, `PARAM_BASS=1`, `PARAM_MIDDLE=2`, `PARAM_TREBLE=3`,
  `PARAM_BRIGHT=4` in `clipper::dsp::AmpModel::ParamId`, plus the chain-level
  `AMP_PARAM_CAB=5` handled by the C ABI wrapper, the chorus
  `PARAM_CHORUS_SPEED=6 / _DEPTH=7 / _MODE=8`, and the M6.7 `PARAM_REVERB=9`) are
  likewise mirrored in `web/src/params.ts` and the worklet. The M0 gain id
  (`clipper::ParamId::PARAM_GAIN=0` in `Processor.h`) still exists in the WASM
  module but the live app no longer uses it.
```

## Native app (JUCE)

`native/` is the desktop shell: a JUCE audio plugin (Standalone + VST3, plus AU
on macOS) that wraps the **identical** portable core (`core/`) — the roadmap's
"re-wrap, not a rewrite." It exists so the user can play through Logic (AU) on
Apple Silicon at native buffer latency instead of the ~20–40 ms WebAudio round
trip. Nothing in `core/`, `web/`, `server/`, or `electron/` changes; `native/`
consumes `core/` as a CMake subproject and links `clipper_dsp` directly.

### Architecture

```
mono in ──► × input trim (−12..+24 dB) ──► board[0] (if engaged) ──► board[1] …
        ──► AmpModel.processStereo (tone stack + volume + bright + JC-120
            chorus/vibrato split: mono → stereo) ──► per-side CabConvolver
            (cabL / cabR, if cab on) ──► × declick envelope (chain edits only)
        ──► OutputLimiter.processStereo ──► stereo out
```

The pedal stage is the **user-ordered board** — see **Native pedal-board parity**
below. It was a fixed RAT → SD-1 pair through the first native phase.

- **`native/src/ClipperEngine.{h,cpp}`** — the whole DSP chain, using the core
  C++ classes **directly** (`RatModel`, `SdModel`, `TsModel`, `MuffModel`,
  `PhaserModel`, `AmpModel` + its owned `ChorusModel`, two `CabConvolver`s,
  `OutputLimiter`) — **not** the `clipper_c_api` C ABI. This mirrors `web/worklet/clipper-processor.js` sample-for-sample. It has
  no JUCE dependency, so the console test can drive it standalone.
- **`native/src/PluginProcessor.{h,cpp}`** — the JUCE `AudioProcessor`. Owns an
  `AudioProcessorValueTreeState` (the param store + state save/restore) and one
  `ClipperEngine`. Pure host glue: no DSP. Mono-in → stereo-out bus layout (also
  accepts a stereo track and takes channel 0 as the mono source).
- **`native/src/PluginEditor.{h,cpp}`** — the **neumorphic** custom editor (visual
  pass): dark-chassis "island" cards on a light porcelain bench, sculpted rotary
  knobs with colour value arcs + readouts, per-section accents, LEDs/jewels, lever
  toggles and a face-switching amp card — the native translation of the web design
  language (see **Editor (neumorphic visual pass)** below). All controls stay
  APVTS-attached; the **build git hash** is shown bottom-right (`CLIPPER_GIT_HASH`).
- **`native/src/ClipperLookAndFeel.{h,cpp}`** — the `LookAndFeel` + a small custom
  **widget kit** (knob, footswitch in four morphologies, lever, power rocker, mode
  switch, chip button, jack, patch cable) implementing that language. This is where
  the CSS box-shadow/gradient recipes are translated to JUCE drawing.
- **`native/src/PedalCard.{h,cpp}`** — one pedal card on the board: the per-type
  face table (eyebrow / wordmark / accent / morphology / knobs), its APVTS
  attachments, its LED and its chain-position chips.

Mono in → stereo out so the chorus bloom works in Logic.

### Parameter map (APVTS ↔ `web/src/rig.ts`)

Knob params are plain `0..1` `AudioParameterFloat`s — the **core owns the taper
laws** (audio-taper volume, RAT/SD gain maps, etc.), identical to the web build,
so the host sees a linear normalized position and the value passes through
unchanged. Toggles are `AudioParameterBool`; oversampling and chorus mode are
`AudioParameterChoice`. Defaults mirror `DEFAULT_RIG` / `*_KNOB_DEFAULTS`.

| APVTS id | type | default | core target |
|---|---|---|---|
| `inputTrim` | float 0..1 | 1/3 (= 0 dB) | worklet-style linear pre-gain |
| `ratOn` | bool | true | chain: engage RAT |
| `ratDist` / `ratFilter` / `ratLevel` | float | 0.7 / 0.4 / 0.8 | `RatModel` id 0 / 1 / 2 |
| `sdOn` | bool | false | chain: engage SD-1 |
| `sdDrive` / `sdTone` / `sdLevel` | float | 0.5 / 0.5 / 0.7 | `SdModel` id 0 / 1 / 2 |
| `tsOn` | bool | true | board: engage the Screamer |
| `tsDrive` / `tsTone` / `tsLevel` | float | 0.5 / 0.5 / 0.75 | `TsModel` id 0 / 1 / 2 |
| `muffOn` | bool | true | board: engage the Pi |
| `muffSustain` / `muffTone` / `muffVolume` | float | 0.6 / 0.5 / 0.6 | `MuffModel` id 0 / 1 / 2 |
| `phaserOn` | bool | true | board: engage the Ninety |
| `phaserSpeed` | float | 0.35 | `PhaserModel` id 0 (SPEED — its only knob) |
| `ampOn` | bool | true | chain: amp power (off ⇒ stereo passthrough) |
| `volume` / `bass` / `middle` / `treble` | float | 0.4 / 0.5 / 0.5 / 0.6 | `AmpModel` id 0 / 1 / 2 / 3 |
| `bright` | bool | false | `AmpModel` id 4 |
| `cab` | bool | true | chain-level cab on/off (per-side `CabConvolver`) |
| `chorusMode` | choice Off/Chorus/Vibrato | Off | `AmpModel` id 8 (0/1/2) |
| `chorusSpeed` / `chorusDepth` | float | 0.3 / 0.5 | `AmpModel` id 6 / 7 |
| `oversampling` | choice 1x/2x/4x/8x | 4x | `RatModel`/`SdModel` `setOversampling` |

Plus the **board** (which pedals, in what order) as a non-automatable child node of
the state tree — see **Native pedal-board parity**.

State save/restore is APVTS XML via `getStateInformation`/`setStateInformation`.

**Smoothing:** the core already one-pole-smooths (~5 ms) every param, so the
plugin does **not** double-smooth. Critically, `processBlock` applies only the
params that **changed** since the previous block (`ClipperEngine::updateParams`),
exactly like the web worklet sets a core param only on a knob message. Re-pushing
an *unchanged* value every block would re-seed the smoother target and — through
the RAT/SD-1 high-gain nonlinearity — perturb the output; change-only application
keeps a steady chain bit-for-bit identical to a single-shot render (this is what
the identical-core test proves). Setup params are pushed once and **snapped** in
`prepareToPlay` (the smoother `prepare()` snaps value → target).

### Latency reporting

`setLatencySamples()` is published from the model latency accessors and updated on
cab toggle / oversampling change:

```
latency = Σ over the BOARD, for each engaged pedal:
              its model's latencySamples()            // OS group delay
              (the phaser is linear -> 0)
        + (ampOn && voice is a valve amp ? its latencySamples() : 0)
        + (ampOn && cab ? 128 : 0)                    // CabConvolver partition
        + 64                                          // OutputLimiter lookahead
```

Off-board pedals contribute nothing however their engaged flag reads — the sum
follows the board, not a pair of fixed slots.

At the default 4× oversampling each dirt pedal reports **72** samples, the cab
partition is **128**, and the limiter lookahead is **64**, so a RAT + SD-1 board with
cab and limiter reports **336** samples. The pedal group delay
tracks the oversampling factor via each model's `latencySamples()` accessor; the
value is re-published whenever `cab` or `oversampling` changes.

### Oversampling

Fixed **4×** default, exposed as an optional `oversampling` choice (1/2/4/8)
mirroring the web select. A change routes to each pedal's `setOversampling`
(resets only the oversampling filter state), never a full re-prepare, so it is
realtime-safe.

### Editor (neumorphic visual pass)

The v1 editor was a tidy **flat** panel; this pass rebuilds it as the native sibling
of the web UI — the roadmapped "native neumorphic UI" debt. The doctrine is the web's
(docs §17): **dark chassis for all, reference via a small-area accent + one morphology
cue + a knowing name**. It ships the **light-bench look only**; a dark theme is future
work (the CSS already carries dark-theme tokens, so it is a token-swap when wanted).

**`ClipperLookAndFeel` — how the CSS recipes translate.** Every value is lifted
verbatim from `web/src/styles/{tokens,pedal,amp,board}.css` into the `skin::` palette,
then the box-shadow/gradient recipes become JUCE draws:

| Web (CSS) | Native (JUCE) |
|---|---|
| bench `--ground #E5E3DE` | `g.fillAll` + a soft vertical `ColourGradient` |
| `.pedal.raised` dark island (`--panel-grad` 160°) + dual warm cast shadow + inset light top rim + inset dark edge | `skin::drawChassisCard`: two `juce::DropShadow` passes (16/18/34 @.30 and 3/4/10 @.22) → diagonal body gradient → top-edge light line + dark inner stroke |
| recessed **well** (inset dark TL + light BR) | `skin::drawWell`: fill `--well` + offset inset strokes under a clip |
| `.knob` anatomy: `--cap-edge` body w/ dual shadow, knurled skirt, `--cap` dome w/ inset rim, ink pointer, the floating **270° value arc** (`conic from 225deg`) + readout | `ClipperLookAndFeel::drawRotarySlider` (body shadow → cap-edge gradient → knurl ticks → cap dome + rims → pointer → `Path::addCentredArc` track+accent). Rotary range pinned to `1.25π…2.75π`; readout `= round(value*100)` in the accent, both dim when the section is bypassed (`.pedal:not(.on)` → `Slider::setEnabled(false)`) |
| lit `.led` / `.jewel` (accent + `0 0 14px` glow) | `skin::drawJewel`: layered alpha glow rings + specular radial fill |
| `.toggle` lever, `.rocker` power, `.mode-switch` | custom `LeverToggle` / `PowerControl` / `ModeSwitch` components |
| Anton condensed hero wordmarks | `skin::wordmarkFont` — a **boldened, 0.82× horizontally-compressed** system sans (no font-file asset to bundle/decompress) |

Accent tokens are the **light-theme root** values: RAT `#F03B24`, SD-1 `#B58900`, JCM
`#A87A18`, Twin `#4E7BA8`, AC30 `#B4612C`, Clean red.

**Layout.** Left→right on the bench: **INPUT** (trim) · the **pedal cards, in chain
order** · the **gear tray** · **AMP** (a single card whose **face switches** with the
amp-voice choice), joined by patch cables. An amp-voice + oversampling selector pill
pair sits top-right; the build stamp bottom-right. Resizable from a minimum that
tracks the board (1040 px empty, 1622 px with all five pedals) up to 2200×1200. The
per-pedal faces live in `PedalCard.cpp`'s face table — see **Native pedal-board
parity**.

**The amp card mirrors the web faces exactly** (per-voice control visibility + accent),
driven by `updateAmpFace()` off the `ampModel` APVTS choice:

| Voice | Accent | Controls shown |
|---|---|---|
| **Clean 120** | red | Vol · Bass · Mid · Treble · Reverb · Bright · Cab · Power · Chorus row (Speed/Depth + Off/Chorus/Vibrato mode) |
| **Eight Hundred** (JCM800) | gold | Presence · Bass · Mid · Treble · Master · Gain · Reverb · Cab · Power (no bright/chorus) |
| **Twin Sixty-Five** | silver-blue | Vol · Bass · Mid · Treble · Reverb · Bright · Cab · Power · Tremolo row (Speed/Intensity, no mode) |
| **Thirty** (AC30) | copper | Vol · Bass · Treble · **Cut** · Reverb · Cab · Power (no mid/bright/chorus) |

`presence`/`jcmMaster`/`jcmGain` etc. are the same APVTS ids the web uses; the AC30
"Cut" knob is the reused `jcmPresence` param, relabelled — no behaviour change.

**Adding a future native pedal/amp face.** It mirrors the web's `FACES`/`Amp.tsx`
tables. For a **pedal**: add a `PedalType`, a row in `PedalCard.cpp`'s `kFaces`
table (eyebrow / wordmark / accent / morphology / knob ids), its APVTS parameters,
and a `case` in the engine's `processPedal` / latency switches. A new footswitch
shape is a new `Footswitch::Shape`. For an **amp voice**: add the `AudioParameterChoice` entry (already done in the
processor), add a `case` in `updateAmpFace()` that sets the wordmark/eyebrow/accent and
pushes the visible `NeuKnob*`s into `ampPrimaryKnobs_`/`ampModKnobs_`, and add the voice
name to the `ampVoiceBox_` item list. For a **pedal**: add a card (a `Footswitch` + its
`NeuKnob`s bound to the new params) and a `drawCard(...)` call in `paint()`. New chrome
(a new knob/toggle morphology) is a new widget in `ClipperLookAndFeel`; the palette and
`drawChassisCard`/`drawWell`/`drawRotarySlider` primitives are reused as-is.

**Screenshots (headless).** A dev-only console target `clipper_editor_snap` (guarded by
the CMake option `CLIPPER_BUILD_SNAPSHOT_TOOL`, default **OFF** — never in a release
plugin build) opens the real editor and writes `Component::createComponentSnapshot`
PNGs: one per amp voice, plus the parity scenes (`native_parity_*.png` — the default
board, four- and five-pedal boards, a reorder, a bypassed pedal, and the chorus/
tremolo rows at the minimum, default and maximum window sizes). Run headless under
Xvfb:

```bash
cmake -B build -DCLIPPER_BUILD_SNAPSHOT_TOOL=ON
cmake --build build --target clipper_editor_snap
xvfb-run -a build/clipper_editor_snap_artefacts/Release/clipper_editor_snap <out-dir>
# → clipper_native_{clean120,eight_hundred,twin,thirty}.png + native_parity_*.png
```

### Native pedal-board parity

The first native phase shipped a **fixed two-pedal chain**: RAT, then SD-1, in that
order, for ever. The web app had six pedal types on a stackable, reorderable board.
The field report was blunt and correct — *"I can't move or swap pedals, meaning I
can't test them all"* — along with four visual faults, and one instruction: **keep
the left-to-right layout.** This section is what parity now means, and what the
four visual bugs actually were.

**What the board is now.** Any of the six AUDIO pedal types — RAT, SD-1, TS, Muff,
Phaser, **Gold** — in any order, each engaged or true-bypassed independently, edited
live. Each type is instantiable **once**, so the board is a subset and permutation of
the six. That single constraint is what keeps the engine simple: every model stays a
plain member of `ClipperEngine`, a reorder is a copy of six ints, and no audio-thread
allocation, handle table or free is involved anywhere. (What it costs is duplicate
instances — two Screamers in a row, which the web *can* express. See **Duplicate pedal
instances** below for what lifting that would take.)

**The tuner is deliberately absent.** It is display-only — no audio DSP; it mutes the
chain and drives a needle from a pitch-detection tap. Shipping a native tuner means a
detector, a needle widget and a repaint clock, and a fake one that looked right and
read nothing would be worse than none. The gear tray lists it as *"Chromatic tuner
(web only)"*, greyed, so the absence is visible rather than mysterious.

#### Parameter model

Two kinds of state, split by what they actually are:

| | Knobs + engaged flags | The board (order + membership) |
|---|---|---|
| Where | APVTS **parameters** | a **child node of the APVTS state tree** |
| Automatable | yes | no |
| Why | they are continuous controls a host can sensibly ride | it is a *topology*; no host can meaningfully automate "the RAT moved after the Muff" |
| Round-trips | with the session | with the session (`getStateInformation` saves the whole tree) |

Every pedal type carries its full knob set as parameters, present whether or not the
pedal is on the board — APVTS layouts are static, so a parameter cannot appear when a
pedal is added. The engine simply ignores off-board pedals (and the identical-core
test pins that: a case leaves the SD-1 *on* but *off the board* and proves it changes
neither the audio nor the reported latency).

New ids, all additive — **every pre-existing id is untouched**:

| APVTS id | type | default | web source |
|---|---|---|---|
| `tsOn` / `tsDrive` / `tsTone` / `tsLevel` | bool, float ×3 | true, 0.5 / 0.5 / 0.75 | `TS_KNOB_DEFAULTS` |
| `muffOn` / `muffSustain` / `muffTone` / `muffVolume` | bool, float ×3 | true, 0.6 / 0.5 / 0.6 | `MUFF_KNOB_DEFAULTS` |
| `phaserOn` / `phaserSpeed` | bool, float | true, 0.35 | `PHASER_KNOB_DEFAULTS` |
| `goldOn` / `goldGain` / `goldTreble` / `goldLevel` | bool, float ×3 | true, 0.35 / 0.5 / 0.7 | `GOLD_KNOB_DEFAULTS` |

The phaser gets **one** knob. The web carries two unused slots so every pedal shares
one param shape; exposing those to a host would advertise controls that do nothing.

The board node stores a comma-separated key list (`"rat,muff,phaser"` — the same
strings `web/src/rig.ts` uses). Parsing drops unknown keys, duplicates and overflow,
so malformed state degrades to a valid board rather than failing. **The audio thread
never reads the ValueTree**: `setChainOrder` also publishes a packed snapshot into an
atomic that `snapshotParams()` unpacks lock-free. That snapshot was 3 bits of length
plus five 3-bit type slots; the gold pedal made the board six deep, which 3-bit slots
would still have held — but only just, and a seventh type would have aliased into a
neighbour with no warning. It is now a 4-bit length plus six 4-bit slots, 28 bits,
with `static_assert`s holding the invariants. Still one `uint32`, so still one store.

Two different defaults, on purpose:

- a **fresh instance** opens on the web's `DEFAULT_RIG`: a single RAT;
- a **pre-parity session** has no board node, so it migrates to the old fixed
  `rat, sd1` pair and reloads sounding exactly as it did — its `ratOn` / `sdOn`
  flags still say which of the two were engaged.

#### Declicked chain edits

Add, remove, reorder, swap and engage-toggle are all topology changes: the signal
path changes between one sample and the next. Each is bracketed by the web worklet's
**6 ms raised-cosine fade** — ramp to zero, swap *at* the zero, ramp back — with the
envelope applied to the amp output ahead of the limiter, exactly where the worklet
applies it. When no edit is in flight the envelope is skipped **entirely** rather
than multiplied by 1.0, which is what keeps a steady chain bit-exact.

Two deliberate differences from the worklet:

1. **Engage toggles are bracketed too.** The worklet flips `node.engaged`
   immediately; switching a high-gain pedal in or out is a step the core's ~5 ms knob
   smoothing does not cover.
2. **A 6 ms zero HOLD sits between the swap and the fade back in.** Pedals keep their
   internal state across a reorder (as they do in the web, which reuses each handle),
   so after the swap a pedal is suddenly fed a differently-phased signal and *rings*
   while its filters and oversampler settle. That settling peaks a few ms after the
   swap — underneath the worklet's immediate fade-in, where the chain-edit test
   measured it at ~70× the steady slew. Holding at zero through the settling window
   costs 6 ms of extra gap, which reads as instant, and removes the tick.

Latency now follows the board: every **engaged, on-board** pedal's oversampling group
delay, summed in series (the phaser is linear and adds none), plus the cab partition
and the limiter lookahead as before.

#### The four visual bugs, and what they actually were

| Reported | Root cause | Fix |
|---|---|---|
| *"The lights are covered over"* | **Paint order inside a component, not z-order.** `Footswitch::paint` drew the LED first and then ran a `juce::DropShadow` for the stomp body — a wide translucent-black blur that reached back over the jewel and ate its halo. `PowerControl` had the identical inversion under its rocker. | Jewels are painted **last** in their component, and the pedal LED moved onto the chassis header at the top right — where `.pedal-top` puts it in the web, and where nothing else draws. |
| *"The knobs for the 120 chorus overlap the divider line"* | The divider was drawn at the modulation row's **top edge**, which is exactly where those knobs' floating value arcs live, and the mode switch was explicitly offset 4 px **above** the row. A knife-edge in the amp grid made it worse: column count came from `knobArea.width / 66`, so a 131 px area gave **one** column, five rows, and the block ran off the bottom of the card. | The row gets a real 40 px gap with the divider centred **in** it; the mode switch starts on the row line. Columns now come from a *minimum* cell width, and cell height shrinks to fit, so no window size can overflow the card. |
| *"There are no cables"* | Never implemented natively. | `skin::drawCable` ports `board.css`: the sag law from `Board.tsx` (`min(70, max(16, abs(dx)·0.16 + 14))`), the shadow / tube / specular-highlight / plug layering, drawn **before** the enclosures so each end tucks into its socket, with `skin::drawJack` sockets on the card edges. |
| *"The foot switches aren't correct"* | Only one generic round stomp existed, with an LED stacked above it. | All four web morphologies: the RAT/phaser round stomp, the Muff's larger one, the SD-1's black rubber **treadle** (pebble face, toe ribs, embossed name) and the TS's hinged metal **pad** — each anchored where the web anchors it, with the 130 ms press thunk. Engaged state is the LED's job, never the switch's. |

#### Board UX

`INPUT · pedal cards in chain order · gear tray · AMP`, left to right, joined by
cables. Each card carries a chip rack: **⠿** drag grip, position number, **◀ ▶**
move, **⇄** swap, **✕** remove. Dragging the grip reorders live under the pointer
using the web's rule — drop before the first card whose centre is right of the
pointer. The tray and the swap menu list only types **not** already on the board.

Two implementation notes, one of which did not survive contact with the user:

- a **move** only permutes the cards, never destroys them, because a live drag calls
  it from inside a card's own mouse handler; add/remove/swap *do* rebuild, so they
  defer to the message loop rather than deleting the chip mid-click;
- ~~the window's **minimum width tracks the board**~~ — **superseded**, see below.

#### The board scrolls (superseding "grow the window")

The parity pass's second implementation note was that the window's minimum width
tracked the board, so five pedals raised the floor to 1622 px. The reasoning was sound as far
as it went — squeezing five cards into 1040 px had produced 64 px slivers and pushed
the amp off its own card, and a legible board is worth asking for room. What it got
wrong was **whose room it is**. The window belongs to the user; a plugin that widens
itself because you added a fuzz is a plugin taking a decision that was never its own.
And it does not scale: the answer to "what happens at eight pedals" was, embarrassingly,
"a wider monitor".

The field instruction was exact: *"let's do a scrollable pedal board, amp and input can
stay, that way we can have n pedals."* So:

- the pedal strip lives in a horizontal **`juce::Viewport`**. The **INPUT** card and the
  **AMP** face stay pinned outside it — the two things you always want on screen are the
  two ends of the signal path, and they are also the two that never move;
- the window minimum is a flat **1040 × 560** again, whatever the chain holds;
- cards **no longer squeeze**. They take their full width and the board overflows. The
  squeeze law existed only to postpone the grow; with scrolling there is nothing to
  postpone;
- a plain **vertical wheel scrolls the board**. JUCE routes `deltaY` to the x axis when x
  is the only scrollable one, so a mouse works, not just a trackpad. (A wheel over a
  *knob* still moves the knob — that is the host-wide convention and the web behaves the
  same; scroll over the chassis or the rail instead.)

Two affordances, because overflow has to be *visible* before anyone thinks to scroll: a
slim neumorphic **scrollbar** (`ClipperLookAndFeel::drawScrollbar` — a carved track and a
soft capsule, so it belongs to the bench rather than arriving as a stock grey OS bar
across the pedalboard), and a soft **edge veil** on whichever side still has board hidden
past it.

**Drag still works, and now auto-scrolls.** Drag a card within 56 px of either viewport
edge and a 24 ms pump slides the board under a stationary pointer, re-evaluating the drop
target each tick — so a pedal can be dragged to a position that is currently off screen.
The card's grip reports in *content* coordinates; the editor keeps the pointer's
*viewport* x separately, because during an auto-scroll the pointer is the thing standing
still.

#### The rail

The pedals stand on something. A **channel milled into the porcelain bench** — the same
inset recipe `board.css` uses for `.board-source`, scaled up to a plank — carrying a
**ribbed rubber mat**. Nothing is photographed: the ribs are strokes (a light edge and a
dark valley each, the way moulded matting catches a lamp) and the depth is shadow, per
doctrine §17. The mat is deliberately a *warm* dark grey rather than the chassis's cool
charcoal, so a pedal never dissolves into the thing it is standing on.

It rides up behind the enclosures and leaves a lip in front, so only the slivers between
pedals and that lip are ever seen — which is exactly what a loaded board looks like. And
it spans the **whole scrolled content**, so on a long chain it runs off both edges of the
viewport: the clearest possible statement that there is more board out there.

#### Cables across the scroll seam

Cables *between* pedals live inside the viewport and scroll for free — they are drawn by
the component the cards live in, in the same coordinate space.

The two **boundary** cables (input → first pedal, last pedal → amp) have one end on the
fixed bench and one end inside the scrolling content. They **track the scroll**: the
board-side jack is pushed through the viewport transform on every repaint, so the span and
the sag follow the board as it moves. When that jack scrolls out past the viewport edge
the end is **clamped to the edge** and a jack plate is drawn there — the cable then reads
as entering a grommet on the side of the board, which is what a real board does with a
lead that leaves it. Clamping is not a cosmetic nicety: an unclamped transform draws the
input cable *backwards over the input card* the moment you scroll right.

The cost is a full editor repaint per scroll step, which is what `BoardViewport::onScroll`
is for. At this component count it is not close to a problem.

#### Duplicate pedal instances (assessed, not built)

"n pedals" has a second reading the native app does **not** yet satisfy. The web can host
two of the *same* type — `createPedalId` mints a fresh id each time, so two Screamers in
a row is a legal rig — where native is one-instance-per-type. Scrolling makes the gap
newly obvious: the board can now be as long as you like, and yet it still tops out at six
pedals because it tops out at six *types*.

What it would actually take, in ascending order of awkwardness:

1. **DSP state — easy.** Each type becomes a small pool rather than a member: e.g.
   `std::array<TsModel, kMaxDup>` prepared up front, or an `OwnedArray` sized on the
   message thread and swapped in at the declick zero. Either keeps the audio thread
   allocation-free, which is the property worth defending. The chain becomes a list of
   `(type, slot)` rather than a list of types, and the packed snapshot widens again —
   at 4 bits of type plus 2-3 bits of slot per entry, eight entries no longer fit a
   `uint32`, so the publish becomes a small double-buffered struct with a sequence
   counter (still lock-free, no longer a single word).

2. **Parameter model — the real cost.** APVTS layouts are **static**. Today's design
   leans on that: every type's knobs exist always, and the engine ignores off-board
   pedals. Duplicates break the one-knob-set-per-type assumption, and there are only
   three honest options:
   - **pre-declare N slots per type** (`ts1Drive`, `ts2Drive`, …). Automatable and
     round-trips for free, but it multiplies the host's parameter list by N for a
     feature most rigs never use, and picking N is picking an arbitrary ceiling —
     which is the grow-the-window mistake wearing a different hat;
   - **pre-declare a flat pool of generic slots** (`slot1Knob1…`, N × 3 floats + a
     bool), with the board node saying which type occupies which slot. Bounded and
     honest about being a pool, but host automation lanes read as `Slot 3 Knob 2` and
     the mapping shifts when the board is edited — automation would silently re-point,
     which is worse than not automating;
   - **take duplicates out of the parameter system entirely** and store per-instance
     knobs in the board node alongside the order. Clean, unbounded, round-trips with
     the session — and gives up host automation for duplicated pedals, which for a
     second Screamer used as a fixed boost is a genuinely reasonable trade.

3. **Session round-trip — easy either way**, since the board node already carries
   arbitrary state and already degrades malformed input to a valid board. A pre-duplicate
   session has one entry per type and migrates untouched.

**Recommendation: option 3 (per-instance knobs in the board node), and not yet.** It is
the only one that delivers what "n pedals" actually means without inventing a ceiling,
and it isolates the change to the state tree plus a pooled-DSP swap rather than doubling
the automatable surface for everyone. But it also means two classes of pedal — automatable
"first of type", non-automatable duplicates — which is a real wart, and worth confirming
against a user who has actually wanted two of something before it is built. The identical-
core test extends to it directly: a board with two Screamers at different drive settings,
hand-composed against two `TsModel`s.

#### Verification

| Check | Result |
|---|---|
| `clipper_identical_core` — 4 amp voices + **4 multi-pedal boards** (one carrying the GOLD box) | ✅ max abs(plugin−ref) = 0.000e+00 (L and R), latency matches the composed models |
| `clipper_chain_edit` — reorder / bypass / add / remove mid-signal | ✅ seam step inside the envelope bound; settles at the new board's own level; a knob move never arms the fade; the hard-splice control proves the bound can fail (15×) |
| Full native `ctest` (2 native + 16 core suites) | ✅ 18/18 |
| Editor snapshots, `native_parity_*.png` | ✅ default board, four- and five-pedal boards with cables + LEDs, a reorder, a bypassed pedal, the chorus row at min/default/max window, the Twin tremolo row, all four amp faces |
| Editor snapshots, `native_scroll_*.png` | ✅ a **six**-pedal board at both scroll extremes and mid-scroll (rail overflowing, boundary cables clamped and tracking), the default board with no scrollbar, the minimum window with the board scrolled and input/amp still pinned, and the GOLD plate face lit and bypassed. The tool *asserts* the six-pedal board overflows, so the scene cannot quietly stop proving anything |

### Build targets

`juce_add_plugin(Clipper …)` with `FORMATS Standalone VST3` and — guarded by
`if(APPLE)` — `AU`. CLAP was skipped (JUCE has no first-party CLAP target; it
would add a helper-clap dependency for no phase-1 benefit). JUCE is pulled via
`FetchContent`, **pinned to tag `8.0.4`**. `CMAKE_POSITION_INDEPENDENT_CODE ON`
is set in `native/CMakeLists.txt` *before* adding `core/` so the static
`clipper_dsp` links into the shared VST3/AU modules (core sources untouched).

### Verified on Linux vs deferred to Mac

| Item | Status |
|---|---|
| FetchContent JUCE 8.0.4 (clone through proxy) | ✅ works |
| Configure + build **Standalone** (Linux) | ✅ |
| Configure + build **VST3** (Linux) | ✅ |
| **Identical-core** console test (bit-exact) | ✅ 0.0 diff L+R, latency matches (four voices + three multi-pedal boards) |
| **Chain-edit** console test (declicked reorder/bypass/add/remove) | ✅ no discontinuity beyond the envelope |
| Core ctest (all suites) still green | ✅ unchanged |
| Neumorphic editor builds (LookAndFeel + widget kit), zero new warnings | ✅ |
| Headless `clipper_editor_snap` PNGs under `xvfb-run` | ✅ four amp faces + every parity scene render correctly |
| Headless Standalone launch under `xvfb-run` | ✅ starts, no crash (no audio HW in container — ALSA device warnings are expected) |
| **AU** build + `auval` + Logic load | ⏳ mac-only, deferred |
| VST3 in a real host (Reaper/Live) | ⏳ not run here |

### The identical-core proof (`native/tests/identical_core_test.cpp`)

The load-bearing test: it instantiates the **real** `ClipperAudioProcessor`,
sets a known full-chain parameter set through the APVTS, and renders an M2-style
220 Hz sine + exponential pluck at 48 kHz in 128-sample blocks. Independently it
renders the same signal through a **from-scratch** chain built from the core
classes directly (`RatModel`/`SdModel`/`AmpModel`/`CabConvolver`×2/`OutputLimiter`),
then asserts the plugin's L **and** R output is **bit-exact** (≤ 1e-6, observed
0.0) against that reference and that reported latency matches. A secondary check
renders through `ClipperEngine` alone to isolate the engine from the JUCE wrapper.
Because both paths run the same core and the same internal delays, they are
time-aligned — no latency offset is applied in the comparison. Wired into `ctest`
as `clipper_identical_core`.

### Building on Linux

```bash
cd native
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # fetches JUCE (first run only)
cmake --build build                                  # all targets
ctest --test-dir build --output-on-failure           # identical-core + core suites
# artefacts:
#   build/Clipper_artefacts/Release/Standalone/Clipper
#   build/Clipper_artefacts/Release/VST3/Clipper.vst3
```

Apt packages required for a JUCE Linux build (installed in the dev container):
`libasound2-dev libjack-jackd2-dev libx11-dev libxcomposite-dev libxcursor-dev
libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libfreetype-dev
libfontconfig1-dev libglu1-mesa-dev libcurl4-openssl-dev libwebkit2gtk-4.1-dev`.

### Building on the user's Mac (the actual target: AU into Logic)

```bash
cd native
# Xcode generator (recommended for macOS; builds Standalone + VST3 + AU):
cmake -B build -G Xcode -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# …or plain Makefiles / Ninja also work:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Default target is Apple Silicon (arm64) for the host machine. Artefacts land under
`build/Clipper_artefacts/Release/`:
- `Standalone/Clipper.app`
- `VST3/Clipper.vst3`
- `AU/Clipper.component`

Getting the AU into Logic:
1. Copy (or symlink) `Clipper.component` to
   `~/Library/Audio/Plug-Ins/Components/`.
2. Validate: `auval -v aufx Clp1 Clpr

**No sound / no input on the Mac Standalone?** Two first checks:
1. macOS mic permission — the app asks on first launch (fixed after phase 1;
   rebuild if your binary predates the MICROPHONE_PERMISSION_ENABLED flag). If
   you denied it once: System Settings → Privacy & Security → Microphone →
   enable Clipper.
2. In the Standalone: Options → Audio/MIDI Settings → set the INPUT device to
   your interface and enable the input channel (JUCE standalones sometimes
   default input to none).` (the codes are `PLUGIN_CODE=Clp1`,
   `PLUGIN_MANUFACTURER_CODE=Clpr` from `native/CMakeLists.txt`). A clean
   `auval` pass is what Logic's plugin manager gates on.
3. Launch Logic; it rescans AUs on start. If it does not appear, reset the AU
   cache (`killall -9 AudioComponentRegistrar`) and relaunch, or run
   `auval` again to surface the validation error.

**Unsigned caveat:** the artefacts are **not code-signed / notarized**. On a
fresh Mac, Gatekeeper may quarantine an unsigned `.component`/`.vst3` downloaded
from the internet; a locally-built one is usually fine, but if macOS blocks it,
clear the quarantine bit with
`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Clipper.component`.
For distribution (not needed for local play) you would sign with a Developer ID
and notarize. Standalone `.app` runs the same core through JUCE's own audio
device I/O for a quick check without a host.

## 17. M6.8 / M6.8.1 — Pedal visual identity: dark chassis for all, reference via accent

**Doctrine amendment (copyright safety, by construction):** every reference
must be TRADEMARK-SAFE from day one — knowing fantasy names only (Rodent,
Super Drive, In-Tune, Eight Hundred), never a real mark, model number, or
company name on any user-facing surface (UI text, menus, marketing); real
gear names may appear only in code comments/docs as descriptive references.
Trade dress: homage never replica — silhouette cues, never exact layouts,
logos, or liveries. This is not just aesthetics; it is the pre-
commercialization posture (see ROADMAP checklist). Known debt: the amp menu's
"JC-120 style" wording and the "Super Drive" name (close to a real mark) are
flagged for rename.

### M6.8.1 — the doctrine revision (current)

M6.8 shipped "shared chassis, distinct souls" with **full-body enclosure tints**
(amber SD-1, slate tuner). Field review rejected the tints: *"the yellow is weird
looking due to the saturation, but real yellow breaks the neumorphism… maybe we
keep the darker, neumorphic RAT color for all pedals/amps (looks nice against the
lighter background) and we reference the gear more subtly, like the rubber stomp
pad on the SD-1 and color (yellow for the knob readings?)."* The revised doctrine —
**the recipe for every future pedal with a real-world analog**:

> **Dark chassis for all** (one shared charcoal wash, the RAT's colour); reference
> the original gear via **(1) a small-area ACCENT colour** on the knob value arcs +
> value readouts + LED, **(2) one MORPHOLOGY cue**, and **(3) a knowing NAME** a
> pedal-lover will get. Homage, never replica — no trademarks/exact names.

Why an accent is neumorphic-safe where a full-body hue is not: a saturated colour
over a *small area* on a dark sculpted surface reads as a lit indicator, not a
painted enclosure — the M4 RAT proved it with its floating red value arcs. A full
body of the same saturation flattens the light/shadow that *is* the neumorphism.

**What changed from M6.8:**

- **One chassis colour.** `--chassis-tint` (the old RAT charcoal, both themes) is
  composited over `--panel-grad` by `.pedal.raised` for **every** pedal type. The
  per-enclosure `--rat-tint` / `--sd1-tint` / `--tuner-tint` slots are **retained
  but neutralized to `transparent`** in all four theme blocks of `tokens.css`, so
  nothing that referenced them breaks and future pedals still have a home.
- **Per-pedal ACCENT tokens** (`tokens.css`, all four theme blocks):
  `--accent-rat` (red/orange, = `--led`), `--accent-sd` (**yellow** — a readable
  gold `#B58900` on light porcelain, a bright `#FFC94D` glow on the dark board),
  `--accent-tuner` (green, = `--seg-green`, the lock colour) + `-glow` pairs.
  `pedal.css` assigns `--pedal-accent` per `data-pedal-type`; the knob **value arc**
  (`.k-arc`), the **value readout** (`.k-val`, now accent-coloured + semibold, dims
  on bypass), and the **LED** all read `var(--pedal-accent, var(--led))` — so amp
  knobs (no `--pedal-accent`) fall back to the original red/faint-ink.
- **SD-1 morphology fixes.** The treadle now owns the **lower body and sits at the
  bottom** (was riding too high). The `.treadle-zone` gains top margin and the pad
  is taller; the compact face **drops the "Stomp" caption** — nothing sits below
  the treadle (the wordmark is embossed on the pad itself). Knob spacing was
  flagged cramped: the compact `.knob-row` gap goes `14px → 30px` and the pedal
  widens `320 → 336px` so three knobs get real air.
- **Tuner spacing fixes.** The lock LED sat right after the model line because the
  centered flex column let `.pedal-top` shrink to content (so `space-between` did
  nothing); `.pedal.tuner .pedal-top { width: 100% }` pushes the LED to the right
  edge. The model line letter-spacing tightens to `.18em` to fit the longer name,
  and the vertical rhythm (header → screen → strip → cents → footswitch) is
  normalized to an even ~16px.
- **Insider names** (homage, no trademarks): the RAT-type is wordmark **"Rodent"**
  (the wink; "Clipper" is the *app* brand, not this pedal) with model line
  `DIRT Nº1 · RODENT-TYPE`; the SD-1 is wordmark **"Super Drive"** with model line
  `DRIVE Nº2 · YELLOW` (winks at the classic yellow overdrive and the new yellow
  accent); the tuner is `IN-TUNE Nº0 · CHROMATIC` (winks at the poly-tuner family).
  No test asserts these strings (selectors use testids + aria labels), so they stay
  free to evolve.

### M6.8.2 — chassis darkness in BOTH themes (orchestrator visual pass)

M6.8.1 pinned one shared charcoal wash, but in **light** theme `--chassis-tint`
is only ~16 % alpha over the light panel, so the pedals read as pale gray — the
doctrine's "darker box against the lighter background" never actually happened.
The rule, made explicit:

> **The bench is light; the hardware is dark.** A pedal enclosure reads as genuine
> dark charcoal in BOTH themes. The BOARD/bench and amp panel follow the page
> theme (light in light, dark in dark); the pedal does NOT — it is dark either way.

Mechanism (all in `pedal.css`, **no `tokens.css` change** — the shared tokens are
left for the amp/bench and the M9.x work): the pedal is a **dark island**. Its
interior neumorphic token context (`--panel-grad`, `--well`, `--ink*`, `--cap`,
`--cap-edge`, `--arc-track`) is pinned to the **dark-theme values on `.pedal`
unconditionally** — byte-for-byte the dark tokens, so it is a no-op in dark theme
and cannot regress it, while in light theme it flips the knobs to dark metal, the
labels to light ink, and lets the accent arcs glow. The interior **sculpting
shadows** (`--sh-*`) are switched to the dark set only in effective-light theme,
and the pedal's own outer drop shadow (`.pedal.raised`) is re-declared there so
the dark enclosure still casts a real, bench-appropriate shadow on the light desk.

Also in this pass: the RODENT/SUPER-DRIVE wordmarks lost their hard `1px 1px`
white drop-shadow (read as a sticker) for a subtle engraved emboss on light
lettering; the SD-1 treadle is now an **explicit black-rubber pad** (pebble
texture + toe grip ribs, embossed light lettering) in both themes — the pad is
the morphology cue; the tuner screen **centers the note-letter + accidental as a
group** with the `♯` anchored at the letter's top-right shoulder and the octave as
a bottom-right subscript, and the cents row shows the **live signed value**
(`−18`, typographic minus) whenever a reading exists (bare `—` only for no
reading); the unlit meter wells are **deepened** for unmistakable lit/unlit
contrast; and the floating chain-chip toolbars were **lifted clear** of the
enclosure top (`board.css`, `-16px → -34px` + head-room).

### The identity system — how a future pedal declares its face

Every knob pedal's identity lives in one `PedalFace` entry in
`web/src/components/Pedal.tsx` (`FACES: Record<…, PedalFace>`). A face declares:

1. **Accent colour** (was: enclosure tint) — the chassis is now identical for all;
   identity is the small-area accent. Add an `--accent-<type>` (+ `-glow`) pair to
   the four theme blocks of `tokens.css` and map it via `--pedal-accent` in the
   `.pedal[data-pedal-type="…"]` rule of `pedal.css`. It colours the value arcs,
   value readouts, and LED.
2. **Face layout** — a `layout: 'stack' | 'compact'` field, surfaced as
   `data-face` on the enclosure and styled in `pedal.css`:
   - `stack` (RAT — *it is the reference*): tall box, big centered 3-knob trio,
     stark condensed **Anton** wordmark, a round stomp, red accent.
   - `compact` (SD-1 — Boss-compact **homage**): a knob row with clear air
     (`--kd: 56px`, `gap: 30px`) riding the top edge over a **wide flat hinged
     treadle** at the bottom (`.fsw-treadle` — the footswitch *is* the treadle,
     grip ribs at the toe, embossed wordmark, nothing below it), a small model
     line, yellow accent. Distinct from the RAT at a glance **even in grayscale**
     (all pedals now share the chassis colour, so the *morphology* carries the
     grayscale read entirely).
3. **Name/typography** — the `model` line + `wordmark` field: the knowing homage
   (RAT's condensed Anton "Rodent" logo vs the SD-1's bold Avenir "Super Drive"
   treadle plate).

To add a pedal: append a `FACES` entry (an `--accent-*` token pair in `tokens.css`
×4 blocks, a `--pedal-accent` map in the `data-pedal-type` rule, and — only if it
needs a new geometry — a `[data-face]` variant in `pedal.css`). The chassis is
free. The footswitch keeps one shared `data-testid="footswitch"` + `role="switch"`
across faces; only its *shape* changes. The recipe extends to **amps** too (they
already sit on the neutral neumorphic chassis; a future amp accents the same way).

### M6.8 (superseded) — original three-axis system

The original pass used a full-body **enclosure tint** per type (`--rat-tint` cool
graphite, `--sd1-tint` warm amber, `--tuner-tint` slate) as axis 1 and typography
as axis 3. Both were revised above (tint → one chassis; typography → the *name*).
The tuner TC-style face and the board's measured-cable layout below are unchanged.

### TC-style tuner face (`Tuner.tsx`, `tuner.css`)

The needle gauge is replaced by two segmented elements on the shared chassis:

- **Segmented meter** — a horizontal strip of **11 discrete LED wells** (5 red per
  side + 1 green center) in recessed neumorphic wells (off = unlit well, the
  neumorphic charm). Full scale ±50 c ⇒ ~10 c/segment. Lit reds form a **bar from
  center outward** on the flat (left) / sharp (right) side; count =
  `clamp(round(|cents|/10), 1, 5)`. The **green center** lights only when
  `|cents| ≤ IN_TUNE_CENTS` (3) held ≥ 350 ms (`LOCK_HOLD_MS`, same dwell the
  needle build used). Segments are driven imperatively each rAF frame via refs
  (`data-on`), so ~60 fps updates never churn React.
- **Segmented note screen** — a big **7-segment** letter (built as SVG polygons,
  no font download) on a recessed dark "screen" (`--seg-face`, dark in both themes,
  like a real tuner readout). Unlit segments stay faintly visible (`--seg-dim`);
  lit ones glow (`--seg-lit` + `--seg-lit-glow`). B and D use the lowercase forms
  so all seven note letters read distinctly; `-` is the no-signal placeholder. A
  sharp indicator (`♯`, CSS pseudo so it stays out of the a11y text) and the octave
  ride beside it.
- LED reds/greens are **tokens** (`--seg-red/-glow`, `--seg-green/-glow`) with
  dark-theme variants (glow on dark, ink on light).

Kept intact: mute-on-stomp (engaged = muted, the one DSP touch is unchanged),
engaged/disengaged states, the ±cents readout (flat = amber, sharp = blue), A=440,
and all testids/roles (`tuner`, `tuner-note` still reads `—` with no pitch via a
visually-hidden text node, `tuner-footswitch`, `tuner-led`, `tuner-cents`). New
`tuner-meter` segments carry `data-seg` / `data-on` for assertion.

### Board

No layout rework. Jacks are anchored at a fixed `top: 120px` on the `.board-unit`
wrapper (not the pedal body), and the cable endpoints are **measured from the live
DOM** every layout change / resize (`Board.measure()` reads each jack's
`getBoundingClientRect`), so the now-**variable pedal heights** (the compact SD-1 is
shorter than the RAT) anchor and route correctly with zero Board change — verified
in both themes with RAT + SD-1 + tuner on the board.

### Test seam

`__CLIPPER_TEST__.pushTunerReading(reading)` (App.tsx, additive) injects a synthetic
`TunerReading` straight into the tuner UI (bypassing the 33 ms live throttle) so the
segmented meter/screen can be driven without a live audio path — used by the new
face test and screenshot capture.

### Build and test (M6.8 / M6.8.1)

```bash
cd web && npm run build && npm test   # tsc + vite clean; full Playwright green
```

The segmented-meter face test (`tests/tuner.spec.ts`) is unchanged and passes: a
pushed flat reading lights red wells on the left only, a sharp reading on the right
only, and an in-tune (0 c) reading lights the green center (after the hold) with
reds dark. **M6.8.1 touched no selector or assertion** — the suite drives pedals
by `data-testid` + aria labels (`Distortion`, `Drive`, `footswitch`, `tuner-*`),
none of which the rename touched, and the "Clipper" heading assertions target the
*app* `<h1>`, not the RAT wordmark (now "Rodent"). Both themes were screenshotted
(board with all three pedals; the tuner engaged on a −18 c flat reading so the red
segments and green screen show). **No core / C-ABI / worklet / engine change** —
this remains a pure web visual pass.

## 21. v1.1 item 1 — TS808 "Screamer": the symmetric sibling of the SD-1 (one shared engine)

ROADMAP v1.1's first gear candidate, made real: **the SD-1 IS the Tube-Screamer
topology**, so the TS808-style "Screamer" ships as a *separate pedal* (`ts`, not
an SD-1 "mode") that reuses **all** of the SD-1's machinery. The two are literally
one engine with two configs. Files: shared `core/include/clipper/dsp/OverdriveEngine.h`
+ `core/src/dsp/OverdriveEngine.cpp`; thin `core/include/clipper/dsp/TsModel.h`
+ `core/src/dsp/TsModel.cpp`; tests `core/tests/test_ts_model.cpp`
(`clipper_ts_tests`); C ABI `ts_*` in `clipper_c_api.cpp`; `--pedal ts` in the
render CLI; worklet `ts` dispatch; `rig.ts` / Pedal / gear-tray / assistant wiring.
Trademark-safe throughout (no Ibanez/TS808/Tube Screamer on any user surface).

### Reuse approach — refactor, don't copy-paste

The M8 SD-1's processing guts were **extracted verbatim** into a shared
`OverdriveEngine` parameterized by an `OverdriveConfig` (mid-hump corner, DRIVE
plateau min/max dB, the two diode knees, 4558 op-amp GBW/slew, tone pivot/tilt,
DC-block corner). `SdModel` and `TsModel` are now ~70-line wrappers that own an
engine built from their config and forward every call. The SD-1 config reproduces
the former in-line constants **exactly**, so the M8 suite passes **byte-for-byte
unchanged** — verified: 2nd harmonic −20.9 dBc, op-amp corner 14 308 Hz, 4× ADAA
worst-alias −116.5 dB all reproduced to the digit. Zero DSP duplication: the two
pedals share the oversampled ADAA clip, the LM308Stage op-amp model, the mid-hump
high-pass, and the tone/level/DC-block chain. The engine gains one measurement
hook the family needs — `setDiodeKnees(vp, vn)` — so the symmetric TS can install
the SD-1's asymmetric knees (and vice versa) for the harmonic A/B without touching
production behaviour.

### The circuit → the model (the TWO differences; everything else SHARED)

- **SHARED family trait — the mid-hump.** Same to-ground leg `Zg = 4.7 kΩ +
  0.047 µF`, so the same non-inverting-gain corner
  `f_mid = 1/(2π·4.7k·0.047µF) = 720.5 Hz`, unity at DC rising to the HF plateau
  `1 + K`. Same `V_out = V_in + f(K·HP720(V_in))` clean-pedestal topology, same
  4558-class op-amp (3 MHz GBW / 1.7 V/µs slew), same tone tilt + LEVEL + DC block.
- **DIFFERENCE 1 — SYMMETRIC clipping.** ONE silicon diode each way (1-vs-1), both
  `Vf ≈ 0.60 V`, so `Vp == Vn == 0.60`. An ODD transfer curve ⇒ the 2nd (even)
  harmonic is ~ABSENT — the **mirror** of the SD-1's 2-vs-1 asymmetry (`0.95 / 0.50`,
  whose even harmonic is its warmth). Smoother, glassier grind.
- **DIFFERENCE 2 — DRIVE pot 500 kΩ** (vs the SD-1's 1 MΩ). Max plateau
  `1 + Zf/R_g = 1 + 500k/4.7k = 107.4×` = **+40.6 dB** (vs the SD-1's +46.6 dB —
  6 dB less top gain). DRIVE maps linear-in-dB over `[12, 40.6] dB`; as with the
  SD-1 the minimum is NOT unity (grinds lightly even at DRIVE 0). The 4558
  closed-loop corner `GBW/A` at max DRIVE ≈ `3e6 / 107.4 ≈ 27.9 kHz` — above the
  audio band, so (unlike the SD-1's 14 kHz) the op-amp adds no audible top-octave
  softening.

### Validation (ctest `clipper_ts_tests`, 44.1 k + 48 k + 96 k, assert-backed)

- **Mid-hump corner ≈ 720 Hz (SHARED):** matches the analytic `1 + K·HP720` within
  **0.04 dB worst** (44.1 k) / 0.07 dB (96 k), well inside the ±1.5 dB bar; 720 Hz
  measures **−2.87 dB** below the plateau (the −3 dB corner), 82 Hz **−18.1 dB**.
- **SYMMETRY → no even harmonic (the mirror of the SD-1's asymmetry):** 220 Hz at
  moderate drive → 2nd harmonic **−159.6 dBc** (44.1 k) / −156.8 dBc (96 k) for the
  symmetric TS, vs the SD-1's **−20.9 dBc** asymmetric warmth on the identical
  stimulus — a ~139 dB contrast, entirely symmetry-driven (forcing the TS to the
  SD-1's asymmetric knees restores the 2nd harmonic to **−19.1 dBc**, confirming
  the curve).
- **Max-DRIVE plateau ≈ 40.6 dB analytic:** measured **40.37 / 40.39 / 40.46 dB**
  (44.1 / 48 / 96 k) vs the analytic `1 + 500k/4.7k` = **40.60 dB** (< 0.25 dB),
  and distinctly below the SD-1's +46.6 dB (the 500k-vs-1M pot).
- **Aliasing (M2 bar):** shipped 4× ADAA at max DRIVE worst-alias **−117.8 dB**
  (44.1 k) / −123.2 dB (96 k), far below the −60 dB bar; ADAA beats naive by
  ~8 dB at 1×. Soft-knee THD rises gradually 1.7 % → 10.3 % → 20.0 % across input
  0.05 / 0.15 / 0.30; min-DRIVE THD 0.8 % at a 0.30 V input (light, not clean).

### A/B render commands (ts vs sd1, same settings)

```
# Screamer (symmetric) vs SD-1 (asymmetric), same knobs, plucked low A:
clipper-render --gen pluck:110:2.0 ts.wav  --pedal ts  --distortion 0.6 --filter 0.5 --level 0.8 --sr 48000
clipper-render --gen pluck:110:2.0 sd1.wav --pedal sd1 --distortion 0.6 --filter 0.5 --level 0.8 --sr 48000
#   -> TS  peak 0.98 / rms 0.147 (less top gain, symmetric — smoother);
#      SD-1 peak 1.22 / rms 0.200 (more gain + asymmetric pedestal push).
# Even-harmonic (symmetry) A/B on a 220 Hz sine (Goertzel of the render):
#   -> TS 2nd harmonic -236.7 dBc (~absent) vs SD-1 -23.5 dBc; both share the odd 3rd (~-13 dBc).
```

### Integration notes

- **One param shape, additive registries.** The Screamer keeps `PedalParams
  {distortion, filter, level}` reading as **Drive / Tone / Level** (like the SD-1);
  the shared registries each gain exactly one entry (`rig.ts` `PedalType` +
  gear tray + `TS_KNOB_DEFAULTS` drive 0.5 / tone 0.5 / level 0.75; worklet `ts`
  dispatch; `add_pedal` enum + coaching; `Pedal.tsx` FACES; `tokens.css`
  `--accent-ts`). The worklet routes per-node by C-ABI prefix (`_rat`/`_sd`/`_ts`).
- **Visual identity (doctrine §17).** Dark chassis (both themes), **GREEN accent**
  (arcs / readouts / LED — the green box). Its own **`slim` face**: an Ibanez-format
  box — knob row across the top, script **"Screamer"** wordmark, a **rectangular
  hinged stomp pad** in the lower body — slimmer than the RAT stack and clearly NOT
  the Boss-compact SD-1 treadle at a glance (grayscale too). Wordmark **"Screamer"**,
  model line **`DRIVE Nº3 · GREEN`**. No trademarks.
- **Assistant.** `add_pedal` gains `'ts'`; the coach knows the Screamer is THE
  stacking pedal (low drive / high level as a mid-forward clean boost into a pushed
  amp; symmetric/smoother vs the SD-1's asymmetric/warmer).
- Core suites all green (M0 + RAT + **SD-1 (unchanged)** + **TS** + amp + triode +
  JCM800, 44.1 k + 96 k); web tsc + vite build; Playwright +2 (`TS worklet:
  Screamer clips SYMMETRICALLY…` and `assistant: add_pedal adds a TS Screamer…`).

## 22. v1.1 item 3 — Phaser ("Ninety"): the script-era 4-stage phaser (docs §22)

The first MODULATION pedal that isn't the amp's chorus: a homage to the script-logo
MXR Phase 90. The spring reverb (§16) built deep first-order-allpass fluency, and
this is where it pays off — the phaser IS a short, swept allpass cascade summed with
the dry. `core/src/dsp/PhaserModel.{h,cpp}`, C ABI `phaser_*`, worklet `phaser`
dispatch, rig type `phaser`, a single-knob `Pedal` face, assistant `add_pedal`
'phaser' + coaching. **Trademark-safe:** wordmark **"Ninety"**, model line
`PHASER Nº4 · SCRIPT`; no MXR/Phase 90 on any user surface (docs §17 doctrine).

### The model — 4 first-order allpasses, one LFO, two moving notches

```
  in ─┬──────────────────────────────────────────► 0.5·dry ─┐
      └─► [AP₁]─►[AP₂]─►[AP₃]─►[AP₄] ─────────────► 0.5·wet ─┴─► out
```

- **Four FIRST-ORDER allpass sections**, bilinear form `H(z) = (a + z⁻¹)/(1 + a·z⁻¹)`,
  `a = (t−1)/(t+1)`, `t = tan(π·fc/fs)` — the −90° phase point sits at `fc` exactly
  (prewarped). Each rotates phase 0 → −180°; four in series rotate 0 → −720°, so the
  50/50 dry+wet sum has magnitude `|cos(θ/2)|` (θ = total allpass phase) and NULLS
  wherever θ is an odd multiple of 180° — at −180° and −540°. That's **exactly TWO
  notches** in-band (4 stages → 2 notches), the Phase-90 comb. For identical stages
  the pair falls at `tan(22.5°)·fc ≈ 0.414·fc` and `tan(67.5°)·fc ≈ 2.414·fc`.
- **Corner sweep** `fc ∈ [200, 2000] Hz` — a clean decade (the JFETs' ~decade of
  drain-source resistance swing). Over a cycle the notches sweep `≈83…828 Hz` (lower)
  and `≈483…4828 Hz` (upper) — the whoosh across the guitar's low-mids and presence.
  **Depth (this span) is FIXED — script-authentic** (the script Phase 90 has no depth
  control).
- **ONE knob: SPEED** → `0.06…8 Hz`, log (`rate = 0.06·(8/0.06)^knob`) — the original's
  famously wide range; slow tape-warble occupies most of the knob, the Leslie-ish fast
  end is compressed at the top.
- **NO feedback / regeneration** — that is the later block-logo ("Script switch"/
  reissue) addition; the SCRIPT voicing modelled here omits it (smoother, less vocal).
  **Ledger note** for a future variant: add a single feedback tap around the cascade
  (`loopIn = dry + g·wet`) to get the block-logo's resonant peak.
- **Per-stage detune ±1.5%** (fixed, deterministic `{0.985, 0.995, 1.005, 1.015}`):
  real JFETs never match, so the four notch contributions don't stack into one
  infinite null. A measurement hook (`setDetune(false)`) disables it for the clean
  analytic reference.

### LFO shape — a slightly-shark-toothed (rounded) TRIANGLE, and why

The corner is modulated in **log-frequency**; the LFO is a **triangle blended with
≈15% aligned cosine** (`roundedTriangle`). Rationale (documented tradeoff, mirrors
the ChorusModel sine justification in §11.2):

- **Triangle, not sine:** a triangle in log-`fc` sweeps the notches at a CONSTANT
  musical rate — equal time per octave rising and falling, the even "searching"
  Phase-90 movement. A sine dwells at the turnarounds (the corner sits at the extremes)
  and reads as a phaser that "parks", not sweeps.
- **Rounded, not pure triangle:** a pure triangle reverses instantaneously at its
  peaks — a corner in `d(fc)/dt` that ticks. The real pedal's op-amp integrator + the
  JFET's soft `R(Vgs)` law round those peaks; the 15% cosine blend reproduces that
  "shark-tooth" (constant-slope body, rounded reversals). Verified by the no-zipper
  spectral-floor test — the reversal leaves no turnaround tick.

### Linear time-varying → no oversampling; zipper is the only risk

There is no nonlinearity (no clipping), so **no oversampling** and **zero added
latency** (`phaser_set_oversampling` is a documented no-op, `phaser_latency_samples`
returns 0). The only aliasing risk is coefficient stepping as the corner sweeps, so the
four allpass coefficients are recomputed **PER SAMPLE** from the continuous LFO (never
block-stepped); the SPEED knob is one-pole smoothed (~8 ms) and the LFO phase is
continuous, so a rate move never clicks and a fast sweep leaves a clean floor.

### Validation — `clipper_phaser_tests` (deterministic, 44.1 / 48 / 96 kHz)

The analytic reference is the EXACT discrete transfer function `0.5·(1 + Π AP_k)` built
from the model's own published coefficient recipe (`cornerHzAtPhase` / `stageDetune`
static accessors), so it cannot drift from the implementation. Measured numbers (44.1 k):

- **(a) Static notch positions:** at frozen LFO phases the RENDERED notches match the
  analytic response to **0.00%** (e.g. ph 0.25 → 262.1 Hz / 1521.9 Hz), EXACTLY 2
  notches in-band, and the pair sits at ≈0.414·fc / ≈2.414·fc (the −8·atan physics).
- **(b) Sweep tracking:** the upper notch measured at the quarter-cycle points rises
  `482 → 1522 → 4677 Hz` (trough → centre → peak) and returns to `1522 Hz` at ¾ — it
  tracks the LFO corner across the cycle.
- **(c) SPEED map:** knob 0 → 0.060 Hz, knob 1 → 8.000 Hz, knob 0.5 → 0.693 Hz (log,
  = √(lo·hi)).
- **(d) Notch DEPTH (mix):** an allpass sum is never flat, so we assert DEPTH, not
  flatness — the composite notch is **48–94 dB** deep even WITH detune on (bar: >20 dB).
- **(e) No zipper:** a fast (8 Hz) sweep on a 1 kHz tone leaves the far-field floor
  (5–11 kHz) at **−130 to −157 dB** rel carrier (bar: <−80 dB) — smooth per-sample
  coefficients, no tick.
- **(f) Bypass/level sanity:** silence → silence; a 0.5 sine stays ≤0.50 in steady
  state (allpass-sum ~unity gain, no blow-up); a ~0 dB comb peak exists.

All 8 native suites remain green (`ctest`): the phaser suite added, nothing else changed.

### Integration (additive everywhere)

- **C ABI** `phaser_create/destroy/set_param/set_oversampling(no-op)/latency_samples(0)/
  process` — byte-for-byte the dirt-pedal opaque-handle shape, so the worklet's generic
  per-node routing drives it uniformly. Param slot 0 = SPEED; slots 1/2 carried, unused.
- **Worklet** (`clipper-processor.js`): a `phaser` branch in `_createPedal` /
  `_destroyPedal` / `_pedalSetParam` / `_pedalSetOversampling` / `_pedalLatency` /
  `_pedalProcess`. `build-wasm.sh` compiles `PhaserModel.cpp` and exports `_phaser_*`.
- **rig.ts:** `PedalType` gains `'phaser'`; `AVAILABLE_PEDAL_TYPES`,
  `PHASER_KNOB_DEFAULTS` (speed 0.35), `PEDAL_KNOB_DEFAULTS`, and the normalizer's type
  coercion all extended — one entry each (strictly additive; old rigs load unchanged).
- **Pedal face** (`Pedal.tsx` `FACES.phaser`, `pedal.css` `[data-face="single"]`,
  `tokens.css` `--accent-phaser`): a NEW `'single'` layout — dark chassis, **ORANGE**
  accent (`#C4611A` light / `#FF8C3A` dark), ONE big centered SPEED knob, round stomp,
  wordmark "Ninety", model `PHASER Nº4 · SCRIPT`.
- **Assistant** (`tools.ts`, `prompt.ts`): `add_pedal` enum gains `'phaser'`; a `'speed'`
  pedal-param alias resolves to slot 0; coaching covers placement (AFTER dirt = vocal
  EVH swoosh, BEFORE = subtler) and speed (slow = tape-warble, fast = Leslie-ish).
- **Render harness:** `clipper-render --pedal phaser --distortion <speed>` (listening).

### Listening pack (scratchpad)

A clean strummed E-major chord (`make_chord.py`) rendered at `phaser_slow.wav`
(speed 0.12 ≈ 0.11 Hz), `phaser_medium.wav` (0.42 ≈ 0.47 Hz), `phaser_fast.wav`
(0.85 ≈ 3.84 Hz), and `phaser_post_rodent.wav` (through the RAT first — the post-dirt
swoosh). All peak-safe (linear pedal, no clipping).

## 23. M10.2 — Ac30Amp (the Vox AC30 "top boost" — class-A chime, cathode bias, no NFB)

M10 continues with the **class-A chime**: a Vox AC30 "top boost"-style combo — the
jangly, compressed counterpoint to the JCM's crunch and the Twin's headroom. It joins
the registry as the **fourth amp voice, `ac30`**, end-to-end (C ABI, worklet, rig,
React face, assistant, native). Built bottom-up from the valve toolbox (`TriodeStage`,
the `LtpInverter`, the shared Koren pentode law, `ReverbModel`) plus a **new EL84 fit**
and **new power-amp physics**, and MEASURED against analytic targets (same discipline
as M9/M10.1). This milestone exists to build FOUR new pieces of machinery: **(a) EL84s,
(b) CATHODE bias with real dynamics, (c) NO negative feedback, (d) tube-rectifier
(GZ34) sag deeper than the JCM/Twin.**

### The model (canon values; every simplification in the ledger)

New portable core (platform-free C++17):

```
core/include/clipper/dsp/Ac30Preamp.h      1x 12AX7 + the interactive top-boost stack + volume (+ TopBoostToneStack)
core/include/clipper/dsp/Ac30PowerAmp.h    hot 12AX7 LTP PI -> TOP CUT -> cathode-biased EL84 quad -> OT -> NO NFB -> GZ34 sag
core/include/clipper/dsp/Ac30Amp.h         composed: preamp -> power -> spring reverb
core/src/dsp/{Ac30Preamp,Ac30PowerAmp,Ac30Amp}.cpp
core/tests/test_ac30_amp.cpp               measurement suite (clipper_ac30_tests)
```

**Signal order:** `guitar → Ac30Preamp → (interstage trim) → Ac30PowerAmp → [spring REVERB]`.

- **Preamp (top boost)** (`Ac30Preamp`): one 12AX7 common-cathode gain stage (Ra 100k,
  Rk 1.5k ∥ 25 µF, B+ 300 V) → the **TOP BOOST tone stack** → channel VOLUME (audio
  taper). Contrast with the Marshall/Fender preamps: a SINGLE gain stage feeding an
  interactive treble/bass network — the chime and jangle come from that bright lossy
  stack plus the class-A power section, not stacked preamp gain. **On the AC30 the
  VOLUME knob IS the overdrive** (it drives the hot phase inverter harder).
- **Top-boost tone stack** (`TopBoostToneStack`): the Vox interactive treble/bass
  "brilliance" network — MNA with trapezoidal cap companions, validated against the
  analytic `H(jω)` derived from the SAME netlist (like the Marshall/Fender stacks).
  Canon values: slope **100k**, treble pot **500k**, bass pot **1M**, caps **47 pF /
  0.022 µF / 0.022 µF** (treble cap / bass cap / series input coupling). NO mid control.
  Its signature: a **gain LOSS** through the passive stack (midband ~−4 dB at flat; no
  makeup gain in the network) and a **strong treble/bass interaction** (the wiper taps
  between the treble-cap node and the bass network — moving one knob shifts the other's
  band). Driven from V1's PLATE (a high-Z source ≈ Ra‖rp, no cathode follower).
- **Phase inverter** (reuses `LtpInverter` with the Koren 12AX7 fit — no new triode
  fit): run HOT off a lower B+ node and DELIBERATELY less balanced than the Twin's
  laser-matched PI (asymmetric 100k/110k plates) so it (i) clips early — its own
  asymmetric soft clip is part of the sound — and (ii) leaves a residual even-harmonic
  imbalance that, with the class-A power stage, is a source of the AC30's **chime**.
- **TOP CUT** (the centerpiece control): the post-PI treble-cut — a pot + cap ACROSS
  the PI outputs, modelled as a one-pole low-pass on each anti-phase PI plate AC drive
  whose corner LOWERS as the knob rises (kTopCutHiHz 8 k → kTopCutLoHz 850 Hz, log
  map). **CLOCKWISE = MORE CUT** (authentic INVERTED sense — the UI labels it **CUT**).
  It sits AFTER the PI, so it tames the top WITHOUT touching the preamp stack's chime
  the way turning the treble knob down would.
- **EL84 push-pull quad, CATHODE-BIASED, class A — the CENTERPIECE.** Four EL84
  pentodes via the shared Koren pentode law (same equations as the M9.3 EL34 / M10.1
  6L6; only the six constants differ). **New EL84 fit** (documented community Koren
  set, kg1/kg2 trimmed to land the operating point): `mu 20, ex 1.35, kg1 1300, kp 42,
  kvb 24, kg2 2400` (±10 % pentode band). Modelled as a PP pair of super-tubes
  (kTubesPerSide = 2). **CATHODE bias:** NO fixed negative supply — the grids sit at
  0 V DC through their grid leaks, and the whole quad shares ONE cathode network
  (**Rk = 50 Ω ∥ Ck = 50 µF**, canon AC30) to ground. The cathode voltage Vk = Rk·(total
  cathode current), so each tube's bias is Vg1k = Vg − Vk (≈ −Vk ≈ −9.5 V at idle). The
  network is modelled EXPLICITLY (a backward-Euler Rk∥Ck node) so the **bias moves with
  the signal**: under sustained drive the average cathode current grows, the cap charges,
  Vk RISES → the bias cools → the famous class-A **bloom** then squash, recovering on
  Rk·Ck (τ = 2.5 ms).
- **NO negative feedback:** `kFeedbackBeta = 0`. The forward path stands alone (the raw,
  immediate voicing IS the point). `setFeedbackEnabled()` is retained ONLY as the test
  seam for the **anti-NFB assertion** (the mirror of the JCM/Twin sign-catcher): toggling
  it leaves the output BIT-EXACT because there is no loop.
- **GZ34 tube-rectifier SAG (deeper than JCM > Twin).** Physics point that shaped the
  model: a BALANCED class-A push-pull draws a **near-constant total B+ current** (the two
  anti-phase tubes swap conduction; their SUM is flat), so — unlike the JCM's fixed-bias
  class-AB, whose average current surges — the AC30's plate rail can't sag from average
  draw. What the valve rectifier actually can't supply is the **delivered signal current**
  (the differential push-pull current into the OT primary, which swells with output). So
  the model is TWO parts: (a) the PHYSICAL rail/screen/cathode integration (a modest
  soft-knee plate-rail droop + the cathode-bias dynamic above), and (b) the **GZ34 sag
  proper** — a demand-envelope COMPRESSION of the delivered output (Idemand = idle draw +
  |differential current|, fast-attack/slow-release, sag = 1/(1 + kSagCompGain·(Idemand −
  Iidle))). Applying the rectifier sag to the delivered output rather than starving the
  tube DC bias keeps the class-A cathode bloom intact — a **documented simplification**
  (the rectifier's peak-current limit, not a full diode+reservoir SPICE). Sag lands in a
  documented **4–8 dB window, DEEPER than the JCM's ~3.4 dB and MUCH deeper than the
  Twin's ~2.1 dB** (ordering Twin < JCM < AC30, asserted).
- **Output transformer** LINEAR v1 (Raa 8 k, n ≈ 31.6, LF 80 Hz / HF 11 kHz; core
  saturation deferred, same as M9.3/M10.1). **Reverb** reuses `ReverbModel` (mono, after
  the power amp; a usability add — the real top-boost channel has none — same call and
  placement as the JCM's added reverb, docs §19; mix 0 = bit-exact passthrough).

### Documented simplifications (the ledger)

- The push-pull quad is a **PP pair of super-tubes** (2 paralleled EL84/side), matched,
  no per-tube variance — same as the Twin's 6L6 quad.
- The **EL84 Koren fit** is a community parameter set with kg1/kg2 trimmed to the operating
  point (pentode fits are loose → ±10 % band); bias is DERIVED to the fit, not a datasheet.
- OT is **linear** (core saturation deferred).
- **GZ34 sag is an envelope model** (the rectifier's peak-current limit voiced as a
  demand-envelope output compression), not a first-principles diode+reservoir SPICE —
  chosen after establishing that a balanced class-A push-pull's near-constant average B+
  current makes a literal rail-droop model produce almost no sag (see §6 in the header).
- TOP CUT is a one-pole per-leg low-pass (the pot+cap across the PI outputs), not a full
  component model; TOP BOOST stack is MNA with the canon values (analytic-H validated).
- The reverb (usability add) has no analog in the real top-boost channel.

### Validation — `clipper_ac30_tests` (deterministic, 44.1 / 48 / 96 k; MEASURED)

Every number is asserted against an analytic target derived IN THE TEST.

1. **DC operating points**: V1 on the self-bias load line (Va 202 V, Vk 1.46 V, Iq 0.98 mA);
   **EL84 Iq 34.9 mA/tube** = the analytic shared-cathode Koren fixed point (±10 %), **Vk
   9.52 V** = the analytic self-bias fixed point (±5 %), rail 309.5 V, screen 285.7 V,
   **Pdiss 10.8 W** (< the ~12 W EL84 max — class A runs hot but bounded); PI balanced.
2. **Top-boost stack** vs analytic `H(jω)`: discrete-vs-analytic worst **< 0.5 dB**; the
   signature **GAIN LOSS** (midband ~−4 dB, |H| never > 0 dB); **treble/bass interaction**
   present (moving bass shifts the treble/mid band); treble knob controls the top over a
   **~20 dB** range.
3. **NO NFB** (the anti-NFB catcher): open == closed **BIT-EXACT** (β = 0); the forward
   voicing stands alone.
4. **Cathode bias** (the centerpiece): idle Vk == analytic fixed point; under sustained
   drive **Vk RISES ~0.75 V** (bias cools → the bloom); after the drive stops it recovers
   toward idle with **τ ≈ 1.0–1.5 ms** vs the Rk·Ck = 2.5 ms RC (0.3–3× band — the demand +
   cap dynamics).
5. **TOP CUT** (inverted sense): 3 kHz drops **−9 → −11.3 → −17.6 dB** as CUT goes 0/.5/1
   (MORE cut = darker), while 150 Hz is untouched (Δ < 0.1 dB).
6. **Sag ordering + window**: **Twin ~2.1 < JCM ~3.5 < AC30 ~4.3 dB**, AC30 in the
   documented 4–8 dB window (same hard-burst attack-vs-settle metric into all three).
7. **Chime**: at moderate drive the AC30's **2nd harmonic (−27.6 dBc) is prominently above
   the fixed-bias Twin's (−31.4 dBc)** at comparable output — the class-A even-harmonic chime.
8. **The product**: chimey-clean THD 0.31 % at low volume, **monotonic** growth to real
   breakup **31.9 %** at max (peak **0.858 ≈ 0.9**); ±10 V slam finite/bounded.
9. **Aliasing** at MAX volume (M2 sweep 4186 Hz): shipped **4× = −159 / −169 / −171 dB** at
   44.1 / 48 / 96 k — far below the −60 dB M2 bar (the smooth top-boost + sag compression
   alias little); 8× buys nothing → **4× ships** (the M2 budget, same as M9/M10.1).
10. All **10 prior ctest suites** still pass, incl. the M9/M10.1 suites bit-exact.

### Integration (the M9.4 pattern, extended to four voices)

- **C ABI** (`AmpChain`): fourth voice `ac30` (`amp_set_model` 0|1|2|3), created + prepared
  up front (4× OS) so the swap is a lock-free int flip. Param routing (all four voices kept
  current): VOLUME → clean120 + twin + **ac30**; BASS/TREBLE → all tone stacks incl. ac30's
  top boost; **MIDDLE → NOT ac30** (the top boost has no mid — the knob is hidden on the
  face); REVERB (9) → all four; **PRESENCE (id 11) is REUSED as the AC30's TOP CUT** (both
  are power-amp HF controls — documented reuse + the inverted-sense mapping); GAIN/MASTER →
  jcm only; BRIGHT/CHORUS → not ac30. The AC30 is a **mono 2×12 combo → dual-mono** into the
  shared cab pair; the app hints at the **clean212** cab (closest 2×12 platform; a future
  alnico 2×12 IR is ledgered) when switching to ac30 with brit412 active.
- **rig.ts / params.ts / audio.ts**: `ac30` in `AmpType`/`AVAILABLE_AMP_TYPES` and
  `AMP_MODEL_INDEX` (index 3); **no new params** (reuses volume/bass/treble/presence→cut/
  reverb/cab). Migration coerces unknown types to clean120; old rigs round-trip.
- **Worklet**: model index 3 passed opaquely, declick-bracketed swap, latency re-published.
- **UI face** (doctrine §17): `Ac30Face` — model line **COMBO Nº4 · TOP-BOOST**, wordmark
  **"Thirty"**, a warm **COPPER/tan accent** (`--accent-ac30`, all 4 theme blocks — the
  diamond-grille brown wink without trade dress). Controls: VOLUME · TREBLE · BASS · **CUT**
  · REVERB + cab + power; hidden middle/gain/master/bright/chorus. The CUT knob reuses the
  presence param slot (labelled CUT, inverted sense). Trademark-safe naming (no "Vox/AC30").
- **Assistant**: `set_amp` gains `ac30`; coaching for the chime/jangle, that the **VOLUME
  knob IS the overdrive**, that **CUT tames the top WITHOUT losing chime** the way turning
  treble down would, and the Rickenbacker/Beatles-to-Radiohead lore.
- **Native**: `ClipperEngine`/APVTS gain the ac30 voice (Amp Model choice index 3); the
  identical-core test gains a **fourth bit-exact case** (AC30: top boost + cathode-biased
  EL84 + TOP CUT + reverb).

### Render harness (`clipper-render --ac30`)

```bash
# Chime chord at moderate volume with a touch of spring, through the clean212 2x12:
./build/clipper-render --gen pluck:110:3.0 --amp 0.2 --sr 48000 --ac30 --ac30-cab \
    --ac30-volume 0.45 --ac30-treble 0.7 --ac30-cut 0.2 --ac30-reverb 0.2 chime.wav
# Cranked bloom/compression (VOLUME maxed — the volume knob IS the overdrive):
./build/clipper-render --gen pluck:82:4.0 --amp 0.5 --sr 48000 --ac30 --ac30-cab \
    --ac30-volume 1.0 --ac30-drive 1.5 breakup.wav
# TOP CUT swept dark (higher = darker, the inverted sense):
./build/clipper-render --gen pluck:147:2.0 --amp 0.3 --sr 48000 --ac30 --ac30-cab \
    --ac30-volume 0.7 --ac30-cut 0.8 topcut.wav
```

`--ac30` renders the FULL composed amp (output normalized, 1.0 == full scale); `--ac30-cab`
runs the clean212 2×12 with the shipped limiter. `--ac30-cut` is the TOP CUT (inverted:
higher = darker). Reuses `--os`.

### §23 amendment — the "muddy" report: CUT knob re-taper (control law, not circuit)

Field report: *"the vox sounds a little muddy to my ear."* The ear was right, and the
cause was a **control-law** bug, not a circuit one. The AC30's CUT reuses the shared
`presence` slot, whose default is **0.5** — and the original bare log map put knob 0.5
at a ~2.6 kHz corner, i.e. the face *opened* with roughly half the top-cut engaged:
measured ≈ 3 dB darker at 3 kHz than the voice with the cut backed off. A real
top-boost is normally played with CUT near minimum — the chime amp was defaulting
into its dark half.

Fix: the corner is now log-mapped from the **skewed** knob `knob^2.3`
(`kTopCutSkew`), so knob 0.5 → corner ≈ 5.1 kHz (a gentle top trim) while the
endpoints are untouched (knob 0 → 8 kHz, knob 1 → 850 Hz — the full dark range still
lives at the top of the travel, where the real control does its work). An analytic
re-taper of the knob law; the filter and its reachable range are unchanged.

Guards added to `test_ac30_amp`: the default 0.5 knob must sit within 2 dB of no-cut
at 3 kHz AND full cut must still be ≥ 5 dB darker than noon (range preserved); plus a
**character guard** — at opening defaults the Thirty must measure ≥ 6 dB brighter at
3 kHz (rel 1 kHz) than the Twin at its defaults (measured ≈ 19 vs 7 dB), so no future
regression can let the chime king open muddier than the blackface clean.

### §23 second amendment — the "not enough breakup" report: the starved phase inverter

Field report: *"I'm fairly sure the ac30 doesn't have enough gain/breakup, it breaks
up less easy than the fender twin."* Measured first, and the ear was right again — but
this time it was not a control law. **The voicing was INVERTED.** A 30 W cathode-biased
class-A combo is the EARLY-breakup amp of the lineup (edge-of-breakup at moderate
volume *is* the Vox sound); an 85 W blackface Twin is the clean-headroom king. Ours had
them the wrong way round.

#### The measurement that opened the case

THD vs the VOLUME knob, 220 Hz, 48 kHz, composed voices at their opening tone knobs,
no cab (it is linear and identical to both, so it cancels). JCM800 shown as the
reference dirt voice (its GAIN knob swept, MASTER 0.5):

**BEFORE** — hot-pickup level (0.316 V peak = −10 dBFS):

| VOLUME | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|---|---|---|
| **AC30** THD % | 0.80 | 0.78 | 0.76 | 0.74 | 0.74 | 0.83 | 1.18 | 2.18 | 4.40 | 8.74 |
| **Twin** THD % | 4.11 | 4.18 | 4.29 | 4.45 | 4.67 | 4.90 | 4.99 | 4.60 | 5.98 | 14.13 |
| **JCM800** THD % | 5.36 | 9.82 | 13.5 | 24.5 | 34.1 | 40.4 | 43.9 | 43.6 | 36.2 | 30.4 |

The Thirty was **cleaner than the Twin at every knob position on its travel**, and only
crossed 5 % THD wide open. At the standard −20 dBFS pluck level it was flatter still
(0.25 % from VOLUME 0.1 all the way to 0.6). That is not an AC30; that is a very
polite hi-fi amp with a Vox tone stack in front of it.

#### The diagnosis, in circuit terms

Stage-by-stage voltages told the story immediately (0.3 V peak in, VOLUME 1.0):

| node | AC30 (before) | Twin |
|---|---|---|
| volts at the volume node | 3.65 V | 54.3 V |
| × interstage handoff → PI grid | **1.10 V** | 8.68 V |
| PI gain per leg | **×9.1** | ×7.4 (12AT7) |
| PI idle current / plate | **83 µA, 291.7 V (97 % of B+)** | 232 µA, 386.8 V |

**The AC30's phase inverter was biased nearly to cutoff.** `LtpInverter` is a
TWO-TERMINAL long tail — grids at 0 V DC, tail resistor straight to ground — so the
standing current is set entirely by `Rtail`, and at the configured 22 k the pair
self-biased at **83 µA per triode with its plates parked 8 V below B+**. No real
guitar-amp LTP idles there: a 12AX7 PI runs **0.5–0.9 mA per triode, plates at 70–85 %
of B+, ~×25–35 per leg** (real circuits get there by returning the long tail to a
negative reference, which a two-terminal tail cannot express). Ours measured ×9.1.

That single number explains the whole field report. On an AC30 **the VOLUME knob IS
the overdrive** — its only job is to drive the phase inverter hard enough to slam the
EL84 grids (which sit at −9.5 V self-bias and need ~9.5 V of swing). With a PI that
deaf, wide open with a hot pickup the model handed the power section 1.8 V when the
section needs ~1.2 V just to *start* moving and ~2 V for 20 % THD. The class-A power
amp — the cathode bias, the EL84 quad, the GZ34 sag, the entire reason M10.2 exists —
was never being driven. The header's claim that the PI "runs HOT and clips early" was
aspiration, contradicted by its own operating point.

Two more constants were compensating for it downstream:

- `Ac30Amp::kInterstageScale = 0.30`, documented as *"higher than the Twin's — the
  AC30 PI is meant to run HOT"*. But that path is **passive** in the real amp (volume
  wiper → coupling cap → grid stopper → channel mixing resistors → PI grid), so it can
  only ever be a divider, and the AC30's wiper carries a single stage's volts where
  the Twin's carries two stages'. 0.30 was an arbitrary trim, not a divider.
- `Ac30PowerAmp::kFullScaleSecV = 7.5` — the output normalization. Because the tubes
  were barely moving, the model needed a small divisor to reach a respectable level:
  the calibration was **hiding the missing drive**.

**Suspects checked and cleared** (recorded so nobody re-opens them):

- *The preamp is one gain stage short of a real top-boost channel* — TRUE, and
  measured: the real board is two ECC83 gain stages plus a cathode follower around the
  treble/bass network; ours is one stage into a stack driven from that stage's plate.
  But a second common-cathode 12AX7 in this configuration MEASURES **×59.3
  (+35.5 dB)** on its own, and folding that into the corrected structure lands the
  OPENING defaults (VOLUME 0.4, standard −20 dBFS pluck → 0.070 V at the PI grid
  today) at **≈ 4.1 V** of PI drive — which the power section's measured sensitivity
  curve puts at **> 20 % THD before the player has touched anything**. Permanently
  saturated, no jangle left. The model therefore keeps the single stage; ledgered below.
- *The cathode follower would recover the stack's loss* — measured, and it does not:
  dropping the stack's source impedance from 33.4 kΩ (plate) to 700 Ω (a follower)
  recovers only **1.8 dB at 220 Hz**. The Vox stack's ~13.8 dB loss is the network,
  not the source. Not worth the extra triode solve.
- *`kFullScaleSecV` should be the physical 15.5 V (√(30 W · 8 Ω))* — rejected. Every
  voice is normalized to its own cranked peak (Twin 24 V, JCM 26 V) so the amps stay
  level-comparable to each other; forcing the AC30 to a wattmeter would have made it
  unable to reach full scale at all. It is a normalization, and it follows the
  measured swing.
- *The volume taper is starving the knob* — no: the shared `audioTaper` (k = 4) is the
  same law the Twin and clean120 use, and it matches a real audio pot. The taper was
  not the problem; what it was scaling was.

#### The fix (gain structure — three constants, no waveshaper, no drive knob)

| constant | before | after | why |
|---|---|---|---|
| `Ac30PowerAmp` PI `Rtail` | 22 kΩ | **2.2 kΩ** | lands the textbook 12AX7 LTP point: **0.53 mA/triode, plates 247.1/244.9 V (82 % of B+), gain ×32**. The model equivalent of the real long tail's negative reference. |
| `Ac30Amp::kInterstageScale` | 0.30 | **0.67** | the wiper→PI-grid path is passive, so it can only be a divider: 0.67 is the two-channel mixing division (each wiper through its own 1 M into the shared PI grid). |
| `Ac30PowerAmp::kFullScaleSecV` | 7.5 V | **10.0 V** | the normalization follows the measured swing, as the Twin's and JCM's do. Properly driven the secondary reaches 11.8 V cranked; 7.5 would have pushed the output 1.4 dB PAST full scale. Cranked power sine is back at peak **0.88**. |

Nothing was added to the signal path: no distortion stage, no waveshaper, no hidden
drive. The circuit is the same circuit — its phase inverter is now biased where a
phase inverter is biased.

Power-section **input sensitivity** after the fix (`Ac30PowerAmp` alone, 220 Hz, volts
peak at the PI grid) — the curve the numbers above are read against, and the reason
the amp is no longer deaf:

| PI grid (V pk) | 0.05 | 0.10 | 0.20 | 0.30 | 0.50 | 0.80 | 1.20 | 2.00 | 5.00 | 8.00 |
|---|---|---|---|---|---|---|---|---|---|---|
| THD % (after) | 1.05 | 2.09 | 4.07 | 6.23 | 10.4 | 11.9 | 14.6 | 22.1 | 24.2 | 30.5 |
| THD % (before) | 0.12 | 0.25 | 0.58 | 1.00 | 2.21 | 4.87 | 9.48 | 18.9 | 35.7 | 40.4 |

Same tubes, same EL84 fit, same sag: **the section always could break up — nothing was
ever reaching it.**

#### AFTER — the same sweeps

Hot-pickup level (0.316 V peak = −10 dBFS):

| VOLUME | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|---|---|---|
| **AC30** THD % | 1.27 | 2.00 | 3.07 | 4.60 | 7.06 | **10.75** | 12.30 | 14.11 | 20.12 | 23.28 |
| **Twin** THD % | 4.11 | 4.18 | 4.29 | 4.45 | 4.67 | **4.90** | 4.99 | 4.60 | 5.98 | 14.13 |
| **JCM800** THD % | 5.36 | 9.82 | 13.5 | 24.5 | 34.1 | 40.4 | 43.9 | 43.6 | 36.2 | 30.4 |

Standard pluck level (0.1 V peak = −20 dBFS) — the jangle side must survive:

| VOLUME | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|---|---|---|
| **AC30** THD % | 0.40 | 0.63 | 0.98 | 1.49 | 2.26 | 3.38 | 4.95 | 7.88 | 11.03 | 12.09 |
| **Twin** THD % | 1.27 | 1.30 | 1.33 | 1.39 | 1.47 | 1.60 | 1.78 | 2.02 | 2.27 | 2.34 |

Breakup onset (first 0.1 step reaching 5 % THD, hot pickup): **AC30 0.5, Twin 0.9**.
At the documented mid-knob 0.6 the Thirty is **2.2× dirtier** than the blackface, and
wide open it sits in saturated class-A crunch (23 % on a sine, 34 % on the suite's
110 Hz / 0.5 V crank case) with the sag envelope holding a long, flat, compressed
sustain — measured on the listening renders as a decay that moves only 0.1 dB over
half a second, the cranked-Vox squash, with no gating or dropout.

At its opening defaults the voice now sits **at** the edge of breakup instead of 15 dB
below it, which is what a real AC30 at "4" does — and it is 13.3 dB louder at defaults
than before (player-expectations RMS delta **−11.7 → +1.6 dB**, level with the JCM's
+1.7). That level move is the honest consequence of a knob that finally reaches the
power tubes, and the per-gear window in `test_player_expectations.cpp` was re-centred
to match.

#### Honesty about the calibration

The absolute gain is calibrated, not transcribed. A literal AC30 top-boost channel has
far more preamp gain than this model (see the cleared suspect above) and a real one
with a humbucker is crunching by "3" on the dial. Our target — audible breakup by
VOLUME 0.5–0.6 at a hot-pickup level, clean jangle below it, saturation above — sits
between our old model (~18 dB too clean) and a literal transcription (~17 dB too
dirty), and it is chosen so the knob's *usable travel* spans clean → edge → crunch the
way the real amp's does. What is NOT calibrated is the phase-inverter operating point:
that is a plain circuit correction, and it is where the fix lives.

One more honest note, recorded rather than fixed: **the Twin's 4–5 % THD floor** in
these tables is preamp clipping, and it is flat across its volume travel because
`TwinPreamp` places its VOLUME pot AFTER both gain stages (V1 → stack → V2 → volume)
where the real AB763 puts it right after V1 — so the knob cannot back the clipping
off. That is the mirror image of this amendment's bug and is **ledgered as a separate
field item**; it is deliberately untouched here (it would move the Twin's goldens and
its own suite), which is why the new guard below compares at a mid-volume point and a
5 % onset threshold rather than assuming the Twin is pristine.

#### The new permanent guard

`test_ac30_amp.cpp` → `testBreakupOrdering` (48 kHz, the AudioWorklet rate the report
was heard at; the voicing claim is rate-independent and the per-rate circuit behavior
is already pinned by the nine tests above at 44.1/48/96 k):

1. the AC30's **breakup-onset volume** (first 0.1 step at ≥ 5 % THD, hot pickup) must
   be **≤ 0.65** — it must actually break up in the usable half of the knob; and
2. it must be **≥ 0.2 of knob travel BELOW the Twin's** onset (measured 0.5 vs 0.9); and
3. at the documented mid-volume **0.6** it must measure **≥ 8 % THD** and **≥ 1.8× the
   Twin's** (measured 10.75 % vs 4.90 %, 2.2×).

Character is guarded in the other direction by the §23 chime/character guard, which
still passes with an **11.6 dB** margin, plus the class-A chime margin (now **9.0 dBc**
of 2nd harmonic over the Twin, was 3.8), the sag ordering and its 4–8 dB window (now
**5.8 dB**), the anti-NFB bit-exactness, the top-boost stack vs analytic H(jω), and
every DC operating point.

#### Two test probes retuned (they encoded the bug, not the physics)

- **Cathode-bias bloom** (`testCathodeBias`): the probe injected **8 V** at the PI
  grid. That was "sustained loud" only against a PI that was 11 dB deaf. With the
  corrected inverter the musical range moved: **0.15 V** at the PI grid is now the
  edge-of-breakup drive (≈ VOLUME 0.34 with a −10 dBFS pickup), and the bloom is
  measured there (**Vk +0.44 V**, recovery τ ≈ 1.3 ms against Rk·Ck = 2.5 ms). Newly
  MEASURED and documented: above ~0.22 V at the PI grid the EL84 grids start
  conducting (7 V of swing against the −9.5 V self-bias), the coupling caps charge and
  the bias shifts **COLD** instead — grid-leak blocking. Both regimes are real; the
  crossover is now a documented property of the model rather than an accident:

  | PI grid drive (V pk) | 0.05 | 0.10 | 0.15 | 0.20 | 0.25 | 0.30 | 0.50 | 1.0 | 8.0 | 45 |
  |---|---|---|---|---|---|---|---|---|---|---|
  | ΔVk (V) | +0.14 | +0.37 | **+0.44** | +0.25 | −0.10 | −0.51 | −1.65 | −2.39 | +0.14 | +0.56 |

- **TOP CUT** (`testTopCut`): the probe drove the power amp at **0.5 V**, which against
  the corrected PI is deep saturation — where the clipped level is drive-independent
  and a treble cut barely moves the output, i.e. the test would have been measuring the
  clipper instead of the filter. TOP CUT is a LINEAR one-pole per leg, so the probe is
  now **0.05 V** (MEASURED THD 0.12 %), back in the section's linear region. The
  re-taper guards from the first amendment are unchanged and still pass (noon is a
  **0.7 dB** cut at 3 kHz, full cut **10.0 dB**, 150 Hz untouched at 0.12 dB).

#### Golden regenerated — deliberately

`core/tests/goldens/ts_ac30.wav` was regenerated with `scripts/update-goldens.sh`. This
is exactly the conscious-drift decision the golden gate exists to force: the Thirty's
voice changed on purpose. **Only that golden moved** — the other four "first five
minutes" rigs re-render byte-identical, which is itself a useful check that the change
is confined to the AC30 voice.

#### Ledger additions (M10.2 simplifications)

- The preamp remains **ONE 12AX7 gain stage** into the top-boost stack, where the real
  top-boost board has two ECC83 gain stages and a cathode follower. Measured cost of
  literal transcription: +30…+35 dB, which our volume taper cannot spend without
  saturating the voice at its opening defaults. The absolute gain is therefore
  calibrated at the (passive, ≤ 1) interstage handoff; the *shape* of the travel —
  clean → edge → crunch — is what the model reproduces.
- `LtpInverter` is a **two-terminal tail** (grids at 0 V, tail to ground), so a PI's
  standing current is set solely by `Rtail`. The AC30's 2.2 kΩ is that network's model
  equivalent, not a parts-bin value. (The JCM's 10 kΩ / Twin's 22 kΩ are untouched —
  their voices and goldens are bit-identical.)

#### A/B listening pack

```bash
# BEFORE/AFTER, same pluck, same knobs — the breakup arrives ~2 knob positions earlier:
./build/clipper-render --gen pluck:110:3.0 --amp 0.3 --sr 48000 --ac30 --ac30-cab \
    --ac30-volume 0.5 --ac30-treble 0.6 --ac30-cut 0.5 ac30_breakup_after_vol50.wav
#   (repeat at --ac30-volume 0.7 / 1.0; the Twin reference for the ordering claim:)
./build/clipper-render --gen pluck:110:3.0 --amp 0.3 --sr 48000 --twin --twin-cab \
    --twin-volume 0.7 --twin-treble 0.6 twin_vol70.wav
```

Rendered peaks (normalized, 1.0 == full scale): AC30 vol 0.5 **0.144 → 0.557**,
vol 0.7 **0.283 → 0.714**, vol 1.0 **0.561 → 0.781**; the Twin at vol 0.7 sits at
0.300 and stays glassy. The post-fix VOLUME 0.5 render is now level-matched with the
pre-fix VOLUME 1.0 render — the same amp, two knob positions of breakup recovered.

## 24. v1.1 item 4 — Muff "Pi": the four-transistor fuzz + the reusable BjtStage

ROADMAP v1.1's item 4, made real: a trademark-safe homage to the early-70s
"ram's head"-family **Big Muff Pi**, shipped as the `muff` pedal **"Pi"**. Its REAL
product is the **reusable BJT gain-stage machinery** — the Fuzz Face (item 5) and the
RG100 (an M10 solid-state amp) both need it. Files: `core/include/clipper/dsp/BjtStage.h`
+ `core/src/dsp/BjtStage.cpp` (the device), `core/include/clipper/dsp/MuffModel.h` +
`core/src/dsp/MuffModel.cpp` (the pedal), tests `core/tests/test_muff_model.cpp`
(`clipper_muff_tests`); C ABI `muff_*` in `clipper_c_api.cpp`; `--pedal muff` in the
render CLI; worklet `muff` dispatch; `rig.ts` / Pedal / gear-tray / assistant wiring.
Trademark-safe throughout (wordmark "Pi", model line `FUZZ Nº5 · PI`; no EHX / Big
Muff text on any user surface — docs §17 doctrine).

### BjtStage — the first BJT (Ebers-Moll) device model (the reusable product)

The BJT sibling of the M9.1 TriodeStage: same nodal-Newton house style, a transistor
instead of a valve. **Device:** transport-form **Ebers-Moll NPN** (2N5088-class:
`Is = 1e-14 A`, `βF = 400`, `βR = 4`, `Vt = 25.85 mV` at 300 K — the high β is why a
Muff has so much gain). The reverse (`Ir`) term is kept, so the transistor SATURATES
on the clip peaks — part of the compression, not a nicety. **Circuit:** a
collector-feedback-biased common emitter (`Vcc 9 V`, `Rc 10 k`, feedback `Rf 470 k`,
emitter degeneration `Re 390 Ω`, input coupling `Cin 100 nF`, feedback cap
`Cf 470 pF`), optionally with an **antiparallel 1N4148 pair** across base↔collector
(the clip stages). Because `Rf` is large and β high, a clip stage's idle collector-base
voltage exceeds the diode knee, so **the clipping diodes conduct near idle** and pin the
collector barely above the base — a stage with almost no clean headroom that clips
essentially always. That is the root of the wall-of-sustain compression, and the DC
solve therefore includes the diodes. **Solver:** per-sample 3×3 nodal Newton (`Vb`,
`Vc`, `Ve`) with an analytic Jacobian, backward-Euler companions for `Cin`/`Cf`, and —
crucially — a **backtracking line search** (Armijo-ish descent on the residual ∞-norm).
A bare Newton oscillates and stalls on the STIFF diode-in-feedback system (the pre-fix
cascade blew up to hundreds of volts from non-convergence); the line search + tangent-
limited `exp`/diode currents make it converge in 6–9 iterations and stay finite/bounded
on a ±10 V slam at every rate. BjtStage owns NO oversampler — MuffModel drives four of
them per oversampled sample through ONE shared `Oversampler`, so the stages compose in a
cascade (unlike TriodeStage, which oversamples itself).

**Reuse notes (what item 5 / the RG100 inherit):** the whole Ebers-Moll device + solver
is behind a `Config` (device params, the R/C bias network, an optional diode pair). The
**Fuzz Face** is two of these with a germanium-ish `Config` (lower β, higher `Is`) and
its 2-transistor direct-coupled feedback bias — plus the pickup-loading source-impedance
model the roadmap flags. The **RG100** solid-state preamp is a BjtStage/op-amp clipping
cascade. Nothing about MuffModel is special-cased into BjtStage; it is a clean building
block, exactly like TriodeStage became for the JCM800 / Twin.

### MuffModel — the 4-stage cascade

`in → ×inputDrive → [Q1 boost] → SUSTAIN pot → [Q2 clip] → [Q3 clip] → mid-scoop TONE
stack → [Q4 recovery] → VOLUME`, the whole nonlinear chain inside one 4× `Oversampler`
so the four solves + the tone stack stay sample-aligned. **The famous mid-scoop tone
stack** (`MuffToneStack`) blends a low-pass leg (`22 k / 0.01 µF`, 723 Hz) and a
high-pass leg (`12 k / 0.01 µF`, 1326 Hz) by the TONE pot; at noon the two legs sum to a
**notch ≈ 980 Hz** (the ~1 kHz scoop), TONE→0 goes dark (LP only), TONE→1 bright (HP
only). Params: slot 0 = SUSTAIN, 1 = TONE, 2 = VOLUME (the real three knobs, the shared
PedalParams shape). Output trim scales the loud recovery stage so the default (SUSTAIN 0.6
/ VOLUME 0.6) peaks ~1.2 V (the downstream limiter guards the ceiling). **Q1 is a CLEAN
input booster** (`kInputDrive 0.5`) and the SUSTAIN pot is a **full-range audio-taper
attenuator into the first clipper** (`kClipDriveMax 6.0`, decibel-linear taper down to a
−54 dB floor) — so the clipping/compression is developed by the high-gain Q2→Q3 cascade
*after* the pot, and rolling SUSTAIN down genuinely cleans up (see the §24 postmortem).

### Validation (`clipper_muff_tests`, 44.1 / 48 / 96 kHz, assert-backed)

- **Per-stage DC vs an INDEPENDENT analytic bias solve (±10% bar):** all four stages
  match to **<0.1%** — Q1/Q4 (no diodes) `Vc = 1.775 V` (`Ic 0.72 mA`), Q2/Q3 (clip)
  `Vc = 1.213 V` (`Ic 0.78 mA`, diodes lightly conducting — the cold Muff bias). The
  test re-derives the operating point with fresh code (numerical-Jacobian Newton), so
  agreement genuinely cross-validates the device model + solver.
- **Mid-scoop:** rendered response matches the analytic `H(s)` to **0.09 dB worst**;
  the noon notch sits at **980 Hz** (−9.05 dB, 2.8 dB below both shoulders); tilt
  extremes clear (TONE 1 is +16 dB brighter at 5 kHz, TONE 0 is +21 dB bassier at
  120 Hz).
- **THD RANGES with SUSTAIN (fixed — see the §24 field-fix postmortem below):** a fuzz,
  not an overdrive, but SUSTAIN is now a full-range control: **min 0.0 → ~44%, default
  0.6 → ~54%, max 1.0 → ~122%** THD (44.1/48/96 k). It RISES (min sustain sheds the wall;
  the fine curve dips slightly near the cold-bias knee, so the test asserts the coarse
  min→default→max trend). >100 % at max is canon fuzz: the ~1 kHz scoop + double clip
  suppress the fundamental below the harmonic sum. (Pre-fix it was ~80–95 % at EVERY
  setting — "no clean setting" — the bug.)
- **Wall of sustain (moved to SUSTAIN ≥ 0.6 territory):** at **SUSTAIN 0.7** a **20 dB
  input sweep** yields **2.4 dB** of output-RMS variation (20 dB in → 0.1 dB out) — the
  compression signature. The test is STRENGTHENED with the collapse: at **SUSTAIN 0.0** the
  same sweep spreads **6.2 dB** (20 dB in → 6.2 dB out) — dynamics return, the wall is gone.
- **Single-coil HUM rejection (the field fix, permanent regression guard):** with a note
  present the 60 Hz hum stays **−51 dB** below it at min sustain (bar ≤ −25 dB); a
  **−40 dBFS hum-ALONE** comes out at **−61 dBFS** at min sustain vs **−10 dBFS** at max
  (delta **51 dB** — min sustain is a real escape hatch). Pre-fix the hum-alone came out at
  **−10 dBFS at EVERY sustain** (blown up to playing level — the reported bug).
- **Aliasing (M2 bar):** shipped **4× worst-alias −103.8 dB** (44.1 k) / −104.4 dB (96 k),
  far under the −60 dB bar; naive (1×) −18.1 dB. VOLUME linear; ±10 V slam finite +
  bounded at all rates (peak ~4 V); silence→silence; deterministic.

### A/B render commands (listening pack → scratchpad)

```
# Muff (fuzz-WALL) vs RAT (hard clip), matched knobs, plucked low A:
clipper-render --gen pluck:110:2.0 muff_A.wav --pedal muff --distortion 0.7 --filter 0.5 --level 0.6 --sr 48000
clipper-render --gen pluck:110:2.0 rat_A.wav  --pedal rat  --distortion 0.7 --filter 0.5 --level 0.6 --sr 48000
#   -> Muff peak 1.39 / rms 0.38 (compressed, sustaining wall);
#      RAT  peak 0.24 / rms 0.13 (hard clip pins near the knee, decays with the pluck).
# TONE scoop sweep (rms drops at the noon scoop as the ~1 kHz notch removes energy):
#   tone 0 -> rms 0.76 (dark) ; tone 0.5 -> rms 0.46 (SCOOPED) ; tone 1 -> rms 0.59 (bright).
# The Gilmour move — Muff into a CLEAN Twin with headroom (two-step: muff, then --twin):
clipper-render --gen pluck:110:2.5 muff_pre.wav --pedal muff --distortion 0.75 --filter 0.55 --level 0.6 --sr 48000
clipper-render muff_pre.wav muff_into_twin.wav --twin --twin-volume 0.35 --twin-reverb 0.25 --twin-cab --sr 48000
```

### Integration (additive everywhere)

- **One param shape, additive registries.** SUSTAIN/TONE/VOLUME ride the shared
  `PedalParams {distortion, filter, level}`; each shared registry gains exactly one entry
  (`rig.ts` `PedalType` + gear tray + `MUFF_KNOB_DEFAULTS` sustain 0.6 / tone 0.5 /
  volume 0.6; worklet `_muff` prefix; C ABI `muff_*`; `add_pedal` enum + coaching;
  `Pedal.tsx` FACES + a NEW `wide` layout; `tokens.css` `--accent-muff`). The worklet
  routes per node by C-ABI prefix (`_rat`/`_sd`/`_ts`/`_muff`/`_phaser`).
- **Visual identity (doctrine §17).** Dark chassis (both themes), **VIOLET accent** (the
  "violet era" wink). Its own **`wide` face** — the Muff is physically HUGE, so a broader
  enclosure with the three knobs in the classic TRIANGLE (SUSTAIN top-left, VOLUME
  top-right, TONE centered below) and a big round stomp low-center. Hero wordmark **"Pi"**
  large and central, model line `FUZZ Nº5 · PI`. No trademarks.
- **Assistant.** `add_pedal` gains `'muff'`; the coach knows it is FUZZ not overdrive
  (thick, saturated, endlessly-sustaining), that the TONE knob is a mid-SCOOP (band-mix
  caveat: add mids elsewhere if it gets buried), and the classic move — a Muff into a
  CLEAN amp with headroom, e.g. the Twin.
- **Build note.** `build-wasm.sh` compiles `BjtStage.cpp` + `MuffModel.cpp` and exports
  `_muff_*`; the two duplicate `EXPORTED_FUNCTIONS` lines left by the phaser/ts merge were
  consolidated into ONE complete list (rat/sd/ts/muff/phaser + amp) so every pedal's
  exports actually ship. Similarly the render CLI's stray empty `else if (a.pedal=="sd1")`
  merge artifact was folded into the `sd1||ts` branch.
- All 12 native suites green (`ctest`: M0 + RAT + SD-1 + TS + phaser + amp + triode +
  JCM800 + JCM power + Twin + **Muff** + AC30); web tsc + vite build; Playwright (`muff
  worklet: fuzz makes massive harmonics + a compressed sustain wall` — now also asserting
  the min-sustain wall COLLAPSE — and `assistant: add_pedal adds a Muff Pi fuzz
  (round-trip)`).

### Field-fix postmortem — "the Pi blows out my single-coil hum, even at min sustain"

**Report.** A player on a single-coil guitar: *"the Pi makes even just my single-coil hum
blown out, even with minimum sustain."*

**Diagnosis (measured).** Two compounding bugs, both in `MuffModel.cpp`'s level/knob
mapping (the device models + tone stack were correct and are unchanged):

1. **Input booster Q1 was overdriven.** `kInputDrive = 12.0` drove a guitar-level signal —
   and even a −40 dBFS hum (0.007 V → 0.084 V at Q1's base) — into Q1's rail-clip region.
   Q1 (no diodes, gain ≈ 19×, clean only below ~0.05 V in) put out a **7 V rail-clipped
   square for essentially any input**. Because the SUSTAIN pot sits *after* Q1, no sustain
   setting could undo that: the noise floor was already compressed up before the pot.
   Measured Q1 THD: 15 % at −30 dBFS in, **116 % at −10 dBFS**.
2. **SUSTAIN taper had a hot floor and the wrong law.** The pot was a **linear** map with a
   **0.06 (−24 dB) floor** — so "min sustain" still passed −24 dB of a 7 V square into the
   clippers. A real Big-Muff SUSTAIN is a 100 kA (audio) pot wired as a full-range input
   attenuator that nearly grounds the clipper input at minimum.

   Net effect (pre-fix, measured, 48 k): a **−40 dBFS hum-alone came out at −10.7 dBFS at
   EVERY sustain** (min −10.7, max −11.8 — a **1 dB** difference; min sustain was useless),
   and THD sat at ~80–95 % at all settings. **Hypothesis 3 (LF/hum filter) was checked and
   REJECTED:** the canon `Cin = 100 nF` coupling already gives 60 Hz **−4.5 dB/stage** vs
   midband (corner ~130 Hz), compounding across four stages — the hum was never
   under-filtered, it was *clipped and compressed up*. No fantasy hum filter was added.

**Fix (authentic gain structure).** Q1 is restored to a **clean booster** and the clipping
drive is moved *after* the pot, so the SUSTAIN pot is a true full-range attenuator into the
high-gain Q2→Q3 cascade (which is where a real Muff's compression is developed):

| constant | was | now | why |
|---|---|---|---|
| `kInputDrive` (pre-Q1) | 12.0 | **0.5** | keep Q1 linear for hum/soft picking (clips only on loud playing) |
| SUSTAIN taper | linear, `0.06 + 0.94·knob` | **decibel-linear audio** `kClipDriveMax·10^((−54/20)(1−knob))` | honest 100 kA pot; knob 0 ≈ −54 dB (≈0.012), knob 1 = `kClipDriveMax` |
| clip drive @ max (`kClipDriveMax`) | — (was just ×1) | **6.0** | develop the wall in Q2→Q3 *after* the pot |
| `kOutputTrim` | 0.32 | **0.40** | default (0.6/0.6) peaks ~1.2 V |

**Per-stage drive @ SUSTAIN 0 (0.2 V hot input, 48 k):** Q1 out stays a clean ~2 V; the pot
attenuates to ≈1.2 % of max, so Q2 sees only tens of mV and the Q2→Q3 cascade stays in its
near-linear region → dynamics track the input. At SUSTAIN 1 the same cascade sees several
volts and slams into the wall.

**Result (measured, all rates).** THD now **ranges** 44 % (min) → 54 % (default) → 122 %
(max). The wall lives at SUSTAIN ≥ 0.6 (0.7: 20 dB in → 0.1 dB out) and **collapses** at 0
(20 dB in → 6.2 dB out). The **hum-alone drops from −10.7 dBFS to −61.1 dBFS at min sustain**
(delta to max now **51 dB** — min sustain is the escape hatch); with a note present the
60 Hz hum sits −51 dB below it. Aliasing at max improved to −103.8 dB. A/B renders in the
listening pack confirm min-sustain hum-alone rms **0.293 → 0.001** (−50 dB) while the
max-sustain fuzz keeps its thick, harmonic-dense sustaining character (H3/f1 ≈ 0.2 → 0.26,
still a wall). The permanent guard is `testHumRejection` (the player's exact signal) plus
the strengthened `testSustainRange` / `testSustainWall` and the web worklet spec's
min-sustain collapse assertion.

## 25. Performance — denormal guard, tube-solver pass, per-gear cost, load meter

The field report behind this milestone: **audio lag/crackle on the web app (and the
Mac build), even on the clean JC-120 path** — "random", periodic, independent of
which amp or pedal is loaded. Two real causes were found and fixed, and the
milestone adds the instrumentation to keep perf honest from here on: a per-unit
cost benchmark (`clipper-bench`), a permanent solver-accuracy regression test, and
a DSP load meter in the web UI.

### 25.1 The denormal cliff (the amp-independent crackle)

**Diagnosis.** A recursive float filter state fed a signal that decays to silence
rings down through the **subnormal** float range (magnitudes below ~1.18e-38).
Arithmetic on subnormals takes a microcoded slow path on real CPUs — and while
native code *could* enable hardware flush-to-zero (FTZ/DAZ), we never did, and
**WASM has no FTZ at all** (the runtime cannot be asked to flush). So every
decaying note tail turned into thousands of slow-path ops on the audio thread.
Worse, a one-pole smoother approaching a **zero** target (bright switch off,
chorus depth 0) never *leaves* the subnormal range — it asymptotes and sticks
there, a **permanent** denormal generator. Every amp and pedal shares these
primitives (`Biquad`, `OnePoleSmoother`), which is exactly why the lag was
independent of the loaded gear.

Measured on the dev container (x86-64, which honors subnormals the same way a
browser's WASM JIT must): an **unguarded** TDF2 biquad running a silent tail is
**24.6× slower** in the default FP environment than with hardware FTZ forced on
(`scripts/denormal_bench.cpp` pattern; 40×10 s tails). That is the cliff the web
app was falling off.

**Fix.** `core/include/clipper/dsp/Denormal.h` — a branchless
`flushDenormal(v)` (float + double overloads): any recursive state whose
magnitude falls below **1e-30 (−600 dB)** is snapped to exactly 0. The floor is
~8 orders of magnitude above the largest subnormal and ~10 below the reverb
loop's long-standing `1e-20` anti-denormal offset (zero interaction), and −456 dB
below the 24-bit noise floor — bit-irrelevant to audio. Applied after **every**
decaying recursive update in the core (audited module by module):

- `Biquad` z1/z2 (every tone stack / shelf / peak in every amp),
- `OnePoleSmoother` (snaps to the target once the residual is sub-floor — kills
  the stuck-at-zero-target generator),
- `PhaserModel` allpass memory (double), `RatModel` shape/LP one-poles,
  `OverdriveEngine` mid/tone one-poles (SD-1 + Screamer), `MuffToneStack` legs,
  `LM308Stage` closed-loop LP, `OptoTremolo` depth smoother (double).

Not guarded, by analysis: `ReverbModel` (its 1e-20 loop offset already prevents
subnormals), chorus delay lines / halfband FIRs / convolver overlap-add (input
history, no feedback — state dies with the input), tube-stage companion caps
(doubles parked at nonzero DC operating points), `OutputLimiter` gain (double
near 1.0).

**Proof.** `clipper_denormal_tests` (ctest #13): the flush invariant; silent-tail
runs over the exact AmpModel voicing filters asserting **no output sample is ever
subnormal and the tail reaches exactly 0**; smoother snap/converge cases; and
**bit-transparency** — the guarded `Biquad`/`OnePoleSmoother` are compared
sample-for-sample against verbatim *unguarded* recurrences over 2 s of
program-level audio and must match **bit-for-bit** (the guard only acts below
−600 dB). With the guard in place `denormal_bench` shows the default FP
environment reaching the hardware-FTZ ceiling (ratio 1.0–1.1×) — the cliff is
gone *in code*, which is the only fix available under WASM.

### 25.2 The tube-solver pass (same equations, same roots, fewer evaluations)

The valve amps dominate CPU: per-sample Newton solves at 4× oversampling. The
pass speeds them up **without touching the circuit equations or the converged
solutions** — no lookup tables, no waveshaper stand-ins:

- **Pentode plate-load Newton** (EL34 / 6L6 / EL84): the Koren law factors as
  `Ip = base·atan(Vp/kvb)` with `base = (2/kg1)·E1^ex` depending only on the
  grids — which are **fixed** during the plate solve. `base` (the softplus+pow)
  is hoisted out of the loop and `dIp/dVp = base/(kvb·(1+(Vp/kvb)²))` is exact,
  so each iteration costs **one `atan`** where it used to cost **three full
  pentode evaluations** (the central-difference derivative is gone). Same
  residual, same exit tolerance, same root.
- **Screen currents reuse the hoisted base**: `Ig2 = base·(kg1/kg2)` — one more
  softplus+pow gone per output-tube leg per oversampled sample.
- **Grid-node solves warm-start** from the previous sample's solution (they used
  to restart from the idle bias every sample).
- **Triode/LTP Koren evals**: `dIp/dE1 = ex·Ip/E1` — algebraically identical to
  the second `pow`, computed from the first.
- **Sparse 3×3 solves**: the TriodeStage and LTP Jacobians have fixed zero
  entries (the grid row doesn't see the plate; each PI plate row couples only
  its own plate + the shared tail), so both systems solve by direct elimination
  instead of general Cramer with column copies.
- **BjtStage (Muff) line search**: trial evaluations now fill the Jacobian too —
  nearly free once the Ebers-Moll/diode exponentials are computed — removing the
  separate refresh evaluation per damped-Newton iteration. The Newton path
  (every iterate, every accepted step) is **bit-identical** to before.

**The accuracy gate is permanent.** `TubeSolverMode.h` exposes a test-only
tolerance scale; `clipper_tube_solver_tests` (ctest #14) renders a riff (plucked
fundamentals + harmonics, attacks, clipping, decaying tails) through each valve
voice at production tolerances and at **1000×-tighter reference tolerances** and
asserts the two renders agree below **−120 dBFS**. Measured residuals: jcm800
**−258.9 dBFS**, twin **−258.9 dBFS**, ac30 **−162.6 dBFS**. A 3 s sweep render
per amp before/after the whole pass agrees to −252.9 / −186.6 / −258.5 dBFS
(jcm/twin/ac30) — inaudible by ~10 orders of magnitude, honestly *not* bit-exact
(the roots are approached along different iterates), which is why the JUCE
identical-core test still passes: plugin and raw engine share the same updated
core, so both sides moved identically.

**Measured speedup** (native `clipper-bench`, 8 s riff, 48 kHz / 128-frame
blocks, one Linux x86-64 container — treat as relative):

| valve unit | before | after | speedup |
|---|---|---|---|
| jcm800 | 1.45× RT (68.8 % CPU) | 1.96× RT (51.1 %) | **1.35×** |
| twin   | 2.04× RT (49.0 %)     | 2.90× RT (34.5 %) | **1.42×** |
| ac30   | 2.25× RT (44.4 %)     | 3.80× RT (26.3 %) | **1.69×** |
| muff   | 3.41× RT (29.4 %)     | 4.10× RT (24.4 %) | **1.20×** |

WASM runs the same source ~1.4–2× slower than native, so the pre-pass WASM
margin of ~1.1–1.4× realtime on the valve amps is what made every scheduling
hiccup audible; the pass converts that into real headroom.

### 25.3 Per-gear CPU cost (the honest table)

`core/tools/bench/main.cpp` (`clipper-bench`, native-only target): every unit
processes the same deterministic riff at 48 kHz in 128-frame blocks after a
warm-up pass; the table reports audio-seconds-per-wall-second (× realtime) and
the share of one realtime stream. Native numbers are a **proxy** for WASM (same
core source); the *ranking* is the point. Post-pass numbers:

| unit | × realtime | % of one 48 k stream | × faster after §32 |
|---|---|---|---|
| jcm800 (valve amp) | 1.96× | 51.1 % | 1.14× |
| twin (valve amp) | 2.90× | 34.5 % | 1.14× |
| ac30 (valve amp) | 3.80× | 26.3 % | 1.12× |
| muff (BJT fuzz) | 4.94× | 20.3 % † | 1.06× |
| rat (dist pedal) | 35.2× | 2.8 % | 1.80× |
| sd1 / screamer (overdrive) | ~43× | 2.3 % | ~2.17× |
| gold (clean-blend drive) | — | — | 1.80× |
| clean amp + chorus + reverb (stereo) | 89.4× | 1.1 % | 1.00× |
| spring reverb alone (mix 0.5) | 114.6× | 0.9 % | 1.00× |
| ninety (phaser) | 298.7× | 0.3 % | 1.00× |
| cab convolver (either IR) | ~460× | 0.2 % | 1.00× |
| clean amp alone (JC-120, mono) | 1403.6× | 0.07 % | 1.00× |
| output limiter | 1677.3× | 0.06 % | 1.00× |
| oversampler alone, 4× (up+down) | — | — | 2.72× |

† **muff row updated 2026-07-25** by the residual early-out (§34, audit finding 12).
Measured interleaved against the pre-fix solver on the same idle machine in the same
session, three runs each: **3.81–3.93× / 25.4–26.2 % → 4.78–4.94× / 20.3–20.9 %**.
The row previously read 4.10× / 24.4 %, measured on different hardware — so take the
before→after *pair* as the result and the absolute figure as machine-dependent. Note
this bench riff decays to silence between plucks, so it under-reports the fix: the
headline win is on **silence**, where the same change is ~13.7× (§34).

Reading it: the **valve amps are the budget** — a valve head plus a dirt pedal,
cab, and reverb is dominated by the head alone; everything linear is noise. The
denormal guard is invisible here by design (it costs a compare+select per state
update) — its payoff is the *absence* of the tail-triggered spikes that no
steady-state benchmark shows.

**The absolute columns predate §32** (the halfband doubled-ring change) and were
taken on a different machine, so they are not directly comparable with anything
`clipper-bench` prints today — which is why the fourth column is a *ratio*: the
same-machine, same-binary-flags before → after multiplier measured in §32, where
the full table (including the new `os2x` / `os4x` / `os8x` resampler-only units and
the % -of-one-stream figures both before and after) lives. Anything at 1.00× is a
control: it contains no oversampled nonlinearity, so it must not have moved, and it
did not.

### 25.4 The web DSP load meter (ground truth on the user's machine)

The web app now reports the audio thread's real headroom instead of guessing:
`audio.ts` wires **`AudioContext.renderCapacity`** (Chromium 116+) and forwards
its ~1 Hz `update` events — `averageLoad` / `peakLoad` / `underrunRatio`, the
fraction of each render quantum's deadline actually spent — to a new **Engine
load** row in the status panel (`DSP 23% · peak 41%`). States: normal ink;
**warn** (amber) above 80 % average — headroom running out, the pre-glitch
signal; **underrun** (red LED glow) when `underrunRatio > 0` — a real dropout,
the objective face of "crackle/lag", also logged loudly to the console so field
reports can paste it back. Feature-detected: browsers without the API get an
honest `n/a (needs Chromium)` rather than a fake 0 %; the meter stops with the
engine and never adds render churn (1 Hz state updates only). The readout lives
in the existing neumorphic status card (`.status` dl), styled through the stock
`--led*` tokens — no new visual language.

- Verification for the milestone: core ctest **14/14** (12 prior + denormal +
  tube-solver regression); the JUCE **identical-core** test green across all
  four amp voices; `web` tsc + vite build green; WASM rebuilt via
  `scripts/build-wasm.sh` with the single consolidated `EXPORTED_FUNCTIONS`
  list intact.

## 26. M11 — Player Expectations Suite (test the player's expectations, not just the circuit)

**Why this milestone exists.** Four field bugs shipped while every circuit metric
was green — each caught only by a player's ears:

1. **RAT "no balls"** — harmonic ratios, alias floors, bit-exactness all passed,
   but input-level calibration + an over-aggressive low shelf left the pedal
   gutless at the levels a real player feeds it (fixed in M6.1).
2. **Cab fizz** — "correct" convolution of a wrong-sounding IR (a −22.9 dB noise
   tail + 2 dB response steps), plus an IN-PLACE aliasing hazard: the web worklet
   calls the convolver with `out == in`, and the null test used separate buffers.
3. **Muff Pi hum blowout** — the input stage rail-clipped even a −40 dBFS
   single-coil hum and min sustain didn't help; no THD/spectrum test ever probed
   MIN-knob settings or a realistic noise floor (§24 postmortem).
4. **AC30 "muddy"** — the CUT knob's control-law default opened the amp 3 dB
   dark. Circuit right, knob taper wrong (§23 re-taper + character guard).

The pattern: **we tested the circuit, not the player's expectations.** M11 is the
missing layer — a permanent suite that drives every piece of gear the way a
player does (default knobs, min knobs, realistic levels, realistic noise, the
worklet's exact calling conventions) and pins what they must hear.
Files: `core/tests/test_player_expectations.cpp` (`clipper_player_expectations_tests`,
ctest #15 — links `clipper_c_api` because it drives the worklet's exact C ABI),
goldens under `core/tests/goldens/`, `scripts/update-goldens.sh`, and
`web/tests/expectations.spec.ts` (Playwright). All core measurements run at
**48 kHz** — the AudioWorklet's fixed rate, where every one of the four bugs was
heard (per-rate circuit behavior stays pinned by the per-gear suites).

### Block A — universal gear invariants (one harness, EVERY gear)

One parameterized harness over every dirt pedal (`rat`/`sd1`/`ts`/`muff`) and
every amp voice (`clean120`/`jcm800`/`twin`/`ac30`), at the app's exact opening
knobs (`rig.ts` defaults), with smoothers settled 0.25 s before measuring.
Standard signal: a 220 Hz pluck peaking at −20 dBFS (RMS −33.5 dBFS).

- **A1 Min-knob usability** (the Muff bug generalized): at defaults AND with any
  single knob at minimum, output must be finite, bounded (pedals < 2 V peak —
  the muff legitimately peaks 1.69 V dark; amps < 1.05 normalized) and, unless
  the knob is an output-level pot, audible (RMS > **−70 dBFS**). Level pots must
  genuinely attenuate (≥ 6 dB below default; every one measured a clean kill,
  −240 dBFS). Quietest legit min-knob case: **ac30 bass=0 at −64.1 dBFS** — the
  passive Vox top-boost stack guts a low-heavy pluck with BASS at zero
  (authentic; the floor separates "authentically quiet" from "silenced").
- **A2 Hum torture standard** (the Pi's field signal, now a floor for ALL future
  gear): 220 Hz note at −10 dBFS + 60 Hz hum at −40 dBFS through every dirt
  pedal at min AND default gain — the 60 Hz component must stay **≥ 28 dB below
  the note**. Measured: rat −36.0/−41.6, sd1 −33.1/−39.3, ts −33.1/−38.2,
  muff −51.1/−55.8 dB (min/default). The input hum sits 30 dB below the note and
  a perfectly linear pedal preserves that, so ~−33 dB at min drive is the
  physical ceiling — the 28 dB bar leaves 5.1 dB margin on today's tightest gear
  while catching a Pi-class blowout (pre-fix: hum at ~−1 dB) outright.
- **A3 Knob monotonicity spot-checks**: gain/drive/sustain at lo/mid/hi must
  yield non-decreasing THD (rat 0→22→37 %, sd1 0→12→37 %, ts 0→6→32 %, muff
  36→38→147 % at 0/0.6/1.0 — §24's coarse-trend contract, jcm800 gain
  0→11→47 %); level pots non-decreasing RMS; tone knobs at extremes must move
  the spectrum the documented direction — pedals via high-band harmonic energy
  (≥ 6 dB; a plain spectral centroid is fundamental-dominated and barely moves),
  amps via the 3 kHz band (≥ 4 dB), including the two INVERTED knobs (RAT
  FILTER: clockwise darkens; AC30 CUT: clockwise darkens).
- **A4 Level sanity** (the "no balls" / "blows the mix apart" guard): output RMS
  delta at defaults must sit inside a documented per-gear window (measured
  ± ~10 dB). Measured: rat +18.6, sd1 +15.6, ts +13.1, muff **+29.4** (the
  sustain wall lifts a DECAYING pluck's RMS by design — a fuzz that didn't would
  be the bug), clean120 −6.0, jcm800 +1.7, twin −13.8, ac30 −11.7 dB. One
  symmetric global bound can't hold that honest spread, so the windows are per
  gear — tight enough that a ±10 dB voicing drift fails loudly.

### Block B — live-convention testing (the test that would have caught the convolver bug)

Every processing entry point is rendered TWICE through fresh instances: once the
way tests find convenient (separate in/out buffers, one big call), once the way
the WORKLET actually calls it (**in-place, `out == in`, 128-frame blocks,
48 kHz**), and the two must agree within float tolerance (2e-5). Covered: the
five pedal C ABIs (`rat_`/`sd_`/`ts_`/`muff_`/`phaser_`), the AmpChain C ABI for
all four voices (cab ON + reverb 0.25 so the spring runs in-place too), the
stereo amp path with chorus ON (128-frame vs big-block; `in` may not alias the
outputs, per the documented contract), and the bare `CabConvolver` +
`ReverbModel`. Measured today: **bit-identical** (max |Δ| = 0) on every path —
the suite pins that so an in-place aliasing bug can never ship silently again.

### Block C — golden "first five minutes" renders

The rigs a new player actually tries first, at DEFAULT knobs, rendered through
the same C-ABI chain the worklet drives, committed as 16-bit mono 48 kHz WAVs
(2 s pluck each, 944 KB total, `core/tests/goldens/`):

| golden | rig | peak / RMS |
|---|---|---|
| `rat_jcm800.wav` | RAT → JCM800 + brit412 cab | 0.237 / −23.8 dBFS |
| `sd1_twin_reverb.wav` | SD-1 → Twin + clean212 + reverb 0.25 | 0.144 / −35.7 dBFS |
| `muff_twin.wav` | Muff → CLEAN Twin + clean212 (the Gilmour move) | 0.178 / −26.4 dBFS |
| `ts_ac30.wav` | Screamer → AC30 + clean212 | 0.063 / −39.3 dBFS |
| `clean120_chorus.wav` | clean 120 + chorus ON + clean212 (stereo → mono mix) | 0.038 / −47.1 dBFS |

Gate: **per-third-octave-band RMS difference ≤ 1.5 dB** (bands within 55 dB of
the golden's loudest band; 7–13 live bands per rig) plus broadband RMS ± 1 dB —
perceptual-ish, so lossless refactors (solver iteration changes, chunking) pass
but voicing drift (a re-tapered knob, a re-voiced shelf, a changed IR) fails.
The gate's own floor (16-bit quantization + windowing) measures 0.11 dB worst.
Regenerating goldens is a DELIBERATE act only:
`./build/clipper_player_expectations_tests --update-goldens` (or
`scripts/update-goldens.sh`), then commit the diff with a justification.

### Block D — web "first five minutes" sim (`web/tests/expectations.spec.ts`)

What a new player does, in the real browser engine: add each dirt pedal at its
opening defaults onto the default rig, stomp it on, play a −20 dBFS note; cycle
all four amp voices at defaults. Asserts per render: (1) audible non-silent
output, (2) no NaN, (3) RMS inside the documented window (dirt pedals
−32..−4 dBFS through the default clean rig; amp voices −42..−6 dBFS), (4)
stomping on/off mid-note does not click (max sample delta bounded by ~2× the
steady-state slew).

**Latent bug found and fixed by (4): the worklet's STOMP was the one topology
change NOT declick-bracketed.** `bypass` messages flipped `node.engaged` (and
amp power) instantly between quanta, stepping the output mid-waveform — a clean
−20 dBFS tone and a driven pedal output differ by hundreds of millivolts at the
flip sample, i.e. an audible pop on every stomp. Chain edits, cab swaps, and
amp-model swaps were all already staged at the raised-cosine declick's fade-out
zero; M11 routes stomps (pedal bypass + amp power) through the SAME bracket
(`_pendingBypass` / `_pendingAmpBypass` in `clipper-processor.js`, applied in
`_commitPending`). Pre-ready stomps still apply immediately (no audio running —
nothing can click), and the synchronous `latency` echo (the tests' delivery
barrier) is preserved.

### Findings ledger (what the new invariants surfaced)

- **Worklet stomp click** — real, fixed (above).
- **AC30 BASS at min is very quiet** (−64.1 dBFS for the standard pluck, ~17 dB
  under its defaults) — authentic passive-stack behavior, documented as the
  audible-floor anchor, not a bug.
- **In-place/live-convention rendering is currently bit-exact everywhere** —
  the convolver-class hazard exists only if code changes; now pinned.
- **Everything else passed on first measurement** — the four shipped field
  fixes (M6.1, M6.5/M6.6, §24, §23) hold with margin under their generalized
  invariants.

- Verification for the milestone: core ctest **15/15** (14 prior + player
  expectations); `web` tsc green; Playwright suite green with the repo config
  (including the four new expectations tests); no DSP source touched (the WASM
  module is unchanged — only the authored worklet JS changed, re-copied to
  `web/public/generated/` exactly as `build-wasm.sh` does).

## 27. v1.1 item 6 — Klon Centaur "Myth": the parallel clean/dirt blend (germanium WDF)

ROADMAP v1.1's famous omission, made real: the gold-boxed "transparent"
overdrive whose whole reputation comes from an architecture no other pedal in
this app has — it does **not** run your signal through a clipper. It **splits**
it: a full-bandwidth CLEAN path and a germanium-clipped one, cross-faded by a
**dual-ganged GAIN pot**. That is why it measures transparent at low gain and
still sounds articulate when pushed, and it is why this is a bespoke model
instead of another `OverdriveEngine` config (the TS-family engine expresses a
feedback clipper with a clean *pedestal*, not a *crossfade*). Files:
`core/include/clipper/dsp/GoldModel.h` + `core/src/dsp/GoldModel.cpp`; tests
`core/tests/test_gold_model.cpp` (`clipper_gold_tests`); C ABI `gold_*` in
`clipper_c_api.cpp`; `--pedal gold` (+ `--gold-silicon` / `--gold-no-clean`) in
the render CLI; a `gold` bench unit; worklet `gold` dispatch; `rig.ts` / Pedal /
gear-tray / assistant wiring; M11 harness entry (blocks A + B) and the web sim.
Trademark-safe throughout: wordmark **"Myth"**, model line **`DRIVE Nº6 · GOLD`**,
and **no** Klon / Centaur / KTR text and **no centaur-and-rider figure** anywhere
on a user surface — that figure is the actual mark; the colour is not.

### Sources — and an honest gap

The intended reference is Jatin Chowdhury, *"A Comparison of Virtual Analog
Modelling Techniques for Desktop and Embedded Implementations"* (arXiv:2009.02833),
which models this exact pedal section-by-section with nodal analysis and Wave
Digital Filters — the same author whose `chowdsp_wdf` we vendor. **The PDF was not
reachable from this environment**: the egress proxy refuses `arxiv.org` and
`ccrma.stanford.edu` (403 on CONNECT), as it did every mirror tried. So this model
follows the paper's **method** (a section per circuit block; WDF for the diode
root; analytic/nodal transfer functions for the linear blocks) while its
**component values** come from the widely published reverse-engineered schematic
and are each flagged as an approximation in `GoldModel.cpp`. The topology is the
claim; the digits are correctable. When the paper becomes reachable, diff its
values against the constants block at the top of that file — nothing else moves.

### The circuit → the model

- **Section 1 — the always-on input buffer.** This pedal buffers even when it is
  switched out, which is why it is famous as a line driver. Modelled as the unity
  op-amp follower it is, carrying only the input network's DC-blocking corner:
  `f_in = 1/(2π·1 MΩ·0.022 µF) ≈ 7.2 Hz`. *Documented omission:* the follower's own
  ~3 MHz corner is far above audio and is not modelled.
- **Section 2 — the ganged GAIN pot (the soul).** One knob, three mapped
  quantities, summed as
  `out = 2·( cleanBlend(g)·x + clipBlend(g)·clip(A(g)·HP₁₀₆(x)) )`:
  - **the crossfade** — `cleanBlend(g) = 1 − 0.55·g` (1.00 → **0.45**: the clean core
    never leaves) against `clipBlend(g) = g` (a linear wiper on the clipped
    signal). At GAIN 0 the clipped half is **switched out**, so the box is a clean
    buffer/boost — not "quiet dirt", *no* dirt. *Approximation:* the real network's
    second wiper law is more interactive than a straight line.
  - **the drive amp** — the other gang in the feedback leg: `A = 1 + g·P/R_g` with
    `P = 100 kΩ` (dual-ganged) and `R_g = 1.5 kΩ`, so `A(0) = 1.00` … `A(1) = 67.67×`
    (**+36.6 dB**). Unlike the TS family, minimum gain is *exactly* unity.
  - **the lows skip the clipper** — the drive path is high-passed *before* the
    diodes at `1/(2π·15 kΩ·0.1 µF) ≈ 106 Hz`, so the bottom end reaches the summing
    node only through the clean half. This is the "it never gets mushy" trait, and
    it is also why the pedal's hum figure *improves* as you turn up.
  - **the op-amp** — TL07x-class (3 MHz GBW, 13 V/µs), via the house op-amp model
    (`LM308Stage.h`, generic despite its name). Closed-loop corner at max gain
    `3e6/67.67 ≈ 44 kHz` — above the band, so unlike the RAT's LM308 (which collapses
    to ~500 Hz) it colours nothing; it is here for honesty and antialiasing.
  - **the germanium pair (WDF)** — antiparallel 1N34A-class diodes, driven through
    the stage's output resistance and shunted by its small cap, built exactly as the
    RAT's silicon clipper is: `R_s = 2.2 kΩ`, `C_p = 4.7 nF` (HF corner ≈ 15.4 kHz),
    root `chowdsp::wdft::DiodePairT`. Device: `Is = 200 nA`, ideality `n = 1.3`
    (carried through the library's `nDiodes` multiplier, i.e. `Vt_eff = n·Vt =
    33.6 mV`), knee ≈ **0.29 V** at 1 mA — about half silicon's, and far more
    importantly a **much softer** knee. `DIODE_SILICON` (1N914-class, `Is = 2.52 nA`,
    `n = 1.0`) exists as a MEASUREMENT-ONLY counterfactual.
  - **the charge pump** — the real box generates a negative rail so its op-amps run
    on ±9 V. Here that is a **headroom statement**: the summing node and the tone
    stage carry a ±8.6 V clamp that, at any playing level, never engages. The
    germanium pair is the only thing in the box that clips — and the suite asserts
    it (1 V in, wide open, peaks 1.60 V).
- **Section 3 — TREBLE.** A shelving tilt about a ~1 kHz pivot, ±12 dB, exactly flat
  at noon, **normal sense: clockwise BRIGHTENS** (contrast the RAT's FILTER and the
  AC30's CUT, both inverted). *Approximation:* the hardware's active tone network
  interacts slightly with the output pot; this is a first-order stand-in with the
  same pivot and range.
- **Section 4 — output stage.** Output buffer + OUTPUT pot (identity linear map,
  the house convention) + the coupling cap's 8 Hz DC block. **OUTPUT at noon is
  exactly unity** (the summing amp's ×2 against a 0.5 pot) — the pedal's calibration
  point, and the reason "GAIN 0 / OUTPUT 50" is a true bypass-alike.

Parameter mapping (normalized knobs, one-pole smoothed ~5 ms as everywhere):

| Param (id) | Knob 0 | Knob 1 | Mapping | Notes |
|---|---|---|---|---|
| `PARAM_GAIN` (0) | clean only (A = 1.00, clip weight 0) | A = 67.67× (+36.6 dB), clip weight 1 | the DUAL-GANGED pot: `A = 1 + g·100k/1.5k`, `clean = 1 − 0.55g`, `clip = g` | one knob, a crossfade — not an amount |
| `PARAM_TREBLE` (1) | −12 dB HF | +12 dB HF | tilt about a 1 kHz pivot | flat at 0.5; clockwise BRIGHTENS (normal sense) |
| `PARAM_OUTPUT` (2) | 0.0 | 1.0 | identity linear gain | **0.5 == unity** (the ×2 summing amp) |

### Validation (ctest `clipper_gold_tests`, 44.1 k + 48 k + 96 k, assert-backed)

- **Transparency at GAIN 0 (the whole reputation, as a number):** response flat
  within **0.138 dB** across 60 Hz–10 kHz (worst at 60 Hz — the coupling corners),
  THD **0.0000 %** on a hot 0.3 V note, and **0.000 dB** (unity) at OUTPUT noon. A
  1 V input at GAIN 0 still measures **0.0001 %** THD: the charge-pump headroom,
  audible as "it doesn't do anything to my sound".
- **The ganged crossfade:** THD **0.00 → 11.66 → 20.60 → 26.31 → 31.73 %** across GAIN
  0/0.25/0.5/0.75/1.0 (44.1 k), with the **clipped share** of the output RMS (measured
  against the clean-half-off counterfactual) rising **0.00 → 0.37 → 0.61 → 0.76 → 0.85**
  — a genuine progressive blend, with the clean core still 0.45 at max.
- **Germanium soft knee, distinguished from silicon:** identical circuit, identical
  drive, only the diode parameters change. Over a 20 dB input sweep at a low gain
  (A = 7.7) the germanium goes **0.22 % → 5.16 % (×23)** while the silicon
  counterfactual goes **0.01 % → 6.15 % (×736)**: germanium bends **~26× earlier** and
  **~30× more gently**. That ratio *is* the "bloom" players describe, and it is the
  test that catches someone quietly swapping in a silicon knee.
- **TREBLE (normal sense):** 6 kHz **−9.42 dB** (dark) → **+11.43 dB** (bright) with the
  low end unmoved (**+0.71 dB** at 120 Hz); on the harmonics the pedal itself makes,
  high-band energy **−17.9 → −5.7 dB** (M11's metric; bar ≥ +6 dB).
- **Aliasing (M2 bar):** shipped **4× worst-alias −93.3 dB** (44.1 k) / **−123.6 dB**
  (96 k) at max GAIN with the brightest treble, far under the −60 dB bar; 1× is
  −20.2 dB and 8× buys −112.1 dB, i.e. nothing audible over 4× — **4× ships**.
- **Headroom + OUTPUT:** 1 V in, wide open → peak **1.60 V** against 8.6 V rails (the
  clamps never engage); OUTPUT linear to **2.000×** per doubling.
- **Hygiene:** ±10 V slam finite and bounded (peak 9.07 V — the rails, as designed),
  silence → silence, deterministic, finite across the knob grid, all three rates.

### M11 conformance (docs §26, additive to the pedal tables only)

- **A1 min-knob:** defaults **−22.0 dBFS** RMS / peak 0.273; `gain=0` **−32.3 dBFS**
  (audible — a clean pedal, not a silenced one), `treble=0` −22.3, `output=0` a
  clean kill (−240).
- **A2 hum torture:** 60 Hz sits **−30.1 dB** below the note at min gain and
  **−32.9 dB** at default (bar −28). This is the **tightest margin of any gear in the
  suite, and necessarily so**: at GAIN 0 this pedal is *linear*, so it can only
  preserve the input's own −30 dB hum-to-note ratio — −30.1 dB *is* the physical
  ceiling for a transparent pedal. The 2.8 dB improvement at default gain comes from
  the 106 Hz pre-clip high-pass keeping hum out of the clipper.
- **A3 monotonicity:** GAIN THD **0.0 → 23.4 → 30.6 %**; OUTPUT −240 → −17.1 → −11.0 dBFS;
  TREBLE HF harmonics −17.9 → −5.7 dB in the documented (clockwise-brightens) direction.
- **A4 level sanity:** **+13.2 dB** RMS delta at defaults (window +3…+23) — between the
  Screamer (+13.1) and the SD-1 (+15.6), which is exactly where a mostly-clean blend
  with a ×2 summing amp belongs.
- **Block B:** `gold_ ABI` renders **bit-identically** (max |Δ| 0.000e+00) in-place at
  128 frames vs separate buffers in one block.
- **Block D (web sim):** **−22.8 dBFS** at defaults through the default clean rig, peak
  0.106 — the quietest of the five dirt boxes, 9 dB inside the −32…−4 window.

### A/B render commands (the two counterfactuals are the point)

```
# The two settings the pedal is actually used at:
clipper-render --gen pluck:110:2.0 klon_transparent.wav --pedal gold --distortion 0.15 --filter 0.50 --level 0.85 --sr 48000
clipper-render --gen pluck:110:2.0 klon_pushed.wav      --pedal gold --distortion 0.85 --filter 0.55 --level 0.70 --sr 48000
#   -> transparent peak 0.805 / rms 0.125 (a clean, tightened boost — the always-on sound);
#      pushed      peak 0.863 / rms 0.256 (rms doubles while the peak barely moves: the
#      clipped half does the work, the clean half holds the transient shape).
# What the pedal would be WITHOUT its defining trait (clean half removed):
clipper-render --gen pluck:110:2.0 klon_pushed_noclean.wav --pedal gold --distortion 0.85 --filter 0.55 --level 0.70 --sr 48000 --gold-no-clean
#   -> peak 0.574 / rms 0.225: the same dirt with the attack flattened — the transient
#      collapses from 0.86 to 0.57 while the body stays. The clean blend IS the pick attack.
# Germanium vs silicon, same drive:
clipper-render --gen pluck:110:2.0 klon_pushed_silicon.wav --pedal gold --distortion 0.85 --filter 0.55 --level 0.70 --sr 48000 --gold-silicon
#   -> peak 0.917 / rms 0.295: silicon clips later and harder — louder and stiffer, the
#      softer germanium bloom replaced with a firmer edge.
```

### Integration notes

- **One param shape, additive registries.** `PedalParams {distortion, filter, level}`
  reading as **Gain / Treble / Output**; each registry gains exactly one entry
  (`rig.ts` `PedalType` + `AVAILABLE_PEDAL_TYPES` + `GOLD_KNOB_DEFAULTS` gain 0.35 /
  treble 0.5 / output 0.7 + the normalizer; worklet `_gold` prefix; the single
  `EXPORTED_FUNCTIONS` list; gear tray; `add_pedal` enum; `Pedal.tsx` FACES;
  `tokens.css` `--accent-gold` in all four theme blocks). Old rigs load unchanged.
- **Visual identity (doctrine §17).** Dark chassis in both themes, **GOLD accent**
  (arcs / readouts / LED). Its own **`plate` face**: knob row over a milled, recessed
  **NAMEPLATE** band with hairline gold rules and *engraved* (cut-in, not embossed)
  lettering, round stomp below. The original is remembered for an *engraved*
  enclosure, so we take the engraving idea and put **type only** on the plate — the
  figure it is famous for is the trademark and is deliberately absent. Wordmark
  **"Myth"** (the pedal that became a legend, hoarded and flipped), model line
  **`DRIVE Nº6 · GOLD`**. Gold is deliberately warmer/browner than the SD-1's lemon
  `--accent-sd` and the amp's brass `--accent-jcm`, and the `plate` silhouette
  separates it from every other face in grayscale.
- **Assistant.** `add_pedal` gains `'gold'`; the coach knows this is the transparent
  one, that its GAIN knob is a **blend** and not an amount, and the three classic
  moves: always-on at low gain with output up; shoving an already-breaking-up amp
  over the edge (the amp distorts, the pedal supplies the push and the tightness);
  and stacking either side of another dirt box.
- Core suites all green (**ctest 16/16**, +`clipper_gold_tests`); `web` tsc + vite
  build green; Playwright +2 (`gold worklet: transparent at min gain…` and
  `assistant: add_pedal adds the GOLD transparent overdrive…`) plus the M11 web sim
  extended to five dirt pedals (4/4 green).

---

## 28. Audit finding 1 — the non-finite parameter guard and the engine reset path

**Session:** 2026-07-25 · `fix/nan-parameter-guard` · plan `docs/work/2026-07-25-nan-parameter-guard.md`
**Fixes:** the first of the three shipping-blockers in `docs/audits/2026-07-24-project-audit.md`.

### The bug

One NaN parameter permanently destroyed all audio, and the in-app assistant could
send one.

The root cause was a single idiom repeated ~14 times across the tree:

```cpp
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
```

Both comparisons are **false for NaN**, so the NaN fell straight through into the
model. `std::clamp(v, 0.0, 1.0)` has the identical hole, and so does JavaScript's
`Math.min(1, Math.max(0, v))`. Once a NaN reached a recursive state — a
`OnePoleSmoother` value, a `Biquad` delay, a WDF cap voltage, a tube Newton warm
start — it **latched**: every subsequent sample was non-finite, and no later write
ever cleared it. `Inf` was handled correctly all along (it clamps to the rail);
NaN was not.

**Measured before the fix** (48 kHz, 1 s window of the house pluck, one NaN written
to parameter 0, then a good value written back and 1 s of silence to settle):

| unit | clean | after ONE NaN parameter | after writing a GOOD value back |
|---|---|---|---|
| `RatModel`    | 0/48000 | 47999/48000 | **48000/48000** |
| `SdModel`     | 0/48000 | 48000/48000 | **48000/48000** |
| `TsModel`     | 0/48000 | 48000/48000 | **48000/48000** |
| `MuffModel`   | 0/48000 | 48000/48000 | **48000/48000** |
| `GoldModel`   | 0/48000 | 48000/48000 | **48000/48000** |
| `PhaserModel` | 0/48000 | 47999/48000 | **48000/48000** |
| `AmpModel`    | 0/48000 | 48000/48000 | **48000/48000** |
| `Jcm800Amp`   | 0/48000 | 47996/48000 | **48000/48000** |
| `TwinAmp`     | 0/48000 | 47999/48000 | **48000/48000** |
| `Ac30Amp`     | 0/48000 | 47999/48000 | **48000/48000** |

Every unit, not just the three the audit sampled. The third column is the important
one: writing a good value back made it **worse**, never better. There was no `reset`
anywhere in the valve-amp tree or the C ABI, so the only recovery was destroy +
recreate the whole engine.

**After the fix: 0/48000 in every cell.**

### The reachable path

`web/src/assistant/tools.ts` declares `minimum: 0, maximum: 1` in the tool JSON
schema, but a JSON schema is a hint to the model, not a runtime check — so any
non-numeric emission (`"max"`, `null`, `{}`) became NaN via `Number(...)`, survived
the TS clamp, and reached `_amp_set_param` as `+data.value`. The worklet's
input-trim handler five lines away *did* guard with `Number.isFinite`; the `param`
handler did not.

### The fix — three layers plus two front-end boundaries

1. **One shared clamp.** `core/include/clipper/dsp/ParamGuard.h` is now the single
   parameter clamp for the whole core. It uses the **inverted comparison order**:

   ```cpp
   inline float clampParam01(float v) { return v > 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f); }
   ```

   For every finite `v` this is **bit-identical** to the old clamp — same branches,
   same result — so replacing all ~14 copies is fidelity-neutral *by construction*,
   not by measurement. For NaN both comparisons are false and it lands on the safe
   low end. No `isnan()` call, no extra branch. `clampParam(v, lo, hi)` generalises
   it; `paramToInt(v, fallback)` covers mode selectors, because `std::lround(NaN)`
   is unspecified and must never reach a switch.

   Every parameter entry point in `core/` now routes through it: the six pedals, the
   four amp voices, all three tone-stack `setKnobs` pot-fraction clamps (a NaN pot
   fraction stamps a NaN conductance into the MNA matrix, and its **inverse** — so
   every sample forever after — is NaN), the three `audioTaper` laws, the three
   `clampR` resistance floors, `OptoTremolo`, and `Processor::setParameter`.

2. **A hard gate at the C ABI.** Every `*_set_param` export in
   `core/src/clipper_c_api.cpp` now begins `if (!std::isfinite(value)) return;`. This
   is a **rejection, not a clamp**: NaN has no in-range meaning, so the knob keeps
   its previous value. Note this changes `Inf` behaviour too — it used to clamp to
   the rail, and is now ignored. That is deliberate (and what the audit asked for):
   a caller sending `Inf` is malfunctioning, and silently jumping a knob to its
   extreme is worse than dropping the message.

   The two layers are complementary, not redundant: the ABI gate is the chokepoint
   guaranteed to be on the *web* path, and the in-model clamps cover the **JUCE
   plugin**, which calls `setParameter()` directly and never touches the C ABI.

3. **A recovery path.** `reset()` now exists down the whole tree — `Oversampler`,
   `OnePoleSmoother`, `TriodeStage`, `BjtStage`, `LtpInverter`, the three preamps,
   the three power amps, the four amp voices, all six pedals, `Processor` — exported
   as `clipper_reset` / `rat_reset` / `sd_reset` / `ts_reset` / `phaser_reset` /
   `muff_reset` / `gold_reset` / `amp_reset`.

   **`reset()` re-parks at the already-solved DC operating point and never
   re-solves.** This is the load-bearing design point. `TriodeStage::prepare()` runs
   a 2-D Newton *and then settles ~12 grid-leak RCs of silent samples* (≈50 k samples
   per stage at 4×/48 k); `Jcm800Amp::prepare()` measures **87.6 ms**. A recovery
   path may not cost that. So `TriodeStage` and `BjtStage` now snapshot their settled
   fixed point in `cachePark()` at the end of `settleDC()`, and the power amps factor
   their idle-parking block into `parkState()` shared by `setOversampling()` and
   `reset()` (so the two can never drift apart). Measured:
   **`Jcm800Amp::reset()` = 0.0003 ms, i.e. ~302 000× cheaper than `prepare()`.**
   `amp_reset` resets **all four voices**, not just the active one — `amp_set_param`
   deliberately keeps every voice current so a model swap lands on the right tone, so
   a poisoned inactive voice would otherwise stay poisoned until switched to — plus
   both per-side cab convolvers (the FDL holds a whole IR of history).

   What `reset()` does NOT touch: knob positions, the oversampling factor, the loaded
   IR, the active model, the cab flag, the sample rate. It un-bricks audio; it does
   not reset the rig.

4. **The worklet boundary.** `web/worklet/clipper-processor.js`'s `param` path now
   rejects non-finite before the value crosses into WASM, mirroring the `_inputGain`
   guard that was already there.

5. **The TypeScript clamps.** `assistant/tools.ts`, `App.tsx`, `components/Knob.tsx`
   and `params.ts` (`trimKnobToDb`) are all finite-safe now. (`rig.ts`'s persisted-rig
   `clamp01` already was.)

### The test — `core/tests/test_nan_guard.cpp` (`clipper_nan_guard_tests`)

Injects `{NaN, +Inf, -Inf}` into **every parameter of every unit** — six pedals and
four amp voices — over **both** the C ABI (the worklet's path) and the direct C++
`setParameter` (the plugin's path), at 48 kHz in 128-frame in-place blocks. 210 ABI
combinations + 51 direct-C++ parameters × 3 poisons.

- **A.** Through the C ABI, a non-finite write is rejected so completely that the
  render afterwards is **bit-identical** (max deviation `0.0e+00`, all ten units) to
  a run that never received it. The player-observable property is *"a malformed
  assistant message must not change my tone"*, and bit-identity checks it with no
  tolerance to argue about.
- **B.** Through direct C++, the render stays finite, the level comes back, and the
  poisoned write is **bit-identical to writing the in-range value it documents itself
  as clamping to** (NaN → 0, +Inf → 1, mode ids → 0). That equivalence claim is the
  sample-exact half, and unlike a comparison against a never-touched reference it is
  not confounded by knob hysteresis (see below).
- **C.** Poison through a channel *no parameter guard can cover* — a NaN **audio
  sample** — first asserting the poisoning actually latches (100 % non-finite, so the
  test is not vacuous), then `reset()`, then requiring the render to be
  **bit-identical** to a clean instance that was also reset. Plus: `reset()` on a
  healthy unit moves its level by ≤ 0.15 dB, and is ≥ 50× cheaper than `prepare()`.

**Verified to fail against unmodified `core/`**: with `core/src` and `core/include`
restored from `main` (and `-DCLIPPER_NAN_TEST_NO_RESET`, which compiles blocks A and
B only so the suite links without the new `reset()` API), block A aborts on its first
combination — `rat_* param 0 <- NaN : 14399/14400 non-finite samples` — and with
block A disabled, block B aborts equivalently on `RatModel param 0 <- NaN`. Both
exit 134.

### Two measurement notes worth keeping

Both came out of building the test and are easy to mistake for bugs.

- **Knob hysteresis is real and is not a guard defect.** Driving the JCM's MASTER to
  1.0 and back charges the triode blocking caps (τ = Rgl·Cc = 22 ms) and pulls the B+
  rail (τ = Rsupply·Creservoir = 7.5 ms), so the next render differs from a
  never-touched reference: max sample deviation 3.2e-2 / level 0.07 dB at a 0.4 s
  settle, falling to 3.5e-6 / 0.000 dB at 3 s. An ordinary in-range 0.5 → 1.0 → 0.5
  sweep leaves **exactly** the same residue (the two renders are bit-identical to each
  other). Any test that compares "after a knob excursion" against "never touched"
  will trip over this.
- **A settled `OnePoleSmoother` stalls one float ULP short of its target, forever.**
  `value_ += coeff*(target_ - value_)` underflows to `value_ += 0` long before the
  residual reaches the 1e-30 denormal snap in `next()`. `reset()` snaps `value_` onto
  the target exactly, so a reset unit's knob is one ULP *different from* (marginally
  more accurate than) a naturally settled one — on the RAT, one ULP of pre-gain
  (~6e-8 of 0.6) emerges at −96.7 dB below peak. This is why block C compares
  reset-against-reset, where it can legitimately demand bit-exactness.

### Suites

Core **ctest 17/17** (16 existing + `clipper_nan_guard_tests`; the new target runs in
~236 s, comparable to `clipper_phaser_tests`), `web` `tsc --noEmit` + `vite build`
green, **Playwright 70/70**, `test:server` 11/11, `test:history` 10/10, `electron`
16/16. The goldens are untouched and still pass, which is the independent check that
the clamp rewrite really is fidelity-neutral.

**Still open from this finding's neighbourhood:** nothing in the UI *calls* the new
reset exports yet — this slice ships the mechanism, not the policy (a watchdog that
notices non-finite output and resets, or an explicit "reset engine" affordance, is a
separate slice). `native/src/ClipperEngine` inherits the clamp fix but has no reset
seam. And the signal path is still NaN-transparent in places — `OutputLimiter::clamp1`
has the same `v > 1 ? 1 : (v < -1 ? -1 : v)` shape.

---

## 29. The audit's systemic finding — tests that assert real properties

**Slice:** `test/assert-real-properties` (2026-07-25). Tests, `core/CMakeLists.txt` and
docs only — **no DSP change**. Sequenced deliberately *before* the circuit-fix phase, per
the audit's own suggested order: "Stand up CI before the circuit work, and fix the vacuous
tests named in each finding. The fidelity changes below need to be *measured*, not asserted,
and the current suite would pass either way."

The 2026-07-24 audit's systemic finding was not any one defect:

> The test suite is large and passes, but a recurring class of test asserts an identity, a
> tautology, or the implementation against a reference derived from the same code — so wrong
> topologies and wrong constants pass. Several findings are things a test *named for that
> property* did not catch.

That is load-bearing for what comes next. The AC30 and shared-phase-inverter fixes (findings
4, 5, 7) will be judged by this suite, and re-blessing a golden against an unverified change
is how a regression becomes canon.

### The XFAIL ratchet (the design decision worth recording)

Correcting these tests exposes real, open defects. The two obvious ways to keep the suite
green are both the disease: `#if 0` makes the property untested again, and loosening the bound
makes the defect canon — which is *literally* how the starved AC30 phase inverter shipped
(docs §23 second amendment: a 150 V window on a 300 V rail).

So `core/tests/support/Xfail.h`. A known-bad property is **measured**, its real number is
**printed**, and the finding plus the owning slice are **named**. The run continues. And:

> **An XPASS is a hard failure.**

The moment somebody fixes the defect, the suite goes red until they delete the XFAIL. An XFAIL
cannot rot into a permanent excuse, and cannot be used to smuggle a genuinely failing property
past review. Verified: shortening `OptoTremolo::kReleaseMs` from 55 ms to 5 ms makes both
tremolo XFAILs XPASS and the binary exits 1.

Visibility was the other half. `ctest --output-on-failure` prints nothing for a passing test,
and a known defect nobody can see is how these findings survived a green suite. So each binary
carrying XFAILs also registers `<target>_xfail_ledger`, which runs it with `--xfail-ledger` and
exits **77** under `SKIP_RETURN_CODE 77`. A plain `ctest` now prints, in its default summary:

```
Test #10: clipper_jcm800_power_tests_xfail_ledger ........***Skipped
Test #12: clipper_twin_tests_xfail_ledger ................***Skipped
Test #14: clipper_opto_tremolo_tests_xfail_ledger .........***Skipped
Test #16: clipper_muff_tests_xfail_ledger .................***Skipped
Test #19: clipper_ac30_tests_xfail_ledger .................***Skipped
Test #21: clipper_player_expectations_tests_xfail_ledger ..***Skipped
```

### Asserts are live on every platform now, and it is a build error if they are not

`core/CMakeLists.txt` guarded `-UNDEBUG` behind `if(NOT MSVC)` in seventeen copies. These
suites are plain `int main` + `<cassert>`, and `NDEBUG` — which CMake defines in every Release
build, the documented build type — erases `assert()`. So on MSVC the entire 1129-line
player-expectations suite compiled to a no-op `main` that printed "All M11 player-expectations
tests passed" and exited 0, **including with zero golden WAVs present**. Not a weaker test: no
test at all, reported green.

MSVC accepts `/UNDEBUG` exactly as GCC/Clang accept `-UNDEBUG`, so the guard was never needed.
The seventeen copies are now one `clipper_add_test_flags()`, and
`core/tests/support/AssertsLive.h` — included by every test `.cpp` — makes it a **compile
error** if `NDEBUG` ever survives again, plus a runtime `requireAssertsLive()` for belt and
braces. Verified: dropping `-UNDEBUG` fails the build with that `#error`.

### The phase inverter, measured properly for the first time

`test_twin_amp.cpp` and `test_ac30_amp.cpp` were commented "PI: balanced anti-phase legs" and
asserted only `quiescentPlate1() > 250 && < 410` — a 160 V window on a 410 V rail, which
admits both a healthy ~330 V and the starved 386.8 V the amp ships with. Nothing anywhere
asserted the **leg ratio**, the property push-pull even-harmonic cancellation actually depends
on (audit finding 8's own words).

`core/tests/support/LtpProbe.h` now defines the three properties **once**, shared by all three
amps so they cannot drift on what "a healthy phase inverter" means:

| property | how | why not the old form |
|---|---|---|
| plate as a **fraction of B+** | `Va/B+`, target 70–85 % | scale-free; an absolute window cannot tell "mid load line" from "parked at cutoff" |
| **standing current per triode** | `(B+ − Va)/Ra` — **Ohm's law on the plate load**, target 0.5–0.9 mA | independent of the Koren law the solver used, so a self-consistent-but-wrong device fit cannot satisfy it |
| **leg balance** | drive a fresh `LtpInverter` configured from the shipped `config()` with 20 mV and compare the two plate swings | this is the cancellation mechanism, and nothing measured it |

Measured (unchanged code at `9923af7`, reproducing the audit exactly):

| amp | Va1 (% B+) | Ip/triode | leg gains | ratio | verdict |
|---|---|---|---|---|---|
| JCM800 | 322.1 V (**94.7 %**) | **0.179 mA** | ×15.44 / ×9.37 | **0.607** | all three XFAIL (findings 7, 8) |
| Twin | 386.8 V (**94.3 %**) | **0.232 mA** | ×7.43 / ×7.51 | 0.990 | plate + current XFAIL; **balance asserted** |
| AC30 | 247.1 V (82.4 %) | 0.529 mA | ×31.76 / ×17.46 | **0.550** | **plate + current asserted**; balance XFAIL |

Every amp carries at least one hard assertion, and no target is XFAILed on all three — the
bars are demonstrably reachable by the shipped code. The AC30 pair is the important one: it
meets the DC targets *only* because `Rtail` was cut to 2.2 kΩ, and that is exactly what wrecks
its balance. Asserting both pins the trade-off, so a future "fix" that restores balance by
re-starving the inverter cannot pass.

`test_jcm800_power.cpp`'s push-pull test lost its part (a) entirely: it built
`pp[i] = f(Vbias+v) − f(Vbias−v)` from the device law and asserted the result's 2nd harmonic
was 40 dB down — the algebraic identity "f(v) − f(−v) is odd", true by construction for any
`f` whatsoever. The **single-ended** reference survives, because one EL34 driven by the Koren
law is genuinely independent: it is the "no cancellation at all" baseline a pair must beat. The
bar moved from 6 dB (not cancellation — a rounding error) to 20 dB. The JCM measures 10.3 dB
below SE where the balanced Twin gets ~30, so it XFAILs against finding 8.

### DC offset, on signal — and why the clean-input case alone has no teeth

All four dirt-pedal tests asserted `|mean(out)| < 1e-3` after 0.3 s of **zeros**. Trivially
true, and not the property anyone cares about: these are asymmetric clippers and an asymmetric
clipper produces DC *from signal*.

Correcting it needed **two** stimuli, and the second is the load-bearing one. Measured: with a
clean input, deleting the RAT's / TS's / GOLD's output coupling cap outright changes their
measured DC by **nothing** — a symmetric or already-blocked clipper does not rectify, so a
clean-input test cannot fail if the cap goes missing, which is precisely the regression worth
catching. Feed **+0.1 V of DC on the input** (an ordinary bad interface, or a mis-biased
upstream buffer) and the cap becomes load-bearing.

| | clean input | +0.1 V input DC |
|---|---|---|
| RAT | 0.000 % of peak | 0.091 % |
| TS | 0.000 % | 0.000 % |
| GOLD | 0.000 % | 0.000 % |
| **Muff** | **18.4 %** (+0.44 V on a 2.38 V peak) | **18.4 %** |

The Muff XFAILs against finding 16 — it has no output high-pass anywhere; the real pedal's
0.1 µF output cap is simply absent, while every sibling carries `dcBlockHz = 12.0` because
(`SdModel.cpp:66`) "the asymmetric clip produces DC". Not fixed here: adding a high-pass to a
fuzz is a **tone change** and needs its own A/B (it will lift the low end and change the
bloom).

### Block size: the property the 128-only comparison could not test

`test_player_expectations.cpp` block B compared in-place 128-frame rendering against one big
call at `tol = 2e-5`, with a comment about "float accumulation across ~60k samples of tube
solves". Every unit measured **exactly 0.000e+00**. Not luck — every unit already chunks
internally at exactly 128, so a 128-frame outer loop cannot produce a different internal call
pattern. The tolerance was ~94 dB looser than the property needed. Structurally vacuous, the
same way finding 3's `testConvolverChunking` was.

Two changes. **Aligned sizes are now BIT-IDENTICAL (`tol` 0.0)** — which is what is actually
true. And the signal is trimmed to 57563 samples (a multiple of neither block size, so every
pass ends on a partial block) and each unit also runs at a **ragged 100 frames**, the only
segmentation that can catch a block-size bug at all — finding 3's `CabConvolver` was exact at
128 and produced an error *larger than the signal* at 100.

That ragged pass found something. The five **dirt pedals** diverge at a ragged size, but only
during the first ~25 ms, converging to ≤ 1.0e-3 relative afterwards: the audit's Medium/DSP
item, *"control-rate parameter sampling defeats the 5 ms smoother at DAW block sizes"*, now
measured end to end through the C ABI the worklet calls. Worst case Muff, **1.473 absolute at
25 ms**. So the *settled* output is asserted for real (≤ 2e-3 relative) and the startup
transient is XFAILed against that item. The phaser, all four amp voices, `CabConvolver` and
`ReverbModel` are bit-identical at every block size from 1 to 256 and are asserted as such.

Also in the same suite: `assert(lv[1] >= lv[0] …)` compared a real level against `lv[0]`, which
is ~1e-12 (a closed pot is silence) — true for any `lv[1]` whatsoever, including a level knob
wired to nothing. `assert(th[1] >= th[0] * 0.95 …)` permitted a **5 % THD decrease** under the
banner "non-decreasing", which is enough slack to hide a small taper inversion. And
`sine(220.0, g.isPedal ? 0.1f : 0.1f, …)` had two identical branches. All three are now
literal, with the level knob additionally required to gain **≥ 2 dB from noon to fully open** —
the audit measured JCM800 BASS at "+9.5 dB lower half / +0.2 dB upper half", and 2 dB cleanly
separates a dead top half from the tightest real case (clean120, +2.9 dB).

### OptoTremolo — the first test it has ever had

The audit said `OptoTremolo` "has no test at all". Half true, and the half that was true is the
half that mattered: `test_twin_amp.cpp` does drive the class (depth vs INTENSITY, the enable
bypass, the rate map, the attack/release asymmetry), but every probe sits at a fixed
`setSpeed(0.4f)` — so the one axis with a real defect on it was never swept. And no render in
the Twin suite reaches the tremolo *through* `TwinAmp`: they all pass INTENSITY 0.

`core/tests/test_opto_tremolo.cpp` pins **speed invariance** — turning the speed knob must
change the *rate* of the throb and nothing else:

| SPEED | rate | peak gain | mean level | depth |
|---|---|---|---|---|
| 0.00 | 1.00 Hz | 0.973 | −6.91 dB | −71.9 dB |
| 0.50 | 3.16 Hz | 0.831 | −8.84 dB | −50.6 dB |
| 1.00 | 10.0 Hz | **0.522** | **−12.03 dB** | −27.2 dB |

A 55 ms release against a 100 ms LFO period means the LDR never discharges, so the cell sits
permanently part-lit and the whole waveform slides down instead of the dips getting closer
together. **A player hears the amp get quieter as they speed the tremolo up**, which no AB763
does. Two XFAILs (mean level within 1.5 dB across the range; peak gain ≥ 0.90 at every speed);
the depth floor and the measured rate map are asserted for real. Block B of the same file
drives SPEED / INTENSITY / TREMOLO_ENABLE *through* `TwinAmp` and pins the envelope swing in
both directions of the enable — the wiring nothing had exercised.

### Playwright

The "no pop / declick continuity" tests posted the topology change **before**
`ctx.startRendering()`, so the swap landed at the opening zero crossing with empty filter state
and *removing the declick entirely* would still pass. Their bounds also carried absolute floors
(`+0.02`, `+0.05`) larger than the step a de-declicked swap actually produces.

The three amp-swap tests now land the swap at 0.3 s into a sustained note via `ctx.suspend()` /
`resume()` (the technique `expectations.spec.ts:255` already used), and the reference is the
render's **own per-sample slew away from the swap, in the same render** — a pop is by
definition a step larger than the signal's natural slew, so no absolute floor is needed.
Measured, swap-window slew ÷ away-from-swap slew:

| swap | shipped declick | declick **deleted** |
|---|---|---|
| clean120 → jcm800 | 2.04 | **7.15** |
| clean120 → twin | 1.66 | **7.16** |
| clean120 → ac30 | 1.19 | **5.12** |

Bar 3.0: 1.5× margin on the pass side, 1.7× on the fail side. Each also asserts real signal on
**both** sides of the swap and a level change across it, so the test cannot quietly revert to
the old shape and still pass. (`cab.spec.ts`'s cab-swap equivalent is owned by
`fix/cab-swap-rt-safety` and was left alone.)

The three "perf smoke" tests asserted `expect(perf.clean).toBeGreaterThan(0)` on a
`performance.now()` delta — positive by construction — and never read an output sample. They
now return the rendered audio too, and assert that both paths made sound, that the two paths
made **different** audio (otherwise the ratio is clean-vs-clean, ~1, and sails under the bound
no matter how slow the valve amp becomes), and that the render is finite.

`'board: move buttons reorder the pedal chain'` asserted only `Array.isArray(chainIds)` and
`length === 2` under a comment reading "ids swapped order" — the one thing it is named for went
unchecked. It now asserts the literal order the **worklet** was told about. `cab.spec.ts`
asserted `irLen > 128` (a property of the committed fixture, not of the code) and
`typeof label === 'string'` (passes for `''`); both now assert the real value. The unasserted
`overall` and `sd1.h3` computations are asserted — `overall` is what distinguishes a phaser
that *sweeps* from one stuck at its deepest notch, and `sd1.h3` is what stops an SD-1 that had
stopped clipping from passing on its 2nd harmonic alone.

Finally, `audio.spec.ts` — 1751 lines covering every pedal's DSP — contained **zero** finiteness
checks. Every assertion in it is a Goertzel bin, an RMS or a max-delta over a *window*, so a NaN
outside that window or in the right channel of a stereo render failed nothing, and NaN
propagates silently into the measurements it does make. Rather than an `allFinite()` call at
each of ~15 render sites (which only guards the sites somebody remembers),
`tests/support/finite-output.ts` patches `OfflineAudioContext.prototype.startRendering` once
per page: every render is scanned, in every channel, over every sample, and a new render site
is covered automatically.

### Teeth — every rewritten test, and the perturbation that proves it

A rewritten test that still cannot fail has not been fixed. Each was perturbed in a scratch
copy, confirmed red, and reverted:

| test | perturbation | result |
|---|---|---|
| JCM800 PI leg balance | `Ra2` 82 k → 150 k (ratio 0.607 → 0.978) | **XPASS → exit 1** |
| JCM800 PI plate / current | `Rtail` 10 k → 1.6 k | fails |
| Twin PI leg balance (hard) | `Ra2` 142 k → the **100 k its own header documents** | fails |
| AC30 PI plate + current (hard) | `Rtail` 2.2 k → the original 22 k (re-starving it) | fails |
| TS DC on signal | bypass the output coupling cap | fails |
| GOLD DC on signal | bypass the output coupling cap | fails |
| block B `tol` 2e-5 → 0 | +1e-6 per call inside `ReverbModel` → **1.341e-07** on the JCM path | fails at 0.0, **would have passed at 2e-5** |
| block B ragged pass | phaser LFO sampled once per call instead of per sample | fails |
| LEVEL knob top half | RAT level saturates at 0.5 | fails |
| OptoTremolo speed sag | `kReleaseMs` 55 → 5 ms | **XPASS → exit 1** |
| `-UNDEBUG` / AssertsLive | drop `-UNDEBUG` | **build error** |
| amp swap "no pop" ×3 | delete the declick bracket | ratio 5.1–7.2 vs bar 3.0 |
| chain reorder | post the chain to the worklet in its original order | fails |
| cab custom label | derive the label from a constant instead of the filename | fails |

Two honest caveats. The **RAT**'s DC assertion holds with ~11× margin on input offset and its
teeth overlap `testPreClipVoicing` — the shaping network's DC gain is already pinned there to
±1.5 dB, so any perturbation large enough to break DC trips that test first. And **GOLD** is
protected at both the input (`kInputHpHz` 7.2 Hz) and the output (`kOutHpHz` 8 Hz); the output
cap alone is enough to fail, but a model with only one cap would be a weaker guard.

### A false alarm worth recording

Mid-slice, a measurement appeared to show the TS passing input DC straight to its output at
unity — `cfg_.dcBlockHz` reading **0** in `prepare()` while `kTsConfig.dcBlockHz`
static-asserts as 12.0. It was **not** a defect: the incremental `build/` directory held a
stale `OverdriveEngine.cpp.o`, because the perturbation harness restored files with `cp`/`mv`,
which preserves the *backup's* mtime — older than the existing object, so `make` skipped the
rebuild. A fresh build directory gave `dcBlockHz = 12`, `dcR_ = 0.998430437`, and correct DC
rejection. **Any perturbation harness must `touch` the file after both patch and restore**, or
it measures stale code. This is also why one earlier "NO TEETH" reading was wrong.

### Suites

Core **ctest 24/24** — 18 real targets (17 existing + `clipper_opto_tremolo_tests`) plus 6
`_xfail_ledger` entries reported as Skipped, carrying **11 XFAILs** between them. Web
`tsc --noEmit` + `vite build` clean, **Playwright 66 passed**, `test:server` 11/11,
`test:history` 10/10, `electron` 20/20. Exactly one compiler warning in the tree, the
pre-existing unused `softLimit` in `tools/render/main.cpp` — no new ones, despite `-Wall
-Wextra` now reaching `clipper_tests` too (it previously got only `-UNDEBUG`).

**The goldens are untouched and still pass, which is the whole point of this slice's scope
discipline:** it changes no DSP, so the golden gate is an independent confirmation that
nothing about how the rig sounds moved while the tests around it were rewritten.

### Left open (deliberately)

Every XFAIL is a real defect this slice made visible and did not fix:

| id | finding | owner |
|---|---|---|
| `finding7-jcm-pi-plate-fraction`, `finding7-jcm-pi-standing-current`, `finding7-twin-pi-plate-fraction`, `finding7-twin-pi-standing-current`, `finding7-ac30-pi-leg-balance` | 7 — the LTP tail returns to ground, so standing current is set entirely by `Rtail` | the `tailRef` fix; one change fixes all three amps |
| `finding8-jcm-pi-leg-balance`, `finding8-jcm-even-harmonic-cancel` | 8 — `Ra2 = 82 kΩ` moves the imbalance the wrong way | `Ra2` → ~120 kΩ **on top of** the tailRef fix (150 k alone reaches 0.978 but is not the physical answer) |
| `finding16-muff-no-output-dc-blocker` | 16 — no output high-pass at all | its own slice; it is a tone change |
| `trem-mean-level-sags-with-speed`, `trem-peak-gain-collapses-with-speed` | Medium/DSP — 55 ms release against a 100 ms period | scale `kReleaseMs` with the LFO period (verified to work) |
| `control-rate-param-sampling-block-size` | Medium/DSP — chunk-end parameter sampling | apply the smoothed value per sample; `RatModel`'s inner loop is the pattern |

Not attempted here: the **tone-stack class** of test, which compares the discrete MNA against
an analytic `H(jω)` derived from the same netlist. Real (findings 5 and the JCM flatness are
exactly that), but fixing it needs published response curves per amp — a research slice.

**`web/playwright.config.ts:34` sets `retries: 2`**, so any fault appearing in under a third of
runs is retried away. Not changed unilaterally in a test-integrity slice, but it is the last
remaining way for a real fault to disappear silently, and it belongs in the next process
slice. Recommendation: `retries: 0` locally and on PRs, keeping retries (if any) only for a
nightly job, so a flaky test is a bug report rather than a shrug.

This slice ran straight into it. The suite is green (66 passed, exit 0), but **four
pre-existing tests needed a retry** — `'RAT worklet: high distortion yields odd harmonics'`,
`'amp: treble knob changes 5 kHz content'`, `'reverb: 0 == dry; up leaves a decaying tail'`,
and the phaser notch test. All four are `OfflineAudioContext` renders, and `audio.spec.ts`'s
own header names the cause: *"Creating many live AudioContexts in one browser process can
starve later OfflineAudioContext renders (they go silent)."* With `retries: 2` they are
invisible; with `retries: 0` they would be four red tests describing a real, reproducible
resource limit.

The same limit bit the rewritten **perf-smoke** tests, and it is worth recording how, because
it is a trap for the next person. Each builds eight `OfflineAudioContext`s in sequence. Run
inside the full suite, one twin pair came back with `voiceDiff` **exactly 0** and a ratio of
**0.85×** — i.e. the amp-model swap silently did not happen and *both* renders were clean120.
Measured in isolation the same settings give ratio 13× and diff 0.22. So the new audio
assertion was correct and the render was degraded. The old test could not have noticed
(0.85 < 150), and a naive fix — asserting the last pair — would have swapped a vacuous test
for a flaky one. The assertion is therefore over the **best of the three pairs**: "at least
one pair rendered genuinely different audio" still fails hard if the swap never works, which
is the property worth having.

## 30. Audit finding 2 — the cab swap gets off the render path

**Session:** 2026-07-25 · `fix/cab-swap-rt-safety` (stacked on `fix/nan-parameter-guard`) · plan `docs/work/2026-07-25-cab-swap-rt-safety.md`
**Fixes:** the second of the three shipping-blockers in `docs/audits/2026-07-24-project-audit.md`.
**ADR:** `docs/decisions/003-cab-double-buffer-at-the-abi.md`

### The bug

A cab change ran **one** C ABI call that synthesised an impulse response, heap-copied
it, peak-normalized it and ran `CabConvolver::prepare` twice (FFT plan plus one
spectrum per partition per side) — and `web/worklet/clipper-processor.js` made that
call **from inside its per-sample loop**, at the declick fade-out zero. Measured
against the 2.667 ms deadline at 48 kHz / 128:

| call | wall time | vs deadline |
|---|---|---|
| `amp_process_stereo(128)` | 0.0235 ms | 0.9 % |
| `amp_set_cab_builtin` (1024-tap) | **10.97 ms** | **411 %** |
| `amp_load_custom_ir` (4096-tap) | **42.66 ms** | **1600 %** |

(Native Release figures from `clipper_cab_swap_tests` on the dev machine; they
reproduce the audit's WASM measurements of 11.4 / 45.7 ms closely.)

So every cab change and every IR upload dropped a run of consecutive render quanta —
an audible **dropout**, not a click. The design comment defended it as inaudible
"because it runs at the output-zero of the declick", which conflates two unrelated
things. Output-zero prevents a **step discontinuity**. It does nothing whatever about
**missing the render deadline**. CLAUDE.md now states that explicitly.

Instrumenting `process()` in the shipped worklet, the worst single render block
spanning a 4096-tap IR load measured **104.9 ms — 3935 % of the deadline, 39
consecutively missed quanta.**

### The fix: double-buffer at the C ABI, not in the convolver

`CabConvolver` is deliberately **untouched** — `fix/cab-block-size` is rewriting that
file for finding 3 in parallel, and a three-way conflict there would be worse than the
bug. Instead `AmpChain` in `core/src/clipper_c_api.cpp` now holds **two** per-side
convolver pairs (`CabPair cabs[2]`) and an active index, which is exactly the pattern
`amp_set_model` already used for the four amp voices. The one call splits in two:

- `amp_prepare_cab_builtin(h, which)` / `amp_prepare_cab_custom(h, ir, len)` — build
  the new cab in the **inactive** pair. Still heavy, still allocating, returns 1 on
  success and **0 on a rejected argument**. Must not be called from a render callback.
- `amp_commit_cab(h)` — `activeCab ^= 1`. One integer write.

`amp_set_cab_builtin` / `amp_load_custom_ir` remain as prepare+commit wrappers, so the
ABI stayed purely additive and the tools, tests and any FFI kept working.

`amp_create` prepares **both** pairs with the default 2×12, so `amp_commit_cab` can
never activate an unprepared convolver. `amp_reset` clears both pairs, for the same
reason it resets the three inactive amp voices: an inactive pair that has ever been
live still holds FDL history, and a poisoned one would spray NaN the moment a cab
change activated it.

**The load-bearing invariant:** every activation is preceded by a `prepare()` of the
pair being activated. Do **not** optimise the prepare away when the requested IR
equals what is already sitting in the inactive pair — that pair's FDL still holds
history from when it was last live, and activating it without a prepare would splice a
stale convolution tail into the output.

### Measured

`clipper_cab_swap_tests` (new, 18th ctest target). Allocations are counted with a
**replaced global `operator new`**, so "allocation-free" is asserted directly rather
than inferred from a clock:

```
[cab swap] render deadline (48k/128)   : 2.6667 ms
[cab swap] amp_process_stereo(128)     : 0.023476 ms/call, 0 allocations
[cab swap] COMMIT (audio-thread step)  : 0.000001 ms/call, 0 allocations
[cab swap] PREPARE builtin (off-path)  : 10.967303 ms/call
[cab swap] PREPARE custom 4096 (off-p) : 42.659175 ms/call, 13 allocations
[cab swap] prepare/commit time ratio   : 9321816x
[cab swap] commit vs one render block  : 0.000050x
[cab swap] commit vs the deadline      : 0.0000 %
```

End to end in the worklet, worst single render block spanning a 4096-tap IR load:
**104.92 ms (3935 % of deadline, 39 quanta missed) → 0.42 ms (15.7 %, none missed).**

**Fidelity is bit-identical, not "within tolerance".** The activated pair is prepared
by the same `CabConvolver::prepare` over the same IR samples with the same 128
partition, and `prepare()` zeroes the FDL — precisely the side effect the old in-place
prepare had. The test drives a scripted session (play → brit412 → play → 4096-tap
custom IR → play → back to clean212 → play → brit412 → play) through the new ABI and
through the **pre-fix construction written out longhand**, and requires
`max|new − old| == 0.0` over 128 000 samples per side. The swap-*back* leg is included
because it is the case double-buffering could plausibly have broken.

### What the worklet does now, and what "off the audio thread" honestly means

The `cab` message handler does all the heavy work immediately and stages only
`{ ready: true }`; `_commitPending()` calls `_amp_commit_cab` and nothing else.

**Be precise about the win.** `AudioWorkletProcessor.port.onmessage` runs on the audio
**rendering thread**. Moving the prep out of `process()` does **not** make it
off-thread — it makes it happen *between* render quanta instead of mid-block. That
converts a guaranteed multi-quantum stall into at worst one late quantum at the moment
of a deliberate user action, and it makes `process()` itself allocation-free. That is a
large, real improvement. It is **not** "now real-time safe". The fully correct fix
computes the partitioned spectra on the **main** thread and copies the finished spectra
in; that needs an ABI exposing the partition layout (or a main-thread WASM instance
used purely as an IR compiler) and is a bigger change — ledgered, not done.

### Two adjacent defects in the same code path, both fixed

**(a) Stale `HEAPF32` across the commit.** `heap` was captured before the output loop
and read after `_commitPending()`, which could `malloc` and detach the view. Every
other site in the file re-fetches — the comment above the input loop explains exactly
why — and this was the one place a `malloc` actually happened. Now re-fetched
immediately after the commit. With the IR prep hoisted out, nothing in the commit path
grows the heap any more, so this is the belt to that braces.

**(b) A second edit inside one fade window committed at non-zero gain.** All four
staging paths began with `if (this._hasPendingEdit()) this._commitPending();`, so a
second edit arriving before the fade reached zero force-committed the first one
wherever the ramp happened to be. Worst case is not "mid-ramp" but **full gain**: when
both messages land in the same message drain no render has advanced the ramp, so
`_declickGain` is still 1.0. Instrumented on the pre-fix worklet, two chain edits
committed from *inside the message handler* at gain 1.0, and the resulting step
measured **0.222 — 7.8× steady-state slew.** Reachable by a drag-reorder, a rapid
add/remove, or the assistant issuing two tool calls in one turn.

Edits now **merge** and ride the fade that is already running (`_stageEdit`). That
required two supporting changes in `_prepareChain`: diff against the **pending** node
list when one exists, and **union** the previous edit's `removed` list. Without the
union, staging "remove pedal X" and then any second chain edit before the fade zero
leaked X's WASM handle forever, because X is already absent from the base the second
edit diffs against. **That leak existed on `main` too**, masked by the eager commit.

Post-fix the same four-edit batch measures **1.00× steady-state slew** — i.e. the edit
window contains no step at all beyond ordinary signal slew.

### A pre-existing artifact this work exposed: the cab's dead partition

Fixing (b) made a latent bug visible. `CabConvolver::prepare()` zeroes the FDL and the
overlap buffer, so a freshly swapped-in cab emits **exact silence** until its FDL
refills — and because a swap lands at an arbitrary offset within the convolver's
128-sample partition grid, that silence can start up to one partition late and then
last a full partition (~2 partitions of dead output after the commit, worst case).

The 6 ms (288-sample) fade-in is *longer* than that gap, so the gap landed in the
**middle of the ramp**: measured stepping from ~59 % gain straight to exact zero and
back, a 128-sample hole in the audio. This is in the shipped v1.1 worklet too; the old
cab-swap test could never see it because it applied the swap before rendering started.

Fixed here, without touching the convolver, by holding the output **parked at zero for
two partitions** (`CAB_SWAP_DEAD_SAMPLES = 256`) after a cab commit, before starting
the fade-in — a new `'hold'` phase in the declick state machine. The whole dead region
now sits inside silence the declick already provides, so the fade-in only ever ramps
real audio. Costs ~5.3 ms on a deliberate cab change and nothing otherwise. Across five
different multi-edit scenarios the edit-window step went to exactly **1.00× the
steady-state slew baseline**.

### Tests

- **`core/tests/test_cab_swap.cpp`** (new target `clipper_cab_swap_tests`). Block A:
  allocation counts (commit 0, `process` 0, prepare > 0) plus wall clock. Block B:
  bit-identity against the pre-fix construction. Block B2: degenerate sequences — a
  rejected IR returns 0, a bare commit with nothing prepared still renders finite
  audible audio. `-DCLIPPER_CAB_TEST_LEGACY_ABI=1` routes the commit through the old
  monolithic call: block A then **fails** (`commit 10.75 ms/call, 403 % of deadline,
  459× a render block, 5 allocations`) while block B still **passes** — which is the
  intended split, since B is the guard that the speed-up cost no fidelity.
- **`web/tests/cab.spec.ts` test 3 rewritten.** The audit called the old version doubly
  vacuous and it was, measurably. It posted the `cab` message *before*
  `startRendering()`, and its "swap" selected the *darker* `brit412` (which a sibling
  test asserts is darker), so lower slew was guaranteed. Verified empirically: the old
  body **passes even with the declick entirely removed** (`0.0018` against a limit of
  `0.0128`). The rewrite lands the swap **mid-note** via `ctx.suspend()`/`resume()` and
  swaps toward the *brighter* cab. It measures 1.00× baseline on the fixed worklet and
  **33.4× (FAIL)** against a declick-stripped worklet.
- **`web/tests/cab.spec.ts` test 3b (new)**, for defect (b): four edits in one message
  drain (two chain removals, an amp-power stomp, a cab swap). **Pre-fix 7.79× → FAIL;
  fixed 1.00× → PASS.**

Note on why the pedal edits in test 3b are *removals* rather than disengages:
`_prepareChain` sets `existing.engaged` on the **live** node object, so an engaged-flag
change arriving in a `chain` message takes effect at the next render quantum rather
than at the fade zero. M11 fixed that for `bypass` (stomp) messages via
`_pendingBypass`, but the `chain` path still has the hole. Out of scope here; ledgered.

### Parity note

`native/src/ClipperEngine` has **no** built-in cab selection and no custom-IR path at
all — it loads the default 2×12 once in `prepare()` — so there is no native counterpart
to this change to keep in step. Audit finding 13 (native declicks neither cab nor amp
power nor amp voice) remains its own slice.

### Suites

Core **ctest 18/18**, `web` `tsc --noEmit` + `vite build` green, `test:server` 11/11,
`test:history` 10/10, `electron` 16/16. Goldens untouched and still passing, which is
the independent check on fidelity-neutrality. **Playwright was not run in this session
— no browser binary was installable in the environment.** The two new/rewritten browser
tests were instead verified by driving the real worklet under Node with a stubbed
`AudioWorkletGlobalScope`, against both the pre-fix and fixed worklets sharing one
rebuilt WASM artifact; all the before/after numbers quoted above come from that.

### §30 amendment — the NATIVE cab/IR picker (2026-07-31)

The paragraph above said native "loads the default 2×12 once in `prepare()` — so there
is no native counterpart to this change to keep in step". True when it was written, and
the reason the owner's question was *"where do I pick my cab/ir?"*: the web has had
Clean 2×12 / Brit 4×12 / user-IR upload since v1.1 and the plugin had none of it. This
amendment is the catch-up (`docs/work/2026-07-31-native-cab-picker.md`).

The engine's convolver pair is now **double-buffered** — `cab_[2][2]`, one live pair and
one spare, plus an atomic `activeCabPair_` — and the split is ADR 003's, with the one
residual that ADR names **fixed rather than copied**:

| Thread | Does |
| --- | --- |
| MESSAGE | `prepareCabBuiltin` / `prepareCabCustom`: synthesise or copy the IR, peak-normalize a user one (M6.6 — never trust the file's level), `CabConvolver::prepare` **both sides of the spare pair**, then ARM. Every allocation the feature makes is here. |
| AUDIO | Notices the arm at the top of `process()`, runs the ordinary declick fade, and at the fade **zero** performs one CAS + one integer flip (`commitCabIfArmed`). No allocation, no lock, no file I/O, no `prepare()`, **no `free()`**. |
| MESSAGE | `retireCab()`: after it has *observed* the swap, `reset()` the retired pair; its heap is released (and reused) by the next message-thread prepare. |

That last row is the difference from the worklet, whose `_commitPending` still calls
`free()` on a removed pedal handle from inside `process()` — a documented bug, and
explicitly not a precedent. The handshake is a single atomic state word (Idle →
Preparing → Armed → Committing → Swapped → Idle); the message thread may take the
inactive pair back from `Armed` (a second click before the audio thread reached its
zero simply rewrites the pending IR), and it is the CAS that stops the audio thread
swapping into a pair being rewritten.

**Declick.** A swap rides the same 6 ms raised-cosine fade a chain edit does, and the
zero HOLD is widened to `max(kDeclickHoldSeconds, kCabSwapDeadSamples = 256)` for the
same reason the worklet holds: `CabConvolver::prepare()` zeroes the FDL, so a swapped-in
cab emits silence for up to two partitions and a fade-in would otherwise ramp that gap.
Measured in `clipper_chain_edit_test`, Clean 2×12 → Brit 4×12 landing mid-note:
**max step at the seam 0.002413 against a bound of 0.003017**, and the seam step is
*exactly* the settled signal's own 220 Hz slope — the fade contributes nothing. The same
switch spliced HARD steps **0.021781, 7.2× the bound**. Perturbation-proven: replacing
the arm with an immediate `commitCabIfArmed()` takes the seam to **0.060711, 20× the
bound**, and the case goes red.

**Latency does not move.** The partition stays 128 for every cab, built-in or user IR
(`clean212 264, brit412 264, custom 264` samples on the test rig), so switching cabs
never asks the host to re-align the track. Asserted in both native tests.

**State.** `cabModel` is an APVTS **choice parameter** (indices 0/1/2 == the C ABI's
built-in indices == web `CabChoice`), so it is automatable and round-trips with the
session. The custom IR's **path is NOT a parameter** — a host cannot meaningfully
automate a file path — it is a property of a `cab` child node of the state tree, exactly
like the board's `order`. Host automation of `cabModel` can arrive on the audio thread,
so the processor's APVTS listener does nothing but `triggerAsyncUpdate()`; the rebuild
happens in `handleAsyncUpdate()`.

**The file path** (`native/src/CabIrFile.cpp`) follows `web/src/cab.ts` deliberately:
mono-ise by **averaging** every channel (not "take channel 0"), resample to the engine
rate (`juce::LagrangeInterpolator` where the web uses an `OfflineAudioContext`), cap at
**4096** samples with a **128**-sample raised-cosine tail fade, and hand the samples to
the engine **un-normalized** — the engine normalizes, in the same place the C ABI does.

**Missing-IR fallback** is the web's convention (`App.tsx`: a rig that says
`cabModel:'custom'` with no IR behind it): fall back to the Clean 2×12 and *say so*
under the chip. One deliberate divergence — native **keeps the path** in the state tree
rather than clearing it, so a session opened before an external drive is mounted is
repaired by re-picking Custom rather than by hunting for the file again. Because that
retry request looks identical to the one that just failed, a *user* action bypasses the
apply-deduplication (`applyCabFromState`'s `force`); the dedup exists so that one click
is one declick fade rather than two (the click applies directly AND through the
listener's async hop).

**UI**: a chip under the Cab lever on the amp card, captioned CAB IR, labelled with the
current cab (a custom IR shows its file name), opening a popup with the two built-ins,
the loaded IR, and "Load IR…" (an async `juce::FileChooser`). Both themes; the fallback
note is the entire error surface — no dialog. `clipper_editor_snap` grew
`native_cab_*` scenes covering both built-ins, a loaded custom IR, the missing-file note
and the 1040×560 minimum window, in light and dark.

**Suites**: `clipper_identical_core` **untouched and green** — max |plugin−ref| =
`0.000e+00` on all eight cases, which is the proof that the default state (Clean 2×12,
no custom IR) still renders bit-identically to the pre-picker engine. `clipper_chain_edit`
green with the new cab case; new `clipper_cab_state` (33 checks) green. **CI note:** the
native job filters `ctest -R 'clipper_identical_core|clipper_chain_edit'`, which does not
match `clipper_cab_state` — add it to the filter in `.github/workflows/ci.yml`.

## 31. Audit "Test & process integrity" — the artifact staleness stamp, the golden blessing ritual, and a reproducible artifact

Two process holes from the 2026-07-24 audit, both of which had already cost something
real by the time they were fixed — plus the reproducibility defect found while fixing them.

### 31.1 `check-artifact.mjs` could not detect a stale artifact

`web/public/generated/clipper.js` is committed build output. The contract is stated in
CLAUDE.md — change `core/` or `web/worklet/`, run `bash scripts/build-wasm.sh`, commit
the artifact in the same commit — and the only thing standing behind it was a script
that called `existsSync` on two paths. It passed for an arbitrarily old artifact, and
it is the sole guard: `prebuild`, `npm test`, and the CI web job all run it.

**What it cost.** On 2026-07-24 two PRs each changed `core/` and each rebuilt
`clipper.js`. The merge conflicted on the binary. Taking either side would have
produced a `main` whose committed engine held **one** of the two fixes while the
committed source held **both** — a rig where a knob you can see in the source does
nothing in the browser — and nothing in the repo could have detected it. The correct
artifact, rebuilt from the merged source, is 173337 bytes; the two conflicting inputs
were 165971 and 172290.

**The fix.** `scripts/build-wasm.sh` now writes `web/public/generated/.build-stamp.json`
(committed, like the artifact) holding a SHA-256 over the *contents* of everything that
affects the artifact, plus a per-input hash map so a failure can name the culprit.
`check-artifact.mjs` recomputes it and fails on a mismatch. See ADR 004 for the four
design decisions inside that; the one worth repeating here is that the **emcc flags are
hashed out of `build-wasm.sh`'s marked region, not recorded into the stamp** — a
self-reported flag list cannot detect its own staleness.

**What is in the hash, measured rather than assumed.** `g++ -std=c++17 -MM` over the
exact 26 translation units `build-wasm.sh` hands to `emcc`, with the same
`-I core/include`, gives the artifact's real file closure:

| | count |
| --- | --- |
| files under `core/src/` compiled into the artifact | **26 of 26** (the emcc list is exactly `find core/src -type f`) |
| headers under `core/include/` in the closure | **36 of 37** (only `OutputLimiter.h` is outside it) |
| files from `core/tests/` or `core/tools/` in the closure | **0** |

So the hash covers `core/src` + `core/include` + the worklet + the flag region — 65
inputs — and deliberately excludes `core/tools/`, `core/tests/`, and `core/CMakeLists.txt`.
Excluding the test and tool trees is what keeps the guard from firing on every test
edit, which is how a guard gets deleted.

The check needs **no toolchain**: recomputing a content hash is pure Node, which is the
whole point, because the CI job that catches this has no emsdk. The recorded emcc
version is compared only when `emcc` happens to be on `PATH`, and a mismatch warns
rather than fails.

`EMSDK_VERSION` is also pinned, `latest` → **6.0.4**. Under `latest`, two machines with
identical source could not agree on the artifact bytes.

**A reproducibility finding that fell out of the bootstrap.** Rebuilding at the pinned
6.0.4 to generate the first stamp produced a file of exactly the same size (173337 B)
differing in exactly **64 bytes** — four 16-character runs, all inside *absolute build
paths embedded in the WASM*. They are `__FILE__` strings from live `assert()`s: the
emcc link uses `-O3` but never defines `NDEBUG`, so the asserts in `Oversampler.h`,
`RatModel.cpp`, `GoldModel.cpp` and `OverdriveEngine.cpp` are compiled into the shipped
engine together with the build directory of whoever produced it. Consequences: the
"6.0.4 reproduces the artifact byte-for-byte" claim holds only when building from an
identically-named directory; a future rebuild-and-`cmp` CI job needs
`-ffile-prefix-map` first; and there is an open question, not settled here, about
whether `assert()` belongs in a shipped real-time audio engine at all. The committed
artifact was therefore left untouched by this slice — only the stamp was added, and the
stamp attests *source content*, which is path-independent.

### 29.2 `update-goldens.sh` blessed any regression in one command, and measured the wrong thing

The goldens (`core/tests/goldens/`, docs §26 block C) are the only defence against
voicing drift and are `.wav` files a reviewer cannot read in a diff. Re-blessing one is
the easiest way in the whole repo to turn a regression into canon. The old script was
`cmake && ./tests --update-goldens`: no clean-tree check, no summary, no confirmation,
no justification.

Worse, the `--update-goldens` path in `core/tests/test_player_expectations.cpp` was

```cpp
if (update) writeGolden(r);
compareGolden(r);   // ← compares against the file it just wrote
```

so the ±1.5 dB third-octave gate could only ever see 16-bit storage quantisation
(≤0.11 dB). It was a guaranteed pass that silently rewrote the references. And because
`compareGolden` also asserts the frame count, a change that altered the render *length*
sailed through too.

**The fix, in the test:** measurement is now separated from the gate.
`measureAgainstGolden()` returns the deltas and asserts nothing; `compareGolden()`
applies the gate; and three modes exist —

- default: the gate, unchanged in behaviour;
- `--golden-report`: measure against the committed goldens and print one
  `GOLDEN-DELTA <name> <status> <rmsDb> <worstBandDb> <worstHz> <bands>` line per rig,
  writing nothing;
- `--update-goldens`: measure against the **previous** golden, print the same lines,
  *then* write. The post-write round-trip check is kept but is now labelled for what it
  is — a check of the wav write path, bounded by quantisation by construction, not a
  voicing gate.

`UNCHANGED` means within 0.15 dB, the measured storage + windowing floor. On the
current source all five rigs report 0.00–0.11 dB against their committed goldens, which
is both a null result for this slice and the calibration for that threshold.

The before/after was measured directly, by planting a wrong golden (`rat_jcm800.wav`
replaced with `muff_twin.wav` — same format and length, different audio) on a throwaway
commit. The pre-fix `--update-goldens` reported **worst band Δ 0.00 dB**, printed "within
the ±1.5 dB voicing gate", and rewrote the file. The post-fix `--golden-report` reports
**17.35 dB @ 800 Hz** (RMS +2.59 dB), flags it `CHANGED`, and writes nothing. A 17 dB
voicing error reported as 0.00 dB is what "compare against the file you just wrote" was
worth.

**The fix, in the script:** `scripts/update-goldens.sh` now requires (1) a clean working
tree, so the golden diff is reviewable on its own; (2) a printed per-golden before/after
table in dB, from `--golden-report`, *before* anything is written; (3) a confirmation
typed at a terminal — read from `/dev/tty`, which a pipe cannot answer (`yes | …` fails)
and CI cannot answer either (no controlling terminal), requiring the exact phrase
`bless N goldens` where N comes from the table; (4) a justification of ≥ 20 characters,
appended to the new `core/tests/goldens/GOLDENS.md` changelog and `git add`ed together
with the goldens. If nothing differs by more than the storage floor the script exits
early rather than churning five files for nothing.

Both confirmation defences were exercised against that planted wrong golden. With no
controlling terminal (the CI case) `yes | bash scripts/update-goldens.sh` prints the
table and aborts on the missing `/dev/tty`. Under a **real pty**
(`yes | script -qec 'bash scripts/update-goldens.sh' /dev/null`), where the `y` genuinely
arrives at the prompt, it aborts on the phrase instead — which is the load-bearing half:
the requirement is not "a tty exists" but "someone typed a sentence derived from the
table". Zero goldens written either way.

CI gains a PR-only `goldens` job as the backstop: a changed `.wav` under
`core/tests/goldens/` with no `GOLDENS.md` change fails the PR.

### Suites

Core **ctest 17/17**, `web` `tsc --noEmit` + `vite build` green, **Playwright 70/70**.
Node suites 45 → **57** (`test:server` 15, `test:history` 10, `electron` 20, and twelve
new cases in `test:scripts`): the new file is `web/scripts/check-artifact.test.mjs`, whose
cases build synthetic repo trees in a temp dir and run the real CLI against them via
`--repo-root`. Pointed at the pre-fix `check-artifact.mjs`, **8 of the 12 fail** —
including all three of the cases that matter (a `core/src` edit undetected, a diverged
worklet copy undetected, a missing stamp undetected). The four that pass are the two
asserting the check does *not* fire (a healthy tree; an edit under `core/tests/` or
`core/tools/`), which pass vacuously when there is no staleness check at all, and the two
that pin the toolchain sub-check as advisory, which call the exported `checkArtifact()`
directly rather than the CLI.

---

## 32. Audit perf item 2 — the halfband resampler's per-tap integer division

**The finding** (`docs/audits/2026-07-24-project-audit.md:309`, "Fidelity-neutral
performance wins", item 2). `HalfbandFilter.h` indexed its ring buffer as

```cpp
for (int p = 0; p < M; ++p)
    acc += e1_[p] * ring_[(w_ - p + M) % M];      // M = M_, a runtime int
```

`M_` / `L_` are runtime `int` members, so the compiler cannot strength-reduce `%`
to a mask even though the value happens to be a power of two: it must emit a
hardware integer division **per tap**. That is 64 divisions per output sample on
the interpolator's tight stage and 65 on the decimator's, at the **oversampled**
rate, for every oversampled pedal (RAT, SD-1, Screamer, Muff, Gold) and — once
per triode stage, so up to eight times over in the JCM800 preamp — every valve
amp. It is the hottest loop in the project, and the audit called it "best
perf-per-effort item in the project".

### The fix: a doubled ring buffer

Allocate `2*M` (resp. `2*L`) floats and maintain the invariant

```
ring_[i] == ring_[i + N]     for all i in [0, N)          (N = M_ or L_)
```

by writing every incoming sample **twice**. The wrapped read then becomes the
unwrapped `ring_[w_ + N - p]`, which is in range for every tap, and `w_` advances
with a compare (`if (++w_ == N) w_ = 0;`) instead of a `%`. The decimator
additionally swaps `std::vector<Tap>` — a struct of `{int, float}`, so the
coefficient stream was strided by 8 bytes — for parallel `int` / `float` arrays.

Two properties are load-bearing and are called out in the header, because both
are the kind of thing a later "tidy-up" would break silently:

1. **Every write must update both copies, including `reset()`.** A half-cleared
   ring keeps re-injecting stale history for a whole filter length — correct
   output for a while, then a ghost. The extra store is a rounding error next to
   a 64-tap FIR and costs nothing in the tap loop.
2. **The tap loop still walks `p` upward, i.e. reads memory backwards.** Float
   addition is not associative. Reversing the loop into a forward-streaming one
   (or reordering the coefficients to match) is mathematically identical and
   **not bit-identical** — measured below.

No SIMD, no reassociation, no `-ffast-math`. This is a data-structure change and
nothing else.

### Bit-identity (the acceptance criterion)

`core/tests/test_halfband.cpp` / ctest target **`clipper_halfband_tests`** carries a
verbatim copy of the pre-change (modulo-indexed) implementation — plus a copy of the
`Oversampler` cascade rebuilt on it — and compares the shipped classes against it
**bit-for-bit**, not within a tolerance. `==` is not sufficient (it calls `+0.0`
equal to `-0.0` and every NaN unequal to itself), so the comparison is a `memcmp`
of the float representations.

| comparison | samples | differing bits | max abs diff |
|---|---|---|---|
| interpolator, tight (M=64) | 480 000 | **0** | 0.0 |
| interpolator, relaxed (M=16) | 480 000 | **0** | 0.0 |
| decimator, tight (L=129) | 120 000 | **0** | 0.0 |
| decimator, relaxed (L=33) | 120 000 | **0** | 0.0 |
| full cascade 1× / 2× / 4× / 8×, ragged blocks | 200 000 each | **0** (up **and** down) | 0.0 |
| after NaN poisoning + `reset()`, vs a virgin instance | 20 000 × 3 factors | **0** | — |

The cascade cases drive the oversampler the way a pedal does (upsample → edit the
buffer in place → downsample) over **ragged** block sizes `{1, 7, 128, 31, 200, 3,
65, 256, 17, 100}`, so the ring wrap lands at every offset instead of always on a
128 boundary, and they compare the *upsampled buffer* as well as the round trip.
The program signal is deliberately hostile: a 12-harmonic plucked riff, a
full-scale square burst every 0.37 s, LCG noise at −40 dBFS, lone full-scale
impulses, and stretches of exact silence (where a stale ring copy shows up as a
ghost). End to end, `clipper-render --alias-report` is identical to the digit
before and after, and the goldens were not touched.

### Measured speedup

`clipper-bench`, 6 s riff @ 48 kHz in 128-frame blocks, best of three runs on one
idle machine, same binary flags — the only difference is the header. A new
`os2x` / `os4x` / `os8x` unit benches the resampler **alone** (up → one multiply
per oversampled sample → down) so the change is neither diluted nor hidden by
whatever nonlinearity sits inside the domain.

| unit | before (× realtime) | after (× realtime) | speedup | % of one 48 k stream |
|---|---|---|---|---|
| **os2x (up+down only)** | 71.5× | **207.3×** | **2.90×** | 1.40 % → 0.48 % |
| **os4x (up+down only)** | 44.6× | **121.5×** | **2.72×** | 2.24 % → 0.82 % |
| **os8x (up+down only)** | 25.4× | **67.3×** | **2.65×** | 3.94 % → 1.49 % |
| sd1 (overdrive) | 36.9× | 80.6× | 2.18× | 2.71 % → 1.24 % |
| screamer (overdrive) | 37.3× | 80.7× | 2.16× | 2.68 % → 1.24 % |
| rat (dist pedal) | 29.7× | 53.5× | 1.80× | 3.37 % → 1.87 % |
| gold (clean-blend drive) | 31.1× | 56.1× | 1.80× | 3.22 % → 1.78 % |
| jcm800 (valve amp) | 1.65× | 1.88× | 1.14× | **60.6 % → 53.3 %** |
| twin (valve amp) | 2.50× | 2.86× | 1.14× | 40.0 % → 35.0 % |
| ac30 (valve amp) | 3.03× | 3.39× | 1.12× | 33.1 % → 29.5 % |
| muff (BJT fuzz) | 3.91× | 4.15× | 1.06× | 25.6 % → 24.1 % |
| ninety (phaser) — *control, not oversampled* | 255.1× | 253.0× | 1.00× | unchanged |
| cab / reverb / limiter / clean amp — *controls* | — | — | 1.00× | unchanged |

Reading it: the resampler itself is ~2.7× faster; the two-diode overdrives, whose
cost is nearly all resampling, come within a whisker of that; the valve amps gain
~13 % overall because their budget is the tube Newton solve, not the filters — but
13 % of the JCM800 is **7 percentage points of one realtime stream**, the single
biggest headroom win available for the effort. The unchanged control rows are the
evidence that nothing outside the oversampled path moved.

Whether the divisions are worth ~2.7× or the audit prototype's ~3.3× depends on the
host's divider latency; the ranking and the bit-identity are the durable results.

### Stopband and latency: unchanged, and measured rather than restated

Bit-identity makes both trivially unchanged, but block A alone cannot catch a wrong
*filter* (it compares the same coefficients through two indexing schemes — perturbing
`kBeta` to 5.0 leaves block A green, see below), so blocks B and C carry absolute,
player-observable references measured through the live objects.

**Stopband**, as worst-case **image** rejection (interpolator: a base-rate tone `f`
puts an image at `baseRate − f` in the doubled-rate stream) and worst-case **alias**
rejection (decimator: a high-rate tone folds to `|f − outRate|`), swept across the
whole audio band up to 20 kHz:

| design | worst image | worst alias |
|---|---|---|
| tight, L = 129 (first 2× stage, 44.1 k base) | **−86.1 dB** | **−87.3 dB** |
| relaxed, L = 33 (later stages, ≥ 88.2 k in) | **−88.5 dB** | **−87.9 dB** |

These are better than the audit's quoted −79.8 / −78.7 because the measurement is
band-limited to what a guitar rig actually carries: the worst case over
*frequencies that matter* (images and aliases landing at or below 20 kHz) rather
than over the whole stopband including the 20–22.05 kHz sliver. Both definitions
describe the same untouched filter design; the test asserts ≤ −78 dB so it is
compatible with either.

**Round-trip group delay**, measured from the composite impulse response by phase
slope (the argmax is an arbitrary pick between two near-equal peaks when the true
delay is fractional, and an energy centroid is biased unless that fraction is
exactly 0.5):

| factor | `latencySamples()` reports | measured / structural true delay | over-report |
|---|---|---|---|
| 1× | 0 | 0 | 0 |
| 2× | 64 | **63.500** | 0.500 |
| 4× | 72 | **71.250** | 0.750 |
| 8× | 76 | **75.125** | 0.875 |

**This is a new finding, not a regression** — block A proves the filters are
bit-identical, so it was always true. Stage `s` contributes `M` samples of
interpolator delay and `M − 1` of decimator delay at its own doubled rate, i.e.
`(2M − 1) / 2^(s+1)` base samples; the missing 1 per stage is the decimator
emitting on the **second** sample of each pair. `latencySamples()` uses
`(2M) >> (s+1)` and therefore over-reports by up to 0.875 base samples — 18 µs at
48 kHz, inaudible, and it errs in the safe direction (the reported latency is never
short). It is left alone here: the API returns an `int`, and the number is
surfaced in the web UI and in the JUCE plugin's latency reporting, so changing it is
a plumbing slice, not a resampler slice. The test now pins the measured delay to
0.01 samples and pins the reported value to within one sample of it, so neither can
drift unnoticed.

### Proving the test has teeth

Per CLAUDE.md, each perturbation was applied in the tree, rebuilt (with a `touch`
after both patch and restore, or `make` measures stale code) and observed to fail:

| perturbation | result |
|---|---|
| drop the mirrored `ring_[w_ + M] = in` write | **block A fails** — 329 277 / 480 000 samples differ, max abs diff 1.6 |
| reverse the tap loop (`p` descending, reads streaming forward) | **block A fails** — 183 049 / 480 000 samples differ, all by ~1 ULP (`max abs diff` prints as 0.0 at one decimal). This is precisely the change a tolerance-based test would wave through, and it is why the comparison is a `memcmp` |
| `kBeta` 7.857 → 5.0 | **block B fails** — image rejection −86.1 → −62.3 dB. Block A stays green, which is the point: identity alone cannot catch a wrong filter |
| `kRelaxedM` 16 → 20 | **block C fails** — `latencySamples()` 72 → 74, measured delay 71.25 → 73.25 |

### Notes for the next slice

- `core/` changed, so the committed WASM artifact must be rebuilt
  (`bash scripts/build-wasm.sh`) in the same change. WASM has no integer-division
  strength reduction to fall back on either, so the win should carry — but it is a
  `div` instruction on a different machine and has not been measured in-browser.
  The DSP load meter (§25.4) is the place to confirm it.
- **Still open, and now quantified:** `Oversampler::latencySamples()` over-reports
  by up to 0.875 base samples (table above).
- Observed while verifying: the `--alias-report` table in **§7** no longer matches
  what the tool prints (`−17.6 / −16.2 / −89.7 / −91.9`, fund-amp 0.40564 at 4×, vs
  §7's `−18.4 / −26.7 / −86.6 / −90.6` and 0.46202). That drift is
  **pre-existing** — the before and after builds print the same table to the digit
  — and dates from the RAT re-voice / input calibration of §11.1. Not corrected
  here, because attributing it belongs with whichever slice moved it.
- Audit perf item **1** (one oversampler per preamp instead of one per triode
  stage) is untouched and is now the biggest remaining resampling win: it removes
  ~4× of the *calls* rather than making each call cheaper, and halves the JCM800's
  7.5 ms latency. It is a fidelity change (less repeated band-limiting), so it
  needs its own slice and its own argument.

---

## 33. Audit finding 11 — the anti-denormal guards the policy never reached

**Slice:** `fix/denormal-guards` (2026-07-25). **ADR 006.** Fidelity-neutral: measured max
deviation over the whole lineup is **1.0151e-30** (−600 dB), every sample of it inside a
silent tail. Goldens untouched. Core ctest 24/24.

### The situation

§25 wrote the anti-denormal policy and `core/include/clipper/dsp/Denormal.h` to enforce it:
WASM has **no flush-to-zero at all**, so a recursive state that asymptotes toward zero sticks
in the subnormal range forever and becomes a permanent audio-thread denormal generator. §25
then applied it to `Biquad` and `OnePoleSmoother` and stopped.

The policy said *every* recursive accumulator. Many models later it had reached about half of
them — and the tool built to watch for exactly this, `scripts/denormal_bench.cpp`, **only
exercised the two classes §25 had already fixed**. So it reported a reassuring ~1.00× cliff on
every run while a RAT ran **1.97× slower on silence than on a full-scale riff** and GOLD pushed
**393 607 subnormal samples per 10 s of silence** into whatever came after it.

That is the transferable lesson, and it is the same one as §29: *a benchmark that only covers
the code you already fixed measures your fix, not your product.* The benchmark now has a
section 2 that drives whole units.

### How to tell a denormal cost from any other cost

Compare a unit's silence time against **the same unit's silence time with hardware FTZ+DAZ
forced on**, not against its signal time. FTZ changes nothing except the cost of subnormal
arithmetic, so `silence >> hwFTZ silence` is a denormal cost and nothing else can explain it.

Comparing silence against signal is the trap: a unit can be legitimately cheaper or dearer on
silence because the tube and BJT Newton solves converge in a different number of iterations
when nothing is moving. The **Muff** is the worked example — 3.01× dearer on silence than on
signal, and **no denormal problem at all**, because its hwFTZ column shows the same 3.01×.
(That cost is `BjtStage`'s Ebers-Moll Newton near the quiescent point. Separate question,
recorded here so nobody "fixes" it with a flush.)

### Measured, per 10 s at 48 kHz in 128-frame blocks (`build/denormal_bench`)

| Unit | signal | silence **before** | hwFTZ silence | silence **after** | subnormal out **before → after** |
| --- | --- | --- | --- | --- | --- |
| RAT | 328 ms | **647 ms (1.97×)** | 305 ms | **309 ms** | 0 → 0 |
| GOLD | 324 ms | **629 ms (1.94×)** | 296 ms | **305 ms** | **393 607 → 0** |
| SD-1 | 291 ms | 341 ms (1.17×) | 267 ms | **272 ms** | **429 236 → 0** |
| Screamer | 291 ms | 355 ms (1.22×) | 270 ms | **268 ms** | **428 761 → 0** |
| AC30 (full amp) | 3177 ms | 1982 ms | 1584 ms | **1603 ms** | **1588 → 2** |
| Processor (GAIN 0) | 42 ms | 23 ms | **2 ms** | **4 ms** | **427 187 → 0** |
| JCM800 / Twin | — | — | (equal) | unchanged | 0 → 0 |
| Muff | 2280 ms | 6855 ms | 6780 ms | unchanged | 0 → 0 (not denormals) |

Every fixed unit's default-environment silence time now **meets its hardware-FTZ floor**,
which is the whole point: on WASM the in-code flush is the only FTZ available.

The two residual AC30 samples are a float-cast transient during the ring-down (a normal
`double` secondary voltage whose `float` cast lands under 1.18e-38 for two samples), not
parked state — the cliff itself is gone.

**The `subnormal out` column is the part that matters beyond CPU.** A pedal emitting subnormal
floats makes every stage *after* it slow too, so SD-1 / Screamer / GOLD were each handing the
amp and the cab ~429 000 subnormal samples per 10 s of silence. It is a chain-wide cost, and it
was invisible because subnormals are numerically fine — they are just slow.

> Note on the SD-1 / Screamer figures: they read **0** subnormal output until the benchmark was
> fixed to set their knobs. `prepare()` snaps the LEVEL smoother onto a 0 target, so a pedal
> left at defaults renders digital silence into its own output and the column is meaningless.
> Set every knob explicitly when benchmarking these.

### What was unguarded, and what it cost

1. **The output DC blockers** — `dcY1_` in `OverdriveEngine::processChunk` (SD-1, Screamer)
   and `GoldModel::processChunk`. Every *other* one-pole state in those same loops was already
   flushed; this one was missed. On silence the recursion degenerates to `y = dcR_ * dcY1_`
   with `dcR_ ≈ 0.9984`, and in the subnormal range **that product rounds back to itself**, so
   the state never reaches zero. `dcX1_` needs no guard: it is an input history (assigned,
   never fed back).

2. **The `chowdsp` WDF capacitor** (`RatModel`, `GoldModel`) — the RAT's entire 1.97×. See
   ADR 006 for why the guard goes through the library's public API rather than a fork.

3. **The three valve tone stacks' cap companions** — `FenderToneStack` (Twin),
   `MarshallToneStack` (JCM800), `TopBoostToneStack` (AC30). Isolated and fed true digital
   silence these measured **18.6×** and **68.2×** against hardware FTZ, the two largest
   denormal cliffs in the core. The AC30 stack measured a clean 1.00× at knobs-at-noon but was
   guarded anyway: its poles move with the knobs, and the `vC_`/`iC_` pair for its series input
   coupling cap is a state the other two stacks do not even have (it was not on the audit's
   list — the audit enumerated the states the *other* stacks share).

4. **The AC30 power section's OT bandwidth pair** — `otLpS_`/`otHpS_`. Bisected in
   `Ac30PowerAmp.cpp`; see below, because the result is not where you would guess.

5. **`core/src/Processor.cpp`** — the one place in the tree that hand-rolls the
   `OnePoleSmoother` recurrence instead of using the guarded primitive, so it never inherited
   the guard. With GAIN at 0 the value asymptotes toward zero and sticks subnormal (its own ULP
   shrinks with it, so the increment can never close the gap), and then **every sample is
   multiplied by a subnormal, forever, including full-scale audio**: 427 187 subnormal output
   samples and an 11–21× slowdown against FTZ. It now uses `OnePoleSmoother::next()`'s rule
   verbatim — snap on the **residual**, not the value — so the duplicate and the primitive
   finally agree. Collapsing the duplicate entirely is a refactor for another slice.

### Three things the measurements corrected, that guessing would not have

**(a) `TriodeStage`'s coupling caps do NOT decay to zero.** The audit's list expected `vCc_`
to. It does not: the grid-leak node parks at the grid-current bias point, so `vCc_` measures
**8.15e-4 V after 20 seconds of digital silence**, smallest nonzero magnitude 1.40e-5, zero
subnormal blocks. `vCk_` parks at `vkQuiescent_` and `vCo_` at `vaQuiescent_` by construction.
So the most-executed loop in the entire core correctly has **no flush at all**, and
`TriodeStage.cpp` does not even include `Denormal.h` — with a comment saying why, and pointing
at `couplingCapVoltage()`, which is public precisely so the claim can be rechecked.

**(b) A guard that cannot fire is not free.** The general rule this slice adds: a state that
rests at a **nonzero DC operating point** — a cathode cap at its bias voltage, a B+ rail, a
screen node, a sag envelope at idle draw, a Newton warm start at 100–300 V — can never be
subnormal, so it gets a comment rather than a flush. Measured, not assumed, at every such site.

**(c) The AC30's cliff was in the OT states, not the TOP CUT states.** The TOP CUT one-poles
filter `va1AC = va1 − quiescentPlate1()` and look like the obvious candidates. They are not:
they rest at **2.84e-14** (one ULP of the LTP plate voltage — the Newton fixed point does not
land exactly on the quiescent value), so they never go subnormal. The OT pair does, because
this amp has **no global NFB loop**: its secondary settles at *exactly* zero on silence, while
the JCM800's and the Twin's idle at ~1e-28 because their feedback loops keep a residual alive.
Bisected on the isolated stage:

| `Ac30PowerAmp` flushes | silence | hwFTZ | ratio | subnormal out |
| --- | --- | --- | --- | --- |
| none | 866 ms | 641 ms | **1.35×** | 1588 |
| TOP CUT only | 834 ms | 629 ms | **1.33×** | 1588 |
| OT pair only | 638 ms | 644 ms | 0.99× | 2 |
| both (shipped) | 638 ms | 644 ms | 0.99× | 2 |

The corollary is that the JCM800 and Twin power sections had **no** measured denormal cost
(3038 vs 3010 ms; 2025 vs 2007 ms). Their OT / presence / feedback flushes are kept as
guard-rails — `parkState()` *does* set those states to exactly zero, so a post-`reset()` amp
fed silence is the case that could bite — but they are labelled as guard-rails, not fixes.

### The masking effect, which is why this needs guarding even where it does not bite today

`FenderToneStack` standalone reaches exactly zero and measured 18.6×. Wired up inside
`TwinPreamp` it never gets there: V1 is a tube stage whose own coupling-cap state parks at a
nonzero bias point, so it feeds the stack a small but **normal** residual forever instead of
true digital silence. Measured inside the preamp, the stack idles at **5.7e-11** and the
composed preamp measures **0.99×** — no cliff.

So the bug is real, the fix is real, and today it is masked in-product by an upstream DC
residual. That masking is incidental: it depends on a tube stage's idle offset and disappears
the moment the stack is driven by anything that emits exact zeros — a bypassed stage, a muted
chain, a `reset()`. Guard the component; assert the property where it holds unconditionally.

### Proving fidelity-neutrality

`flushDenormal` only acts below 1e-30 (−600 dB, ~456 dB under the 24-bit LSB), so the claim is
that no audible trajectory reaches it. That was measured rather than argued, three ways:

1. **Whole-lineup A/B.** All eleven units (six pedals, four amps, `Processor`) rendered over
   program audio at knobs **min / noon / max**, pre-guard vs post-guard, compared byte for
   byte. **99 671 of 4 752 000 samples differ; max |Δ| anywhere = 1.0151e-30**, i.e. the guard
   floor. GOLD, SD-1, Screamer, Muff, Phaser, Clean 120, JCM800 and Twin are **bit-identical
   everywhere**. The differences are RAT's silent tail (32–55 samples), `Processor` at GAIN 0
   (the intended fix — exact zero instead of 1.7e-43), and the AC30 at VOLUME 0, whose entire
   render peaks at 1.475e-15 and is therefore below the guard floor end to end.
2. **The goldens and `clipper_player_expectations_tests`** — including its bit-identical
   (tol 0.0) block B — pass **untouched**.
3. **In-suite bit-transparency assertions** using `==`, not a tolerance: the WDF network
   against a real unguarded `chowdsp` network, and `Processor` against the verbatim pre-guard
   recurrence over 96 000 samples.

### The tests, and how they were made to bite

`clipper_denormal_tests` gains a block 2. It could not simply assert on output samples the way
block 1 does, because **most of block 2's state is `double` and a double subnormal is invisible
in the audio** — the models emit a float cast of a double node voltage, and `float(1e-310)` is
exactly `0.0f`. An output-only test cannot distinguish a flushed model from one grinding
through subnormals forever. That is the mechanism by which this defect survived both a green
suite and a clean benchmark.

So the affected classes expose one const diagnostic, `double maxAbsRestingState()` — the
largest |value| among the states whose value **at rest is zero**, i.e. exactly the states
`Denormal.h` must flush — and the test asserts it is **exactly 0.0** after a silent tail. It
deliberately excludes state that rests at a nonzero DC point, and it is documented per class
with the measurement behind the exclusion. Same role as `lastOutputPeak()` / `lastMaxIters()`;
not used by the audio path. `TwinPowerAmp`, `Jcm800PowerAmp` and the composed `TwinPreamp`
deliberately **do not** get one, because nothing in them rests at zero and the assertion would
have had no teeth.

Every new assertion was then perturbation-checked — remove exactly one flush, rebuild, require
the suite to go red (`touch` after both patch and restore, per §29's hard-won note):

| guard removed | result |
| --- | --- |
| `OverdriveEngine` `dcY1_` | suite fails |
| `GoldModel` `dcY1_` | suite fails |
| `FenderToneStack` companions | suite fails |
| `MarshallToneStack` companions | suite fails |
| `TopBoostToneStack` companions | suite fails |
| `Ac30PowerAmp` OT low-pass | suite fails |
| `Processor` residual snap | suite fails |

The WDF test carries its own teeth internally: it asserts that the **unguarded reference
network really does go subnormal**, so if a future `chowdsp` bump changes that, the test says
"this no longer reproduces finding 11" instead of passing vacuously.

### Still open after this slice

- **The Muff's 3.01× silence cost** is `BjtStage`'s Ebers-Moll Newton, not denormals
  (confirmed: hwFTZ shows the same ratio). Its own perf slice.
- **`Processor` still duplicates `OnePoleSmoother`.** The recurrences now agree exactly, but
  the duplication remains; collapsing it touches `Processor`'s prepare/reset/parameter seams.
- **`web/public/generated/*` must be rebuilt** — `core/` changed, so the committed WASM
  artifact is stale until `bash scripts/build-wasm.sh` runs. This slice fixes a cliff that is
  *worst* on WASM (no FTZ at all), so it is worth nothing to a player until the artifact ships.

---

## 34. Audit finding 12 — the Muff cost more CPU when you were *not* playing

**Slice:** `perf/muff-newton-earlyout` (2026-07-25). **Touches:** `core/src/dsp/BjtStage.cpp`,
`core/include/clipper/dsp/BjtStage.h`, `core/include/clipper/dsp/TubeSolverMode.h`,
`core/tests/test_muff_model.cpp`, `core/tests/test_tube_solver.cpp`. No component value, no
Ebers-Moll change, no damping-strategy change. Goldens untouched.

### The symptom, and why the two obvious explanations were both wrong

The audit measured the Muff at **59 % of one core on silence vs 20 % while playing** — 3.2×
*more* expensive idle than in use. It ruled out the two things one would reach for first:
forcing hardware FTZ/DAZ changes nothing (so not denormals), and the iteration count does not
rise (so not extra Newton iterations).

Both rebuttals hold up, and the iteration count is even more damning than the audit thought.
On this build a parked stage reports **1** Newton iteration (0 once the off-by-one below is
fixed) against **8–13** on signal. Idle did *strictly less* Newton work than playing and still
cost ~2.7× the wall time.

### The cause: an unsatisfiable line search

`dampedNewton` accepts a trial step only on a **strict** residual decrease:

```cpp
if (infNorm3(rt) < cur * (1.0 - 1e-4 * lam)) break;
lam *= 0.5;
```

and its only exit was on **step size** (`|lam·dx| < 1e-9` V). There was no residual-based
early-out. At the quiescent point the residual is already at the floating-point floor, so *no*
trial step can make it strictly smaller — the search runs all 30 backtracks, every iteration,
and each backtrack is a full Ebers-Moll + diode system evaluation with 4 `std::exp` calls.

Instrumented, per solve (4 stages × 4× oversampling × every sample):

| input | system evaluations / solve | iterations exhausting all 30 backtracks |
|---|---|---|
| silence | **31.00** | **100 %** |
| tiny (0.001) | 4.10–5.41 | 1.5–2.1 % |
| hot DI (0.20) | 6.05–6.65 | 1.9–2.0 % |
| ±20 V slam | 9.92–13.47 | 6.2–10.9 % |

31 evaluations, 124 `exp` calls, per stage per sample, to reproduce an answer already in hand.

### The fix, and the tolerance

A residual-norm early-out at the top of the iteration: if the KCL residual ∞-norm is at
tolerance, return without solving or line-searching. The residuals are **node currents**, so
the tolerance is a **current in amps** — `kNewtonResidualTolA`, scaled by the existing
`tubeSolverTolScale()` rather than a second knob (`TubeSolverMode.h`'s scope note now covers
the BJT solver too).

The audit also suggested short-circuiting the backtracking when `cur` is already at tolerance.
**That is dead code** once the top-of-loop check exists — `cur` is `infNorm3(r)` with `r`
unchanged between the two points, so the branch is unreachable. Not added.

**This is an accuracy trade, and the slice's main finding is that it cannot be otherwise.**
The pre-fix solver drove the residual all the way to the floating-point floor, so *any*
early-out that fires declines refinement it performed. Bit-identity was the acceptance bar and
it is unreachable. Swept against the pre-fix solver over 25 renders (5 sustain × 5 input
levels, 2 s each at 48 kHz, byte-compared):

| `kNewtonResidualTolA` | fires when parked? | worst difference vs the pre-fix solver |
|---|---|---|
| 1e-13 | yes | −81.8 dBFS abs / −89.0 dB rel |
| 1e-14 | yes | −104.3 / −111.8 |
| 1e-15 | yes | −108.5 / −116.0 |
| 1e-16 | yes | −124.3 / −129.5 |
| **1e-17** | **yes** | **−127.4 / −134.1** ← chosen |
| 1e-18 | **no** | −132.5 / −137.7 |
| 1e-19 | **no** | bit-identical (never fires) |

The floor of the window is the **idle residual ceiling**: the largest residual a parked stage
ever presents, measured at **2.0600e-18 A**. Below that the early-out stops firing and the
pathology returns. That ceiling is remarkably stable — 2.0600e-18 to five figures at every one
of 44.1/48/88.2/96/192 kHz × 1/2/4/8 oversampling × sustain MIN/mid/MAX — because at the parked
point the two large companion terms `gCin·(vin−Vb)` and `histCin` cancel exactly, leaving the
residual set by the DC branch currents (~0.9 mA scale) and not by `gCin = Cin/T`. So the margin
does not erode with rate or oversampling factor.

**1e-17 is the tightest value that still fires**: 4.9× above the idle ceiling, and **7.4 dB
inside the project's established −120 dBFS solver-accuracy gate** — the same bar every valve
solver's early exit is held to (§25). Per solve the error is 1e-17 A ÷ ~1.9e-4 S ≈ **53
femtovolts** of node voltage; the −127 dBFS output figure is that 53 fV amplified by the Muff's
four cascaded high-gain stages and accumulated through their recursive state, i.e. a
cascade-gain figure rather than a per-solve one.

**The DC operating-point solve deliberately opts out** (`tol = 0.0`). It runs once inside
`prepare()`, so early-outing it buys nothing measurable — and it is not free, because its final
iterate seeds `vbQ_`/`vcQ_`/`veQ_` and `settleDC()`, so stopping it earlier shifts the quiescent
point every render is referenced to. Measured, that shift contributed ~3× more output
difference than the per-sample early-out alone. `tol = 0.0` makes `cur <= tol` fire only on an
exactly-zero residual, where the Newton step is exactly zero, so opting out preserves the old
behaviour exactly rather than approximately.

### Secondary fix: an iteration count above its own cap

`dampedNewton` returned `it + 1`, which over-reports by one when the loop exhausts `maxIter`.
This was not hypothetical: a ±20 V slam at 96 kHz × 4 made `lastMaxNewtonIterations()` report
**61** against `kMaxNewtonIter == 60`. It now counts iterations that actually moved the iterate,
so a parked stage reports 0 and an exhausted loop reports exactly 60.

### Measured results

**Silence vs signal**, 10 s of audio at 48 kHz in 128-frame blocks, interleaved before/after on
an idle machine, two passes (wall ms):

| sustain | input | before | after | speedup |
|---|---|---|---|---|
| 0.00 | silence | 6797–6820 | 498–501 | **13.6×** |
| 0.00 | hot 0.20 | 2375–2511 | 1857–1863 | 1.28× |
| 0.60 | silence | 6871–7002 | 494–498 | **13.9×** |
| 0.60 | hot 0.20 | 2479–2534 | 2007–2108 | 1.22× |
| 1.00 | silence | 6837–6982 | 488–534 | **13.4×** |
| 1.00 | hot 0.20 | 2598–2653 | 2114–2186 | 1.21× |

**Silence/signal ratio 2.58–2.87× → 0.23–0.27×.** Idle is now *cheaper* than playing, which is
the expected ordering; the audit's target of ~1.0 is beaten because the fix removes 30 of 31
evaluations rather than merely equalising. System evaluations per solve on silence:
**31.00 → 1.00**. Signal also improves (1.21–1.28×) because program material spends a lot of
its time in decayed, already-converged samples.

`clipper-bench --unit muff`: **3.81–3.93× realtime / 25.4–26.2 % → 4.78–4.94× / 20.3–20.9 %**
(§25.3 table updated).

### What is pinned, and proved to have teeth

- **`clipper_muff_tests` → `testIdleSolverCost`** — a parked stage does **0** Newton iterations,
  across 48 rate × oversampling × sustain combinations. Asserted as a solver-work count, not a
  wall-clock time, so it is not flaky on a shared CI box. It is checked after driving real
  signal through first, so it covers a stage that fell quiet after being played rather than one
  sitting on its `prepare()`-time park.
- **`clipper_tube_solver_tests`** — the Muff joins the production-vs-reference-mode −120 dBFS
  gate. Reference mode scales the tolerance to 1e-20, *below* the idle ceiling, so the early-out
  never fires there and the reference render **is** the pre-fix solver: this measures the
  early-out's true audio cost on every run. Measured −138.5 to −228.8 dBFS across sustain
  MIN/mid/MAX at hot-DI and loud input.
- **Perturbation check** (both directions, confirmed red then restored green):
  `kNewtonResidualTolA = 1e-19` → `clipper_muff_tests` RED (early-out stops firing);
  `= 1e-11` → `clipper_tube_solver_tests` RED (truncates real work); restoring the `it + 1`
  over-report → `clipper_muff_tests` RED.

### Two traps this slice hit, recorded so the next person does not

1. **A whole bit-identity sweep was vacuous.** `MuffModel`'s `PARAM_VOLUME` smoother defaults
   to **0**, so a Muff driven only through `PARAM_SUSTAIN` renders **digital silence** — and 25
   of 25 renders compared byte-identical for the wrong reason. The `assert(peak > 0.01)` guard
   in `test_tube_solver.cpp` is what caught it and is commented to stay. Any harness that
   renders a pedal must assert its output is non-silent before comparing anything.
2. **The worst case was not the obvious one.** The early-out's audio cost at hot-DI level is
   −228 dBFS; at a *loud* input (riff ×5) and sustain 0.70 it is −127 dBFS, 19 dB worse. A test
   that only ever drove hot DI would have reported a number 100 dB better than the truth. The
   gate now drives MIN/mid/MAX sustain at **both** levels.

### Newly found, left as an XFAIL

Widening the old ±10 V single-oversampling-factor slam test to **±20 V across every rate ×
factor** exposed a pre-existing defect: **6 of 16 combinations exhaust the 60-iteration cap**
(2× oversampling at all four base rates, and 4× at 88.2 and 96 kHz). Output stays finite and
bounded — this is not the old cascade blow-up — but at those samples the solve has not
converged, so the audio there is not the circuit's answer. Confirmed pre-existing by measuring
the identical counts on the pre-early-out solver (which reported the cap as 61). The shipped
desktop path (4× at 44.1/48 kHz) converges in 17–18 iterations, but 96 kHz × 4 is a real user
configuration. Recorded as `muff-slam-exhausts-newton-cap` in `clipper_muff_tests`' XFAIL
ledger; **do not** paper over it by raising `kMaxNewtonIter`, which buys iterations rather than
convergence.

### Not affected

The control-rate parameter-sampling XFAIL still measures the Muff at **1.473 absolute inside
25 ms**, unchanged — it is about which smoothed value a chunk keeps, not about the solver, so
it stays open. The valve solvers (`TriodeStage`, the three power amps) use a plain step-size
exit with **no** backtracking line search, so finding 12 does not apply to them.

**The WASM artifact must be rebuilt** (`bash scripts/build-wasm.sh`) — `core/` changed.

---

## 35. Audit finding 6 — parameter smoothing on the valve amps

**Slice:** `fix/valve-amp-smoothing` (2026-07-25). **Touches:**
`core/include/clipper/dsp/OnePoleSmoother.h`, the three valve preamps
(`Jcm800Preamp`, `TwinPreamp`, `Ac30Preamp`) and their tone stacks,
`core/tests/support/StepSlew.h`, `core/tests/test_param_smoothing.cpp`.

**The finding** (audit finding 6). `OnePoleSmoother` was used by the pedals, the clean
`AmpModel`, chorus, phaser and reverb — and by **none** of `Jcm800*`, `Twin*`, `Ac30*`,
which assigned knob values straight to state. Both `web/worklet/clipper-processor.js`
and `native/src/ClipperEngine.h` carry a comment saying *"Plain knob moves are NOT
bracketed — the core's ~5 ms one-pole smoothing already declicks those."* That was true
for pedals and **false for exactly the three flagship amps**, which sit behind up to
76 dB of gain. In the web app a knob drag pushes values at pointer rate, so it was a
continuous stream of these steps.

### What the metric is, and why the audit's version was not enough

The project's own definition (`native/tests/chain_edit_test.cpp`): a steady 220 Hz
sine, the largest sample-to-sample step in a window around the knob move, divided by
the signal's own steady-state slew measured before it. `core/tests/support/StepSlew.h`
now holds it once, shared. Two refinements the audit's table needed:

* **Worst of 16 successive step positions.** A step delivered at an output zero
  crossing produces no discontinuity at all, so a single fixed position measures luck
  rather than the amp. This is the same lesson as the §29 "land the change mid-render"
  rule, applied to a parameter instead of a topology change.
* **Two step sizes** — a `0.05` arrow-key step (`KEY_STEP` in `Knob.tsx`) and a `0.40`
  preset/assistant-sized jump, because they exercise different parts of the ramp.

### Measured

The four knobs the audit named, its figure → this slice's 0.40-jump ratio:

| knob | audit (unsmoothed) | after |
|---|---|---|
| AC30 VOLUME — its primary overdrive control | **38.9×** | **0.98×** |
| Twin VOLUME | **29.5×** | **0.95×** |
| JCM800 MASTER | **10.3×** | **0.96×** |
| JCM800 GAIN | **6.8×** | **0.90×** |
| clean120 VOLUME *(already smoothed — control)* | 2.0× | 0.99× |

A ratio **below 1.0** means the seam step is smaller than the signal's own steady-state
slew: the knob move is buried under the waveform's natural sample-to-sample motion.

Worst case across every knob on all three amps: **1.11×** (0.05 step) / **2.07×**
(0.40 jump), against the already-compliant clean amp's **1.05× / 2.84×** measured in
the same run. The valve amps now sit at or below the amp that was already correct.

**The same-metric before number is in the suite**, not just in the audit: block C
splices the identical move with no smoothing and measures **22.95×** (jcm800 GAIN),
**26.80×** (MASTER), **28.46×** (twin VOLUME), **27.79×** (ac30 VOLUME) against a 12×
bar. ~28× → ~2×, same metric, same signal, same binary.

### Fidelity: bit-identical for any static render, and that is enforced

`OnePoleSmoother` is now `OnePoleSmootherT<T>`; `OnePoleSmoother` is the **unchanged**
float instantiation used by all eleven existing users, and `OnePoleSmootherD` is the
double sibling the valve amps use. This is what makes the change provably bit-neutral
rather than merely close: the valve amps carry pot fractions and post-taper scale
factors as **doubles** all the way to the multiply, so a settled double smoother
returns *exactly* the constant the code used before. Smoothing them through a float
would perturb the last mantissa bits of a static render, which both the goldens and
`identical_core_test` forbid.

`settled()` (new) reports when the ramp has landed exactly on target, so a parked
smoother is a no-op and the tone-stack matrix is **not rebuilt at all** while idle.

* **All five goldens `UNCHANGED`, worst band 0.00 dB.**
* Block D asserts bit-identity for knobs set *after* `prepare()` versus before —
  `max |Δ| 0.000e+00` on all three valve amps. `clean120` reports `DIFFERS`
  (3.304e-02), the control proving the check distinguishes the two cases instead of
  passing vacuously.
* Core ctest **26 → 27** targets (`clipper_param_smoothing_tests`).

**Deferred snap** is what buys that. `prepare()` marks the unit unprimed and the first
`process()` snaps every smoother onto its target. The house convention is "push
targets, then prepare", but the C ABI prepares inside `amp_create` and the params
arrive *afterwards* — without the deferred snap, every golden and every `amp_*` render
would ramp for 8 ms from the prepare-time defaults.

### Tone stacks: control-rate rebuild, not matrix interpolation

The three stacks smooth the *knob fractions* per sample and re-derive the 5×5
conductance matrix and its inverse at a **32-sample control rate** — what `AmpModel`
already does with its four biquads. Interpolating between two matrix inverses was
rejected: the inverse of a conductance matrix is **not affine in the pot fraction**, so
a blend of two inverses is not the inverse of any network. The cap states are physical
node voltages and currents, and carrying them across a small matrix perturbation *is*
what a real pot wiper does, so no state migration is needed.

### CPU: no regression, measured as an interleaved same-machine A/B

3 runs each, alternating binaries; run 1 is a warm-up outlier on both sides, so these
are runs 2–3, as % of one 48 kHz stream:

| unit | `main` | this slice | |
|---|---|---|---|
| jcm800 | 58.21 / 58.04 | 58.48 / 58.49 | +0.6 % relative |
| twin | 39.70 / 39.42 | 38.95 / 38.57 | slice faster |
| ac30 | 33.86 / 34.83 | 33.74 / 33.65 | slice faster |

Two of three move faster and the third moves +0.6 %, inside a 2.2-point run-to-run
spread on `main` alone.

**Read this before quoting an absolute bench number.** The JCM800 measures ~58 % here
against the **53.3 %** recorded in §32, which looks like a 5-point regression from this
slice and is not: `main` measures 58.04–60.25 % on the *same machine in the same
session*. §32's absolute column was taken on different hardware. The only defensible
form of this claim is an interleaved same-machine A/B.

### Not smoothed, on measurement

`Jcm800PowerAmp` PRESENCE and `Ac30PowerAmp` TOP CUT are already applied per sample and
both measure at or below the clean amp's baseline, so a smoother there would be pure
cost. They are pinned by the new test regardless, so a regression cannot creep in.

### Teeth, proven by perturbation

Reverting `gainSm_.next()` / `masterSm_.next()` to `.target()` — a once-per-block
constant, exactly the pre-fix behaviour — makes the suite fail on jcm800 GAIN at
**6.23×** (0.05 step), **16.75×** (0.40 jump) and **27.49×** (0.40 drop), with a
message naming audit finding 6. Restored, it passes again at the identical 1.11× /
2.07×. The file was `touch`ed after both patch and restore: a restored backup carries
the backup's mtime, `make` skips the rebuild, and you measure stale code (the trap
recorded in §29).

**The WASM artifact was rebuilt** — `core/` changed. sourceHash 71e5ce4f02fb…,
65 inputs, 181571 bytes.

---

## 36. Audit finding 15 — the diode ideality factor, and a reference fitted to the bug

**This is a deliberate tone change.** The RAT is now ~5 dB louder and correspondingly
cleaner at the same DISTORTION setting, because the diode clamp it runs into sits
where a real 1N4148 pair's clamp sits. Nothing was re-gained to hide that; see
**"The voicing question"** below for the numbers and the recommendation.

### The defect

`core/src/dsp/RatModel.cpp` built its stage-2 clipper as

```cpp
chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes { P1, kDiodeIs, kDiodeVt, 1.0 };
// kDiodeIs = 2.52e-9;  kDiodeVt = 25.85e-3;
```

`Is = 2.52 nA` is the 1N4148's SPICE saturation current. The card it comes from reads
`IS=2.52n **N=1.752**` — and the ideality factor was dropped. `DiodePairT`'s fourth
argument is `nDiodes`, used internally as `Vt_eff = nDiodes * Vt`, which is exactly
where an ideality factor belongs (`GoldModel` already used it that way for its
germanium pair, `kGeIdeality = 1.3`). With `n = 1.0` the junction's effective thermal
voltage was 1.752× too small, so the pair only reached 0.6 V at roughly 30 A.

Measured settled clipping-node voltage of the actual WDF tree (192 kHz = 4× the
worklet rate, DC drive, 20 000 samples of settling):

| drive `vin` | `n = 1.000` (shipped) | `n = 1.752` (1N4148) | Δ |
|---|---|---|---|
| 0.5 V | 0.2926 | 0.4489 | +3.72 dB |
| 1 V | 0.3218 | 0.5476 | +4.62 dB |
| 3 V | 0.3548 | 0.6219 | +4.88 dB |
| 10 V | 0.3761 | 0.6778 | +5.12 dB |
| 30 V | 0.4047 | 0.7089 | +4.87 dB |
| 100 V | 0.4212 | 0.7379 | +4.87 dB |
| 600 V | 0.4341 | 0.7906 | +5.21 dB |

So the pedal clipped at **0.32–0.43 V** where this file, `RatModel.h` and §6 bullet 2
all documented **±0.6 V** — 5–6 dB low, with a harder knee than a real silicon pair.
`BjtStage.h` has always had the physics right (`nVt = 0.0453`, i.e. n ≈ 1.75), so the
codebase disagreed with itself about what a silicon junction is.

Through the model, at the shipped default knobs (DISTORTION 0.7 / FILTER 0.4 /
LEVEL 0.8), the clipping-node peak on a 0.3 V-peak 220 Hz sine:

| DISTORTION | before | after |
|---|---|---|
| 0.2 | 0.213 V | 0.223 V |
| 0.4 | 0.323 V | 0.549 V |
| 0.7 (default) | **0.381 V** | **0.678 V** |
| 1.0 | 0.418 V | 0.737 V |

### The second-order damage: a reference fitted to the defect

`core/include/clipper/dsp/DiodeClipperADAA.h` is the memoryless `Vk·tanh(x/Vk)`
stand-in that exists **solely** so `--stage2 adaa` can compare first-order ADAA
against oversampling on the same nonlinearity. Its header said

> "The WDF diode node … saturates around ~0.33-0.39 V under realistic overdrive …
> Vk = 0.35 places the tanh ceiling in the WDF's measured mid/high-drive output band."

That measurement was real, and it was a measurement **of the bug**. Nothing tied the
two constants together afterwards, so fixing the diode without refitting `Vk` would
have left the A/B path silently comparing two different circuits — the worst failure
mode available to a comparison path, because it keeps agreeing.

`kDefaultVk` is now **derived by measurement**: a least-squares fit in dB of
`Vk·tanh(x/Vk)` against the settled WDF node voltage over 41 log-spaced drive levels
spanning 0.5–100 V (the range the RAT's op-amp output covers from a quiet pick at low
DISTORTION to a cranked knob on a hot humbucker).

| fit target | optimum `Vk` | rms dB error | error at the shipped 0.35 |
|---|---|---|---|
| old (`n = 1.000`) WDF | 0.3703 | 0.716 dB | 0.860 dB |
| **new (`n = 1.752`) WDF** | **0.6659** | **0.605 dB** | **5.346 dB** |

`kDefaultVk = 0.67` (the rounded optimum; 0.607 dB rms, a 0.002 dB penalty over
0.6659). Note the old 0.35 was already 0.14 dB worse than its *own* target's optimum.

Model-level WDF-vs-ADAA gap at default knobs, 220 Hz, three input levels — this is
the number the A/B path's honesty rests on:

| input peak | before (both wrong, consistently) | after (both right) |
|---|---|---|
| 0.10 | rms +0.09 dB / peak −0.19 dB | rms +0.72 dB / peak +0.57 dB |
| 0.30 | rms −0.42 dB / peak −0.71 dB | rms +0.20 dB / peak −0.10 dB |
| 0.60 | rms −0.75 dB / peak −0.98 dB | rms −0.04 dB / peak −0.26 dB |

The *before* column looks fine, and that is the point: the two stages were wrong
**together**. Had the diode been fixed and `Vk` left at 0.35, the ADAA peak would sit
~5.7 dB below the WDF.

### The same constant, in GOLD

`core/src/dsp/GoldModel.cpp` had `kSiIdeality = 1.0` in its **silicon
counterfactual** — the measurement-only diode option that exists to show what the
germanium pair buys. Dropping the ideality factor pulls the silicon knee *down toward*
the germanium's, so the A/B measured almost nothing. Settled node voltage in GOLD's
network (`Rs = 2.2 kΩ`, `Cp = 4.7 nF`):

| drive | Ge (`n = 1.3`) | Si (`n = 1.0`) | Si (`n = 1.752`) | old contrast | new contrast |
|---|---|---|---|---|---|
| 1 V | 0.2485 | 0.3022 | 0.5150 | +1.70 dB | **+6.33 dB** |
| 3 V | 0.2888 | 0.3346 | 0.5868 | +1.28 dB | +6.16 dB |
| 10 V | 0.3223 | 0.3558 | 0.6423 | +0.86 dB | +5.99 dB |
| 30 V | 0.3529 | 0.3844 | 0.6732 | +0.74 dB | +5.61 dB |
| 100 V | 0.3741 | 0.4009 | 0.7023 | +0.60 dB | +5.47 dB |

Measured *through the model*, clean half switched out, 0.3 V sine: the contrast goes
from **+0.96…+1.56 dB** to **+5.88…+6.11 dB**, which is the ~6–7 dB a real
1N34A-vs-1N4148 comparison gives. **The germanium side was and is correct** (n = 1.3,
knee 0.286 V at 1 mA, matching its own documentation), so GOLD's actual voice — the
pedal players hear — is **unchanged by this slice**. Only its counterfactual moved.

### The voicing question — measured, reported, NOT compensated

Raising the ceiling 5 dB makes the RAT louder and, at a given knob, less saturated.
This pedal has history here: §11.1 (M6.1) records a "the RAT has no balls" report
fixed with an input trim and a +54 → +66 dB max-gain change. CLAUDE.md forbids
calibrating a constant to absorb an error elsewhere (`kFullScaleSecV` absorbed two
factor-of-2 mistakes that way), so **nothing was re-gained here**. The numbers:

At the shipped default knobs (0.7 / 0.4 / 0.8), 220 Hz sine, tail RMS/peak/THD:

| input peak | out rms | out peak | rms dBFS | THD |
|---|---|---|---|---|
| 0.05 | 0.2469 → **0.4120** | 0.2717 → 0.4677 | −12.15 → **−7.70** | 31.4 % → 25.7 % |
| 0.10 | 0.2663 → **0.4586** | 0.2860 → 0.5021 | −11.49 → **−6.77** | 35.1 % → 32.2 % |
| 0.30 (hot humbucker) | 0.2882 → **0.5092** | 0.3039 → 0.5420 | −10.80 → **−5.86** | 38.3 % → 36.5 % |
| 0.60 | 0.3001 → 0.5283 | 0.3152 → 0.5525 | −10.46 → −5.54 | 38.4 % → 38.6 % |

A 110 Hz pluck at 0.3 V peak: rms −12.59 → **−8.39 dBFS** (+4.20 dB), peak 0.305 →
0.534.

Onset of clipping, DISTORTION knob position where THD crosses a threshold (0.3 V sine):

| threshold | before | after |
|---|---|---|
| THD ≥ 5 % | dist 0.24 | dist 0.32 |
| THD ≥ 10 % | dist 0.27 | dist 0.35 |

**Judgement: a compensating voicing slice is NOT indicated.** The onset moves by
0.08 of knob travel — less than one position on a ten-mark face — and at the shipped
default (0.7) the pedal is still at 36 % THD, i.e. thoroughly saturated. The change
is in the *opposite direction* to the M6.1 complaint: this is +5 dB of output, not
less. What genuinely does want a look, as its own slice, is **downstream staging** —
5 dB more into the amp and the worklet's soft limiter is a level-structure question,
not a RAT-voicing one. The golden below shows exactly that: the JCM800 absorbs the
level (broadband RMS moves −0.15 dB) and converts it into a different harmonic
distribution.

### Aliasing did not regress

The knee got *softer*, so the alias floor should hold or improve.
`build/clipper-render --alias-report` (f0 = 4186 Hz, dist 0.70):

| os | worst-alias before | worst-alias after | fund amp before → after |
|---|---|---|---|
| 1 | −18.1 dB | −17.8 dB | 0.412 → 0.730 |
| 2 | −21.0 dB | −21.1 dB | 0.424 → 0.744 |
| 4 (default) | −104.1 dB | −102.4 dB | 0.428 → 0.757 |
| 8 | −106.2 dB | −106.3 dB | 0.434 → 0.767 |

The 4× floor moves 1.7 dB while the fundamental rises **4.98 dB**, so alias-to-signal
actually improves by ~3.3 dB. `clipper_rat_tests` also measures 4× at −87.7 dB against
its own harsher in-test conditions and −90.9 dB at dist 1.0 (bar −60 dB).

### Goldens: `rat_jcm800` MOVED and was deliberately NOT re-blessed

`clipper_player_expectations_tests --golden-report`:

```
GOLDEN-DELTA rat_jcm800       CHANGED   -0.15  6.50  800  10
GOLDEN-DELTA sd1_twin_reverb  UNCHANGED +0.00  0.07  317  13
GOLDEN-DELTA muff_twin        UNCHANGED +0.00  0.00  252  13
GOLDEN-DELTA ts_ac30          UNCHANGED +0.00  0.00 1008   7
GOLDEN-DELTA clean120_chorus  UNCHANGED -0.00  0.11  252   7
```

Only the RAT rig moved, which is the scope check: nothing else in the chain was
touched. Per third-octave band, `rat_jcm800`:

| band | golden | new | Δ |
|---|---|---|---|
| 200 Hz | 48.74 | 48.46 | −0.27 |
| 400 Hz | 36.34 | 36.65 | +0.31 |
| 635 Hz | 23.99 | 26.49 | **+2.50** |
| 800 Hz | 10.02 | 16.52 | **+6.50** |
| 1008 Hz | 12.94 | 7.09 | **−5.85** |
| 1270 Hz | 12.35 | 12.15 | −0.20 |
| 1600 Hz | 10.63 | 15.52 | **+4.89** |
| 2016 Hz | 7.89 | 6.59 | −1.30 |
| 2540 Hz | 4.60 | 7.55 | **+2.96** |
| 3200 Hz | 2.51 | 4.34 | +1.83 |

Broadband RMS barely moves (−0.15 dB) because the JCM800 is already compressing;
what changes is *where the energy sits* — the upper-mid harmonic structure is
redistributed by up to 6.5 dB as the amp is driven further into its own nonlinearity.
That is an audible tone change and it is the intended consequence.

**The goldens were left un-blessed and `clipper_player_expectations_tests` therefore
fails on this branch.** Re-blessing is a human ritual (`scripts/update-goldens.sh`
requires a clean tree, a confirmation typed at a real tty, and a `GOLDENS.md`
justification) and it is not this slice's call to make — a reviewer should look at the
table above and decide. See §31.

### Tests: what changed and why

- **`test_rat_model.cpp` `testClippingCeiling`** — had ONE property, relative
  ("doubling the input barely moves the peak"). A saturating clipper satisfies that at
  *any* ceiling, which is precisely how this defect survived four milestones. Added an
  **absolute** property against an external reference (the 1N4148 SPICE card and its
  0.62–0.72 V datasheet forward-drop band): the clipping node must land in 0.60–0.85 V
  at realistic drive, plus a soft-knee check that the toe is below the slammed ceiling.
  *Teeth proven:* the pre-fix diode fails it by 5–6 dB. Measured 0.727 / 0.737 V, toe
  0.664 V.
- **`test_rat_model.cpp` `testAdaaTracksWdf` (new)** — drives the WDF and ADAA stage-2
  modes through the real model at the shipped default knobs and asserts their rms and
  peak agree within 1.5 dB. This is the assertion that would have caught the fitted
  reference. It does **not** re-derive `Vk` from `RatModel`'s constants, so it is not
  a tautology. *Teeth proven:* with the corrected diode and `kDefaultVk` put back to
  0.35 it fails on the rms bound.
- **`test_rat_model.cpp` `testFactorOneRegression`** — hardcoded os=1 sample values,
  **regenerated**. This is a drift guard, not a property: it pins whatever the
  single-rate path does, so a deliberate clipper change necessarily invalidates it and
  it must be *recaptured*, never loosened. The samples grew by about the 5 dB the
  ceiling grew (index 2048: −0.34848 → −0.61019, +4.86 dB; index 4095: −0.35369 →
  −0.63336, +5.05 dB), which is the check that nothing *else* moved with it. Precedent:
  it was regenerated the same way for M6.5.
- **`test_gold_model.cpp` `testGermaniumKnee`** — **re-baselined, and its bounds got
  TIGHTER, not looser.** Say it precisely, because the sloppy version of this sentence
  reads as the one thing CLAUDE.md forbids ("don't loosen a bound to go green"): as
  *assertions* both bounds are now strictly harder to satisfy — onset ratio `5.0×` →
  `20.0×` and slope ratio `0.25×` → `0.05×`. What got looser is the **margin between
  the bound and the measurement** (onset 26×/5× = 5.2× of headroom → 189×/20× = 9.4×),
  which is the point: neither assertion was pinning the bug, both were genuine
  properties that the bug made *harder* to satisfy, because a dropped ideality factor
  pulled the silicon knee toward the germanium's. Measured 26× → **189×** onset and
  0.032 → **0.0063** slope ratio.
- **`test_gold_model.cpp` `testDiodeLevelContrast` (new)** — the knee test compares
  *shapes* via ratios and passed happily while the two ceilings sat 1 dB apart. This
  asserts the thing the A/B is for, against the external ~6 dB 1N34A-vs-1N4148
  reference, with the clean half switched out and the measurement taken past both
  knees: contrast must land in 4.0–9.0 dB. *Teeth proven:* reverting `kSiIdeality` to
  1.0 fails it. Measured 5.88–6.11 dB.

### Still open after this slice

- **The goldens.** `rat_jcm800` is stale by up to 6.50 dB in one band, by design. A
  human decides.
- **The committed WASM artifact.** `core/` changed, so `web/public/generated/*` and
  `.build-stamp.json` are stale and `check-artifact.mjs` will correctly fail. Out of
  this slice's remit (see §31 — the fix is `bash scripts/build-wasm.sh`, never a
  hand-edited stamp).
- **Downstream level staging.** The RAT delivers ~5 dB more into the amp than it did.
  Its own slice, with the table above as the input.
- **§7's `--alias-report` table** is still stale (noted in §32); the numbers in this
  section supersede it for the RAT.
## 37. Audit finding 16 (DC half) — the Muff's missing output coupling cap

**Slice:** `fix/muff-output-dc-blocker` (2026-07-25). **Touches:** `core/src/dsp/MuffModel.cpp`,
`core/tests/test_muff_model.cpp`. **ADR 009.** **Deliberate tone change** — the `muff_twin`
golden is re-blessed, by 0.80 dB in one band.

### The finding, and why it is only half fixed here

Audit finding 16 names **two** defects: the Muff has **no output DC blocker** and **almost
no bass**. This slice fixes the first. The second is held to its own slice, and the reason
is recorded in ADR 009: fixing the bass means adding a series base resistor whose value
cannot be taken from the schematic until an upstream discrepancy is settled — this model's
clip-stage base node measures **~1.8 kΩ against the real stage's ~4 kΩ**, so the schematic's
100 kΩ produces 33 dB of divider loss here and turns SUSTAIN into a threshold switch.
Choosing 47 kΩ instead makes the pedal sound right by compensating for a number that is
itself wrong, which is the practice CLAUDE.md explicitly forbids. It may still be the right
call — but it is a separate argument, and bundling it would have bought an uncontroversial
fix at the price of that argument.

The split is justified by measurement, not taste. Golden movement, the two halves:

| | broadband | worst band |
|---|---|---|
| **output cap alone (this slice)** | **−0.03 dB** | **0.80 dB @ 5080 Hz** |
| both halves together | −5.85 dB | 26.04 dB @ 1270 Hz |

The base resistors account for essentially the entire tone change.

### The defect

`MuffModel::processChunk` ended `w[i] = x * outGain;` — no high-pass anywhere in the model.
`BjtStage::processSample` returns `Vc - vcQ_`, which removes the **quiescent** DC only, not
the dynamic DC that four asymmetrically-clipping common-emitter stages rectify when driven.
Every sibling pedal carries `dcBlockHz = 12.0` for precisely this reason (`SdModel.cpp`:
"the asymmetric clip produces DC"). Measured before: up to **+0.47 V, 28 % of peak**.

### The fix, and where it sits

The real pedal's **0.1 µF output cap into the 100 k VOLUME pot**:
`f = 1/(2π·100k·0.1µ) = 15.92 Hz`. Derived from the Muff's own two components rather than
copied from the siblings' 12 Hz. Placed **after Q4 and before the VOLUME multiply**, because
in the pedal the cap precedes the pot, and **inside the oversampled domain**, so it also
blocks the rectified DC before it reaches the decimator.

`kOutputTrim` **stays at 0.40** — see the hazard below.

### Measured

DC on signal, across three SUSTAIN settings and a +0.1 V input-offset case (the offset case
matters because deleting a coupling cap changes nothing on a clean input — §29):

* **+0.47 V / 28 % of peak → 0.00000–0.00003 % of peak**, against a 1 % bar.

What is *not* fixed, and is now measured, printed and named rather than skipped:

* `finding16-muff-almost-no-bass` — low E (82.4 Hz) at **−41.14 dB** re 1 kHz, open A
  **−34.04 dB**, 60 Hz **−49.87 dB**. This **independently reproduces the audit's "41 dB
  down" to the digit**, which is a useful confirmation that the audit's number was right.
* `muff-slam-exhausts-newton-cap` — still **6 of 16** rate × oversampling combinations at
  the 60-iteration cap. The base-resistor branch fixes this as a *side effect* (worst 18/60,
  3.3× margin): a series resistance ahead of the exponential base-emitter junction bounds
  the base-current step, so there is less stiffness for the damped Newton's line search to
  globalize against. That win is claimed on that branch, not this one.

The opposite-direction bar stays a **hard assert** on both branches: 30 Hz must remain
> 12 dB down (measured **−72.01 dB**). A first-order-per-stage network cannot satisfy both
directions by accident, so the pair is what makes either meaningful — if someone deletes a
coupling cap or DC-couples a stage to chase the bass bars, this fails.

### Two traps this slice surfaced

**1. A removed `ledgerMain` would have silently disabled the XFAIL ledger.** The bundled
slice deleted the `ledgerMain` call from `main()` on the assumption it would have no XFAILs
left, while `core/CMakeLists.txt` still registered `clipper_muff_tests_xfail_ledger`. Left
that way, `--xfail-ledger` runs the whole suite and exits **0**, so ctest reports the ledger
entry as **Passed** instead of `***Skipped` — the line whose entire job is to advertise open
defects becomes a silent duplicate test run. Verified fixed: ledger exits **77**.

This is the third instance of the same shape, and it is worth naming as a class: the
advisory native job that failed on 100 % of runs (§ CI), the artifact check that only
verified existence (§31), and now this. **A guard that is present but inert is worse than an
absent one**, because its presence is read as coverage.

**2. `kOutputTrim` is a stacking hazard.** The bundled version raised it 0.40 → 0.45 to hold
the TONE 0 corner under the 2.0 V ceiling *once the bass returned*. This slice leaves it at
0.40, and measures SUSTAIN 1.0 peaking at **2.0096 V** — already marginally over that
ceiling from the cap alone. Whoever lands the base-resistor half must **re-derive the trim
against the combination**, not stack a second adjustment on top. Absorbing two unrelated
corrections into one constant is exactly how `kFullScaleSecV` ended up hiding two separate
factor-of-2 mistakes.

**The WASM artifact was rebuilt** — `core/` changed.

## 42. Audit finding 7 — the phase-inverter tail reference, and every constant that had been fitted around a starved inverter

Finding 7: *"The phase-inverter model cannot express a real long tail; two of three amps idle
near cutoff."* The fix itself is four lines of solver arithmetic. Everything else in this
section is the fallout, because **three amps' worth of downstream calibration had been fitted
against the defect** — and this is the section to read before re-deriving any valve-amp
constant, because it is the largest single example of that pattern in the project.

The slice shipped in two phases in one day: the circuit fix (`tailRef`, the per-amp
calibration, the XFAIL ratchet), then the un-fitting (staging, normalization, the Twin's
plate load, six test probes/bounds). They are written up together here.

### 42.1 The defect

`LtpInverter` modelled the long tail as **two terminals** — `Rtail` straight to ground — so
one number had to set two independent things: the tail *resistance* is what rejects common
mode, and the tail *reference* is what sets the standing current. A real long-tailed pair
returns its tail through a large resistor to a **negative** reference, and gets both. Ours
could only trade one for the other, and all three amps paid:

| amp (before) | plate % of B+ | Ip / triode | leg gains | ratio |
|---|---|---|---|---|
| JCM800 (10 k to ground) | 94.7 / 95.4 ✗ | 0.179 / 0.191 mA ✗ | ×15.44 / ×9.37 | 0.607 ✗ |
| Twin (22 k to ground) | 94.3 / 93.1 ✗ | 0.232 / 0.200 mA ✗ | ×7.43 / ×7.51 | 0.990 |
| AC30 (2.2 k to ground) | 82.4 / 81.6 | 0.529 / 0.501 mA | ×31.76 / ×17.46 | 0.550 ✗ |

Targets (`core/tests/support/LtpProbe.h`): 70–85 % of B+, 0.5–0.9 mA per triode, leg ratio
≥ 0.90. Four of nine met. The AC30's ✓ column is the §23 second amendment, which bought the
DC point by shrinking the tail until the pair was no longer long-tailed — the trade this
model could not avoid. The Twin's "0.990" was balance between two nearly-cut-off legs.

### 42.2 The fix

`LtpInverter::Config` gains `double tailRef = 0.0` (volts). The tail residual becomes
`k1.Ip + k2.Ip − (Vk − tailRef)/Rtail` in `prepare()` and the same with `gTail` in
`processSample()`. The Jacobian is untouched (`∂/∂Vk` of the tail term is unchanged), and
`tailRef = 0` is the old behaviour **bit-for-bit** — verified before any config changed:
27/27 ctest and all five goldens digit-identical.

Per-amp calibration, all rate-independent (the LTP solve is memoryless), each chosen to
maximise the **minimum normalised margin** to the three targets:

| amp | Rtail | tailRef | Ra2 | plate % B+ | Ip / triode | legs | ratio |
|---|---|---|---|---|---|---|---|
| JCM800 | 10 k (real 2204 part) | **−12 V** | 82 k (finding 8) | 80.0 / 81.6 ✓ | 0.680 / 0.763 ✓ | ×29.68 / ×20.86 | 0.703 ✗ (finding 8) |
| Twin | **22 k** (kept long) | **−26 V** | 142 k → **119 k** | 80.7 / 79.6 ✓ | 0.792 / 0.703 ✓ | ×13.93 / ×13.90 | **0.998 ✓** |
| AC30 | 2.2 k → **10 k** | **−10 V** | 110 k | 79.3 / 78.5 ✓ | 0.622 / 0.587 ✓ | ×27.56 / ×25.13 | **0.912 ✓** |

**Eight of nine targets are hard assertions now** (was four). Five finding-7 XFAILs XPASSed
and were deleted in the same slice. The one miss is the JCM's leg balance — audit finding 8,
moved 0.607 → 0.703 by the tail fix but owned by the 82 kΩ plate load. Its push-pull
even-harmonic cancellation is **unchanged at 8.1 dB**, confirming the tail was not that
defect's cause.

`LtpProbe`'s tail current was silently wrong the moment `tailRef ≠ 0` (`Vk/Rtail`
under-reported the JCM's by 6×); it now reads `(Vk − tailRef)/Rtail` and cross-checks against
Ip1+Ip2 exactly.

### 42.3 The audit was wrong about the Twin's plate load, and measurement said so twice

The audit (and the slice plan) said the Twin's 142 kΩ plate load "exists in no AB763 and was
an artifact compensating the starved tail", and predicted ratio ≈ 1.0 from matched
100k/100k loads once the tail was fixed. **Measured, with the tail fixed:**

| Twin Ra2 (Rtail 10 k, tailRef −6 V) | 100 k | 110 k | 120 k | 130 k | 142 k | 150 k |
|---|---|---|---|---|---|---|
| leg ratio | **0.718** | 0.774 | 0.828 | 0.879 | 0.938 | 0.976 |

Sweeping `tailRef` from 0 to −16 V at Ra2 = 100 k never passes 0.760, and Ra2 = 100 k needs
`Rtail ≈ 47 k` *and* `tailRef ≈ −60 V` to reach 0.925 — not an AB763 either. **Unequal plate
loads are how a finite-tail LTP is balanced**; that is the resistor's job, not a bug. Had the
brief been followed here it would have turned a *green* assertion red (perturbation pass C
below).

But the **value** was still fitted around the defect — §20 recorded "measured swing ratio
1.007" against the old 22 k-to-ground tail — so it was re-derived by measurement to the same
documented convention (legs balanced to ~1 %), against the corrected tail:

| Twin Ra2 (Rtail 22 k, tailRef −26 V) | 118 k | **119 k** | 120 k | 125 k | 130 k | 142 k |
|---|---|---|---|---|---|---|
| leg ratio | 0.9905 | **0.9978** | 0.9949 | 0.968 | 0.936 | 0.873 |

With the tail fixed the balance crossover moves **below** 142 k, not above it: a longer
effective tail needs less plate-load compensation.

### 42.4 The Twin's tail deliberately does NOT go to the 10 k on the schematic

The plan said take the Twin's `Rtail` 22 k → 10 k, "the AB763's documented shared tail", and
the first phase did. It measures as a mistake, and the reason is worth generalising: **in a
model whose only common-mode rejection is one resistor, that resistor cannot be shortened.**
This amp injects its global NFB **single-endedly into the cold grid**, so half of the
feedback signal is common mode; what leaks through a finite tail arrives **in phase** on both
6L6 grids, which is precisely the drive a push-pull pair cannot cancel, and it comes out as
2nd harmonic. Power section alone, 110 Hz, 0.5 V at the PI grid, h2 relative to the
fundamental:

| PI tail config | open loop | closed loop | Δ from closing the loop |
|---|---|---|---|
| 22 k to ground (the starved original) | −45.9 dBc | −47.3 dBc | −1.4 dB (NFB *reduces* h2, as it should) |
| 10 k / −7 V | **−61.9 dBc** | **−33.0 dBc** | **+24 dB (the loop GENERATES h2)** |
| 22 k / −26 V (shipped) | −73.7 dBc | −39.9 dBc | +15 dB |

The corrected inverter is 16–28 dB cleaner *open loop*; the short tail threw all of it away
and then some. Player-observable consequence, at the documented settings (VOLUME 0.5, hot
0.10 V DI, 110 Hz — the Twin's clean-headroom bar, < 4 % THD):

| Twin clean bar | THD |
|---|---|
| before finding 7 | 2.96 % |
| 10 k tail, drive restored | **4.5 % FAIL** |
| 22 k tail, drive restored (shipped) | **3.41 % PASS** |

The 10 k on the drawing sits above a 470 Ω-per-cathode network and a real negative return
that a two-node tail does not represent; 22 k is this model's stand-in for that network, and
it is the value the amp already shipped. ADR 014 records the decision.

### 42.5 The staging constants were absorbing the missing PI gain

Fixing the inverter roughly doubled its leg gain (JCM ×15.44 → ×29.68, Twin ×7.43 → ×13.93 —
both ≈ +5.7 dB), and the composed amps arrived **+3.4 to +5.7 dB hot, clipping past full
scale** (JCM cranked peak 0.899 → 1.163, Twin 0.933 → 1.661). The AC30 sailed through
(+0.4 dB) — because §23 had already re-derived *its* staging against a fixed inverter. That
asymmetry is the whole diagnosis: `Jcm800Amp::kInterstageScale` and `TwinAmp::kInterstageScale`
had been fitted around starved inverters. Un-fitted here, in the same slice as the fix, per
the ADR 008 precedent.

**Do not divide by the PI's gain ratio.** The global NFB absorbs about half of it. The
quantity that has to be restored is the power section's **closed-loop** gain — normalized-out
per volt at the PI grid, which in the linear region is proportional to the power-tube grid
drive:

| f0 (Hz) | 82 | 110 | 220 | 440 | 880 | 1760 | 3520 |
|---|---|---|---|---|---|---|---|
| JCM before | 0.1129 | 0.1208 | 0.1288 | 0.1321 | 0.1369 | 0.1460 | 0.1530 |
| JCM after | 0.1890 | 0.1938 | 0.1980 | 0.2017 | 0.2116 | 0.2336 | 0.2566 |
| **ratio** | 1.674 | 1.604 | 1.537 | 1.527 | 1.545 | 1.600 | 1.677 |
| Twin before | 0.0761 | 0.0777 | 0.0792 | 0.0796 | 0.0797 | 0.0796 | 0.0792 |
| Twin after | 0.1150 | 0.1163 | 0.1175 | 0.1179 | 0.1179 | 0.1179 | 0.1179 |
| **ratio** | 1.511 | 1.497 | 1.484 | 1.480 | 1.480 | 1.481 | 1.488 |

Mean over the guitar fundamental range (82–880 Hz): **JCM 1.577, Twin 1.490**. So

- `Jcm800Amp::kInterstageScale` **0.25 → 0.16** (0.25/1.577 = 0.1585; 0.16 lands within 1 % of drive, +0.08 dB)
- `TwinAmp::kInterstageScale` **0.16 → 0.107** (0.16/1.490 = 0.1074)

The residual spread is the NFB loop's own frequency shaping and no broadband trim can remove
it. Below clipping the amps' **drive** is now what it always was; what changed is the
ceiling.

### 42.6 …and the normalizations had to follow the swing, exactly as §23 said

`kFullScaleSecV` is an output **normalization**: it cannot fix THD or breakup ordering, and it
was measured to be nearly **insensitive to staging** at the cranked calibration point (the
preamp is already delivering 100+ V there, so halving the trim leaves the power section just
as saturated). What it must follow is the measured cranked swing — §23's rule, *"every voice
is normalized to its own cranked peak so the voices stay level-comparable"*.

And the cranked swing genuinely moved, because a starved inverter **could not drive the output
tubes to full power**:

| amp | cranked secondary peak before | after | rated full-power peak (√(P·8 Ω)) |
|---|---|---|---|
| JCM800 | 26 V · 0.899 = 23.4 V (34 W of 50 W) | 29.3 V (54 W) | 28.3 V |
| Twin | 24 V · 0.933 = 22.4 V (31 W of 85 W) | 37.7 V (89 W) | 36.9 V |

Both models now reach their rated power for the first time. Re-derived to the documented
`~0.9` cranked peak:

- `Jcm800PowerAmp::kFullScaleSecV` **26 → 33 V** — cranked peak 1.128 → **0.888–0.890** (44.1/48/96 kHz)
- `TwinPowerAmp::kFullScaleSecV` **24 → 42 V** — cranked peak 1.571 → **0.898**

**This is the only reason the two voices are quieter below clipping** — the JCM by ~2.0 dB,
the Twin by ~4.9 dB. It is a re-referencing of full scale, not a loss of drive, and it is
forced: any Twin normalization that keeps the cranked peak inside full scale costs at least
20·log10(37.7/24) = 3.9 dB. The alternative was a cranked Twin clipping 66 % past full scale
into the chain's output limiter.

Each constant is tied to **its own** convention — the staging to the power-tube drive, the
normalization to the cranked swing — deliberately, so neither absorbs the other's error. That
is the `kFullScaleSecV` failure mode this project already paid for once.

### 42.7 Measured: before → after, composed

Sine 220 Hz, no cab, at the documented knob settings (the full table is
`clipper-render`-reproducible; `kInterstageScale`-only rows omitted):

| rig | in V pk | out RMS before | after | Δ dB | THD before → after |
|---|---|---|---|---|---|
| JCM800 (G .7 M .5) | 0.032 | −19.78 | −21.53 | −1.75 | 10.50 → 10.91 % |
| JCM800 (G .7 M .5) | 0.100 | −16.28 | −17.00 | −0.72 | 29.43 → 33.43 % |
| JCM800 (G .7 M .5) | 0.316 | −16.03 | −16.31 | −0.28 | 44.30 → 51.66 % |
| JCM800 (G .2 M .3) | 0.316 | −29.89 | −32.05 | −2.16 | 6.93 → 6.54 % |
| Twin (VOL .5) | 0.032 | −43.95 | −48.88 | −4.93 | 0.49 → 0.56 % |
| Twin (VOL .5) | 0.316 | −24.10 | −28.97 | −4.87 | 4.94 → 5.63 % |
| Twin (VOL 1.0) | 0.316 | −8.97 | −10.43 | −1.46 | 15.73 → 9.77 % |
| AC30 (VOL .5) | 0.316 | −11.38 | −10.99 | +0.39 | 7.17 → **2.34 %** |
| AC30 (VOL 1.0) | 0.316 | −9.67 | −9.91 | −0.24 | 23.28 → **30.69 %** |

NFB, 1 kHz, small signal — both loops still negative and both still match `1/(1+βA_real)` to
0.02 dB:

| amp | reduction before | after | analytic |
|---|---|---|---|
| JCM800 | −3.41 dB | **−5.977 dB** | −5.995 |
| Twin | −3.24 dB | **−5.24 dB** | −5.24 |
| AC30 | 0 (β = 0, bit-exact open == closed) | 0 | — |

⚠️ **The JCM's "meaningful loop" window (`> 2 dB && < 6 dB`) now passes at 5.977 dB — 0.023 dB
of margin.** Untouched by phase 2 (it is a power-section property and staging is upstream).
Nobody should move the JCM's phase inverter again — finding 8 moves `Ra2` — without watching
that number.

Goldens (`--golden-report`, nothing re-blessed — the gate is ±1.0 dB broadband, ≤ 1.5 dB per
live band):

| golden | phase 1 (circuit fix only) | phase 2 (constants un-fitted) | worst band |
|---|---|---|---|
| `rat_jcm800` | +4.39 dB | **−1.77 dB** | 1.80 dB @ 200 Hz |
| `sd1_twin_reverb` | +3.43 dB | **−4.89 dB** | 5.00 dB @ 317 Hz |
| `muff_twin` | +3.87 dB | **−4.74 dB** | 4.92 dB @ 800 Hz |
| `ts_ac30` | +0.44 dB | **+0.44 dB** (AC30 untouched by phase 2) | 1.22 dB @ 800 Hz |
| `clean120_chorus` | UNCHANGED −0.00 dB | **UNCHANGED −0.00 dB** | 0.11 dB — the scope check |

The two Twin rigs' movement is now *the normalization*, essentially flat across the band
(−4.87 to −5.00 dB per band), and the JCM's is a −1.8 dB level shift with a ~1.3 dB tilt.
`clean120_chorus` sits at the gate's own quantisation floor, which is the proof the change is
confined to the three valve voices. **The four moved goldens are NOT re-blessed** — that is
the owner's call, with these tables.

### 42.8 What the AC30 lost, and why it is ledgered rather than fixed

The AC30's *level* barely moves through this change, but its **character does**, and this is
the most important measured result in the section: **the Vox's mid-volume "breakup" and its
"class-A chime" were both its phase inverter's 2:1 leg imbalance leaking 2nd harmonic past
the push-pull's cancellation.** Balance the legs (0.550 → 0.912) and the even harmonics go
with them, while the section's own clipping barely moves (220 Hz, hot 0.316 V pickup):

| VOLUME | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 |
|---|---|---|---|---|---|
| THD before % | 4.60 | 7.06 | **10.75** | 12.30 | 14.11 |
| — of which even | 4.55 | 6.90 | 10.39 | 10.99 | 7.99 |
| THD after % | 1.55 | 2.29 | **3.80** | 6.58 | 15.18 |
| — of which even | 1.42 | 1.77 | 2.25 | 2.12 | 1.59 |
| odd before % | 0.65 | 1.53 | 2.77 | 5.52 | 11.63 |
| odd after % | 0.62 | 1.45 | **3.06** | 6.23 | 15.10 |

h2 at VOLUME 0.6: **−19.76 → −33.03 dBc**. The odd (clipping) column is *unchanged to
slightly higher*. So the §23 breakup-ordering guard and the chime guard were, in part,
measuring a defect — and with the defect fixed they fail:

| §23 guard | bar | before | after |
|---|---|---|---|
| AC30 breakup onset | ≤ 0.65 | 0.5 | **0.7** |
| AC30 onset vs Twin's | ≤ twin − 0.2 | 0.5 ≤ 0.7 | **0.7 ≤ 0.3** |
| AC30 THD at VOLUME 0.6 | > 8 % | 10.75 % | **3.80 %** |
| AC30 vs Twin at 0.6 | > 1.8× | 2.19× | **0.63×** |
| AC30 vs Twin 2nd harmonic (chime) | > +3 dB | +4.0 dB | **−21.2 dB** |

**Restoring them needs DRIVE, and the AC30 has none left to give.**
`Ac30Amp::kInterstageScale` is a *passive* divider (0.67 — §23 derived it as the two-channel
mixing division, and a passive path cannot exceed 1.0), and §23 already measured that
transcribing the missing second ECC83 gain stage is **+30…+35 dB**, which saturates the voice
at its opening defaults. Restoring the imbalance instead would re-break the leg-balance
assertion this slice just made hard. So all five bars are **ledgered as XFAILs with their
bounds untouched** (`finding7-ac30-chime`, `-breakup-onset`, `-breakup-vs-twin`, `-mid-thd`,
`-mid-vs-twin`), owned by an **AC30 gain-structure slice**. An XPASS is a hard failure, so
whoever re-voices that amp cannot leave them stale. This is the largest open item this slice
creates, and it is a *voicing* decision for the owner, not a bug to be quietly absorbed.

### 42.9 Six test probes and bounds that had been calibrated against the defect

Every one of these is a **probe or reference re-derivation with the physics named**, checked
against the pre-fix circuit as well as the post-fix one (the §36 discipline: a fixed
reference improves agreement on the old circuit too). No bound was loosened to go green; the
one bound that was genuinely wrong is called out as such.

1. **The presence-shelf reference** (`test_jcm800_power.cpp`) computed `βA` from the
   **closed-loop** 4 kHz gain where the formula needs the **open-loop** A. Fixed by measuring
   a feedback-disabled section; the 1.5 dB bound is untouched and agreement improves on both
   circuits (before: 1.04 → 0.35 dB; after: 2.43 FAIL → 0.79 dB).
2. **Power-compression monotonicity** asserted "incremental gain never rises" across drives
   0.5–16 V. That held only because the starved inverter's own gain expansion was finished
   below the first probe point (leg gain ×6.88/×7.24/×7.30 at 0.1/0.3/0.5 V — +6 %, all of it
   under 0.5 V). With the inverter corrected the knee sits at ~2 V, inside the probe range,
   and the stage now stiffens 3.6 % before compressing — which is real ("bloom"). Re-derived
   as three measured bars, all holding on both circuits: RMS monotonic; expansion to the
   peak-gain drive **< 1.10×** (1.000 before, 1.036 after); monotone compression past the
   peak, **> 6 dB** to 16 V (12.04 dB before, 12.49 after).
3. **The JCM's sag-recovery window**. The model contains **two** RCs after a burst: the
   supply reservoir (Rsupply·Creservoir = 7.5 ms) and grid-blocking (Rg·Cc = 4.84 ms — a hard
   burst charges the coupling caps, the grids sit cold, the tubes draw *less* than idle and
   the rail climbs faster than its own RC). Before the fix the starved inverter never pushed
   the grids into conduction at this probe: droop 31 V, recovery 7.71–8.44 ms ≈ the pure
   supply RC, so a "±25 % of the supply RC" bound held. After: droop 50 V, recovery
   **5.54–6.08 ms** — it moved from one modelled RC toward the other, and the old bound
   clipped it at 96 kHz by 1.5 %. Re-derived to the property the model actually has: the
   recovery lies **between the two RCs, ±25 %**.
4. **The sag-ordering probe** (`test_ac30_amp.cpp` and `test_twin_amp.cpp`): 45 V at
   `PARAM_DRIVE` noon = **90 V at the PI grid**, ~30× the inverter's clipping onset. At that
   level the metric stops measuring the rail: after the fix the **JCM's rail droops MORE**
   than the Twin's (57.4 V vs 50.5 V, as their supply impedances predict) while the envelope
   ratio says the opposite (2.26 vs 2.63 dB), because both outputs are pinned to their
   clipping ceiling at attack *and* at settle. Probe re-derived to **2.0 V** (4 V at the
   grid — still hard: the JCM's attack peak is 0.74 of full scale), where the metric tracks
   the rail (droop Twin 13.4 V / JCM 37.3 V / AC30 1.6 V) and the ordering and both windows
   hold on **both** circuits: Twin 1.03 < JCM 1.96 < AC30 5.74 dB before, 1.16 < 1.95 < 6.03
   after. Every bound unchanged.
5. **The AC30's cathode-bias bloom probe**, retuned by §23 for exactly this reason once
   already (8 V → 0.15 V), moved again: the AC30's legs went ×31.76/×17.46 → ×27.56/×25.13,
   and the grid-conduction crossover with it (~0.22 V → ~0.40 V at the grid). At the old
   probe the recovery τ landed exactly *on* the 0.3·Rk·Ck bound at 96 kHz. Probe 0.15 → 0.125
   (0.25 V at the grid, where `PARAM_DRIVE` noon doubles it); both bounds untouched. The
   measured table of bloom-vs-drive is in the test.
6. **Two "oversampling works" bars** (`test_jcm800_power.cpp`, `test_twin_amp.cpp`) asserted
   that **2×** beats **1×** by 8 dB. That is not a property of the resampler: the 1× figure
   is a function of how much the stage distorts, so cleaning the stage up shrinks the gap
   without touching anything that ships. The corrected inverter is much cleaner, so the JCM
   power section's 1× went −21.8 → −29.7 dB and its 1×→2× delta 13.9 → 4.3 dB **while the
   shipped 4× floor improved, −116.6 → −121.4 dB**. Re-derived onto the shipped factor:
   1×→4× > 60 dB (power section: 94.9/101.5/83.7 before, 91.7/94.9/82.8 after) and > 40 dB
   (composed Twin: 49.8/56.5/50.0 before, 46.1/46.4/48.5 after), plus the unchanged
   −60 dB M2 bar and the unchanged "8× buys nothing" bar.

**The Twin's cranked-breakup bar is the one *bound* that changed** (`> 25 %` → `> 18 %`), and
it changed for the same reason as the AC30's ledger: total THD at that operating point was
39.99 % before and 21.42 % after, but the drop is nearly all **even** harmonics (21.81 % →
9.92 %) — the inverter's imbalance — while the **odd** (clipping) component is 33.52 % →
18.98 %. So ~12 points of the old 40 % was a defect counted as breakup. The amp still clips
hard at full power (peak 0.898) and is still 6.3× dirtier than its clean bar. A **new**
assertion was added at the same time, which the old one lacked and which an unbalanced
inverter cannot fake: **odd-harmonic THD > 15 %** (33.5 % before, 19.0 % after). ⚠️ The
untouched `cranked > clean × 6` bar now passes at **6.27×** — 0.4 dB of margin; flagged, not
adjusted.

### 42.10 Still open after this slice

- **The AC30's voicing** — five ledgered XFAILs (§42.8). Needs an AC30 gain-structure slice.
- **Finding 8** — the JCM's `Ra2 = 82 k` (leg ratio 0.703) and its 8.1 dB of push-pull
  cancellation, with the JCM's NFB margin at 5.977 of 6.0 dB to watch. The Twin's own plate
  pair still needs 19 % more load on leg 2 than leg 1 to balance, which is a model question
  of the same family.
- **The Twin's closed-loop 2nd harmonic** is still ~7 dB above its pre-fix figure at matched
  drive (−39.9 vs −47.3 dBc), because the single-ended NFB injection's common-mode component
  is only rejected as well as one tail resistor rejects it. A differentially-referred
  feedback injection, or a longer tail still, would recover it — measured trend, its own
  slice.
- **The four moved goldens** need an owner bless (§42.7). Nothing was re-blessed here.

### 42.10.1 CPU

Interleaved same-machine A/B (the pristine pre-finding-7 bench binary alternated with the
phase-2 one, 3 rounds), % of one 48 kHz stream: **jcm800 52.39 → 52.86 (+0.9 %)**,
**twin 35.25 → 35.47 (+0.6 %)**, **ac30 29.55 → 29.63 (+0.3 %)**. Base run-to-run spread is
0.7–0.9 points, so all three are inside noise — no Newton-convergence cost at the new
operating points, consistent with `clipper_tube_solver_tests` and `clipper_nan_guard_tests`
passing unmodified. (Absolute columns are machine-dependent — do not quote them elsewhere.)

### 42.11 Perturbation proofs (and two that did not fire, which is also information)

Every patch and every restore followed by `touch` (§29's mtime trap). Transcripts:
`perturbation.txt` (phase 1), `perturbation-p2.txt` + `final-p2.txt` (phase 2).

| # | perturbation | expected | result |
|---|---|---|---|
| A | `tailRef` back to 0 on all three amps (phase 1) | the new hard PI assertions fail | **fired** — `LtpProbe.h:170` plate-fraction assert on all three suites |
| B | AC30 back to `Rtail 2.2 k`, `tailRef 0` (the §23 workaround) | the newly-hard leg-balance assert fails | **fired** — `LtpProbe.h:194` |
| C | Twin `Ra2` → 100 k with the tail correct (what the audit proposed) | balance assert fails | **fired** — `LtpProbe.h:194`; documented proof the audit's Twin proposal was wrong |
| D | Twin staging starved (`kInterstageScale` 0.107 → 0.03) | the NEW odd-harmonic cranked-breakup bar fails | **fired** |
| E | compression probe range truncated to 2 V (never reaches compression) | the > 6 dB compression bar fails | **fired** |
| F | the rail integrator's reservoir doubled in the MODEL only (`gRes_ = 2·kCreservoir/T`, analytic RC unchanged) | the re-derived sag-recovery window fails | **fired** (11.6 ms vs a 9.375 ms ceiling) |
| G | Twin + JCM supply character SWAPPED (`kRsupply` 80 → 300 and 150 → 20) | the re-derived 2 V sag probe catches the inverted ordering | **fired** |
| H | Twin supply alone softened 3.75× (`kRsupply` 80 → 300) | ordering inverts? | **did NOT fire** — Twin sag 1.16 → 1.82 dB, still under the JCM's 1.96. The metric responds, the ordering is simply robust to that much |
| I | JCM supply alone stiffened 7.5× (`kRsupply` 150 → 20) | ordering inverts? | **did NOT fire** — JCM sag 1.96 → 1.53, still above the Twin's 1.16 (its sag has a floor from grid blocking) |
| J | `kCreservoir` doubled | recovery window fails? | **did NOT fire**, and correctly so: the test computes its analytic RC *from* that constant, so both sides scale (5.8 → 11.6 ms against a window that also doubled). That is what makes it a discretization check; F is the perturbation that has teeth |

H, I and J are recorded rather than hidden because they bound what the two re-derived
sag properties actually catch: the ordering survives a 7.5× change in one amp's supply
impedance (it takes a swap of the two amps' character to break it), and the recovery window
catches the model disagreeing with its own constants, not the constants themselves.

## 43. The Muff SUSTAIN taper floor — the knob had no authority below its default

*Date: 2026-07-30 · Branch: `claude/amps-pedals-fixes-6f557i` · Field report: "way too gainy, even on sustain=14 it's pretty powerful"*

The owner's post-v1.1 testing round reported the pedal "way too gainy, even on
sustain=14". The report said "the rat", but measurement said otherwise: the RAT at
DISTORTION 0.14 is digitally clean (0.0 % THD) and 25 dB below its default level; the
**Muff** at SUSTAIN 0.14 was the **hottest point of its entire sweep** — −4.7 dBFS,
~1 V peak, 43 % THD at a realistic 0.1 V 220 Hz pluck. 'Sustain' is the Muff's gain
knob; the two pedals had been conflated in the report, and the measurement settles it.

### 43.1 The defect

Across the whole SUSTAIN travel (TONE 0.5, VOLUME 0.6, 0.1 V input) the output moved
less than 2.2 dB and THD never dropped below 27 % — the knob had **no authority**. The
§24 field fix had made the pot a true full-range attenuator with a −54 dB floor
(`kSustainFloorDb`), which was deep enough to stop the hum blowout it was fixing, but
not deep enough for the clip stages: their clean window ends at **~1–3 mV at the base**
(the diodes conduct at idle — documented canon, `BjtStage.h`), and −54 dB below the
6× max drive still delivers ~11.5 mV of a 0.1 V pluck into Q2 at knob **zero** —
4–10× past clean. Q3 then sits in its 30 % THD region and Q4 is re-slammed to its
ceiling, which is why the output level barely moved: every knob position ended at the
same wall. Knob ≈ 0.14 was the worst spot because Q2/Q3's gain-*expansion* hump
(+6.7 dB at ~30 mV input — the cold-bias transfer steepens before it saturates) parks
exactly there at realistic input.

### 43.2 The fix — a piecewise taper with the break pinned at the default

`sustainDrive()` is now piecewise decibel-linear (`MuffModel.cpp`):

- **knob ≥ 0.6** (`kSustainBreak`): the previous law **verbatim** —
  `kClipDriveMax · 10^((−54/20)·(1−k))`. Same expression, same constants, same
  floating-point result: the shipped default and everything above it is **bit-identical
  by construction**, so the `muff_twin` golden, the sustain-wall tests and the hum
  bars at default drive cannot move. Verified at suite level: A1 defaults, A2 default,
  A3 probes 0.6/1.0 and the golden report all identical to `main` to the digit.
- **knob < 0.6**: decibel-linear from `kSustainMinDb = −84` (knob 0) to the −21.6 dB
  the upper law gives at the break.

A real audio pot is manufactured as two resistive segments meeting mid-rotation, so a
slope break is truer to the part than a single exponent; landing the break on the
default is what pins the golden. Setting `kSustainMinDb = −54` reproduces the old
single-exponent law *exactly* (both segments collapse onto the same dB line), which
makes the perturbation proof a one-constant flip.

−84 was chosen by measurement, not taste math: −80 left knob 0.14 at 20.5 % THD,
above the ~15 % "real Muff at sustain 1–2" feel the report asked for; −84 measures
14.2 %. Deeper floors start threatening the A1 audibility floor (−70 dBFS) at the
knob-0 escape hatch, which still leaks by design (−41.5 dBFS at 0.1 V — a real pot's
wiper never quite grounds).

### 43.3 Measured results (48 kHz, 220 Hz, TONE 0.5 / VOLUME 0.6)

0.1 V pluck (single-coil), before → after:

| knob | RMS dBFS | THD % |
|---|---|---|
| 0.00 | −6.2 → **−41.5** | 36.4 → **1.9** |
| 0.14 | −4.7 → **−26.0** | 42.8 → **14.2** |
| 0.30 | −5.1 → −5.9 | 27.1 → 38.0 |
| 0.60 (default) | −6.1 → −6.1 (bit-identical) | 38.6 → 38.6 |
| 1.00 | −4.8 → −4.8 (bit-identical) | 150.5 → 150.5 |

At 0.316 V (hot pickup) the bottom of the knob keeps ~40 % THD — that is **Q1**
clipping (0.158 V at its base against its ~0.05 V clean limit), upstream of the pot,
which no taper can or should remove: a hot humbucker into a fuzz front end clips.

Knock-on measurements, both improvements: A2 hum torture at min gain improved
−51.4 → **−62.0 dB** (the near-linear low end no longer compresses hum up toward the
note); A1 `sustain=0` moved from −17.6 dBFS (audibly still a fuzz, the defect in one
number) to −45.5 dBFS (quiet-but-present, 24.5 dB above the −70 audibility floor).

### 43.4 Tests

`testSustainRange` (`test_muff_model.cpp`) now pins the player property: SUSTAIN 0.15
at a 0.1 V pluck must measure **< 20 % THD** and sit **≥ 15 dB below the wall's RMS**
(measured 16.1 % / 19.9 dB at all three rates). Perturbation-proven: flipping
`kSustainMinDb` to −54 (the exact old law) fails the THD bar at 43 % — `touch` after
both patch and restore, per §29. The A2/A3 reference tables in
`test_player_expectations.cpp` are re-baselined for the two Muff rows the fix moved;
the 0.6/1.0 probes and every other gear row are untouched. Goldens: **zero moved**
(`muff_twin` reports Δ 0.00 dB) — this slice needs no blessing.

Out of scope, still owned elsewhere: the ~2 mV clip-stage headroom question (whether
the idle-conducting bias is itself right is a research slice needing an external
reference), the bass defect (`finding16-muff-almost-no-bass`, ADR 009), and the
±20 V slam ledger (`muff-slam-exhausts-newton-cap`, §34) — the taper change does not
touch the solver.

## 44. The Twin's VOLUME pot position — headroom recovered from the AB763 order

*Date: 2026-07-30 · Branch: `claude/amps-pedals-fixes-6f557i` · Field report: "breakup at 50 on the twin means not enough headroom"*

Slice 2 of the field-report round. The model applied the channel VOLUME **after** the
V2 recovery stage, so V2's drive was volume-independent: at hot pickup level
(0.316 V) the amp carried a **~4.4 % THD floor the knob could not remove** — measured
identical V2 drive at VOL 0.1 and VOL 0.5, and a stage decomposition put the
distortion in V2 (60.9 V peak, 4.2 %, all even) with V1 and the stack clean. In the
real AB763 the channel volume sits **between the tone stack and V2's grid**, so
turning it down unloads every following stage — that pot position is where a
blackface's headroom physically comes from.

### 44.1 The fix

`TwinPreamp::process` now applies the per-sample VOLUME + BRIGHT treble-bleed loop
between `tone_.process` and `stage_[V2].process`; V2 is the last element of the
preamp. The bright bleed moves with the pot because the cap physically sits
across it. Smoothing unchanged (finding 6 discipline); no constants re-derived — at
VOL 1.0 the pot is unity and bright contributes zero, so the fully-open chain is
**bit-identical** by construction (verified: identical RMS/THD to the digit at
VOL 1.0 in the same-harness A/B).

### 44.2 Measured (220 Hz, 48 kHz, same harness both sides)

0.316 V hot pickup, THD % by VOLUME — before → after:

| VOL | 0.1 | 0.3 | 0.5 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|
| before | 4.14 | 4.51 | 5.32 | 6.90 | 7.96 | 9.01 | 9.62 |
| after | **1.42** | **1.50** | **2.18** | **4.23** | 5.85 | 7.79 | 9.62 |

Breakup onset (5 %) moves from **VOL ≈ 0.45 to ≈ 0.75**. RMS is matched to
**≤ 0.08 dB at every knob position** (≤ 0.01 dB at pluck level) — this is a headroom
change, not a level change. At pluck level (0.1 V) the amp now sits at 0.45–0.69 %
through VOL 0.5 (was 1.29–1.67 %). At 110 Hz the pre-fix floor is even uglier —
8.5–13 % everywhere, 10.56 % at VOL 0.5 — which is the number the new test bar quotes.

### 44.3 Knock-ons, all measured

- `testProduct`'s clean bar improved 3.41 % → **1.17 %** (its 4 % bar unchanged — it
  simply stopped being tight). Cranked figures (21.4 % total / 19.0 % odd / 0.898
  peak) **unchanged**, because VOL 1.0 is an identity.
- **New perturbation-proven bars** in `test_twin_amp.cpp` (`testHeadroomSagStability`):
  hot pickup at VOL 0.5 < 5 % (measured 3.48 % at its 110 Hz probe) and VOL 0.9 both
  > 5 % and > 2× the mid-volume figure (measured 10.4 %). Moving the volume loop back
  after V2 fails the mid bar at 10.56 % — 2× over.
- The five `finding7-ac30-*` XFAILs still XFAIL (confirmed; the Twin comparisons got
  *harder*, which is the expected direction — the AC30 slice owns them).
- The web amp-level drift guard passed **without** re-centring (Playwright 71/71):
  the guard probes at a level where RMS moved ≤ 0.08 dB.
- Goldens: `sd1_twin_reverb` **+2.18 dB RMS (worst band +5.00 dB @ 2540 Hz)** and
  `muff_twin` **+5.85 dB (+4.82 @ 4032 Hz)** — the two pedal-driven Twin rigs. The
  pedals drive the Twin far past 0.316 V, so pre-fix V2 was a hard compressor on
  those rigs; the new renders are louder and brighter because that compression was
  the defect. `rat_jcm800` / `ts_ac30` **UNCHANGED at 0.00 dB** and `clean120_chorus`
  at 0.11 dB (the scope check). Re-blessed with owner authorization (see GOLDENS.md).

### 44.4 What this deliberately does not do

No re-taper of the VOLUME law (the knob's travel was never the complaint — its
*authority* was), no constant re-derivation, no touch of the power section. The Twin
comparison rows inside the AC30 XFAIL bars now describe a healthier Twin; the AC30
slice re-measures them when it lands.

## 45. Audit finding 8 — Ra2, the JCM800 PI's plate pair, re-measured with an honest tail

*Date: 2026-07-30 · Branch: `claude/amps-pedals-fixes-6f557i` · Slice 3 of the field-report round*

Finding 8 said the 2204's "asymmetric for balance" plate pair (100 k/82 k) does not
balance this model's legs. §42's tail reference improved the ratio 0.607 → 0.703 and
explicitly deferred the plate pair; this slice is the deferred re-measure and fix.

### 45.1 The sweep, and the value

Post-tailRef sweep (scratch replica of `Jcm800PowerAmp`, digit-identical to shipped at
82 k): 82 k → 0.703, 100 k (the audit's guess) → 0.841, 110 k → 0.915, **120 k →
0.988**, 130 k → 0.944. `Ra2 = 120 k` shipped. DC operating points stay inside the
project windows (plates 76–78 % of B+, 0.72–0.76 mA/triode). Like `tailRef`, this is a
model parameter calibrated to land the real circuit's *measured property* (balanced
anti-phase legs) rather than a parts-bin value — the real 82 k balances the real
circuit, whose tail return and parasitics this three-node model does not reproduce.

### 45.2 Measured effects

- **Leg balance** ×27.58/×27.24 = **0.988** (bar ≥ 0.90) — `finding8-jcm-pi-leg-balance`
  XPASSed and is DELETED; the balance is the hard assertion in `LtpProbe.h` now.
  Perturbation: Ra2 back to 82 k fails it at 0.703.
- **Push-pull even-harmonic cancellation improved but is NOT fixed: 8.1 → 12.7 dB**
  under the single-ended EL34 baseline (bar ≥ 20; the Twin gets ~30).
  `finding8-jcm-even-harmonic-cancel` therefore STAYS XFAILed, re-owned honestly: a
  balanced small-signal divider is necessary but not sufficient, and the remaining
  even content survives matched leg gains — candidate mechanisms (unmeasured): the
  PI legs' unequal *output impedances* driving the EL34 grids, the single-ended NFB
  injection's common-mode half, large-signal leg divergence. Its own investigation;
  do NOT chase it by re-detuning Ra2.
- **Full-amp THD floor** (220 Hz, 0.1 V, MASTER 0.5): GAIN 0.2 3.59 → 2.71 %, GAIN 0.3
  6.59 → 5.07 %, GAIN 0.5 13.2 → 10.7 % (diagnosis harness). The A3 reference row
  moved 0.0→11.1→46.5 to **0.0→9.3→48.5** — two points of even-harmonic hair off the
  mid-gain floor, slightly harder PI drive at max. Output level +0.2–0.3 dB.
- **NFB window re-derived** (`test_jcm800_power.cpp`): the loop deepened 5.98 →
  **6.37 dB measured vs 6.35 analytic** — the `1/(1+β·A_real)` identity holds within
  0.02 dB, so the old < 6.0 ceiling (calibrated against the 82 k pair) was moved to
  < 8.0 with the derivation in the comment: the window guards token (< 2 dB) and
  runaway (> 8 dB; a doubled β measures ~10) loops, the identity guards the value.
- **Composed 48 k alias floor re-derived**: the balanced pair swings harder per grid
  volt, so the max-GAIN/max-MASTER compound IMD floor at 48 k moved −58 → measured
  **−54.7 dB** (44.1 k −65.5, 96 k −64.5, all still M2-clean). Bar −55 → −52, same
  ~3 dB margin structure, with the escape hatch named in the comment (oversample the
  PI if it drifts further, don't lower the bar again). The aliasing printf now runs
  *before* its asserts so failures carry their numbers.
- **Golden `rat_jcm800`: +0.14 dB RMS, worst band 0.26 dB @ 1008 Hz** — inside the
  ±1.0/±1.5 gates, so the suite is green with NO re-bless; the drift is recorded here
  instead. The other four goldens unchanged (scope check).

### 45.3 What the player hears

Low-gain (GAIN 0.2–0.3) is noticeably less hairy — the ~99 %-even-harmonic dirt the
unbalanced pair leaked at *every* level is down ~4.6 dB at the power section — and
palm-mute fundamentals carry less 2nd-harmonic low-mid mud (the 180–220 Hz content the
chug diagnosis measured). The cranked ceiling is slightly *more* saturated. This slice
plus the coming 470 pF bright cap are the measured pair behind the owner's "gain way
too powerful / still flabby" report; the GAIN-taper question is deliberately deferred
until both land and the owner re-tests at unity trim (docs §44 note on the trim-80
discovery: the field reports were made at +16.8 dB input boost).

## 46. The AC30 gain structure — the top-boost channel completed

*Date: 2026-07-30 · Branch: `claude/amps-pedals-fixes-6f557i` · ADR 015 · Owner decisions: stack correction in-slice; VOLUME between V1 and V2*

The slice the §42.8 ledger commissioned: five `finding7-ac30-*` XFAILs owned by "an AC30
gain-structure slice (the missing top-boost gain stage + VOLUME re-taper)", plus audit
finding 5 (the tone stack's structural mid notch), folded in by owner decision because
they are one physical circuit. Field report driving it: "the vox isn't voxy at all —
not jangly, duller than the Twin, and it doesn't break up with volume correctly."

### 46.1 What was missing, what was wrong

The §23 model was ONE triode into the tone stack into a volume multiplier. The real
top-boost channel is V1 → the top-boost gain triode (V2) → a cathode follower driving
the tone network → volume → PI. And the stack netlist carried THREE structural errors:

1. **Cb stamped in parallel to ground** instead of in series with the bass rheostat —
   audit finding 5's headline: a hardwired ~−36 dB mid hole no knob could dial.
2. **No load on the wiper output** — finding 5's second item.
3. **The slope resistor fed the treble pot's BOTTOM** (the "bass" node) instead of the
   top — NOT in the audit; found by measurement in this slice when the corrected stack
   was driven from the follower's low impedance and the treble knob measured 1.9 dB
   *inverted*. With the R1∥Ct-split-to-opposite-ends arrangement, the slope resistor is
   the lower-impedance path at HF, so the "bass" end of the pot was the brighter node.
   R1 and Ct both belong on the pot top; the pot bottom connects only through the bass
   branch, whose frequency-dependent shunt is what the bass knob actually controls.

### 46.2 The build

- `Ac30Preamp`: `STAGE_COUNT` 1 → 3 — V2 (canonical 100k/1.5k‖25µ warm stage, same as
  V1) and a direct-coupled cathode follower (Rk 100 k, `gridBias` from V2's plate at
  prepare, the `Jcm800Preamp` pattern). The stack's source impedance is the follower's
  measured **226 Ω** (was V1's ~45 k plate). `latencySamples()` sums all three stages
  (72 → 216 samples at 48 k; +3 ms — the known per-stage-oversampling cost, its
  consolidation stays a roadmap item).
- **VOLUME between V1 and V2** (owner decision — the historic top-boost-kit insertion
  point): the knob sets how hard V2/CF/stack/PI are driven, so breakup TRACKS the knob.
  Per-sample smoothed as before.
- `TopBoostToneStack`: 6-node MNA (new N5 under the series Cb), the 500 k wiper load,
  R1 to the pot top, and the bass rheostat behind a **square-law knob map** (the real
  1 M bass pot is log; linear left the entire Vox "V" in the last 5 % of rotation).
- Constants re-derived by measurement, each to its §42.6 convention:
  `audioTaper` k **4 → 8** and `kInterstageScale` **0.67 → 0.03**, chosen JOINTLY by a
  parameter search against the five voicing bars simultaneously (the k=8/s=0.03 row of
  the search table: clean 0.1 % at knob 0.35, onset ≈ 0.52, 10.8 % at 0.6, monotonic to
  80 % at full, h2 margin +4.6 dB); `kFullScaleSecV` **10.0 → 12.2** from the measured
  cranked secondary swing (10.97 V) on the same product-probe convention as the JCM/Twin.

### 46.3 Measured (48 kHz; hot pickup = 0.316 V, pluck = 0.1 V, 220 Hz)

| Property | before (§42.8) | after |
|---|---|---|
| Breakup onset (5 % THD, hot) | VOL ≈ 0.7 (and flat) | **VOL ≈ 0.5–0.6, tracking the knob** |
| THD at documented 0.6 hot | 3.80 % | **10.8 %** (Twin 3.0 % → 3.6×) |
| Composed h2 at 0.6 | −33.0 dBc (< Twin) | **−19.8 dBc, +10.8 dB over the Twin** |
| Clean bar (0.35, pluck) | — | **0.24 %** (chimey-clean) |
| 880 Hz vs the 220 Hz–3 kHz band | −26 dB hole | **flat within 1.4 dB** |
| Mid fullness vs the Twin (700 Hz rel own 3 kHz) | — | **+12.0 dB contrast** |
| Treble knob authority @ 6 kHz | +4 dB (against V1's 45 k source) | **+8.3 dB (follower-driven, corrected netlist)** |
| Cranked peak | 0.87 | **0.898/0.901** (re-derived full scale) |

The five XFAILs XPASSed → deleted → their unchanged bars are hard assertions
(`testChime`, `testBreakupOrdering`); the AC30 suite carries **zero** known-bad
properties and its `--xfail-ledger` CMake registration is removed (repo XFAILs 15 → 10
across 4 ledgers). The chime probe was re-derived to the composed amps per §42.9 — in a
defect-free model both PIs are balanced and both push-pull stages rightly cancel their
own evens, so the old power-section-only probe structurally cannot show class-A chime;
the evens live in the single-ended V2 + cathode-bias bloom, which is where the probe
now looks (the power-section figures stay printed as a PI-regression tell).
`testCharacterGuard` was re-derived from "3 kHz rel 1 kHz beats the Twin" (its 1 kHz
reference sat INSIDE the notch — a vacuous pass of the §42.8 trap class) to three
measured properties: mid-fullness contrast vs the Twin, band flatness (the anti-notch
bar), and mids-over-bass.

### 46.4 Perturbations (touch after both edit and restore, per §29)

- Full pre-§46 netlist restored (both runtime and analytic): suite RED at the stack's
  own bars (gain-loss/discretization) before the character guard is even reached —
  defense-in-depth noted in the guard's comment.
- Volume taper back to k = 4: RED at the product clean bar (5 % exceeded at 0.35).
- Parallel-Cb alone (with R1-top + load): produces a *different* wrong stack, caught by
  the stack's midband-loss bar — recorded to show the notch needs the R1-split as an
  accomplice; the full reversion is the historical regression.

### 46.5 Knock-ons

- `ts_ac30` golden: **−4.94 dB RMS, worst band +17.96 dB @ 800 Hz** — the notch filling
  in, plus the honest re-normalization; owner-blessed (GOLDENS.md). Other four
  byte-stable except `rat_jcm800`'s documented §45 +0.14 drift.
- Web `amp.spec.ts` swap test: its "swap landed" detector asserted a > 20 % RMS change
  across the clean120→AC30 swap — a LEVEL discriminator that lost its teeth when the
  re-normalization landed the AC30's level near clean120's at that probe. Re-derived to
  a harmonic-signature detector (h2+h3 rel f1; ~120× contrast) probed at volume 0.75
  where the AC30 is honestly driven, with the no-pop ratio keeping its own calibrated
  volume-0.4 probe. The drift-guard windows held without re-centring (AC30 row moved
  −4.9 dB, well inside ±8).
- CPU, interleaved same-machine A/B (two rounds each): AC30 **30.07/30.43 % →
  46.54/47.15 %** of one 48 k stream (+55 % relative — the two extra per-stage
  oversampled triode solves), still below the JCM800's 54.51/55.75 % in the same runs.
  Latency +144 samples (preamp 72 → 216). Consolidating the preamp cascade into one
  shared oversampling domain is the named follow-up for both numbers.

## 47. The JCM800 gain-pot bright cap — the 470 pF the model never had

*Date: 2026-07-30 (overnight) · Branch: `claude/jcm-bright-cap-6f557i` · Slice 4 of the field-report round*

The "flabby chugs" diagnosis (2026-07-30) ruled out sag dynamics (measured tight: 0.17–0.35 dB
burst compression, zero chug-to-chug drift) and the coupling corners (they match the 2204
schematic), and landed on a missing component: the real 2204 has a **470 pF bright cap across
the gain pot** (top lug → wiper), and the model's gain network was a frequency-flat scalar. At
mid gain the model therefore fed the clipping stages substantially more relative sub-200 Hz
than the real amp — full-bandwidth low-frequency clipping is the flabby-chug signature, and it
was gain-dependent, matching the report.

### 47.1 The network, solved exactly

Source → Rs = 470 k → pot top → [Ru = (1−w)·1M ∥ 470 pF] → wiper (out, high-Z into V1B) →
Rl = w·1M → ground:

    H(s) = (n0 + n1·s)/(d0 + d1·s),  n0 = Rl, n1 = C·Ru·Rl,
                                     d0 = Rl+Rs+Ru, d1 = C·Ru·(Rl+Rs)
    H(0) = Rl/(Rl+Rs+Ru) = kGainDivider·w   ← EXACTLY the pre-§47 scalar
    H(∞) = Rl/(Rl+Rs);  zero at 1/(2π·C·Ru) ≈ 385 Hz at noon

Settled levels are therefore **unchanged by construction** — the change is purely spectral.
**Correction of the diagnosis figure:** the agent's +10–18 dB tilt came from a pot-only law
(HF → 1/w); the series 470 k loads the shorted-cap divider and roughly halves the tilt in dB.
The honest, shipped numbers: **+8.0 dB at GAIN 0.5, +5.7 dB at GAIN 0.7, → 0 fully open**.

### 47.2 Implementation

One-pole/one-zero bilinear in `Jcm800Preamp`'s per-sample gain loop; coefficients re-derived at
a 32-sample control rate from the smoothed wiper (`rebuildBrightCap` early-outs when parked —
finding-6 discipline), state denormal-guarded (rests at zero) and cleared in `reset()`. At
wiper ≥ 0.9995 the bilinear pole would sit at z = −1 (the network collapses to the DC divider),
so the code takes the exact pre-§47 scalar path — which doubles as the GAIN 1.0 bit-identity
guarantee (verified: identical render hash against the pre-slice build).

### 47.3 Measured

- Preamp drive tilt into V1B (5 kHz vs 110 Hz, rel GAIN 1.0 so the tone stack cancels):
  **7.8 dB at gain 0.5 (analytic 7.5), 5.6 at 0.7 (analytic 5.5)**, 0.0 at 1.0 (bit-identical
  hash). Asserted by the new perturbation-proven `testBrightCap` (kBrightCapF = 0 fails the
  ≥ 4 dB bar outright); the chain-gain analytic test now carries the same nodal |H(f)|.
- The single-tone full-amp response barely moves (the power stage compresses single tones
  toward the same ceiling) — the fix lives in the drive spectrum into the clippers, which is
  where chug tightness is made.
- Golden `rat_jcm800`: **−0.44 dB RMS, worst band 6.73 dB @ 1008 Hz** — a real spectral
  re-voice of the RAT→JCM rig (it renders at GAIN 0.7). Outside the ±1.5 dB band gate, so the
  bless is the owner's; until authorized the branch's core CI job is red at the golden gate by
  design (the finding-15 precedent). Other four goldens byte-stable (scope check).

## 48. The spring reverb's wet trim — the field report, measured and halved

*Date: 2026-07-31 (overnight) · Branch: `claude/twin-reverb-6f557i`*

Owner, at unity input trim: "the reverb is still about twice as strong as I'd expect, at
least on the twin sixty five." Measured on a decaying 220 Hz note through the composed
TwinAmp: with `kWetGain = 3.0` the wet reached PARITY with the dry at knob **0.40** and sat
**+6.5 dB over** the note at 0.60 — a drowned mix by mid-knob. "About twice as strong" is
−6 dB of wet: **kWetGain 3.0 → 1.5** moves parity to ~0.60 and leaves the `sd1_twin_reverb`
golden's 0.25 at −14.6 dB under the note. The equal-power squared knob law is untouched
(0 stays bit-exact dry). The constant was a documented taste trim, so the owner's calibrated
report is the honest derivation source; the full before/after table lives in the plan file
and the constant's comment. Golden `sd1_twin_reverb` −0.83 dB RMS / 6.02 dB @ 504 Hz —
owner-blessed (GOLDENS.md) before merge; every other rig renders reverb 0 and is byte-stable.

### §48 amendment — "the top half is too strong to ever use" is the instrument, not the model

*Date: 2026-07-31 · Branch: `claude/reverb-coaching-6f557i` · docs + prompt only, zero DSP change*

Follow-up report after the trim above landed: the top half of the reverb knob is still
"too strong to ever use." The owner commissioned research before touching `kWetGain` a
second time, and it came back saying the knob is behaving like the real amp. On a blackface
Fender the usable range **is** the bottom third: real owners run reverb **1-2.5 out of 10**,
past **3-4** it takes off into Dick Dale surf territory, and there is a whole **dwell-mod
industry** (tank swaps, dwell/mix pot mods) for players who want the top of the dial to do
something else. Against that, our measured parity point — wet reaching parity with dry at
knob **~0.60** — is if anything **polite**: a real Twin is drenched by ~0.35.

**Decision (owner, 2026-07-31): keep the authentic law. No DSP change.** `kWetGain = 1.5`,
the squared equal-power knob law and the bit-exact dry at 0 all stand exactly as §48 left
them, and no golden moves. Re-tapering to make the top half "usable" would be modeling a
modded amp while claiming a stock one, and it would put a second taste constant on top of
the one the field report already calibrated.

The fix moved to the **assistant** instead: the coach now says where the knob is usable
(15-35 for classic spring presence, ~40+ as deliberate surf/drip), and says that this
matches real Twin behavior rather than apologizing for it. The text is appended to the
Twin's `reverb` bullet in the **stable** `SYSTEM_PROMPT` block of
`web/src/assistant/prompt.ts` — no new tool, no volatile rig-state change, so the
`cache_control` prefix is undisturbed — and it closes by noting that the same spring model
backs the reverb knob on all four heads. Plan file:
`docs/work/2026-07-31-reverb-coaching.md`.

## 49. The Muff clip stages' series base resistors — the blowout was the missing bass

*Date: 2026-07-31 (overnight) · Branch: `claude/muff-series-rs-6f557i` · ADR 009's deferred half, minimal slice*

The owner's max-sustain report ("so distorted I can basically hear nothing") and audit finding
16's bass half are one defect: the clip stages' missing input network. Without the schematic's
series base resistance the feedback diodes cannot form their limiting divider — at high drive
the stage blows past the ±0.6 V clamp (collector dragged to 6.7 V, phase +30°), each stage
preferentially amplifies the previous stage's distortion products over the note (fundamental
partially CANCELLED at 110 Hz: 1177 % THD), and the 470 pF Miller cap has nothing to work
against so the ~1.2 kHz anti-harshness rolloff never forms. Full diagnosis with the external
references (ElectroSmash topology, the C6/C7 DC-block) in the 2026-07-30 research report;
key figures reproduced in the plan file.

`BjtStage::Config::Rs` carries the resistor through the solver (residual, Jacobian, DC solve,
companion update) with **Rs = 0 reducing exactly to the stock solver** — verified bit-identical
by render hash, so the RAT/GOLD/SD/TS users are untouched. Q2/Q3 get the schematic 10 k.
Measured at max sustain, 0.1 V / 220 Hz: **150.5 → 39.8 % THD at an unchanged −4.6 dBFS** —
the wall stayed a wall (input sweep spread 0.09 dB at SUSTAIN 0.7) and became articulate.
Low E through the pedal: −41 → −14.2 dB re 1 kHz — most of the bass back; the residual is the
DC-coupled diode branch still loading the base node (~1.8 k vs the real ~4 k), owned with the
re-worded `finding16` XFAIL by ADR 009's DC-blocked-branch follow-up (a 4th Newton node).
The slam ledger improved 6 → 5 cap-exhausting combos and stays. The §43 sustain floor was
re-derived on the corrected circuit (clean window ~10× wider): −84 → **−70**, landing the same
player bars (knob 0.14 = −25.9 dBFS / 10.7 %; perturbation lineage in the test comment).
Golden `muff_twin` −3.06 dB RMS / 3.76 dB @ 4032 Hz (the un-blown-out default voice) —
owner-blessed before merge; A2/A3 reference rows re-baselined.

## 50. The GOLD gain gang re-derived — the schematic's law, not a linear pot

*Date: 2026-07-31 (overnight) · Branch: `claude/gold-gang-law-6f557i` · field reports "warm, vowley, tinny… gainy at even 35+" and "still warm, and too saturated really" + the 2026-07-31 Klon research report*

### 50.1 The defect: three approximations, all pushing the same direction

The GOLD's §27 build modelled the dual-gang GAIN as a crossfade with a linear drive
law. Against the published schematic (the ElectroSmash / Chowdhury reference the §27
header itself names), all three laws were wrong, and every error added gain:

1. **Drive law.** Shipped: `A = 1 + g·100k/1.5k` — linear, unity at 0, **67.7× at
   max**, and **24.3× at the shipped 0.35 default**. Real: gang 1's lower half sits
   in the drive op-amp's *ground leg*, so `A = 1 + 422k/((1−g)·100k + 17k)` =
   **4.61× → 25.82×**, end-loaded (half the dB range in the last quarter-turn). The
   shipped default was delivering the real pedal's **knob-0.99** drive — the owner's
   "gainy at even 35+" was literally correct.
2. **Drive-path input network.** The drive branch attenuates before the diodes see
   anything: ~0.20× @ 220 Hz / 0.65× @ 1 kHz on the real topology. The model passed
   0.90 @ 220 — its 106 Hz corner belongs to FF1, the always-on clean-bass path,
   and had been mis-assigned to the drive branch. Now `kDrivePreScale = 0.65` + a
   600 Hz one-pole HP, fit to the reference rows (|H|·pre = 0.22 @ 220 Hz vs the
   reference 0.20, 0.56 @ 1 kHz vs 0.65).
3. **The blend laws.** Shipped: clean fades 1.0 → 0.45 while clip rises 0 → 1 — a
   true crossfade. Real: the knob changes **drive, not mix** — the clean feed is
   ~constant (gang 2's divider) and the dirt path's summing weight is **fixed**
   (`kClipBlendWeight = 0.65`, fit). The "clean fades out" folklore is only
   relative to the growing dirt. One idealization kept deliberately: `clipBlend(0)
   = 0` (a short fade-in below g = 0.15), preserving the model's documented
   bit-exact-clean GAIN-0 contract — the real unit measures 0.2–3.9 % THD even at
   min. That is a product contract here, not a physics claim, and the code comment
   says so.

The germanium clipper itself was measured **right** in the corrected topology (the
§36 vindication continues): the "warm/vowley/tinny" reports trace to the drive law
slamming the diodes at knob positions the real pedal keeps clean, not to the diodes.

### 50.2 Measured (220 Hz; THD at 0.1 V / 0.3 V input)

| GAIN | before | after | reference (real unit, research report) |
|---|---|---|---|
| 0.00 | 0.00 % (contract) | 0.00 % (contract) | 0.2–3.9 % |
| 0.35 (default) | 19.2 / 13.0 % | **1.41 / 6.37 %** | mostly clean, a little grit |
| 1.00 | 30.6 % | **15.3 / 15.2 %** | 15–25 % |

Monotonic through the travel; drive-gain span asserted at both ends (`driveGainAt(0)
≈ 4.6068`, `driveGainAt(1) ≈ 25.8235`). Block-A player rows re-baselined: defaults
RMS −22.0 → −27.8 dBFS, default-rig delta +13.2 → +7.3 dB (A4 window re-centred
3..23 → −3..17), THD sweep 0.0→23.4→30.6 → **0.0→2.3→15.3**, treble authority
−25.9→−15.3 (same +10.6 dB span), hum default row −32.9 → −32.0 dB.

### 50.3 Probe re-derivations (the §42.9 discipline)

Two GOLD suite probes were sized to the old, too-hot drive and stopped reaching
their own property once the drive path attenuated:

- **Germanium knee** (`testGermaniumKnee`): the old probe (A = 7.7, 0.01–0.1 V)
  no longer straddled the knee at all. Re-derived to max gain with a 0.025–0.25 V
  sweep — the diode node sees ≈ 0.14–1.4 V, onset-to-deep, which is the straddle
  the knee properties need. Bounds unchanged.
- **Ge-vs-Si ceiling contrast** (`testDiodeLevelContrast`): the old rows (gain
  0.10 at 0.3 V) never reached either ceiling post-fix — the comparison would have
  read the linear path. Re-derived to 0.6 V at gain ≥ 0.35 (node 0.8–3.4 V, past
  both knees).

`testCrossfade` asserts the new flat clean feed and both ends of the gang law;
`testAnalyticLaws` asserts the law symbolically (`1 + 422k/117k`, `1 + 422k/17k`).
Perturbation: restoring the pre-§50 linear law fails at the gang-law assert
("drive gain law drifted from A = 1 + 422k/((1-g)*100k + 17k)"), then the suite is
green again on restore — recorded in the plan file.

### 50.4 Scope

No golden moved: GOLD is in no golden rig, and `--golden-report` on the branch shows
exactly the three inherited stack deltas (§47 rat_jcm800, §48 sd1_twin_reverb, §49
muff_twin) with ts_ac30/clean120_chorus UNCHANGED. The GAIN-0 path is bit-exact
unchanged (transparency contract), so the web "transparent at min gain" spec is
untouched by construction. Deliberate tone change at every other knob position,
argued against the schematic law + the reference THD rows above.

## 51. The JCM800 GAIN pot's taper — breakup moved from "20" to "30", the top pinned

*Date: 2026-07-31 · Branch: `claude/jcm-gain-taper-6f557i` · Owner field report, round 3*

The last knob-feel item of the 2026-07 field-report round, and the first one the owner
made at **unity input trim** with §45 (Ra2) and §47 (the bright cap) already in the
build: *"Breakup is still slightly early. 20 sounds like what I want 30 to sound like.
I like the total saturation though, 100 is perfect right where it is."* No circuit
change — the 2204's GAIN network is now schematic-correct — so this is purely the pot
law, and the sentence is itself the design equation.

### 51.1 The derivation

Both JCM pots ran the same audio taper `(e^{kx} − 1)/(e^k − 1)` with `k = 4`. GAIN's
`k` is now its own constant, derived rather than dialled:

    gainTaper(0.30) == audioTaper_{k=4}(0.20) = 0.022865358743438216
    →  kGainTaperK = 5.0521652926683824   (bisected; residual 4.6e-16 relative)

`kMasterTaperK` stays 4.0, byte-for-byte (verified: the IEEE-754 bit fingerprint of
`audioTaper` over 101 knob positions is `572cafc287be552a` before and after). The GAIN
pot's wiper is the **only** thing the knob controls — the §47 bright-cap coefficients
are rebuilt from that same wiper — so the remap is exact end-to-end: the new GAIN 0.30
render reproduces the pre-§51 GAIN 0.20 render to **−147.0 dBFS absolute / −119.6 dB
relative to peak**, i.e. inside the project's own −120 dBFS solver gate (§25). And
`taper(1) = 1` for *any* k, so the top of the knob is untouched **by construction** —
render hash `484ed0c37929dc69` at GAIN 1.0, identical across the change, which is the
owner's "100 is perfect" honoured exactly. GAIN 0.0 is likewise byte-identical.

Across the travel the wiper drops 7.0 dB (knob 0.05), 6.2 (0.20), 4.1 (0.50), 2.6
(0.70), 0.9 (0.90), 0.0 (1.00) — a reshape of the lower travel, not an attenuator.

### 51.2 Measured — THD vs GAIN, composed `Jcm800Amp`

The §45 probe convention (220 Hz, MASTER 0.5, BASS/MID 0.5, TREBLE 0.6, PRESENCE 0.5,
48 kHz, 4×, THD = harmonics 2..8), at the §45 level of 0.10 V peak:

| knob | THD before | THD after | | knob | THD before | THD after |
|------|-----------|-----------|-|------|-----------|-----------|
| 0.05 | 0.71 % | 0.43 % | | 0.55 | 16.42 % | 9.96 % |
| 0.10 | 1.35 % | 0.73 % | | 0.60 | 22.16 % | 13.14 % |
| 0.15 | 2.15 % | 1.11 % | | 0.65 | 27.87 % | 18.87 % |
| 0.20 | 3.18 % | 1.61 % | | 0.70 | 32.96 % | 25.82 % |
| 0.25 | 4.43 % | 2.28 % | | 0.75 | 37.39 % | 32.17 % |
| 0.30 | 5.73 % | **3.18 %** | | 0.80 | 41.20 % | 37.60 % |
| 0.35 | 6.92 % | 4.32 % | | 0.85 | 44.45 % | 42.16 % |
| 0.40 | 8.13 % | 5.61 % | | 0.90 | 47.14 % | 45.90 % |
| 0.45 | 9.70 % | 6.85 % | | 0.95 | 49.23 % | 48.78 % |
| 0.50 | 12.18 % | 8.15 % | | 1.00 | **50.60 %** | **50.60 %** |

Monotonic before and after. The bolded pair is the remap reading itself back: the new
0.30 row *is* the old 0.20 row (3.178 % / −31.00 dBFS, to every printed digit), and the
1.00 row is bit-identical.

### 51.3 The onset, and an honest correction to the plan's assumption

The plan assumed the ≥ 5 % THD onset already sat at knob 0.20. **It does not at the
§45 probe level** — measured first, before any constant was chosen:

| input (V peak) | 0.05 | 0.075 | 0.10 | 0.15 | 0.20 | 0.25 | 0.30 | 0.40 | 0.50 |
|---|---|---|---|---|---|---|---|---|---|
| pre-§51 onset knob | 0.406 | 0.324 | 0.272 | **0.2057** | 0.165 | 0.137 | 0.117 | 0.089 | 0.071 |

The owner's reported "20" reproduces at **0.15 V peak** — an ordinary unity-trim
pickup level — and the §45 0.10 V probe is simply a quieter reference that reads the
same amp at 0.272. Both are reported, and the shipped constant is derived from the
owner's sentence rather than from either probe:

- **0.15 V (the field-report anchor): onset 0.2057 → 0.3064.** The bar.
- **0.10 V (the §45 reporting convention): onset 0.2717 → 0.3764.**

The knob-space map is level-independent (it remaps the wiper, not the circuit), so one
constant moves every level's onset by the same transform. Choosing k to land the 0.10 V
onset on 0.30 instead would have delivered only −1.6 dB at knob 0.20 — a quarter of
what the report asks for — and was rejected: the owner's ear, not the quieter probe,
is the calibration source, exactly as §48 took "about twice as strong" as −6 dB.

### 51.4 The test bar

`testGainTaperOnset` (`test_jcm800_power.cpp`, run once at 48 kHz) brackets the onset
with two composed-amp renders at the 0.15 V anchor: **THD 4.27 % at knob 0.28 (< 5)**
and **5.93 % at 0.34 (≥ 5)**, plus monotonicity through 0.50 / 0.70 and the law
properties (`gainTaper(1) == 1` and `audioTaper(1) == 1` exactly, `gainTaper(0) == 0`,
the design-equation residual < 1e-15, MASTER still on k = 4 by value). Perturbation:
`kGainTaperK` back to 4.0 → **THD 7.10 % at knob 0.28**, `Assertion 'tLo < 0.05 &&
"JCM800 GAIN breaks up EARLIER than knob 0.28 (taper too hot)"' failed` — restored,
green again.

### 51.5 Scope, and the golden

Player-expectations reference rows re-baselined (comments only, no bars moved): A1
defaults RMS −33.5 → −37.6 dBFS and peak 0.126 → 0.095; A3 GAIN THD 0.2→10.8→48.5 →
**0.2→6.9→48.5**; A3 MASTER level −240→−18.4→−8.9 → **−240→−21.9→−7.5** (the master's
own law is untouched — what moved is the drive reaching it, and a less-driven power
section decompresses, so the master's top half is worth **+14.4 dB** where it was
+9.5); A3 treble −30.3→−15.7 → −29.3→−14.1; A4 default-rig delta +1.7 → **−2.5 dB**,
comfortably inside the unchanged −10..+10 window. The web amp-level drift guard needed
no re-centring (Playwright green).

Golden **`rat_jcm800`: −1.08 dB RMS, worst band 4.50 dB @ 2016 Hz** — the rig renders
at GAIN 0.7, where the taper is 2.6 dB colder, and the power section gives back half of
it. Outside the ±1.0 dB gate, so the bless is the owner's; until authorized the core
suite is red at that one assert by design (the §36/§47 precedent). The other four
goldens are UNCHANGED (≤ 0.00/0.11 dB) — the scope check.

## 52. The GOLD dirt summing weight — the schematic's network, and an honesty gate that fired

*Date: 2026-07-31 · Branch: `claude/gold-summing-6f557i` · field report (owner, Drive doc "Clipper Feedback"): "Much better, but still too much gain. Edge of breakup is around 5, 0 is fully transparent which is good, 100 gain and it sounds like a marshall at mid-high gain… really they only get creamy/crunchy at max."*

### 52.1 What was fitted, and what it is now

§50 left exactly one fitted constant in this pedal: `kClipBlendWeight = 0.65`, the
dirt path's weight at the summing node. This slice replaces it with the published
circuit's own number.

**Sources.** The ElectroSmash gain-stage and full-circuit schematics (the reference
§27 already names), re-checked component-for-component against Jatin Chowdhury's
`KlonCentaur` reference implementation — `ChowCentaur/GainStageProcessors/{PreAmpStage,
AmpStage,FeedForward2,ClippingStage,SummingAmp}.h/.cpp` plus `Paper/Klon_Model.tex`
and `Paper/Figures/{FullCircuit,GainStageCircuit}.png`. §27's honesty note records
that the DAFx-19 paper "was NOT reachable from this build environment"; **it was
reachable this time**, netlist and figures included, via the author's repository
rather than arxiv.org. Every number below is from that netlist and is cross-checked
against the schematic figure.

**The summing stage is a transimpedance amp, not a mixer.** U2A's inverting input is a
virtual ground; three paths push a *current* into it and one feedback resistor turns
the sum into a voltage:

| path | route to the virtual ground | transconductance |
|---|---|---|
| dirt | diode node → `C10` 1 µF → `R16` 47 kΩ | `1/R16` = **21.277 µS** |
| clean, bass (FF1) | node B → `R7` 1.5 kΩ → (`C16` 1 µF ∥ `R19` 15 kΩ) | 5.87 µS @ 220 Hz |
| clean, treble (FF2) | gang-2 wiper → `C11` 2.2 nF + `R15` 22 kΩ (+ `R16`) | 0.15 µS @ 220 Hz |
| feedback | `R20` 392 kΩ ∥ `C13` 820 pF | the transimpedance |

`C10` is the only post-diode network before the summing resistor and it is a **3.4 Hz**
high-pass (`1/(2π·47k·1µF)`), so there is **no in-band post-diode attenuation** to
divide out: the dirt branch measures 47.000 kΩ at 220 Hz. Hence

```
dirt transimpedance   = R20 / R16 = 392k / 47k = 8.3404
clean transimpedance  = R20 · (G_FF1 + G_FF2) = 1.96 @ 110 Hz / 2.28 @ 220 / 1.82 @ 1 kHz
kClipBlendWeight      = (R20/R16) / kSumGain  = 8.3404 / 2.0 = 4.1702
```

The middle row is the slice's **independent check**, and it lands: this model has
carried `kSumGain · cleanBlend = 2.0` since §27 (justified there as "1 + R/R", which
was the wrong reasoning for the right number), and the schematic's clean transimpedance
is 1.82–2.28 across the band — the flat 2.0 is inside ±0.8 dB of it. So the clean side
needed nothing; only the dirt side was a fit. Expressed as a ratio at one frequency the
dirt weight is 3.59 (82 Hz) to 4.88 (1 kHz), band-rms 4.45; the code ships the absolute
form because it reproduces *both* paths' transimpedances instead of picking a frequency.

`kClipBlendFadeTo = 0.15` was re-examined in the same derivation and **deliberately
left alone**. The real network gives it no support whatsoever — the weight is fixed at
every knob position (both gangs are in the drive amp's ground leg and the pre-amp
divider, never in the mix) and the real unit is never clean (0.2–3.9 % THD at min). The
fade exists only to hold `clipBlend(0) = 0`, this model's bit-exact-clean product
contract; no derivation supports any particular span, so re-picking it would have been
a taste move smuggled in beside a derivation.

`kDrivePreScale = 0.65` / `kDriveHpHz = 600` were **validated, not changed**. The same
netlist gives the real drive path as `H_pre(f,g)·H_amp(f,g)` (the pre-amp divider
`C3`/`R6`/`C5`/gang-1 into the amp stage `R10b`/`R11`/`R12`/`C7`/`C8`); its shaping
`H_pre·H_amp/A(g)` measures **0.2234 @ 220 Hz / 0.5343 @ 1 kHz** at the shipped
g = 0.35, against this model's `kDrivePreScale · HP600` = **0.2238 / 0.5574**. §50's fit
was right to 0.02 dB / 0.35 dB at the default.

### 52.2 Measured (48 kHz, 220 Hz, TREBLE 0.5, OUTPUT 0.5)

GAIN 0 is **bit-identical** before and after — FNV-1a over the render is
`85a97e9efc5686ba` (220 Hz 0.15 V sine) and `5b6b300ab9fd3a0d` (0.5 s white noise) on
both sides. The transparency contract is preserved by construction.

THD %, and the dirt-only RMS as a ratio to the GAIN-0 (clean) RMS — i.e. the
dirt-to-clean level at the summing node:

| GAIN | THD 0.10 V before→after | THD 0.15 V before→after | dirt/clean 0.15 V before→after |
|---|---|---|---|
| 0.00 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 |
| 0.15 | 0.86 → 1.59 | 2.06 → 3.96 | 0.641 → 4.114 |
| 0.35 (default) | 1.41 → 2.42 | 3.25 → 5.84 | 0.727 → 4.662 |
| 0.50 | 2.27 → 3.63 | 4.66 → 7.97 | 0.807 → 5.178 |
| 0.75 | 5.82 → 8.33 | 8.69 → 13.55 | 0.988 → 6.340 |
| 1.00 | 15.30 → 19.60 | 16.77 → 23.70 | 1.289 → 8.270 |

Output level at the 0.15 V anchor: GAIN 1.0 **−14.00 → −0.75 dBFS (+13.3 dB)**;
default GAIN 0.35 **−16.39 → −5.34 dBFS**. Breakup onset (first GAIN whose THD crosses
a bar, 0.15 V): 1 % 0.08 → **0.02**, 3 % 0.32 → **0.08**, 5 % 0.53 → **0.28**,
10 % 0.81 → **0.61**.

### 52.3 THE HONESTY GATE FIRED. Read this before touching the constant

The plan file's gate says: *if the schematic-derived weight does not land the owner's
percept, do NOT re-fit to taste — ship the derived value and report the gap.* It fired,
and in the direction nobody expected:

**The derived weight is 6.42× LARGER than the fit it replaces.** Breakup onset moves
*earlier* (5 % THD at GAIN 0.53 → 0.28), the pedal gets **13.3 dB louder** at max, and
max THD at the 0.15 V anchor goes 16.8 → 23.7 % — the top of the 15–25 % reference band
rather than the middle of it, and 27.5–28.3 % at 0.30/0.50 V, outside it. None of the
plan's acceptance criteria is met except "ratio = schematic value", which is met by
construction. **The mix was not the cause of "too much gain", and the schematic says so
in the strongest possible terms: in the real pedal the dirt is coupled to the summing
node roughly six times harder than this model had it.**

Two coupled defects explain where the perceived gain actually lives, both measured
here, both named as the next probe (as the gate requires):

1. **The diode node runs hot.** This model's germanium pair is `Is = 200 nA, n = 1.3`
   → knee **0.286 V** at 1 mA, through `Rs = 2.2 kΩ`. The reference implementation's
   pair — fitted to a real unit — is `Is = 15 µA, Vt = 25.85 mV` → knee **0.109 V**,
   through the schematic's `R13 = 1 kΩ`. That is ~2.1× (6.5 dB) of clamped amplitude,
   and the derived summing weight multiplies it. The schematic's weight is right; it is
   being applied to a node that is too loud. **This, not the mix, is the next slice.**
   Note the direction of the residual: even so, this model's *drive* is COLDER than the
   reference at high gain — the reference's `C7 = 82 nF` across `R10b` gives the amp
   stage a 1 kHz gain of 100.5× at g = 1 against its 25.8× DC law, where this model
   holds a gain-independent 0.557 shaping. Correcting that makes the dirt hotter still.
2. **The 495 Hz summing-amp pole does not exist here.** `C13 = 820 pF` across
   `R20 = 392 kΩ` low-passes the *whole sum* at `1/(2π·392k·820p)` = **495 Hz** in the
   real pedal, and the tone control's active treble stage boosts it back. That pole is
   what keeps a clipped Klon *creamy*, and its absence is the best available explanation
   of "sounds like a Marshall at mid-high gain". It was **not** ported in this slice
   because it cannot be ported alone: on this model's flat clean feed it would put a
   ~−14 dB midrange hole in the GAIN-0 path and destroy the transparency spec. It needs
   the tone stage in the same slice, and the owner has signed off the current tone.

### 52.4 What it cost the suite: two XFAILs, both the same root cause

`clipper_gold_tests` gains an XFAIL ledger (repo ledgers 4 → 5; a plain `ctest` now
shows `clipper_gold_tests_xfail_ledger ... ***Skipped`):

- **`gold-summing-rails-engage`** — §27's headroom statement is "the germanium pair is
  the only clipper in the box". At max GAIN with an ordinary 0.2 V pick it now depends
  on the TREBLE knob: at noon the tone stage peaks **2.97 V** against the ±8.6 V
  charge-pump rails (fine), at max it reaches **8.62 V** and the clamp engages.
- **`gold-summing-alias-at-treble-max`** — the shadow of the first. The rail clamp is at
  *base rate*, after the downsampler, so its products do not move with the oversampling
  factor: measured 1× −20.3 / 4× −26.5 / 8× −26.4 dB at 44.1 kHz. A −60 dB "aliasing"
  assertion on that stimulus no longer measures aliasing.

`testAliasing` was therefore **split**, not loosened: the −60 dB M2 bar is now asserted
**hard** on a stimulus where it is still isolated (TREBLE at noon, rails idle), where
the clipper measures **−108.3 dB at 44.1 k / −127.7 dB at 96 k** at 4×; the max-treble
case keeps running, keeps printing its real number, and is the XFAIL above.
`testHeadroomAndOutput`'s 1 V wide-open peak guard moved 3.0 → 6.0 V for the same
reason and it is stated in the comment: 3.0 was a snug drift guard around §27's 1.60 V
measurement, the *property* is the ±8.6 V rail, and the new measurement is 4.302 V —
6.0 dB inside it. The case that genuinely violates the property is the XFAIL, not a
widened bound.

Do **not** answer either XFAIL by re-fitting `kClipBlendWeight`, and do not answer them
by raising `kRailVolts` — that is a real supply.

### 52.5 Probe re-derivations, M11 rows and scope

`testGermaniumKnee` and `testDiodeLevelContrast` needed **no** re-derivation (unlike
§50): the knee probe's two ratios are essentially unmoved (onset 189× → 160×, slope
ratio 0.0063 → 0.0071) because the weight scales the dirt, not the knee, and the Ge/Si
contrast runs with the clean half switched OUT so the weight cancels in the dB
difference (5.88–6.11 → 5.84–6.12 dB). Their recorded absolute THDs were re-baselined.
`testCrossfade` gains the perturbation-proven bar pinning the derived weight —
restoring `0.65` fails it ("dirt summing weight drifted from the schematic's
R20/(R16*kSumGain)"), and the suite is green again on restore.

M11 rows re-baselined: A1 defaults RMS **−27.8 → −15.9 dBFS** (peak 0.273 → 1.285 V,
still inside the 2.0 V pedal ceiling), A2 hum default **−32.0 → −39.6 dB** (min-gain
−30.1 unchanged — GAIN 0 is bit-exact), A3 GAIN THD **0.0→2.3→15.3 → 0.0→3.6→19.6**,
A3 treble HF **−25.9→−15.3 → −22.9→−12.3** (knob authority still exactly +10.6 dB),
A4 window re-centred **−3..17 → 9..29** on a measured +19.3 dB.

**NO golden moved.** `--golden-report` on this branch: all five UNCHANGED
(`rat_jcm800` +0.00, `sd1_twin_reverb` +0.00, `muff_twin` +0.00, `ts_ac30` +0.00,
`clean120_chorus` −0.00) — GOLD is in no golden rig, and the GAIN-0 path is bit-exact,
so the web "transparent at min gain" spec is untouched by construction too. Deliberate
tone change at every other knob position, argued against the schematic and shipped
unfitted, with the gap reported above.

## 53. The Muff clip stages' DC-blocked diode branch — the 4th Newton node

*Date: 2026-07-31 · Branch: `claude/muff-dc-diodes-6f557i` · ADR 009's named follow-up: audit finding 16's remaining half, and the owner's "much much better. It's still a little gutless, it doesn't scream through either now."*

§49 gave the clip stages their series base resistors and closed the max-sustain blowout,
but left the bass 14 dB down and the XFAIL `finding16-muff-almost-no-bass` open, re-owned
to this slice by name. The residual had one cause, and it was a missing component rather
than a wrong constant.

### 53.1 The component, and the reference

Published Big Muff clip stage (ElectroSmash *Big Muff Pi Analysis*; guitarscience.net
*A Case Study: Re-engineering the Big Muff π*, which works the same network):

| designator | value | role | in this model |
|---|---|---|---|
| RS | 10 k | series base resistor | `Config::Rs`, landed §49 |
| RA | 100 k | base-to-ground bias | `Config::Rbg`, **used here** |
| RF | 470 k | collector-base feedback | already present |
| RC | 10 k | collector load | already present |
| **C6 / C7** | **1 µF** | **in series with the feedback diode pair** | **`Config::Cdiode`, new** |
| — | 470 pF | Miller cap across RF | already present |
| — | 100 nF | input coupling | already present |

The published base-network impedance is **RB = RS//RA//RF = 10k//100k//470k = 8.9 k**.
This model measured **~1.8 k**, and ADR 009 refused to pick a compromise resistor value
against that discrepancy — it sent the question here instead.

**C6/C7 are the whole answer.** They keep the diodes out of the DC network. Without them
the branch carries DC: the idle collector-base voltage clears the diode knee, so the pair
**conducts at idle** and clamps the collector 0.26 V above the base. That did two things,
and the model had been documenting the first of them as canon:

* it destroyed the stage's headroom (`BjtStage.h` said "a Big-Muff-family stage has almost
  no clean headroom and clips essentially always" — an artifact, not a fact), and
* it shunted the base node to ~1.8 k, putting each clip stage's **input coupling corner at
  ~900 Hz** — which is audit finding 16's missing bass, both halves of it in one mechanism.

`Config::Rbg` is the resistor §49 plumbed and deliberately left unused, because it only
parks the stage deeper in the knee while the diodes are still shorting the base at DC. It
is **used, not superseded**: RA and C6/C7 are one decision and land together.

### 53.2 The 4-node solve

`BjtStage` gains a fourth unknown, Vd — the junction between the cap and the diode pair:

```
    Vc ──┤ Cd ├── Vd ──(D± pair)── Vb

    Icd = gCd·(Vc−Vd) − gCd·vCd      backward-Euler companion, vCd = Vc − Vd
    Id  = 2·Isd·sinh((Vd−Vb)/nVt)    the pair, now referenced to Vd

    r1 (base)      = … + Id(Vd−Vb) − Ib
    r2 (collector) = … − Ic − Icd
    r3 (emitter)   = Ie − Ve/Re
    r4 (node d)    = Icd − Id(Vd−Vb)
```

Jacobian columns become (Vb, Vc, Ve, Vd); the diode partials move to ∂/∂Vb = −gd and
∂/∂Vd = +gd, and gCd fills the (collector, d) block. The 4×4 is solved by Gaussian
elimination with **partial pivoting** rather than Cramer, because gd spans ~20 decades
between off and slammed.

At DC the cap is open, so r4 degenerates to −Id(Vd−Vb) = 0 — i.e. **Vd = Vb with zero
branch current**. The row is never singular (gd = Isd/nVt·(eˣ+e⁻ˣ) > 0 everywhere). So the
DC solve finds the diodes OFF and the stage biases on RF + RA alone.

**`Cdiode == 0` keeps the 3-node path, bit-identically.** Both systems are instantiations
of one templated damped Newton, so the globalization, the residual early-out and the
iteration accounting cannot drift apart, while `Sys::eval` + `solve3x3` + `infNorm3` are
frozen byte-for-byte. Proof: 15 digests (5 rates × 3 stage shapes — no-diodes Q1/Q4,
DC-coupled diodes + Rs, DC-coupled diodes bare) compared against the pre-slice
header+library, **identical including the Newton iteration counts**; and the whole
`MuffModel` at 10 configurations (4 knob settings + ragged 44.1/96 kHz × 1/2/8 oversampling)
identical before the branch was switched on.

### 53.3 The step-limiting defect this uncovered — and the slam ledger

Switching the branch on took the ±20 V slam ledger from 5 of 16 combinations at the Newton
cap to **7 of 16**, which is the wrong direction. Traced rather than tuned. A failing
sample at 48 kHz × 4:

```
it= 2 cur=2.717e-03 lam=0.03125 dx=[-6.15e+00  1.00e+01 -3.42e-01  1.00e+01]
it= 3 cur=2.487e-03 lam=0.00000 dx=[-1.00e+01 -1.00e+01  8.62e-01 -1.00e+01]
it= 4 … 59 identical, cur frozen at 2.487e-03
```

The step vector is pinned at the damped Newton's **gross safety clamp**, which clips each
**component** to ±10 V. Clipping components rotates the step off the Newton direction, and
a rotated direction is not guaranteed to be a descent direction for the residual norm — so
every one of the 30 backtracks was rejected, `lam` collapsed to 2⁻³⁰ while |lam·dx| stayed
just above the 1e-9 step-size exit, and the solve **stood still** for the whole cap. It was
never diverging.

The 4-node path therefore **scales** the direction to the same 10 V bound instead
(`Sys4::limitStep`). Same magnitude, direction preserved:

| ±20 V slam, worst Newton iterations of 60, all 16 rate × oversampling | result |
|---|---|
| component clamp (the 3-node rule) | **60 at 16 of 16** — not converged |
| direction-preserving scale | **16 at 16 of 16** — converged |

Amplitude sweep at 48 kHz × 4 with scaling: 1 V=10, 5 V=14, 10 V=14, 20 V=16, 30 V=17 — it
degrades gracefully instead of falling off a cliff.

Whole-pedal ledger: **5 of 16 at the cap → 0 of 16, worst 18 of 60**. The XFAIL
`muff-slam-exhausts-newton-cap` XPASSed, is deleted, and the property is asserted outright.

**The 3-node path still clips component-wise, deliberately.** Fixing it there would move
every existing `BjtStage` user's audio, and bit-identity is this slice's contract. It is a
named, measured follow-up.

**A test that had no teeth, found by checking:** the whole-pedal slam test can no longer
prove the globalization, because §53 also un-fits `kClipDriveMax` (below) and a ±20 V slam
at the pedal input now reaches Q2's base six times smaller. Swapping `Sys4::limitStep` back
to component clipping leaves that test GREEN. So the property is asserted where it lives —
`testStageSlamConvergence` drives a clip-configured `BjtStage` directly.

### 53.4 The constants that came off, not on

**`kClipDriveMax` 6.0 → 1.0.** A real SUSTAIN is a 100 kA pot wired as a passive divider
between Q1's coupling cap and Q2's 10 k series base resistor: at the top of its travel it
passes Q1's output through, and it can never pass **more**. 6× was 15.6 dB of gain no pot
can provide, and it existed to slam clip stages that had no headroom. With the branch
blocked each stage biases at Vc = 4.95 V and makes its real ~28 dB (Rc//Rf / Re =
9.8 k / 390, against the reference's ~29 dB theoretical / ~25 dB measured), so the
compensation comes off. This is CLAUDE.md's rule applied in the direction it usually is
not: the error was found, so the constant fitted around it goes back to the physical value
rather than being re-tuned. At the shipped default the voice barely notices (6.0 gives
−4.5 dBFS / 36 % THD, 1.0 gives −4.8 / 33 %) because the diodes set the level, not the
drive; what it buys is the bottom of the knob.

**`kSustainMinDb` −70 → −65**, re-derived against the *same* §43 player bar (knob ~0.15 at
a 0.1 V pluck is a tame fuzz — §49 measured 10.7 % THD there). Sweep at 220 Hz / 0.1 V /
48 kHz:

| floor | THD @ 0.15 | dB below the wall |
|---|---|---|
| −54 | 20.2 % | 3.0 |
| −60 | 14.9 % | 4.3 |
| −62 | 12.9 % | 4.8 |
| **−65** | **9.8 %** | **5.8** |
| −70 | 3.8 % | 8.0 |

### 53.5 Measured results

Operating point, Q2/Q3 (48 kHz × 4), before → after:

| | before | after | published |
|---|---|---|---|
| Vc | 1.213 V | **4.946 V** | low-to-mid volts off 9 V |
| Vc − Vb | 0.261 V | **4.160 V** | — |
| Ic | 0.777 mA | **0.397 mA** | ~0.4 mA |
| idle diode current | conducting | **0.00e+00 A** | zero (C6/C7) |

Small-signal response re 1 kHz (SUSTAIN 0.15, 0.002 V, 48 kHz):

| Hz | 30 | 41.2 | 60 | **82.4** | 110 | 220 | 440 | 1000 | 4000 |
|---|---|---|---|---|---|---|---|---|---|
| before | −42.87 | −32.61 | −21.83 | **−14.24** | −8.74 | −1.07 | +0.74 | 0.00 | +1.32 |
| after | −29.36 | −20.11 | −11.13 | **−5.48** | −1.87 | +2.21 | +2.28 | 0.00 | −7.83 |

The low E clears its −6 dB bar for the first time (audit finding 16 filed it at −41.14 dB;
§49 got it to −14.24). 30 Hz is still rejected at −29 dB, so the pedal is still a coupled
circuit. The 4 kHz column going −7.83 is the 470 pF Miller caps finally working against a
real base-node impedance.

Level / THD (220 Hz, 0.1 V, TONE 0.5 / VOLUME 0.6, 48 kHz):

| knob | before | after |
|---|---|---|
| 0.00 | −38.1 dBFS / 2.6 % | −18.7 / 0.4 % |
| 0.15 | −24.9 / 11.9 % | −10.2 / 9.8 % |
| 0.60 (default) | −4.6 / 36.6 % | −4.8 / 33.1 % |
| 1.00 | −4.6 / 40.8 % | −4.3 / 37.6 % |

The wall stayed a wall **and** stayed articulate: SUSTAIN 0.7 spread across a 20 dB input
sweep 0.09 → 0.40 dB, max-sustain THD 40.8 → 37.6 % (nowhere near the pre-§49 150 %).

**The "scream" proxy** — an exponentially decaying 0.30 V pluck (τ = 0.6 s), time for the
output fundamental to fall 20 dB below its own peak, minus the input's own 1.400 s:

| | 110 Hz | 220 Hz |
|---|---|---|
| SUSTAIN 0.6, before → after | +1.375 s → **+2.575 s** | +1.775 s → **+2.700 s** |
| SUSTAIN 1.0, before → after | +2.875 s → **+4.050 s** | +3.275 s → **+4.175 s** |

The note is held roughly a second longer at every setting: the stages now have a bias to
clip around, so they keep clamping the fundamental long after it used to fall away.

Other ledgers: aliasing 4× at max sustain −116.1 dB (bar −60); DC on signal 0.00003 % of
peak (bar 1 %); hum-alone at min sustain −51.9 dBFS, 41.8 dB below max; tube-solver
production-vs-reference worst **−126.4 dBFS** (gate −120), so the §34 accuracy trade still
holds on the 4-node solve; the idle residual ceiling on the 4-node path measures
**7.05e-19 A** against the 3-node's 2.06e-18, so the 1e-17 early-out has *more* margin
(14.2× rather than 4.9×), and a fully parked stage still does **0** Newton iterations
across all 48 rate × oversampling × sustain combinations.

### 53.6 The CPU cost, and the honest part of it

**This slice is expensive.** Interleaved same-machine A/B, `clipper-bench`, muff row:

| | before | after |
|---|---|---|
| pass 1 | 5.62× realtime / 17.79 % of a stream | 3.34× / 29.94 % |
| pass 2 | 5.63× / 17.76 % | 3.21× / 31.15 % |

`denormal_bench` section 2, per 10 s: signal 1734 → 3095 ms, **silence 382 → 3728 ms**.
The hwFTZ column tracks it exactly (3139 / 3771 ms) and subnormal outputs stay 0, so this
is **not** a denormal cliff — §33's rule applied.

The silence figure needs saying plainly, because it partially undoes §34's headline. A
played-then-quiet clip stage is **no longer at a static fixed point**: its 1 µF DC-block cap
can only discharge through the diodes' own leakage (2·Isd/nVt = 1.9e-7 S, τ ≈ 5 s), which is
what the real circuit does, and it leaves a residual around **2e-11 A** — six decades above
the early-out's 1e-17, so the early-out cannot fire while it relaxes. Measured cost:
**1.75 system evaluations per solve** against 1.00 fully parked, with **0.00 %** of
iterations burning all 30 backtracks. So audit finding 12's *pathology* (31.00 evals) has
not returned; what has gone is the claim that idle is free.

**A blind spot this exposed:** `testIdleSolverCost` carried a comment saying it covered "a
stage that fell quiet after being played", and it did not — the assertion read the fresh
`probe` model and never `m`. The played-then-quiet path was measured by nothing. It is
asserted now, against a bar that says what is true rather than what would be nice.

Named follow-up for the cost: the 4×4 has real structure (J[2][3] = J[3][2] = 0, and the
emitter row does not see Vd), so a specialised solve or a Schur complement onto the
existing 3-node system should recover most of the per-sample overhead. Not attempted here —
it is a perf slice with its own bit-identity bar.

### 53.7 Tests, XFAILs and the golden

`clipper_muff_tests` now has **ZERO** known-bad properties, so its `--xfail-ledger`
registration is removed from `core/CMakeLists.txt` in the same slice (leaving it would
report Passed instead of `***Skipped` — the same "guard that looks present and does
nothing" shape ADR 009 recorded). Core ctest entries 25 → 24; repo XFAIL ledgers 4 → 3.

New/changed tests: `testDiodeBranchIsDcBlocked` (Vd − Vb = 0 and idle diode current
0 A — the property C6/C7 exist for); an **absolute** clip-stage bias block in
`testDcOperatingPoints`, deliberately not derived from this netlist; `analyticBias` extended
with Rbg and with the DC-blocked branch leaving the DC system; `testStageSlamConvergence`;
the played-then-quiet assertion in `testIdleSolverCost`; and the honest re-derivation of the
§43 level bar (15 dB → 4 dB) with its reason in the code — on the corrected circuit the
diodes clamp Q2/Q3's output near 0.65 V at any drive, so the knob governs saturation far
more than level, which is what a Big Muff does.

Perturbation transcript (each patched, rebuilt with `touch` after **both** patch and
restore, run, reverted):

| perturbation | result |
|---|---|
| delete `clip.Cdiode` (mirrored in the test netlist) | RED — clip-stage bias bar (Vc 1.213 V) |
| `Sys4::limitStep` clips components instead of scaling | RED — `testStageSlamConvergence`, 16 of 16 at the cap |
| `kSustainMinDb = −54` | RED — SUSTAIN 0.15 THD bar (20.2 %) |
| `kClipDriveMax = 6.0` | RED — SUSTAIN 0.15 THD bar (23.6 %) |
| delete `clip.Rbg` (mirrored in the test netlist) | RED — clip-stage bias bar |

**Golden:** `muff_twin` moves **−1.09 dB RMS / 13.18 dB worst band @ 252 Hz** (13 bands) —
the bass coming back at the default sustain, which is the change. The other four are
UNCHANGED (`rat_jcm800` +0.00, `sd1_twin_reverb` +0.00 / 0.02 dB, `ts_ac30` +0.00,
`clean120_chorus` −0.00 / 0.11 dB) — the scope check. **NOT blessed by this slice**, so
`clipper_player_expectations_tests` fails on this branch until a human blesses it; that is
the intended end state, and core ctest is red at exactly that one gate and nowhere else.
Playwright is 71/71 green with no probe re-derivation, and the node + electron suites pass.

**The WASM artifact was rebuilt** (`core/` changed) — all three files in the same commit.

## 54. The GOLD clipping stage — the three defects §52 named, fixed against the reference netlist

*Date: 2026-07-31 · Branch: `claude/gold-fidelity-6f557i` (stacked on `claude/gold-summing-6f557i`) · owner decision on §52's honesty gate: "full fidelity slice"*

### 54.1 What §52 left, and why all three had to move together

§52 replaced this pedal's last fitted constant (`kClipBlendWeight`) with the schematic's
own `R20/(R16·kSumGain) = 4.1702` — and the pedal got **louder and dirtier**, the opposite
of the field report that commissioned it. Its honesty section named three coupled defects
and said the mix was not one of them. This slice fixes all three, against the same
oracle: Jatin Chowdhury's `KlonCentaur` reference implementation, whose netlist is a
section-by-section model of the real pedal and whose clipper parameters are **fitted to a
real unit**. The derived summing weight 4.1702 is untouched, and its perturbation test
still guards it.

| # | defect (§52's words) | fix | reference file |
|---|---|---|---|
| 1 | "the diode node runs hot" | `Is` 200 nA → **15 µA**, ideality 1.3 → **1.0** (Vt 25.85 mV), `Rs` 2.2 kΩ → **R13 = 1 kΩ**, plus the node's own **R16 = 47 kΩ** load | `ClippingStage.h` |
| 2 | "the 495 Hz summing-amp pole does not exist here" | `C13 = 820 pF` across `R20 = 392 kΩ` → a **495.06 Hz** one-pole, on the dirt branch (ADR 016) | `SummingAmp.h` |
| 3 | "the reference's drive shaping is strongly gain-dependent" | the drive amp is now the **R10b/R11/R12/C7/C8 network** (`AmpStageNetwork`), not a scalar `A(g)` | `AmpStage.h` |

They had to move together because they push in opposite directions: fix 1 takes the dirt
down ~8 dB, fix 3 puts it back up at mid-band, fix 2 shapes what is left. Shipping any one
alone would have been a level change dressed as a fidelity fix.

### 54.2 Three-way table 1 — the drive-node |H| (input → the diode node's source)

`BEFORE` = `kDrivePreScale·HP600·A(g)`; `AFTER` = `kDrivePreScale·HP600·H_amp(f,g)`;
`REF` = `H_pre(f,g)·H_amp(f,g)` from the netlist. The Δ columns are each model's error
against the reference, in dB.

| f (Hz) | GAIN 0.35 BEFORE | AFTER | REF | Δ before | Δ after | GAIN 1.00 BEFORE | AFTER | REF | Δ before | Δ after |
|---|---|---|---|---|---|---|---|---|---|---|
| 82   | 0.541 | 0.563 | 0.746 | −2.79 | −2.45 | 2.273 | 2.651 | 3.595 | −3.98 | −2.64 |
| 110  | 0.720 | 0.763 | 0.857 | −1.51 | −1.01 | 3.027 | 3.879 | 4.449 | −3.35 | −1.19 |
| 220  | 1.375 | 1.516 | 1.373 | +0.01 | +0.86 | 5.778 | 10.659 | 9.853 | **−4.63** | **+0.68** |
| 500  | 2.558 | 2.677 | 2.523 | +0.12 | +0.52 | 10.746 | 33.722 | 32.397 | **−9.59** | **+0.35** |
| 1000 | 3.426 | 2.856 | 3.284 | +0.37 | −1.21 | 14.393 | 56.011 | 65.316 | **−13.14** | **−1.33** |
| 3000 | 3.918 | 1.555 | 2.264 | +4.76 | −3.26 | 16.459 | 39.149 | 57.184 | −10.82 | −3.29 |
| 6000 | 3.975 | 0.988 | 1.497 | +8.48 | −3.61 | 16.702 | 21.576 | 32.735 | −5.84 | −3.62 |

The headline is the GAIN 1.00 block. The old flat-`A` drive was **−4.6 dB at 220 Hz,
−9.6 dB at 500 Hz and −13.1 dB at 1 kHz** against the real stage; it is now **+0.7 /
+0.35 / −1.3 dB**. Worst error anywhere in the table falls from 13.1 dB to **3.6 dB**, and
that residual is all at 3–6 kHz and belongs to `kDrivePreScale`/`kDriveHpHz` (see 54.7).

### 54.3 Three-way table 2 — the diode node's static transfer curve

Solving `(Vsrc − V)/Rs = V/Rload + 2·Is·sinh(V/nVt)`. `AFTER` *is* the reference's own
network, so the third column is redundant by construction — which is the point.

| Vsrc (V) | BEFORE V | AFTER = REF V | BEFORE over REF |
|---|---|---|---|
| 0.05 | 0.0483 | 0.0215 | +7.00 dB |
| 0.10 | 0.0930 | 0.0378 | +7.81 dB |
| 0.20 | 0.1553 | 0.0581 | +8.53 dB |
| 0.50 | 0.2173 | 0.0857 | +8.08 dB |
| 1.00 | 0.2501 | 0.1056 | +7.49 dB |
| 5.00 | 0.3116 | 0.1494 | +6.39 dB |
| 20.0 | 0.3598 | 0.1858 | +5.74 dB |

Knee at 1 mA: **0.2862 V → 0.1086 V** (2.635×, 8.42 dB). Measured through the *rendered*
model at 1 V input wide open, node-referred: **0.336 V → 0.148 V** — that is the
perturbation-proven bar in `testClippingStageFidelity`.

**Why the reference's fit supersedes the datasheet numbers, said plainly.** The pre-§54
pair (`Is = 200 nA, n = 1.3`) was a datasheet-shaped guess for "a point-contact germanium
diode". The reference's pair is fitted to a real Klon's *measured* clipper. For THIS pedal
a fit to the actual device beats a generic datasheet, and that is the whole argument. Note
what did **not** change: §36's finding was that the *silicon counterfactual* had its
ideality dropped to 1.0 and had stopped being a 1N4148 — that fix stands untouched
(`kSiIdeality = 1.752`), and the Ge/Si contrast is still an asserted property, re-derived
in 54.8 rather than abandoned.

**A consequence worth knowing before you write a probe:** at `Is = 15 µA` the pair's
zero-bias incremental resistance is `Vt/(2·Is) = 861.7 Ω`, against **84.0 kΩ** at the old
`Is = 200 nA`. The reference germanium therefore has **no truly linear region** — it loads
its own node by 6.78 dB even at microvolts. That is the "germanium bloom" as a number, and
it is why every small-signal bar in the suite is a *ratio*, in which the loading cancels.

### 54.4 Three-way table 3 — the dirt path after the summing node, and the creamy filter

The dirt's transimpedance, node voltage → summing-amp output:

| f (Hz) | BEFORE (flat `R20/R16`) | AFTER = REF (`R20/R16·\|pole\|`) | Δ |
|---|---|---|---|
| 82   | 8.340 | 8.228 | −0.12 dB |
| 220  | 8.340 | 7.622 | −0.78 dB |
| 500  | 8.340 | 5.869 | −3.05 dB |
| 1000 | 8.340 | 3.701 | −7.06 dB |
| 3000 | 8.340 | 1.358 | −15.76 dB |
| 6000 | 8.340 | 0.686 | −21.70 dB |

That column is the whole "creamy" argument: at a 220 Hz fundamental the pole costs the
note 0.78 dB and its 5th harmonic 7.1 dB. **Where it is applied — the dirt branch only —
is ADR 016**, and the short version is that "pole on the dirt" is algebraically identical
to "pole on the sum with the clean feed pre-emphasized", which is the idealization this
model has carried since §27 (`kSumGain = 2.0` flat, against a real composed clean path
that measures 2.25 at 82 Hz and 0.76 at 1 kHz). The dirt side becomes exactly the real
composed transfer; the clean side does not move a bit — literally.

### 54.5 The GAIN-0 transparency contract: bit-exact, proven by hash

FNV-1a over four full renders at 48 kHz / 4× / GAIN 0 / OUTPUT 0.5, **before and after**:

| stimulus | hash before | hash after |
|---|---|---|
| 220 Hz 0.15 V sine, TREBLE noon | `85a97e9efc5686ba` | `85a97e9efc5686ba` |
| 0.5 s white noise, TREBLE noon | `d10d3ffca9077b36` | `d10d3ffca9077b36` |
| 220 Hz sine, TREBLE 0.0 | `449ef98662e22ec2` | `449ef98662e22ec2` |
| 220 Hz sine, TREBLE 1.0 | `2217f25842819f17` | `2217f25842819f17` |

Identical, so the plan file's 0.25 dB fallback contract never fired. The mechanism is worth
stating: `clipBlendAt(0) == 0.0` exactly, its smoother starts and stays at 0, and
`0.0f * finite == 0.0f` — every change in this slice lives on the far side of that
multiply. The web "transparent at min gain" Playwright spec is untouched for the same
reason, and so is the M11 A2 min-gain row (−30.1 dB, unmoved).

### 54.6 Measured, at the field-acceptance anchor (0.15 V / 220 Hz, TREBLE noon, OUTPUT 0.5)

| GAIN | THD % before → after | out dBFS before → after | dirt/clean before → after |
|---|---|---|---|
| 0.00 | 0.000 → 0.000 | −19.50 → −19.50 | 0.000 → 0.000 |
| 0.10 | 3.40 → **2.86** | −9.50 → **−14.05** | 2.67 → **0.99** |
| 0.35 (default) | 5.84 → **4.59** | −5.34 → **−11.33** | 4.66 → **1.71** |
| 0.50 | 7.97 → **5.62** | −4.52 → **−10.74** | 5.18 → **1.90** |
| 0.75 | 13.55 → **8.25** | −2.90 → **−9.46** | 6.34 → **2.40** |
| 1.00 | 23.70 → **14.59** | −0.74 → **−7.90** | 8.27 → **3.51** |

Breakup onset (first GAIN whose THD crosses the bar, same anchor): 1 % **0.02 → 0.03**,
3 % **0.08 → 0.11**, 5 % **0.28 → 0.42**, 10 % **0.61 → 0.87**. THD at other input levels,
max GAIN: 0.10 V **19.6 → 13.1 %**, 0.30 V **27.5 → 14.4 %**, 0.50 V **28.3 → 14.7 %** —
the level-dependence has nearly gone, which is what a soft knee behind a fixed drive does.

**HONESTY GATE, second reading.** The plan set five acceptance rows. Four land, one misses:

1. *near-clean at knob ≤ 0.10* — **partially**. 2.86 % THD at GAIN 0.10 (was 3.40 %) is
   grit, not clean; the 1 % crossing is at GAIN 0.03. Against the reference rows the real
   unit measures 0.2–3.9 % THD **even at minimum**, so 2.9 % at knob 10 is inside the real
   pedal's own clean range — but it is not "near-clean" in the sense the owner asked for,
   and this model's own GAIN 0 is silent-clean by contract, so the knob's bottom decade is
   steeper than a real one's. Reported, not fitted.
2. *grit onset mid-knob or later* — **yes**: 5 % at GAIN 0.42, 10 % at 0.87 (§52: 0.28 / 0.61).
3. *max THD in the reference's 15–25 % band* — **MISSED, by 0.4 points**: 14.59 % at the
   anchor, 13.1–14.7 % across 0.10–0.50 V inputs. It sits just under the band rather than
   inside it. Nothing was re-gained to reach it; the gap is the report.
4. *the creamy tilt present* — **yes, and honestly mixed**. The dirt's 3 kHz-vs-500 Hz
   harmonic ratio at max GAIN goes **−23.89 → −25.28 dB** (1.4 dB darker), and the M11
   whole-pedal HF-harmonic row goes **−22.9 → −29.4 dB** (6.5 dB darker). The pole alone is
   worth 12.7 dB of that tilt; fix 3's C7 boost gives some back, which is why the mid-knob
   figure at GAIN 0.35 actually gets *brighter* (−53.36 → −49.54 dB). Both are the
   reference netlist's own behaviour, not a compromise between them.
5. *max output back within a few dB of §50's* — **partially**: §50 measured −14.00 dBFS at
   this anchor, §52 took it to −0.74, and §54 lands at **−7.90** — 6.1 dB above §50 and
   7.2 dB below §52. Closer to §50 than to §52, but "a few dB" is generous for 6.1.

M11 rows re-baselined: A1 defaults **−15.9 → −22.6 dBFS** (peak 1.285 → 0.443 V), A2 hum
default **−39.6 → −35.6 dB** (min row −30.1 UNCHANGED — GAIN 0 is bit-exact), A3 GAIN THD
**0.0→3.6→19.6 → 0.0→4.2→13.1**, A3 treble HF **−22.9→−12.3 → −29.4→−18.9** (knob authority
still exactly +10.6 dB, for the fourth re-baseline running), A4 default-rig delta
**+19.3 → +12.5 dB**. The A4 window was **NOT** re-centred: 12.5 is comfortably inside the
9..29 §52 opened, and re-snugging a window around the newest measurement is how a drift
guard becomes a fit.

**NO golden moved.** `--golden-report` on this branch: `rat_jcm800` +0.00, `sd1_twin_reverb`
+0.00, `muff_twin` +0.00, `ts_ac30` +0.00, `clean120_chorus` −0.00 — GOLD is in no golden
rig and the GAIN-0 path is bit-exact.

### 54.7 What was re-scoped rather than changed, and what is left

`kDrivePreScale = 0.65` / `kDriveHpHz = 600` **stand**, but what they represent changed.
§52 validated them against `H_pre·H_amp/A(g)` — the whole drive path's shaping — because
the model had no amp-stage network. With `H_amp` now exact, they stand for `H_pre` alone,
and re-measured against *that*:

| f (Hz) | 82 | 110 | 220 | 500 | 1000 | 3000 |
|---|---|---|---|---|---|---|
| `H_pre` (reference, g = 0.35) | 0.1167 | 0.1317 | 0.2027 | 0.3921 | 0.6409 | 0.9279 |
| model `0.65·HP600` | 0.0880 | 0.1172 | 0.2238 | 0.4161 | 0.5574 | 0.6374 |
| Δ dB | −2.45 | −1.02 | **+0.86** | **+0.52** | **−1.22** | −3.26 |

Within ±1.3 dB across the guitar core band — the same accuracy §52 recorded, now measured
against the right target. `H_pre` is really a one-pole HP with a ~1105 Hz corner into a
0.93 shelf, where the model has a 600 Hz corner into a 0.65 shelf; refitting to
(0.93, 1105) is a candidate for a later slice and was **not** done here, because this slice
already moves three networks and a fourth constant would have had no isolated perturbation
proof. `H_pre` is also mildly knob-dependent below g = 0.15 (0.1735 vs 0.2027 at 220 Hz),
where the dirt is muted by the contract fade anyway.

Deliberately still out of scope, and named: the FF1/FF2 clean feed-forward networks (they
are what would end the flat-clean idealization — ADR 016's honest cost), the reference's
±4.5 V clip on the drive amp's output (this model documents ±8.6 V charge-pump rails
instead, and the diodes clamp long before either), `kCp = 4.7 nF` (retained as an
anti-alias guard-rail; with the new Thevenin source R13 ∥ R16 = 979 Ω its corner is
34.6 kHz, out of band), and the OUTPUT pot law.

Implementation notes for whoever touches `AmpStageNetwork` next: it is a plain bilinear
transform at `K = 2·fs` with **no** frequency warping, which is fine because every pole of
that network sits between 148 Hz and 1.10 kHz across the whole knob travel and it runs at
the *oversampled* rate. Its coefficients are rebuilt once per chunk (the same control-rate
discipline the scalar `A` had) and `setFromGain` no-ops when the leg has not moved, so a
parked knob costs one comparison. Its `y1`/`y2` are recursive states that rest at zero, so
both are `flushDenormal`-guarded per the §33 scope rule.

### 54.8 The suite: two XFAILs XPASSed and were deleted, four bars added

`clipper_gold_tests` had a two-entry XFAIL ledger, both naming **this** slice as their fix.
Both XPASSed, so per the ratchet they are gone and their properties are hard assertions:

| §52 XFAIL | before | after | now asserted as |
|---|---|---|---|
| `gold-summing-rails-engage` | tone-stage node **8.624 V** vs the 8.600 V rail | **1.274 V** (44.1 k) / **1.166 V** (96 k) | `testHeadroomAndOutput`, bar = the rail itself |
| `gold-summing-alias-at-treble-max` | 4× **−26.5 dB**, flat in the OS factor | 4× **−92.9 dB**; 1× −27.3, 8× −106.8 | `testAliasing`, TWO bars: −60 dB **and** ≥ 20 dB of 1×→4× improvement |

The second gets two bars deliberately: the defect was never "the number is too high", it
was "the number stopped depending on the oversampling factor", and only the second bar can
fail on that. `clipper_add_xfail_ledger(clipper_gold_tests)` is removed from
`core/CMakeLists.txt` — **ctest 26 → 25 entries, repo ledgers 5 → 4**.

New `testClippingStageFidelity`, one bar per fix plus the identity:

1. **diode node** — 1 V in wide open clamps at **0.1477 V** node-referred (band 0.10–0.22;
   the reference's static solve at that drive is 0.168 V, the pre-§54 pair gives 0.336).
2. **summing pole** — the dirt path's 3 kHz sits **17.64 dB** below its 500 Hz (bar −12;
   without the pole, −4.93).
3. **drive C7** — the dirt path's 500-vs-110 Hz tilt is **7.90 dB at GAIN 0.15 and
   15.97 dB at GAIN 1.00**, a spread of **8.07 dB** (bar 5). Without C7 the drive amp is a
   flat `A(g)` and every other filter in that path is knob-independent, so the four knob
   positions measure 7.27 / 7.26 / 7.24 / 7.23 dB — a spread of **−0.04 dB**, flat to the
   measurement. The cleanest perturbation in the slice.
4. **the §50 identity** — the drive network's DC gain *is* `A(g)`, because the ground leg is
   recovered from the smoothed `A` (`R10b + R11 = R12/(A−1)`) rather than written down
   twice. Measured through the model at 5 Hz: GAIN 0.35 → 1.00 raises the dirt by
   **12.480 dB**; §50's law says **12.468**.

Two probes re-derived, in the §42.9 / §50.3 discipline:

- **`testDiodeLevelContrast`** — its band moved 4.0–9.0 dB → **11.0–18.0 dB**, and this is a
  change of *reference*, not a loosened bound. The old band came from a datasheet argument
  (1N34A ~0.3 V vs 1N4148 ~0.65 V ≈ 6 dB) about a germanium this model no longer has. The
  two device models now in the file are 0.1086 V (the reference's fit to a real unit) and
  0.5839 V (the 1N4148 SPICE card `IS=2.52n N=1.752`, corroborated by its datasheet's
  ~0.62 V typical), i.e. **14.61 dB** apart analytically; measured through the whole dirt
  path, **12.70–14.29 dB**. It still catches the bug it exists for: restoring §36's
  `kSiIdeality = 1.0` measures **8.51–10.22 dB** and fails the 11.0 floor. That
  regression now has *two* independent bars, because `testGermaniumKnee` catches it
  harder still — its onset ratio collapses 139× → **3.8×** against a 20× bar.
- **`testGermaniumKnee`** — re-checked, **no change needed**. The sweep now puts the node at
  0.069–0.128 V around the 0.109 V knee, still a straddle from the toe to well past it.
  Ratios: onset 160× → **139×** (bar 20×), slope ratio 0.0071 → **0.00565** (bar 0.05).

Web: the `gold worklet` Playwright spec passes unchanged, but note its `pushedH3 >
0.1·pushedF1` bar now measures **0.1056** — 5.6 % of margin, where it had ~2×. It was left
alone (the property is real, and lowering it would be loosening a bound to go green), but it
is now the tightest guard on this pedal and the next slice to touch the dirt path should
expect to re-derive it.

---

## §55 — The AC30 dynamic supply: retiring the static sag saturator (audit finding 4)

**Slice:** `docs/work/2026-07-31-ac30-sag.md` · branch `claude/ac30-sag-6f557i` · base
`7e2a10d`. **Deliberate tone change.** Files: `core/include/clipper/dsp/Ac30PowerAmp.h`,
`core/src/dsp/Ac30PowerAmp.cpp`, `core/tests/test_ac30_amp.cpp`, `core/CMakeLists.txt`.
The two departures from the real circuit this slice makes deliberately — the constant
Thévenin source resistance and the **derived** `kVsupply` — are recorded in
**ADR 017** (`docs/decisions/017-ac30-supply-source-impedance-and-derived-rail.md`).
Read it before moving either constant toward a published figure.

### 55.1 What was actually there

Audit finding 4 says "the AC30 'sag' is a static saturator". It was worse than that
phrasing suggests. Step 6b of `processSampleOS` built a demand envelope from
`|ipUp − ipDown|` and multiplied the **OT secondary** by `1/(1 + kSagCompGain·(Idemand −
Iidle))`. Since `vSec ∝ (ipUp − ipDown)`, that is algebraically

    y = x / (1 + k·|x|)

— a memoryless soft clipper behind a 2.6 ms/40 ms envelope follower, sitting *after* the
transformer, where no supply can reach. Three measurements taken on the pre-fix binary
before it was removed:

| probe | pre-fix |
| --- | --- |
| small-signal step, 0.05 → 0.10 V at the PI grid (6.02 dB in) | **5.02 dB out** — a full dB of "sag" on a dead-clean signal |
| drive sweep 0.5 → 25 V at the PI grid (34 dB in) | 0.395 → 0.434 peak = **0.82 dB out** — a brick wall |
| THD at 82.41 Hz as a ratio to THD at 440 Hz (composed, VOL 0.70/0.80/0.85) | **1.77× / 1.62× / 1.53×** |

The third row is the mechanism behind the owner's low-end report: the envelope rides a
full-wave-rectified signal, so it ripples at 2·f₀, and for a low E (82.41 Hz → 165 Hz
ripple) that is inside its own passband. It amplitude-modulated exactly the notes an AC30
is played on.

### 55.2 What replaced it

The saturator is **retired outright**, not shrunk — with the supply modelled for real
there is nothing left for it to stand in for, and a shrunken copy would be a fitted
constant sitting on physics that already covers it. Two coupled parts, both backward-Euler
per oversampled sample, both read by the tubes on the next sample (ms constants against a
µs step — the JCM/Twin decoupling):

**(a) The supply.** A Thévenin HT source behind a **constant** series resistance charging
the reservoir, discharged by the total cathode current. Every term sourced:

| constant | value | source |
| --- | --- | --- |
| `kRrectGz34` | 75.6 Ω | Philips/Mullard GZ34 published drop, 17 V @ 225 mA |
| `kRptSecondary` | 59.0 Ω | AC30 PT HT winding measures 118 Ω end-to-end; a full-wave CT rectifier conducts through one half |
| `kRsupply` | 134.6 Ω | the sum |
| `kCreservoir` | 32 µF | common later-spec AC30 HT smoothing (τ = 4.3 ms) |
| `kVsupply` | 335.1131 V | **derived to preserve the pre-slice idle point** — see below |

The soft "rectifier knee" (`Reff = kRsupply·(1 + kRectKnee·I)`) is **gone**, and that is a
measured call: a GZ34's own drop is *sub*-linear in current (space charge, ≈ I^⅔ — 75.6 Ω
at 225 mA falling toward ~50 Ω at 250 mA) while a capacitor-input supply's shrinking
conduction angle adds effective resistance that *rises* with load. Over this amp's
190–260 mA window they very nearly cancel, so a constant Thévenin resistance is the honest
model and the growing knee was a voicing knob with a physics label on it.

**A correction this slice made against its own first draft.** The draft had 78 Ω here,
the published 7 H / 260 mA / **78 Ω DCR** AC30 choke. Two things are wrong with that: the
7 H/78 Ω unit is the "Brian May / Dave Peterson" spec (a mod, not the stock part), and
decisively, **in a stock AC30 the OT centre tap is fed from the reservoir *before* the
choke** — the OT primary's red lead lands on the same filter cap as GZ34 pin 8, and the
choke feeds the screen/preamp node downstream. Putting the choke DCR in the plate path
would have been modelling somebody's modification and calling it the amp.

**`kVsupply` is derived, and deliberately not measured off a real amp.** A published AC30
idles with its OT centre tap at 342 V; this model idles at 309.49. Moving `kVsupply` to
close that gap would be calibrating one constant to cover another's known error — audit
finding 9's EL84 screen fit runs ~3× hot (12.67 mA/tube against a real 3–5), so the whole
draw is already wrong in a way finding 9 owns, and that is precisely the failure mode that
put two factor-of-2 mistakes inside `kFullScaleSecV`. So the idle point is **preserved
exactly** and the source impedance becomes the derived value:

    kVsupply = 309.4904457 V + 0.1903616816 A × 134.6 Ω = 335.1131 V

Verified: idle rail 309.4904457 → **309.4904201 V**, Vk 9.5180841 → **9.5180832 V**,
Ip 34.92347 → **34.92347 mA/tube**, Ig2 12.66695 → **12.66695 mA/tube**. Seven figures.
Every difference this slice measures is therefore **dynamics**, not operating point.

**(b) The shared cathode**, and two component corrections that belong to it:

- `kCkCathode` **50 µF → 250 µF**. The AC30's cathode network is one 50 Ω resistor shared
  by all four EL84s bypassed by 250 µF (JMI original; 220 µF later). τ = Rk·Ck goes
  **2.5 ms → 12.5 ms**, and that is load-bearing rather than cosmetic: a 2.5 ms bias
  constant *tracks* a low-E note's own envelope (82.41 Hz = a 12.1 ms period) and
  modulates it; 12.5 ms integrates across several cycles and lets the note stand up.
- `kCoupRg` **1 MΩ → 220 kΩ**, the AC30's actual EL84 grid-leak value and the one both
  sibling power sections already carry. The stale megohm looks like it was picked to make
  the blocking τ read "22 ms" so it matched the Twin's comment (the Twin gets 22 ms from
  220 k × 0.1 µF; copying the *number* rather than the *resistor* at the AC30's 0.022 µF
  needs a megohm). Perturbation-measured both ways, peak Vk on the burst sweep:

  | grid V | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | 220 k | 9.628 | 9.660 | 9.710 | 9.765 | 9.928 | 10.222 | **10.347** |
  | 1 MΩ | 9.625 | 9.661 | 9.685 | 9.744 | 9.779 | 9.966 | **10.104** |

  The megohm suppresses the class-A bias rise by about a third above 4 V (deeper grid
  blocking pushes the grids negative and takes back the extra average draw that is
  supposed to charge Ck) while *adding* output-envelope compression (1.65–2.03 dB against
  220 k's 1.02–1.08). The stale value was buying "sag" from the wrong mechanism. The
  correct resistor is kept and the lost dB is reported, not compensated.

`kFullScaleSecV` **12.2 → 18.733**, re-derived on the identical §46 probe (VOLUME 1.0,
0.5 V at 110 Hz through the composed `Ac30Amp`, whole-render peak). Cranked secondary
measures **16.858 V at 48 kHz** (16.860 at 44.1, 16.863 at 96 — rate-independent to
0.03 %); 16.860 / 0.9 = 18.733 and the peak measures back at 0.8999 / 0.9000 / 0.9002.
*Honest note:* on this slice's base that same probe measures **8.169 V**, i.e. the shipped
12.2 was already leaving cranked at peak 0.6696 rather than the ~0.9 §46 recorded when it
chose the value. That pre-existing 2.8 V drift is not explained by this slice and was not
caused by it; it is simply swept up by the re-derivation.

### 55.3 Measured results

**Sag / bias / rail** (400 Hz burst, DRIVE 1.0, PI grid = 2× the listed input; the shared
JCM/Twin convention):

| | pre-fix | post-fix |
| --- | --- | --- |
| AC30 output-envelope depth | 6.03 dB | **1.07 dB** |
| Twin / JCM depth (unchanged) | 1.16 / 1.76 dB | 1.16 / 1.76 dB |
| AC30 rail droop | 1.6 V | **1.9 V** |
| Twin / JCM rail droop | 13.4 / 37.3 V | 13.4 / 39.1 V |
| AC30 Vk under drive (4 V grid) | 9.518 → 10.270 | 9.518 → **9.710** |
| cathode recovery τ vs Rk·Ck | 1.50 ms vs 2.50 | **5.44 ms vs 12.50** |
| small-signal step (6.02 dB in) | 5.02 dB | **5.98 dB** |
| dynamic range 0.5 → 25 V grid | 0.82 dB | **2.37 dB** |

**Low-end**, composed amp, low-E pluck (82.41 Hz + 2nd/3rd partials, 0.30 V peak, 900 ms
decay; band 82–220 Hz, attack peak vs the 300 ms settled level):

| VOL | band RMS dBFS | attack pk dBFS | settled 300 ms | compression dB | full RMS dBFS |
| --- | --- | --- | --- | --- | --- |
| 0.70 pre | −14.31 | −5.40 | −11.26 | 5.86 | −14.08 |
| 0.70 post | **−12.13** | **−3.56** | −9.21 | 5.65 | −12.03 |
| 0.80 pre | −13.66 | −3.59 | −10.37 | 6.78 | −13.23 |
| 0.80 post | **−10.88** | **−3.22** | −7.51 | 4.29 | −10.48 |
| 0.85 pre | −13.60 | −2.88 | −10.17 | 7.28 | −13.03 |
| 0.85 post | **−10.97** | **−3.17** | −7.41 | 4.24 | −10.36 |

**And the honesty correction that goes with it.** The owner's report was "the low end is
thin", and a thin low end could equally have been a steady-state *level* tilt. It was not,
and this slice does **not** fix one. Per-frequency fundamental level (one steady 0.25 V
sine at a time, composed amp, knobs noon), expressed as dB relative to the 440 Hz row:

| VOL | 82 Hz pre → post | 110 Hz pre → post | 165 Hz pre → post |
| --- | --- | --- | --- |
| 0.70 | −2.64 → **−2.97** | −1.63 → −1.80 | −0.73 → −0.78 |
| 0.80 | −3.43 → **−3.52** | −2.25 → −2.30 | −1.11 → −1.16 |
| 0.85 | −3.81 → **−3.91** | −2.52 → −2.62 | −1.27 → −1.37 |

The tilt is **0.1–0.3 dB deeper**, not shallower. What the low end gains here is dynamic,
not spectral: less low-frequency-specific distortion (THD at 82 Hz as a ratio to THD at
440 Hz, **1.77/1.62/1.53× → 1.52/1.36/1.33×**), a pick attack that survives, and **6.8 dB**
of restored dynamic range — the 0.05 → 25 V drive sweep spanned 13.1 dB of output and now
spans 19.9. If a steady-state low-end tilt is still wanted
after listening, it is the tone stack's or the OT's, and it is a different slice.

### 55.4 The protected property — the owner's words, and the honesty gate

The slice's brief protected the high-volume distortion character ("the distortion sounds
better on our one than logic pros"): each of h2..h8 to move less than ~1 dB, and any larger
movement to be **reported, not fitted**. Composed amp, 220 Hz, 0.15 V peak, knobs noon,
steady-state 400–900 ms window, dBc re the fundamental.

**At fixed input — the player's view (same guitar, same knob):**

| VOL | | f0 dBFS | h2 | h3 | h4 | h5 | h6 | h7 | h8 | THD% |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.60 | pre | −11.32 | −24.94 | −45.89 | −53.07 | −60.31 | −73.72 | −76.69 | −94.80 | 5.69 |
| 0.60 | post | −11.01 | −24.68 | −61.81 | −55.52 | −62.84 | −81.04 | −92.11 | −100.83 | 5.84 |
| 0.60 | **Δ** | +0.31 | **+0.26** | −15.92 | −2.45 | −2.53 | −7.32 | −15.42 | −6.03 | +0.15 |
| 0.85 | pre | −8.38 | −11.43 | −20.76 | −23.65 | −30.69 | −25.22 | −34.75 | −31.48 | 29.94 |
| 0.85 | post | −5.19 | −11.12 | −17.38 | −18.81 | −32.68 | −26.34 | −35.69 | −30.35 | 33.58 |
| 0.85 | **Δ** | +3.19 | **+0.31** | +3.38 | +4.84 | −1.99 | −1.12 | −0.94 | +1.13 | +3.64 |

**THE GATE FIRED**, at h3/h4 at VOLUME 0.85 (+3.4 / +4.8 dB) and at h3/h5/h7 at VOLUME
0.60 (−15.9 / −2.5 / −15.4 dB). Nothing was fitted back. What the numbers say:

- **h2 — the harmonic that *is* the AC30's character, and the one the whole §46 chime
  argument rests on — moved +0.26 dB and +0.31 dB.** Inside the 1 dB gate at both
  volumes. It is 6–8 dB above everything else at VOL 0.85 and 21 dB above at 0.60.
- The harmonics that collapsed at VOL 0.60 are **h3, h5 and h7 — the odd ladder**, and
  that is the expected signature of removing `y = x/(1+k|x|)`, which is an odd function
  and therefore generated exactly those. The model stopped making distortion it had no
  circuit reason to make.
- The rises at VOL 0.85 are the amp running **3.2 dB louder at the same knob** because the
  brick wall came off. That is the point of the slice, not a side effect.

**Level-matched control** (input bisected so f0 lands on the pre-slice level — isolates
character from loudness):

| VOL | | input V | h2 | h3 | h4 | h5 | h6 | h7 | h8 | THD% |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.60 | pre | 0.15007 | −24.93 | −45.88 | −53.06 | −60.30 | −73.71 | −76.69 | −94.79 | 5.70 |
| 0.60 | post | 0.14446 | −24.95 | −61.25 | −56.60 | −64.00 | −82.27 | −93.63 | −102.03 | 5.66 |
| 0.85 | pre | 0.07519 | −17.16 | −25.56 | −31.96 | −29.21 | −35.31 | −42.78 | −52.24 | 15.55 |
| 0.85 | post | 0.02820 | −21.50 | −52.15 | −45.04 | −50.46 | −62.11 | −72.58 | −75.87 | 8.44 |

Level-matched, **every** harmonic falls, because the amp is 8.5 dB louder and therefore
needs 8.5 dB less input to reach the same output. This table is reported for completeness;
the fixed-input table above is the one that describes what a player hears.

**Fizz at max volume** (recorded only — out of scope): VOLUME 1.0, 220 Hz at 0.30 V. The
5–16 kHz *harmonic* content moves −30.36 → **−28.18 dBc** and the non-harmonic (alias/IM)
floor −124.57 → **−119.71 dBc**. The sag did **not** incidentally tame the fizz; both
rose ~2–5 dB with the extra drive that removing the wall released. Alias floor at max
volume from `testAliasing` is unmoved in kind: 4× measures −73.5 / −70.4 / −76.6 dB at
44.1 / 48 / 96 kHz against the −60 bar.

### 55.5 Tests

`testSag` is **rewritten**, and the change is a reversal, so read the reasoning before
touching it. The old assertions were "AC30 output-envelope sag depth is in a 4–8 dB window
and is the DEEPEST of the three (Twin < JCM < AC30)". Both halves were measuring the
retired saturator. The correct physics is the other way round: the JCM is fixed-bias class
AB behind a 150 Ω/50 µF supply and drops its rail 39.1 V on this burst, the Twin drops
13.4 V, and the AC30 is class A (near-constant total draw) behind a GZ34 — the stiffest
valve rectifier — and drops 1.9 V. Five bars now, with the perturbation transcript
(restore step 6b in a scratch copy, `touch`, rebuild):

| bar | post-fix | perturbed | verdict |
| --- | --- | --- | --- |
| A small-signal linearity, 6.02 dB in → out > 5.5 dB | 5.98 | **4.96** | **RED** |
| B dynamic range 0.5 → 25 V grid > 1.5 dB | 2.37 | **0.84** | **RED** |
| C Vk rises monotonically with drive | 9.518→9.710→9.928→10.347 | monotonic | PASS (mechanism guard, not a saturator discriminator — documented as such) |
| D rail droop AC30 < Twin < JCM, in volts | 1.9 < 13.4 < 39.1 | 1.7 < 13.3 < 39.0 | PASS (a *fixed reference* — true on both circuits, so not a bound chosen to pass) |
| E envelope depth AC30 < Twin < JCM | 1.07 < 1.16 < 1.76 | **6.05 / 1.16 / 1.77** | **RED** |

Three of five go red under the perturbation; C and D are documented in the test as
mechanism/reference bars rather than discriminators. `testOperatingPoints`' analytic
shared-cathode mirror dropped the same `kRectKnee` factor the circuit did.

**One new XFAIL — `finding4-ac30-bias-swing-short`.** Retiring the saturator made the bias
swing measurable against a published real-amp number for the first time, and the model
falls short: a real AC30's shared cathode goes **10.0 V → 12.5 V (+25 %)** at full output,
ours reaches **9.518 → 10.347 V (+8.7 %)** when driven to absurdity (64 V at the PI grid).
The cathode network is already the published 50 Ω ∥ 250 µF; what is short is how much
extra **average** current the tube model draws when driven. Attribution from the idle point
this model already had: **34.92 mA plate + 12.67 mA screen** per tube against a real
~45 + ~5. The *total* is nearly right (190.4 mA vs the published 200) and the *split* is
badly wrong — finding 9's ~3× hot screen fit — so a quarter of the standing draw sits in a
screen term whose curvature does not grow with drive the way the plate term's does, and the
class-A average-current rise that charges Ck is suppressed in the same proportion. Owned by
findings 9 and 10 (the screen fits, the plate load line). **Do not chase it with a gain
term on the cathode integrator.** Repo ledgers 3 → 4; ctest 24 → 25 entries.

**The audit said this first, and it is worth reading its exact words next to the
measurement.** `docs/audits/2026-07-24-project-audit.md:182`: *"At full grid drive the EL84
screen draws 32.5 mA → 9.3 W, 4.6× its limit; real EL84 idle screen current at 35 mA plate
is 3–5 mA. `Ac30PowerAmp.h:49-50` says kg1/kg2 were 'trimmed to land the ~35 mA/tube idle'
— the trim hit the plate target by wrecking the screen. Since the screen node drives sag and
total supply draw, the AC30 sags ~3.6× too deeply."* The attribution here was arrived at
independently, from the idle currents, and lands on the same constant. **One nuance the
audit could not see, because the saturator was in the way when it was written:** the screen
error inflates the *standing* draw (which is why the total lands near the published 200 mA
despite a cold plate) while *suppressing the incremental* one, so with a real supply in
place the AC30 does not sag "3.6× too deeply" — it sags about a third as much as it should.
Same defect, opposite sign, and only visible once the waveshaper stopped supplying a
number of its own.

### 55.5b The `ts_ac30` golden — the table for the bless decision

**NOT blessed. Nothing was written.** `core/tests/goldens/` is untouched by this slice and
`clipper_player_expectations_tests` is therefore RED at exactly one assert
(`compareGolden`'s `|rmsDeltaDb| < 1.0` on `ts_ac30`) and nowhere else — core ctest is
**24 of 25**, with the four `_xfail_ledger` entries Skipped as designed. The other four
goldens are **UNCHANGED**, which is the scope check:

    GOLDEN-DELTA rat_jcm800      UNCHANGED +0.00  0.00 @  252 Hz  (12 bands)
    GOLDEN-DELTA sd1_twin_reverb UNCHANGED +0.00  0.02 @ 3200 Hz  (12 bands)
    GOLDEN-DELTA muff_twin       UNCHANGED +0.00  0.00 @ 5080 Hz  (12 bands)
    GOLDEN-DELTA ts_ac30         CHANGED   -1.86  3.12 @  200 Hz  ( 8 bands)
    GOLDEN-DELTA clean120_chorus UNCHANGED -0.00  0.11 @  252 Hz  ( 7 bands)

Full third-octave table for `ts_ac30` (Screamer → AC30 + clean212 cab), every band within
55 dB of the golden's loudest:

| band (Hz) | golden dB | new dB | Δ |
| --- | --- | --- | --- |
| 200 | 31.19 | 28.07 | **−3.12** |
| 400 | 21.81 | 18.97 | −2.85 |
| 635 | 13.73 | 11.09 | −2.64 |
| 800 | 7.09 | 4.62 | −2.47 |
| 1008 | 1.13 | −1.20 | −2.33 |
| 1270 | −4.25 | −6.46 | −2.21 |
| 1600 | −12.81 | −14.63 | −1.82 |
| 2016 | −20.18 | −21.95 | −1.76 |
| **broadband RMS** | | | **−1.86** |

Every band moves the same way and the movement shrinks monotonically with frequency — this
is the `kFullScaleSecV` re-normalization (−3.72 dB) partly given back by the amp's larger
real swing, plus the 0.1–0.3 dB of extra low tilt §55.3 already records. There is no band
that moves *up*, and no band that moves in isolation, so the rig's *voice* is intact and
its *level* dropped. Whether that is worth blessing is the owner's call.

### 55.5c M11 rows re-baselined, and a rot correction

`ac30`'s A4 level-sanity row was recorded as **+1.6 dB**. The pre-§55 code measures
**−4.09 dB** on the same probe, so that figure had rotted before this slice touched it; the
slice's own contribution is **−3.4 dB**, to **−7.46**. Crucially it lands only on the clean
end of the knob:

| VOLUME | A4 delta pre | post |
| --- | --- | --- |
| 0.30 | −11.25 | −14.87 |
| 0.40 | −4.09 | **−7.46** |
| 0.50 | +2.53 | −0.34 |
| 0.70 | +13.64 | **+13.36** |

At 0.70 the amp moves **0.28 dB**. It did not get quieter; it got a **wider knob** — the
same fact the drive sweep reports as 0.82 → 2.37 dB. The `ac30` window is re-centred
**−8..+12 → −18..+2** (the file's own convention is the measurement ± ~10 dB; the old
window left 0.5 dB of margin, which is not a guard). Noted in passing and **not fixed
here**: `twin`'s recorded −13.8 has rotted to −18.8 the same way, which is not this slice's
doing and belongs to whoever next touches the Twin.

### 55.6 Denormals, reset, real-time safety

**No new recursive state was added** — `vRail_`, `vScreen_` and `vk_` already existed and
are already in `parkState()` / `reset()`; the retired `iSagEnv_` and its two envelope
coefficients were **removed** from the class and from `parkState()`. `clipper_nan_guard_tests`
block C therefore needs nothing new, and passes.

Per ADR 006's scope rule, none of the three gets a `flushDenormal`, and the comment now
carries the measured resting values rather than an assertion: at idle and again after a
20 s silent tail at 48 kHz / 4×, `vRail_` = **309.4904 V**, `vScreen_` = **285.6766 V**,
`vk_` = **9.5181 V**. A flush at 1e-30 on a 300 V node is unreachable code in the hottest
loop in the file. The states that *do* rest at zero on this stage (TOP CUT, OT) keep their
flushes and are unchanged.

No allocation, no branch on a new envelope, and one fewer `fabs` + divide per oversampled
sample than before. Latency is unchanged: this touches nothing in the oversampling
topology, and `latencySamples()` still forwards `os_.latencySamples()`.

### 55.7 CPU — no measurable difference, and the noise floor is the story

Interleaved same-machine A/B, the §35 discipline: two `clipper-bench` binaries built from
the two source trees, alternated, **and order-balanced** (each pair run once as
AFTER-then-BEFORE and once as BEFORE-then-AFTER) because a first pass at this measured a
clean-looking 7-point "regression" that was entirely an artifact of which binary ran first.
Twelve runs, `ac30` row, % of one 48 kHz stream:

| pair | first | second |
| --- | --- | --- |
| 1A | AFTER 32.25 | BEFORE 38.31 |
| 1B | BEFORE 30.34 | AFTER 38.85 |
| 2A | AFTER 39.26 | BEFORE 36.04 |
| 2B | BEFORE 36.93 | AFTER 38.98 |
| 3A | AFTER 34.87 | BEFORE 38.94 |
| 3B | BEFORE 42.97 | AFTER 36.57 |

**AFTER mean 36.80 % (range 32.25–39.26); BEFORE mean 37.26 % (range 30.34–42.97).** The
difference is **0.46 points, with AFTER marginally cheaper** — against a within-binary
spread of **12.6 points on BEFORE alone**, i.e. the noise is ~25× the effect. The
defensible claim is **no measurable CPU change**, which is also what the code predicts: the
slice deletes a `fabs`, a compare-select, a multiply-add and a divide per oversampled
sample and adds nothing. Latency is unchanged — nothing in the oversampling topology moved,
and `latencySamples()` still forwards `os_.latencySamples()`.

Two cautions for whoever benchmarks this file next. Absolute columns are machine-dependent
and not citable elsewhere (the standing rule in CLAUDE.md). And **do not run an unbalanced
A/B here** — the first sample taken in this session, before balancing, read "BEFORE 31 % /
AFTER 38 %" and would have been written up as a regression; the second, also unbalanced,
read the opposite. Only the balanced set is meaningful.
## 56. The GOLD drive pre-filter — the reference's real H_pre, and a noise floor found on the way

**Branch:** `claude/gold-prefilter-6f557i` · **Plan:** `docs/work/2026-07-31-gold-prefilter.md`
· **Oracle:** `github.com/jatinchowdhury18/KlonCentaur`, `ChowCentaur/GainStageProcessors/PreAmpStage.{h,cpp}`

§54 closed with a named piece of unfinished business. Its `kDrivePreScale` / `kDriveHpHz`
pair — a scalar 0.65 into a one-pole 600 Hz high-pass — had been **re-scoped, not
changed**: §50 fitted it, §52 validated it against `H_pre·H_amp/A(g)` because the model had
no amp network to separate it from, and §54 re-pointed it at `H_pre` alone once
`AmpStageNetwork` carried `H_amp` exactly. §54 reported it as "within ±1.3 dB across the
guitar core band" and named the refit as a candidate. This is that refit.

### 56.1 The netlist, derived

The reference's `PreAmpWDF` is a divider: the input coupling cap in series with two shunt
legs, output taken across the shunt pair (`voltage(Vbias) + voltage(R6)` = the voltage
across `S1` = the voltage across the parallel node).

```
     in --||-- +------------------+------------------+
          C3   |                  |                  |
               |  R6 || C5        |  R7              |
               |     |            |   |              |
               |  gang-1 (g·R_pot)|  R19 || C16      |   out = V across this node
               |     |            |   |              |
              GND   GND          GND GND
```

Every value is the reference's own: `C3 = 0.1 µF`, `C5 = 68 nF`, `C16 = 1 µF`,
`R6 = 10 kΩ`, `R7 = 1.5 kΩ`, `R19 = 15 kΩ` (its `ResVs Vbias2`), and the shunt leg
`g·100 kΩ` (its `ResVs Vbias`, set by `setGain`). With

```
  Z_S1 = (R6 ‖ 1/sC5) + g·R_pot        Z_S2 = R7 + (R19 ‖ 1/sC16)
  H_pre(s) = Zp / (Zc3 + Zp),          Zp = Z_S1 ‖ Z_S2
```

and `n1 = (R6+Rg) + s·C5·R6·Rg`, `n2 = (R7+R19) + s·C16·R19·R7`, `a = C5·R6`, `b = C16·R19`:

```
  num = s·C3·n1·n2                                  (a zero at DC)
  den = n1·(1+s·b) + n2·(1+s·a) + s·C3·n1·n2
```

So **H_pre is third order and knob-dependent** — the GAIN pot is dual-ganged and its
*other* half is this divider's shunt leg. That is why no scalar-times-one-pole was ever
going to sit inside a dB of it, and it is also why the fix costs no new knob state: the
model already recovers the drive amp's ground leg from the smoothed `A` (§54's identity),
and the divider's half is that leg's complement, `g·R_pot = (R_pot + Rleg) − leg`. One
wiper, two networks, `driveLegOhms()` shared between them.

Discretized with the plain bilinear transform at `K = 2·fs` — the same map the reference's
own trapezoidal WDF capacitors use.

### 56.2 Table 1 — |H_pre| against the reference, before → after

Measured at 48 kHz × 4 (the shipped 192 kHz oversampled rate). The **oracle** is the
reference's own WDF tree, rebuilt on this repo's pinned `chowdsp_wdf` and driven sample by
sample — an independent solver, not an analytic restatement of our own netlist. The
**model** column is recovered end to end through `GoldModel` (dirt path, clean half out,
20 µV probe so the germanium pair is at its zero-bias incremental resistance) as
`stand-in × 10^((dirt_after − dirt_before)/20)`, which is exact because the pre-filter is
the only thing that changed between the two builds.

| f (Hz) | stand-in | ref g=0.15 | model | err | *was* | ref g=0.35 | model | err | *was* | ref g=0.90 | model | err | *was* |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 41 | 0.04388 | 0.09370 | 0.09370 | −0.000 | *−6.59* | 0.09802 | 0.09802 | +0.000 | *−6.98* | 0.10085 | 0.10086 | +0.001 | *−7.23* |
| 82 | 0.08715 | 0.11292 | 0.11292 | −0.000 | *−2.25* | 0.11674 | 0.11673 | −0.001 | *−2.54* | 0.11916 | 0.11915 | −0.001 | *−2.72* |
| 110 | 0.11607 | 0.12755 | 0.12756 | +0.000 | *−0.82* | 0.13167 | 0.13167 | +0.000 | *−1.10* | 0.13425 | 0.13426 | +0.000 | *−1.26* |
| 220 | 0.22158 | 0.19611 | 0.19611 | +0.000 | *+1.06* | 0.20268 | 0.20268 | +0.000 | *+0.77* | 0.20656 | 0.20657 | +0.000 | *+0.61* |
| 500 | 0.41205 | 0.37780 | 0.37779 | −0.000 | *+0.75* | 0.39207 | 0.39208 | +0.000 | *+0.43* | 0.39931 | 0.39931 | +0.000 | *+0.27* |
| 1000 | 0.55192 | 0.62191 | 0.62190 | −0.000 | *−1.04* | 0.64092 | 0.64092 | +0.000 | *−1.30* | 0.64946 | 0.64947 | +0.000 | *−1.41* |
| 3000 | 0.63114 | 0.92084 | 0.92084 | +0.000 | *−3.28* | 0.92799 | 0.92798 | −0.000 | *−3.35* | 0.93092 | 0.93092 | −0.000 | *−3.38* |
| 6000 | 0.64045 | 0.97837 | 0.97839 | +0.000 | *−3.68* | 0.98051 | 0.98052 | +0.000 | *−3.70* | 0.98136 | 0.98136 | +0.000 | *−3.71* |
| 10000 | 0.64248 | 0.99214 | 0.99213 | −0.000 | *−3.77* | 0.99293 | 0.99292 | −0.000 | *−3.78* | 0.99325 | 0.99324 | −0.000 | *−3.78* |

**Worst |error| over 41 Hz – 10 kHz × three knob points: 7.23 dB → 0.0006 dB.** Restricted
to §54's own core band (82 Hz – 3 kHz): **3.38 dB → 0.0006 dB.** The ±1.3 dB §54 quoted was
the 82 Hz – 1 kHz window at one knob position; the honest whole-band figure was 7.23.

**Phase (table 2), against the continuous-time divider: worst 0.06° across the same grid**
(41 Hz +34.68° / 220 Hz +54.90° / 6 kHz +10.38° at g = 0.35). This matters and is not
decoration — see 56.5.

### 56.3 Two convention traps in the oracle, both artifacts of how the reference *reads* its tree

Anyone re-running the comparison will hit these, and both look like disagreements about the
circuit until they are measured.

1. **Polarity.** `PreAmpStage::processSample` returns `voltage(Vbias) + voltage(R6)` from
   inside a `PolarityInverterT` subtree, so its output is the physical divider **negated**:
   measured phase difference **180.08° at 41 Hz**, magnitudes agreeing to **0.0000 dB**.
   There is no inverting element between the input cap and this node in the real pedal, so
   the model is right to be non-inverting — and it is load-bearing, because the dirt branch
   is *summed* with the clean feed, so a sign error would change the sum.
2. **One sample of delay.** The same function reads its element voltages *between*
   `Vin.incident(...)` and `I1.incident(...)`, so it reports the previous sample's
   solution. After removing the 180°, the residual phase error is **0.077° at 41 Hz, 0.41°
   at 220 Hz, 5.63° at 3 kHz, 18.75° at 10 kHz — exactly 360·f/192000 at every point**,
   i.e. one sample at the oversampled rate. This model has no such delay.

### 56.4 The thing this slice found that was not on the list: a −73 dBFS noise floor

The netlist network is **third order**, and its slowest pole (the C3/node corner, ~60 Hz)
runs at the **oversampled** rate. At 192 kHz that pole sits at radius 0.998, and a direct
form amplifies its own state rounding by roughly `(1−r)^−order`. With `float` state — which
is what the first cut of this slice had, following `AmpStageNetwork`'s existing style — the
model emits **audible broadband hiss**, and no other bar in the gold suite can see it,
because every one of them is a single Goertzel bin while the noise is broadband.

Measured against an otherwise identical **long-double**-state build, 0.15 V / 220 Hz, added
noise floor (rms over the settled second):

| rate × OS | float state | double state |
|---|---|---|
| 48 kHz ×4 (**shipped**) | **−73.4 dBFS** | **−136.2 dBFS** |
| 48 kHz ×8 | −56.3 | −129.7 |
| 96 kHz ×4 | −51.8 | −130.4 |
| 96 kHz ×8 (worst) | **−32.0** | **−112.5** |

So the shipped state type is `double`, and the same reasoning was then applied to §54's
`AmpStageNetwork`, which had carried `float` state since it landed: isolated measurement
(pre-amp double in both arms, amp float vs double) puts its own cost at **−120.1 dBFS on
the shipped path and −101.7 dBFS at 96 kHz × 8** — right on, and then past, the project's
own −120 dBFS gate. Widening it is one word and it is taken here under the slice's
drive-path latitude. It is **not** bit-identical, and the residual it removes *is* the
noise; GAIN 0 never runs either network, so the transparency contract is untouched.

**Honest residual:** −112.5 dBFS at 96 kHz × 8 is still above the −120 dBFS gate. The
structural cure is a cascade of first-order sections — every pole *and* every zero of this
network is real (`num = s·C3·n1·n2` factors, and a passive RC ladder has no complex poles),
so it factors exactly — and it is **deliberately not taken here** so this slice keeps one
isolated change per proof. Named follow-up.

### 56.4b The second thing it found: `flushDenormal` does not converge above first order

`clipper_denormal_tests` went red on `testPedalSilenceIsExactlySilent<GoldModel>` — "pedal
never settled to EXACT digital silence within 7 s of silence" — and it was **right**, on a
defect the usual one-liner hides.

The house anti-denormal idiom is `y1 = flushDenormal(y)`: guard the newest recursive tap.
For a **one-pole** that is complete, because the newest tap *is* the state. For a
direct-form recursion of order N it is not: the older taps keep whatever they held when
they were newest, and the recursion re-injects them with coefficients whose magnitudes are
around 3. So `y1` gets zeroed, the next sample computes `y = −a2·y2 − a3·y3` which is
*larger* than either, and the state **pumps itself back over the floor and stays there**.

Measured on the GOLD before the fix: the output plateaus and never reaches digital silence —
**3.373e-27 at 2 s of silence, 6.364e-27 at 4 s, 1.531e-26 at 8 s, 3.300e-27 at 16 s.** Not
decaying; sitting. Bisected two ways: bypassing the divider settles in **1.523 s** (HEAD
settles in 1.481 s), and keeping the divider but zeroing the whole state together settles in
**1.501 s**. It is the flush, not the network.

Both networks now test **all** their recursive taps and zero the state as a unit when every
one of them is under the floor. `AmpStageNetwork` was not actually failing — its poles are
decades higher (148 Hz – 1.10 kHz) so its ring-down passes through the floor fast enough to
hide the hazard — but it is written correctly rather than luckily. Still bit-transparent:
the floor is 1e-30, ~600 dB below any audio, and the GAIN-0 hashes and every field row are
unchanged by the change.

**Generalize this before writing the next high-order filter in this repo.** `Denormal.h`'s
`flushDenormal` is a scalar guard and its contract is per-value; the *policy* ("guard every
recursive state whose rest value is zero") is what the one-liner only satisfies at first
order. Any DF1/DF2 of order ≥ 2 needs the whole-state form.

### 56.5 Field rows — reported, not aimed at

0.15 V peak, 220 Hz, TREBLE 0.5, OUTPUT 0.5, 48 kHz × 4.

| GAIN | THD % before | THD % after | RMS dBFS before | RMS dBFS after |
|---|---|---|---|---|
| 0.00 | 0.000 | 0.000 | −19.499 | −19.499 |
| 0.10 | 2.860 | 2.267 | −14.054 | −14.132 |
| 0.15 | 3.620 | 2.996 | −11.969 | −12.071 |
| 0.25 | 4.058 | 3.481 | −11.667 | −11.686 |
| **0.35** (default) | **4.586** | **4.020** | **−11.329** | **−11.302** |
| 0.50 | 5.617 | 5.029 | −10.744 | −10.669 |
| 0.75 | 8.251 | 7.638 | −9.456 | −9.314 |
| 1.00 | 14.588 | 13.563 | −7.905 | −7.521 |

Breakup onset (bisected on THD at the same anchor): **5 % at GAIN 0.4163 → 0.4963**,
**10 % at 0.8682 → 0.9023**.

The owner's standing note was context, not a target: *"potentially still a bit gainy by a
touch but let's not change for now."* This refit moves in that direction — about 0.6 points
of THD off the default and a whole 0.08 of knob travel added before 5 % breakup — **by
accident of the component values, and nothing was tuned to produce it**. The mechanism is
in table 1: at the 220 Hz probe the real divider is 0.6–0.8 dB *quieter* than the stand-in
was, so slightly less signal reaches the diodes. Above 1 kHz it is 1.3–3.8 dB *louder*,
which is the same change reading the other way — the drive path is now treble-weighted the
way a Klon's is, rather than shelved flat from 600 Hz up.

**GAIN 0 is BIT-EXACT.** Six render hashes (sine and noise × TREBLE 0/0.5/1) identical
before and after: `a0c5464fd80df762` / `55796eb6d0d9100c` / `bc7f8af7af3b75e2` /
`0dfffa814cb19735` / `0a168e72781032a1` / `b4b142672d039bcf`. **All five goldens UNCHANGED**
(`--golden-report`: rat_jcm800 +0.00, sd1_twin_reverb +0.00 / worst band 0.02, muff_twin
+0.00, ts_ac30 +0.00, clean120_chorus −0.00) — GOLD is in no golden rig, and the scope
check holds.

### 56.6 The web spec's `pushedH3` bar — §54's prediction came true, and the re-derivation

§54 closed by warning that `pushedH3 > 0.1·pushedF1` measured **0.1056** — 5.6 % of margin —
and that "the next slice to touch the dirt path should expect to re-derive it". It did:
post-refit the same case measures **0.09774**, and the bar is crossed.

The interesting part is *why*, because it is not "less distortion". Reproducing the spec's
exact case in the core (0.5 s at 48 kHz, GAIN 0.9, 0.3 V at 220 Hz, OUTPUT 0.5, the spec's
own unwindowed Goertzel from 0.12 s):

* `f1` **0.738968 → 0.771566 (+0.375 dB)** — the fundamental got **louder**
* `h3` **0.077826 → 0.075409 (−0.274 dB)** — the third harmonic barely moved

`pushedF1` is the **vector sum** of the dirt branch and the parallel clean core. Decomposing
it (clean 0.299684 ∠+154.81°, dirt 0.525684 ∠−150.01°) reproduces the measured total to
**−0.0000 dB**, so the decomposition is exact. The refit rotates the dirt branch by
**−15.48°** and scales it by −0.6094 dB, and the rotation alone makes the *denominator*
bigger. The bar was measuring a phase relationship, not a distortion level.

The new constant is that predicted movement and nothing more, computed from the netlist plus
**pre-refit measurements only** — no post-refit number enters the derivation, so the check
against the post-refit measurement is a prediction rather than a fit:

| step | value |
|---|---|
| `H_pre(220 Hz, g=0.90)`, reference vs stand-in | 0.20656 ∠+54.39° vs 0.22158 ∠+69.86° |
| correction `r` | ×0.93225 (−0.6094 dB), ∠−15.477° |
| clipper's own compression over that drive change (measured pre-refit) | f1 ×0.98332, h3 ×0.97614 |
| predicted post-refit `f1` | 0.771613 (**measured 0.771566, +0.0005 dB**) |
| predicted post-refit `h3` | 0.076203 (**measured 0.075409, +0.091 dB**) |
| predicted `h3/f1` | 0.09876 (**measured 0.09774, +0.09 dB**) |
| predicted bar move | 0.1 × 0.93773 = **0.09377** |

Shipped bar: **0.0938**, rounded *up* from the derivation, i.e. strictly harder than it. The
measurement clears it by **4.2 %** against the old 5.3 %. The contrast clause
(`pushedH3 > cleanH3 × 20`) is untouched and got no easier: GAIN 0 is bit-exact, so
`cleanH3` is unmoved and it measures **167×** against a bar of 20×.

### 56.7 The M11 ragged-block window — re-derived, and made harder

`clipper_player_expectations_tests` block B went red on `gold_ ABI` at 100-frame blocks:
settled 1.22e-02 against a 2e-03 bar. The stream is **not** corrupted — the difference
decays to **exactly 0** by 400 ms:

```
   50 ms 1.3e-2 | 110 ms 1.4e-3 | 200 ms 1.3e-5 | 300 ms 2.0e-7 | 400 ms EXACTLY 0
```

`kBlockSettleSecs` was 0.05 s, justified as "past every parameter smoother's ~5–8 ms
settle". That derivation is **measurably incomplete**: what has to settle is not the
smoother, it is the slowest recursive *state* the differing parameter trajectory excites on
its way through, and §56's divider carries a 1 µF cap across 15 kΩ — a ~30 ms corner, five
times anything the old stand-in had. Re-derived to **0.25 s**, which is three orders of
magnitude inside the bar for the slowest unit in the file.

Widening a window can only make a test easier, so the same change adds the bar that
actually separates "smoothed differently" from "stream corrupted", and which the old test
never had: **the difference must CONVERGE.** Over the final quarter of every render, every
unit measures **exactly 0.0** — all four amp voices, the phaser, the convolver, the reverb
and four of the five dirt pedals — with one exception, the **Muff at 3.34e-05**, which is
physics already on the record: §53's 1 µF series-diode caps discharge only through diode
leakage (τ ≈ 5 s), so a trajectory difference cannot wash out inside a 1 s render. Bar set
at **1e-4**: 20× tighter than the settled bar, 3× above the one non-zero unit, and
unreachable by a stream that never converges at all. Post-change `gold_ ABI` measures
settled **1.47e-06**, tail **0.00e+00**.

### 56.8 Tests, and the perturbations that prove them

`testClippingStageFidelity` bars (1)–(3) are §54's and are unchanged in kind; bar (2)'s
printed reference moved from −17.6 dB to **−13.92 dB** because the divider is +3.7 dB
brighter over that same pair — the 495 Hz pole did not move, the signal reaching it did.

* **(4) the gang identity, restated and harder.** §54 asserted "the drive amp's DC gain IS
  A(g)" on the argument that every other filter in the dirt path is knob-independent and
  cancels. §56 breaks that argument on purpose. The bar is now the *product* of both gang
  halves' published laws, each written from component values in the test
  (`refPreAmpMag()`), against a 0.5 dB tolerance. Measured **13.756** vs a prediction of
  **13.764**; §50's drive law alone predicts 12.468.
  Perturbations: revert the divider to the stand-in → **12.480** (fails by 1.284 dB);
  recover the divider's half as `R10b` instead of its complement, i.e. the two gang halves
  wired backwards with each network still individually well-formed → **7.212** (fails by
  6.552 dB).
* **(5) the divider itself**, two one-sided absolute bars because the stand-in was wrong in
  *opposite* directions at the two ends of the band, each placed at the midpoint of the
  two measured values:
  * bottom, dirt path 41 Hz vs 500 Hz: **−9.60 dB** now, **−17.01** with the stand-in, bar
    **−13.3** (the divider's own contribution is −12.04 dB)
  * top, dirt path 6 kHz vs 1 kHz: **−21.50 dB** now, **−23.90** with the stand-in, bar
    **−22.7** (the divider's own contribution is +3.69 dB)
* **(6) `testDrivePathNumericalFloor`**, new, at 44.1 and 48 kHz. Drives a pure tone,
  projects out every harmonic up to Nyquist, and asserts the residual is below the
  project's own **−120 dBFS** gate. Measured **−135.4 to −142.4 dBFS**; narrowing either
  network's state back to `float` measures **−72.4 to −80.1** and fails by 40 dB. GAIN 1.00
  is deliberately not probed: at max drive the model's own 4× alias floor (−93 dBFS, §54)
  would dominate, and the bar would be measuring aliasing instead of arithmetic.

### 56.9 What did NOT move

`kClipBlendWeight = 4.1702` (§52), the 495 Hz summing pole and the reference germanium fit
(§54), `kSumGain = 2.0`, the tone stage, the clean path, the output network, `kRailVolts`,
every other pedal and every amp. `kDrivePreScale` and `kDriveHpHz` are **deleted** — there
is nothing left for them to stand in for.

## 57. M10.3 — the Orange OR120 "Overdrive": the MID-FORWARD head (the fifth amp voice)

The early-70s Orange OR120 joins the lineup as amp voice **4** (`orange`), together with a
synthesised Orange-style 4×12 cab (`orange412`). It is EL34 push-pull like the JCM800 and
**reuses that amp's power machinery wholesale** — the Koren EL34 fit, the per-tube
plate-load Newton, the grid-coupling/blocking solve, the OT bandwidth pair and the
rail/screen sag integrator (§18). No new device model was fitted for this voice.

Everything that makes it an Orange is therefore a **circuit** difference, and the slice's
own acceptance bar was that those differences must be **measurable against the JCM800**,
not asserted. §57.4 is that bar.

> **AMENDED 2026-07-31 — THE SCHEMATICS ARRIVED.** The voice shipped (PR #41) as an
> explicit **documented reconstruction**: no OR120 schematic was reachable from the build
> container, so the topology came from forum prose and nearly every component value was
> invented under physical constraints. §57.1 said in terms: *"Do not re-tune any of them
> toward a sound; find the schematic."* The owner supplied it, and this section is now
> written against the circuit. **Ten defects were corrected**, one of them structural: the
> model ran `V1A → VOLUME → V1B → F.A.C. → James stack`, and the real amp is
> `V1A → James stack → GAIN → V1B → F.A.C. → power amp`. Everything below is the
> corrected circuit; the reconstruction's figures are kept only where they are needed to
> state what MOVED, and every movement is reported rather than compensated.

### 57.1 Provenance — the transcribed parts list, and the three things NOT on the sheet

**Read this before changing any constant in `OrangePreamp.h` / `OrangePowerAmp.h`.**

**Sources, owner-supplied 2026-07-31.** The images are not in the repository; the
transcription is reproduced in full in `docs/work/2026-07-31-orange-schematic-correction.md`
and that transcription is what the code is written against.

1. **`OR120 GRAPHIC MkII — Early 1970's`** — B. Hickmott's 2004 redraw of the 1972 factory
   schematics, in three sheets (Preamp / Output-amp / Power supply). **PRIMARY**: a
   complete, unambiguous netlist.
2. **`ORANGE GRAPHIC MkII`**, the post-'74 factory sheet with a full parts list
   (R1–R38, C1–C26). **CROSS-CHECK**, and the source of the later-era values.
3. **Orange Amp Field Guide, OR120 HEAD** — confirms *"Phase Inverter: Cathodyne type:
   1/2 x 12ax7"*, fixed bias, solid-state rectifier, 4× EL34, 1.5× 12AX7 preamp, and the
   panel layout **Input – F.A.C. – Bass – Treble – H.F.Boost – Gain – Reverb Send – Reverb
   Return**.

**ERA: the model is the EARLY 1970s amp (source 1).** It is the one with a complete drawn
netlist, it is the "picture graphics" amp this voice always targeted, and it carries the
**1n5 treble cap** — the one value the reconstruction had actually sourced, and the reason
owners say the early treble knob "brings high mids up with it". Source 2 is the post-'74
amp (330 pF treble, C13/C14) and is used only as a cross-check. **The two eras also differ
in the driver→inverter coupling** (see §57.3): the early amp AC-couples through 68 n, and
the post-'74 parts list has exactly four 0.068 caps all accounted for elsewhere, which
implies the later amp is DC-coupled. The first release was an early-70s amp wearing
post-'74 coupling.

#### The transcribed parts list

**Preamp sheet**

| Element | Value | Note |
| --- | --- | --- |
| input jacks → grid stopper | **68 k** each | 1 M grid leak on V1A |
| V1A / V1B plate load `Ra` | **220 k** to D+ | *was 100 k* |
| V1A / V1B cathode | **2k2 ∥ 50 µF** | fully bypassed; *was 820 Ω ∥ 25 µF* |
| V1A plate → stack | **68 n** | |
| James `R1` (input series → BASS top) | **100 k** | |
| BASS pot | **1 M LOG** | |
| BASS pot upper-section cap | **2n2** | a true Baxandall bass: a cap across EACH half |
| BASS pot lower-section cap | **22 n** | |
| BASS pot bottom → ground | **22 k** | *was 100 k* |
| BASS wiper → TREBLE pot (`R2`) | **100 k** | |
| TREBLE series cap `C2` | **1n5** | the EARLY value (post-'74: 330 pF) |
| TREBLE pot | **1 M LOG** | *was 250 k* |
| TREBLE pot bottom → ground `C3` | **10 n** | *was 470 pF* |
| GAIN pot | **1 M LOG** | the panel's GAIN; this amp's only volume control |
| GAIN wiper → V1B grid | **330 p** | *did not exist in the reconstruction* |
| V1B grid leak | **1 M** | |
| V1B plate → F.A.C. | **68 n** | |
| **F.A.C. ladder** | **[through] · 4n7 · 4n7 · 2n2 · 1n · 330p** | *was 47n · 22n · 10n · 4n7 · 1n5 · 330p* |

**Output-amp sheet**

| Element | Value | Note |
| --- | --- | --- |
| effects loop | 100 k send feed + 100 k return | the F.A.C. drives this into the driver grid |
| driver plate load | **100 k** to C+, **1 n across it** | *was 300 k, no cap* |
| driver cathode | **1k5 + 220 k** | plus the BOOST network and the NFB injection |
| **H.F. Boost** | **1 k LIN pot + 2 mH CHOKE + 0.47 µF to ground, with 100 k** | *was a one-pole shelf in the feedback path* |
| global NFB | OT **16 Ω tap** → **15 k** → driver cathode | *was 27 k, and from the 8 Ω tap* |
| driver → cathodyne | **68 n + 1 M grid leak** — **AC-COUPLED** | *was DC-coupled* |
| cathodyne | **Ra = Rk = 100 k** to C+ | equal split loads; *was 180 k* |
| cathodyne legs → EL34 grids | **68 n → 2k4** | *coupling was 22 n* |
| EL34 grid leaks | **220 k + 220 k** to N.B | two per side ⇒ **110 k** at the modelled node |
| EL34 screen resistors | **1 k** (pin 4), R35–R38 1 K 5 W | *was a shared 470 Ω with a 47 µF bypass* |

**Power-supply sheet**

| Element | Value |
| --- | --- |
| rectifier | **8× 1N4005 bridge** (solid state), 1 A HT fuse |
| A+ (OT centre tap) | 2× 100 µF **in series** = **50 µF**, 100 k balancers |
| A+ → **choke** → B+ (EL34 screens) | 2× 32 µF in series = **16 µF** |
| B+ → **33 K 2 W** → C+ (driver + cathodyne) | 16 µF |
| C+ → **33 K 2 W** → D+ (V1A + V1B) | 16 µF |
| negative bias | 1N4005 half-wave, 2× 22 µF 63 V, 22 K, 1 k → N.B |

Note the supply **ordering**: `A+` (the OT centre tap) is taken from the reservoir
**BEFORE** the choke, and the screens come **after** it — the same structural question §55
had to settle for the AC30, here stated explicitly on the sheet.

**Post-'74 cross-check (source 2):** `VC1/VC2/VC3 = 1M log` (BASS/TREBLE/VOLUME),
`VC4 = 1k lin` (BOOST), `VR1 100 K` preset (SET BIAS), `VR2 100 R` (SET MIN HUM),
`SW2` = 2-pole 6-way F.A.C., `D1–9 = 1N4005`, `L1` = HT choke, `R35–R38 = 1 K 5 W`
(screens), `R24–R27 = 2K2` (EL34 grid stoppers), `C25/C26 = 100 µF 450 V`,
`C13/C14 = 330 pF` (the later treble cap). The 80 W version is identical but with V3 & V6,
R24, R27, R35, R38 omitted.

#### The three things the sheets do NOT carry, named rather than hidden

1. **Where the cathodyne's 1 M grid leak returns to.** The sheet records the resistor and
   not its return node. It cannot be ground: measured, a 100 k / 100 k cathodyne with its
   grid at 0 V DC idles at **Vk = 5.47 V, Ip = 54.7 µA** and could never swing the 48 V the
   transcribed EL34 fixed bias needs — the amp would make no power at all. The only
   arrangement that keeps the transcribed **equal** split loads is the textbook one, a tap
   in the cathode leg, and the tap is **DERIVED, not fitted to a tone**: it is placed so the
   stage idles at the CENTRE of its own compliance (`Vkc = C+/4`, so `Vak = C+/2`), which is
   the cathodyne's own published design rule, and the result is then checked against an
   ABSOLUTE number from the same schematic — the swing must clear the EL34s' 48 V. It
   measures **104.23 V**, and the derived tap is **1432 Ω of the 100 k leg**.
2. **The 2 mH choke's winding resistance.** A real iron-cored 2 mH part measures single-digit
   ohms; the model uses **8 Ω**, which sets the H.F. Boost branch's Q (measured **8.15** at
   full boost). Nothing else depends on it.
3. **The HT choke's DCR and inductance.** Because they are unknown, the model does not split
   `A+` from `B+`: the choke is treated as ideal at DC, which makes the two reservoirs one
   **66 µF** node and leaves the transcribed per-tube **1 k** as the only screen impedance.
   The consequence is real and is reported — the screens now track the rail
   instantaneously, where the reconstruction had a 47 µF screen filter that does not exist
   on the sheet, and that costs about **4 W** of ceiling (§57.9).

**Still a reconstruction after this pass, because the sheets do not carry them:** the OT's
`Raa` (1.7 k) and its 45 Hz / 14 kHz corners, the HT winding's Thévenin source
(`kVsupply` 510 V behind `kRsupply` 70 Ω), the −48 V bias value (the sheet gives 63 V caps,
so it is under that), the EL34 **2k4 grid stoppers** (not modelled as a separate element),
and the GAIN/BASS/TREBLE pots' taper LAW — the sheets give the letter (LOG) and not the
curve, so all three keep the house audio law with k = 4 (§51's `kMasterTaperK`).

### 57.2 The preamp — `OrangePreamp` (the STRUCTURAL correction)

```
guitar in -> 68k grid stopper (1M leak)
  -> V1A  1/2 ECC83, Ra 220k to D+, Rk 2k2 || 50uF (FULLY bypassed)
  -> 68n  -> JAMES / passive-Baxandall stack (BASS + TREBLE, no MID)
  -> GAIN 1M log            (the panel's GAIN — this amp's only volume control)
  -> 330p -> V1B grid (1M leak)
  -> V1B  1/2 ECC83, Ra 220k to D+, Rk 2k2 || 50uF
  -> 68n  -> F.A.C. six-way series coupling cap -> the OUTPUT AMP
D+ = 368.24 V (solved through the transcribed 33K droppers).
```

**Defect #1, and it is the important one.** The first release ran
`V1A → VOLUME → V1B → F.A.C. → James stack`, i.e. it treated the James network as a
**terminal EQ**. It is not. It sits between V1A and the GAIN pot, and the difference is a
*response* difference, not merely a frequency-response one:

* The stack is driven by **V1A's 220 k plate** (source impedance measured **45 534 Ω** —
  a genuinely high-Z source; there is no cathode follower anywhere in this amp) and its
  insertion loss is made up **by V1B**. So turning GAIN up drives V1B harder *with an
  already-EQ'd signal*: BASS and TREBLE change what gets distorted, not what comes out of
  the distortion.
* The **F.A.C. is the last thing in the preamp** and feeds the **power amp**, so it trims
  the drive into the phase inverter as well as the bandwidth.
* The **GAIN pot is the stack's LOAD**, so its setting changes the network's own response.
  It is inside the same MNA for exactly that reason.
* The **330 p between the GAIN wiper and V1B's grid** is a real, deliberate high-pass that
  did not exist in the reconstruction at all. With the wiper's own source impedance and the
  1 M grid leak it corners at **≈ 436 Hz at noon**, and it is the single largest audible
  consequence of this whole pass (§57.9).

The James network, the GAIN pot and the 330 p/1 M grid network are **one MNA** (9 nodes,
five trapezoidal capacitor companions, the same numerical shape as `MarshallToneStack`,
§14) so they cannot desynchronize and the coupling corner MOVES with the knob:

```
Rs*  : source - F                 (* V1A's PLATE impedance, 45.5 k)
R1   : F   - A          100k      C2   : F - T          1n5   (the EARLY treble cap)
RB   : A -((1-b)RB)- W -((b)RB)- B   1M LOG    RT : T -((1-t)RT)- OUT -((t)RT)- U  1M LOG
Cbu  : A   - W          2n2       C3   : U - GND        10n
Cbl  : W   - B          22n       RG   : OUT -((1-g)RG)- G -((g)RG)- GND  1M LOG (GAIN)
R3   : B   - GND        22k       Cg   : G - GRID       330p
R2   : W   - OUT        100k      Rgl  : GRID - GND     1M
```

Two PARALLEL branches summing through `R2` at the treble wiper is still the whole
mechanism, and it is still why the mids survive — but note the corrected shapes: BOTH
halves of the bass pot carry a cap (2n2 upper, 22 n lower — a true Baxandall bass), the
bass leg sits on 22 k rather than 100 k, and the treble pot is 1 M rather than 250 k.

The F.A.C. is its own small network **after V1B**, because that is where it is:

```
source (V1B plate, 45.5 k) - P - Cfac - Q - 200k (100k send feed + 100k return) - GG - 1M - GND
Cfac: [straight through] / 4n7 / 4n7 / 2n2 / 1n / 330p
```

Measured **discretization** check against each netlist's own complex nodal solve, five knob
combinations × 82 Hz…6 kHz: worst |error| **0.481 dB at 44.1 kHz, 0.403 dB at 48 kHz** for
the James+GAIN+330 p network and **0.002 dB** for the F.A.C. one. The worst cell is always
TREBLE at minimum at 6 kHz — bilinear frequency warping, `tan(πf/fs)/(πf/fs) = 1.0672` at
44.1 kHz, i.e. the discrete pole sits 6.7 % high. Reported movement: the reconstruction's
network measured 0.291 dB worst against a 0.35 bound; the transcribed one measures 0.481
because its treble pot is 1 M rather than 250 k and the extreme is sharper, so the bound is
the warp figure with margin (0.55). Note the standing §29 limitation — an analytic
reference derived from the same netlist validates the discretization and *cannot* catch a
wrong topology. What validates the topology here is §57.4.

Measured **preamp DC**, and the whole transcribed dropper chain (both rates identical):

| node | value |
| --- | --- |
| EL34 rail (A+, the OT centre tap) | **499.34 V** |
| screens, behind the per-tube 1 k | **495.83 V** |
| C+ = B+ − 33 K × (driver + cathodyne) | **416.93 V** |
| D+ = C+ − 33 K × (V1A + V1B) | **368.24 V** |

| stage | Va (plate–cathode) | Vk | Ip (solver) | Ip (Ohm's law) | plate as % of D+ |
| --- | --- | --- | --- | --- | --- |
| V1A | 205.17 V | 1.631 V | 0.7412 mA | 0.7338 mA | 56.2 % |
| V1B | 205.17 V | 1.631 V | 0.7412 mA | 0.7338 mA | 56.2 % |

Both stages land inside the project's documented 0.5–0.9 mA window on the transcribed
220 k / 2k2 load line. The reconstruction's invented 100 k / 820 Ω also landed inside it —
at 1.34 mA, i.e. 80 % hot — which is a good illustration of why a window is not a
derivation. **Both rails are asserted by Ohm's law on the transcribed 33 K**, independent
of the fixed point that solved them.

One numerical note worth keeping: the D+ chain is a fixed point whose loop gain measures
about **−7**, so the obvious `ip += k·(f − ip)` iteration oscillates for any k above ~0.25.
Both the stage current and the rail are solved by **bisection** on a monotone residual,
which is unconditionally stable and costs nothing at `prepare()` time.

**Deliberate absences, each with a measurable consequence:** no bright cap across the GAIN
pot (the 2204's 470 pF one tilts drive into its second stage by +6–8 dB at mid travel,
§47); no cold-biased second stage (the 2204's 10 k unbypassed V1B); no mid control; no
master.

### 57.3 The power section — `OrangePowerAmp`

```
driver V2A  (Ra 100k to C+ with 1n across it, Rk 1k5 + 220k)
   <- global NFB from the OT's 16 OHM TAP through Rfb 15k, into the CATHODE
   <- H.F. BOOST: 1k lin pot + 2 mH CHOKE + 0.47uF to ground, also at the CATHODE
   | AC-COUPLED: 68n into a 1M grid leak
cathodyne V2B (Ra = Rk = 100k to C+)  -> plate = C+ - Vk, cathode = Vk
   | 68n -> 2k4
4x EL34, fixed bias -48 V, 110k grid leak per side, 1k screen resistor per tube,
   Raa 1.7k, solid-state bridge supply
   -> OT (45 Hz / 14 kHz) -> the 16 ohm tap -> back to the driver's cathode
```

**Defect #7 — the pair is AC-coupled, so the joint Newton was SPLIT.** The reconstruction
made the driver's plate node the cathodyne's grid node and solved a single 3×3 Newton in
(Vpd, Vkd, Vkc). The early-70s sheet puts a 68 n cap and a 1 M grid leak between them, so
this build solves a **2×2** Newton for the driver and a **1-D** Newton for the cathodyne,
with the coupling network folded into the driver's plate node as one conductance plus a
history current. That is not a refactor: it changes what the driver's plate is loaded by,
and it un-pins the cathodyne's DC from the driver's.

**What did NOT change, and must not.** The split loads are still EQUAL, so both legs are
read off ONE current through TWO equal resistors:

| property | Orange cathodyne | JCM800 LTP |
| --- | --- | --- |
| leg balance (min/max leg gain) | **0.999965** — *topological* | 0.988, and it took audit finding 8 + a resistor sweep to get there (§45) |
| leg phase | exactly anti-phase | anti-phase |
| driver gain / split-load gain | **−43.735 / +0.9733** | n/a (the LTP amplifies) |
| plate + cathode node sum | **416.93 V ≡ C+**, exactly | n/a |
| compliance clip | Vk pins at **0.0000 / 208.202 V** (idle 104.233) → **−104.23 / +103.97 V** | tail-steering cutoff |

**A first-release claim this correction REFUTES.** §57.3 used to say the compliance clip
was "asymmetric by construction" (−132.7 / +67.3 V), because the DC-coupled driver's plate
pinned the cathodyne's grid off-centre. The transcribed AC-coupled stage is biased at the
**centre** of its own compliance, so the two limits are within **0.25 %** of each other.
The test now asserts what is true — symmetric to better than 1 %, both excursions clearing
the EL34s' 48 V, and the conduction rail pinned at exactly 0 — which is a stronger check
than the old one, not a weaker one.

**A measurement trap the split created, worth knowing before you probe this stage.** With
the pair DC-coupled, a DC-step probe was a legitimate way to read the leg gains. It is not
any more: a step decays through the 68 n / 1 M coupling (τ ≈ 68 ms), and identical code
measured a split-load gain of **0.95 at a 3 ms settle and 0.76 at a 23 ms settle**. The
leg-gain probe is now a steady 440 Hz tone read with signed in-phase amplitudes, which has
no such ambiguity and still shows the anti-phase.

Measured **power-section DC**:

| node | value |
| --- | --- |
| driver plate / cathode / current | 271.44 V / 1.945 V / **1.4549 mA** |
| cathodyne cathode / plate / current | 104.23 V / 312.70 V / **1.0423 mA** |
| cathodyne grid (the derived bias tap) / Vgk | 102.740 V / **−1.493 V** |
| the derived tap itself | **1432 Ω** of the 100 k cathode leg |
| EL34 rail / screen | 499.34 V / 495.83 V |
| EL34 Ip / Ig2 per tube | **34.57 mA** / 3.51 mA |
| EL34 plate dissipation | **17.26 W = 69 % of the 25 W rating** |

**Reported movement in the DC windows.** The reconstruction's invented 300 k / 180 k loads
from 320 / 400 V put both triodes in the project's 0.5–0.9 mA gain-stage window. The
transcribed 100 k / 100 k loads from a solved 416.93 V put the driver at **1.45 mA** and the
cathodyne at **1.04 mA**. Neither is a defect — a 12AX7 with a 100 k plate load and a 1k5
cathode is the classic British gain stage, and a cathodyne is not a gain stage at all, so
the window never applied to it. `Ipc == C+/(4·Rsplit)` is now asserted as the **identity**
it is, and the driver's current is cross-checked by Ohm's law on its own plate load.

**Defect #8 — the H.F. Boost is an INDUCTOR.** Transcribed: a 1 k LIN pot + a **2 mH
choke** + 0.47 µF to ground, at the driver's cathode. That is a series R-L-C shunting the
cathode resistor, resonant at `1/(2π√(LC))` = **5191.1 Hz** with Q **8.15** at full boost
(the pot is a rheostat: 1 k of damping at minimum, 0 at maximum). Below resonance the
branch is capacitive and already bypassing, so what the control actually does is move the
cathode bypass corner *and* put a peak on top of it. Measured on the composed amp:

| H.F. BOOST | 220 Hz | 1 kHz | 3 kHz | 5 kHz | 8 kHz | tilt (5 k − 220) |
| --- | --- | --- | --- | --- | --- | --- |
| 0.00 | −21.15 | −10.47 | −9.54 | −9.82 | −11.40 | **+11.33** |
| 0.25 | −21.16 | −9.53 | −8.50 | −8.90 | −10.71 | +12.26 |
| 0.50 | −21.27 | −8.25 | −6.97 | −7.59 | −9.81 | +13.68 |
| 0.75 | −21.51 | −6.45 | −4.58 | −5.68 | −8.59 | +15.83 |
| 1.00 | −21.91 | −4.33 | −4.00 | −4.30 | −7.03 | **+17.62** |

**+6.32 dB** of lift across the control, with 220 Hz moving **0.77 dB** — it moves the
*tilt*, not the level. The reconstruction's one-pole-in-the-feedback-path version measured
+6.80 dB, so the headline number barely moved; the mechanism, the shape and the place in
the circuit all did.

**Denormal scope for the new branch, decided by measurement (ADR 006), and it is the §59
case rather than the §56.4b one.** The 0.47 µF rests at the **driver's cathode**
(**1.9453 V** after a 4 s silent tail), not at zero, because the branch carries no DC
current — a state that looks zero-resting and is not. The branch current and the choke
voltage do rest at zero mathematically, but they are computed as `(Vkd − Eh)`, a
cancellation of two ~2 V quantities, so they **floor at 1.386e-16** and can never reach the
subnormal range at all. 292 decades of head-room means a flush there is unreachable code in
the hottest loop in the file: comment, not guard, with the measured floor asserted in-test.

**Global feedback** returns to the driver's cathode, so the loop encloses the driver **and**
the inverter (the 2204's loop starts at the PI's second grid and encloses only the PI).
Transcribed: from the **16 Ω tap** through **15 k**, against the reconstruction's 27 k from
the 8 Ω tap — so the injected divider is `Rk_eff/(Rk_eff+Rfb) = 0.0891` times a further
`√2` for the tap, against 0.0526. Measured loop depth **7.07 dB** (open-loop 0.00905 →
closed-loop 0.00401 at 440 Hz) against the reconstruction's 7.81: the divider got 2.4×
deeper and the forward gain dropped by about the same order, because the driver's plate
load went from an invented 300 k to the transcribed 100 k.

**The supply.** The rectifier is a solid-state bridge, so `kRsupply` is **70 Ω** behind the
transcribed reservoir — 2× 100 µF in series at A+ plus the 16 µF at B+, one node because
the choke is ideal at DC (§57.1), so **66 µF** against the reconstruction's invented
100 µF. The screens sit behind the transcribed **per-tube 1 k with no bypass cap**, where
the reconstruction had a shared 470 Ω into a 47 µF filter that does not exist on the sheet.
That is a real dynamic change: the screens now follow the signal instantaneously.

**`kFullScaleSecV = 43.086` is re-derived on the corrected circuit** (the §23 convention:
every voice is normalized to its own cranked peak). Cranked — VOLUME 1.0, 0.50 V peak in,
220 Hz — the secondary reaches **38.777 V peak = 93.98 W into 8 Ω**, and 38.777 / 0.90 =
43.086 puts the cranked peak at a measured **0.9000** (0.9018 at 44.1 kHz, 0.8980 at
96 kHz — a 0.42 % spread). The NFB tap reads the real secondary volts, never this, so the
loop gain is independent of it.

**AND THE AMP NO LONGER MAKES ITS RATED 120 W — reported, not tuned away.** The power
section's own ceiling, driven directly at the driver's grid, measures **92.71 W with
feedback and 93.56 W without**, so the §42 "smallest value that reaches rated power"
criterion is **unsatisfiable** here and was not used (see §57.9 for what replaced it).
Attribution, measured rather than guessed: it is dominated by the transcribed EL34 grid
network — **68 n into 110 k**, against the reconstruction's 22 n into 220 k — which couples
the inverter's legs into the grids about 3× more stiffly and therefore drives them harder
into conduction, so §18's blocking mechanism shifts the bias further toward cutoff at the
peaks. The transcribed per-tube 1 k screen resistors with no bypass cap are worth about
**4 W** on their own (at 1 Ω the ceiling measures 97.7 W). Open item; **do not close it by
re-inventing a screen filter cap or by softening the grid coupling.**

### 57.4 THE BAR — measurably not a re-skinned JCM800

The metric is scale-free so the two stacks' different insertion losses cannot flatter
either: **the minimum response across 300–800 Hz relative to the mean of the 100 Hz and
4 kHz responses**, at noon. Negative = a mid SCOOP, positive = a mid BUMP.

**THE BAR SURVIVED THE CORRECTION.** Both contrast bounds are **unchanged** (6.0 dB on the
networks, 4.0 dB on the composed amps) and both are still met. One one-sided margin moved
and is reported below rather than bargained down.

**(a) the tone networks, from their own netlists** — the James probed at its own output
node (the treble wiper, loaded by the GAIN pot), driven from V1A's measured 45.5 k plate
impedance with the GAIN wiper at its measured noon value (0.1192):

| | Orange James | Marshall FMV |
| --- | --- | --- |
| mid-notch metric @ noon | **+0.75 dB** (a BUMP) — *was +2.32* | **−6.03 dB** (a SCOOP) — unchanged |
| **contrast** | | **6.78 dB** — *was 8.35*, bar unchanged at **6.0** |

dB relative to each network's own 1 kHz:

| f | James (transcribed) | James (reconstruction) | FMV |
| --- | --- | --- | --- |
| 82 Hz | −2.99 | −3.00 | +9.55 |
| 220 Hz | −1.30 | −1.05 | +6.79 |
| 440 Hz | −0.28 | +0.11 | +3.61 |
| 660 Hz | **−0.07** | **+0.27** | +1.47 |
| 1 kHz | 0.00 | 0.00 | 0.00 |
| 2.2 kHz | +0.02 | −1.53 | +2.44 |
| 5 kHz | +0.01 | −3.08 | +4.28 |

The FMV peaks at **both ends** and dips in the middle; the James is flat-to-slightly-domed
through the middle and rolls off only at the bottom. The transcribed network is a *milder*
bump than the invented one — its bass leg sits on 22 k rather than 100 k and its treble
branch is flatter — so the one-sided bound the first release shipped (`orangeNotch > +1.0`,
measured +2.32) **would now fail at +0.75**. §57.4 said in terms that those one-sided
margins were "recorded rather than snugged" and that "a future component-value correction
inside either stack is allowed to move them", so the SIGN stays a hard assertion
(`orangeNotch > 0`, `marshallNotch < −3`) and the teeth move to the CONTRAST bar — which is
**unchanged at 6.0 and now has 0.78 dB of margin instead of 2.35**, i.e. it is a harder
test than it was, not an easier one.

**(b) the composed amps, rendered** — both at tone knobs noon, a clean level, same input,
same metric (deliberately *not* "dB re 1 kHz": the FMV's own notch minimum sits at ~1 kHz,
so normalizing there would hide the very thing being measured):

| f | Orange (dB re its 660 Hz) | JCM (dB re its 660 Hz) |
| --- | --- | --- |
| 110 Hz | **−20.26** | −2.91 |
| 220 Hz | −11.20 | −2.47 |
| 330 Hz | −6.14 | −1.66 |
| 440 Hz | −3.13 | −0.90 |
| 660 Hz | 0.00 | 0.00 |
| 1 kHz | +1.87 | +1.15 |
| 2.2 kHz | +3.06 | +6.82 |
| 4.4 kHz | +2.64 | +9.78 |

| | Orange | JCM |
| --- | --- | --- |
| composed mid-notch | **+2.67 dB** (*was +1.15*) | **−5.09 dB** (unchanged) |
| **contrast** | | **7.76 dB** (*was 6.24*), bar unchanged at **4.0** |

**The composed half of the bar got BETTER, and the reason is defect #1 plus the 330 p.**
The transcribed preamp takes the bottom out of the Orange exactly where the FMV is boosting
it, so the two amps diverge harder. That −20.26 dB at 110 Hz is the headline audible
consequence of this pass and it is discussed as such in §57.9.

### 57.5 No master volume — breakup tracks the VOLUME knob (the §46 convention)

Composed amp, 0.15 V peak / 220 Hz (the §51 unity-trim probe), tone knobs noon, F.A.C. 0.2:

| VOLUME | THD | RMS | (reconstruction THD) |
| --- | --- | --- | --- |
| 0.10 | 1.56 % | −28.21 dBFS | 0.48 % |
| 0.20 | 1.86 % | −20.20 | 1.36 % |
| 0.30 | 2.95 % | −14.47 | 2.96 % |
| 0.40 | 3.92 % | −9.63 | 4.11 % |
| **0.50** | **8.60 %** | −6.51 | **7.86 %** |
| 0.60 | 16.73 % | −5.73 | 17.08 % |
| 0.70 | 27.26 % | −5.22 | 23.15 % |
| 0.85 | 38.81 % | −4.96 | 35.11 % |
| 1.00 | 41.59 % | −4.89 | 47.29 % |

≥5 % THD onset at **VOLUME 0.50**, unchanged; clean end 1.62 % (*was 0.76*), cranked end
41.59 % (*was 47.29*). Both THD and RMS stay monotonic in the knob. The RMS column
flattening above 0.5 while THD keeps climbing is the power section compressing — the point
of an amp with no master.

**What is different is WHY 0.50 is 0.50.** In the first release that number was a
consequence of a chosen `kInterstageScale`; here `kInterstageScale` is **1.0** — an
un-fitting, because the preamp now emits the driver's grid voltage in volts and there is
nothing left for a trim to represent (§57.9). The onset lands at 0.50 on its own.

### 57.6 The F.A.C. — a real high-pass that walks

Composed amp at VOLUME 0.3, low E (82 Hz) and 1 kHz, at a clean 0.02 V probe so neither
band is level-limited:

| position | cap | low E | 1 kHz | tilt |
| --- | --- | --- | --- | --- |
| 1 | **straight through** | −41.05 dB | −16.72 dB | +24.33 dB |
| 2 | 4.7 nF | −41.50 | −16.72 | +24.78 |
| 3 | 4.7 nF | −41.50 | −16.72 | +24.78 |
| 4 | 2.2 nF | −42.81 | −16.74 | +26.08 |
| 5 | 1.0 nF | −46.40 | −16.79 | +29.61 |
| 6 | 330 pF | −54.72 | −17.35 | +37.37 |

Every click to the right takes low end away and never adds any, the 1 kHz-to-low-E tilt only
grows, and the whole switch spans **13.67 dB** of low E (**13.69 dB** at the suite's hotter
0.1 V probe). Both the monotonicity and the span are asserted.

**Two transcription facts this table makes visible, and neither was tidied away.**

1. **The fat end is a straight-through click and a 4n7, not a 47 n.** §57.1 used to quote a
   forum's *"330 p to .047"* for the range; the real ladder tops out at **4n7** — a 10×
   error at that end — plus a position with no cap at all. The audible consequence is that
   the top three clicks are nearly identical (0.45 dB apart at 82 Hz), where the invented
   47n/22n/10n ladder spread them evenly.
2. **Positions 2 and 3 carry the SAME 4n7** on both factory sheets (C19 and C20). SW2 is a
   2-pole 6-way switch, so its second pole plausibly does something the single-line
   transcription does not carry. The ladder ships **literally**, and the test asserts
   `kCaps[1] == kCaps[2]` so a future session cannot quietly "fix" it into a smooth ramp
   without deciding to.

The span also shrank against the reconstruction's **17.21 dB**, for the same reason: the two
extra octaves of capacitance at the fat end were invented.

### 57.7 Antialiasing, DC and the rest

* **Alias floor**, the house composed probe (4186 Hz at 0.3 V into a fully cranked amp — the
  same stimulus `test_jcm800_power.cpp` uses, so the two numbers are comparable):

  | factor | 48 kHz | 44.1 kHz | (reconstruction, 48 / 44.1) |
  | --- | --- | --- | --- |
  | 1× | −17.7 dB | −15.2 dB | −10.2 / −11.3 |
  | 2× | −20.6 | −20.4 | −11.1 / −11.9 |
  | **4× (shipped)** | **−73.0** | **−50.8** | −67.1 / −59.2 |
  | 8× | −73.1 | −61.8 | −68.2 / −59.4 |

  **48 kHz improved by 5.9 dB; 44.1 kHz got 8.4 dB WORSE and now fails the −56 dB bar the
  first release shipped.** The bar was NOT loosened — it is a hard assertion at 48 kHz and
  an **XFAIL** at 44.1 kHz (`orange-schematic-alias-44k1`), so `clipper_orange_tests`
  registers a ledger for the first time (repo ledgers 4 → 5, ctest entries 28 → 29).
  Attribution was measured, not guessed: the 44.1 kHz 4× floor reads **−52.0 / −50.8 /
  −50.7 dB at H.F. BOOST 0 / 0.5 / 1.0**, so the new resonant cathode network is *not* the
  cause; and it improves 11 dB going to 8×, so it is genuine foldover rather than the
  rail-clipping signature §54 describes. The named fix is one shared oversampling domain
  around the whole preamp+power cascade (§57.13), never a lower bar. The "4× must beat 1×
  by ≥ 12 dB" clause stays hard at **both** rates (35.6 dB of margin at 44.1 kHz).

  > **AMENDMENT 2026-08-01 (§63.14): THAT NAMED FIX WAS BUILT, RUN ON THIS AMP, AND
  > REFUTED — the XFAIL stays and its owner is corrected.** §63.8 registered the same
  > bar failing on the Rockerverb and this section's own text concluded that "two amps
  > failing the same bar at the same rate … is the architecture speaking". The
  > Rockerverb field-report slice implemented exactly the candidate above — ONE
  > `Oversampler` around the whole cascade, both halves clocked at 192 kHz with their
  > own resamplers at 1× — on **both** amps and measured them:
  >
  > | composed cranked 4× | 44.1 kHz | 48 kHz |
  > | --- | --- | --- |
  > | **Rockerverb** before → after | −52.7 → **−72.3** | −80.1 → **−84.9** |
  > | **OR120** before → after | −50.8 → **−48.7** | −73.0 → **−67.8** |
  >
  > **It fixes the Rockerverb by 19.6 dB and makes the OR120 WORSE at both rates**, so
  > the OR120 half was **reverted** (`OrangeAmp.cpp` is byte-identical to its
  > pre-slice state; only this banner and the XFAIL's `fix` string changed). The
  > mechanism, measured rather than assumed: removing the intermediate band-limitings
  > lets each stage's own products reach the NEXT nonlinearity unfiltered. That is a
  > large win when the later stages are triodes (the Rockerverb's four) and a loss
  > when the dominant nonlinearity is a hard rail — and this amp's cathodyne clips on
  > **compliance** (Vk pinned to [0, C+/2], §57.3), the hardest clipper in the lineup.
  > **The XFAIL is re-owned to the CATHODYNE, not to the domain layout.** Still never
  > a lower bar.
* **DC offset ON SIGNAL** (§29 / `support/DcOffset.h`), VOLUME 0.7, 220 Hz: **0.172 % of
  peak** with a clean input and **0.172 %** with +0.1 V of DC on the input (*was 0.126 %
  both*) — the coupling caps and the OT's own LF corner hold, and the +0.1 V case is the one
  that makes the assertion able to fail.
* **reset() + ragged blocking**: a whole-buffer render vs the same render after `reset()` in
  128-frame blocks differs by **0.000e+00**; every sample finite.
* **Denormals** (§33, ADR 006). The James+GAIN+330 p network now has **ten** zero-resting
  companions and the F.A.C. network two; after being driven then silenced for 4 s,
  `maxAbsRestingState()` measures **exactly 0.0** on both. The power section's H.F. Boost
  branch is handled the other way and by measurement — see §57.3.

### 57.8 The Orange 4×12 cab

Unchanged by this pass (the schematics are the head's; the cab is synthesised in the §15
modal house style, exactly as `brit412` was — **no captured third-party IR is downloaded or
committed**). Voicing: low cut 62 Hz (vs the Brit's 72), box modes moved down, **no 200 Hz
chunk**, and a broad **+4 dB peak at 1.2 kHz** — the Orange bark, pointed the same way as
the amp's stack rather than being a second, independent EQ opinion.

Both IRs are peak-normalized to unity (M6.6), so **absolute** dB is the fair comparison:

| f | Orange | Brit |
| --- | --- | --- |
| 60 Hz | −9.88 | −7.69 |
| 100 Hz | −6.19 | −3.87 |
| 200 Hz | −3.32 | −0.27 |
| 500 Hz | −3.06 | −1.62 |
| 1 kHz | −0.29 | −1.68 |
| 1.2 kHz | −0.03 | −1.56 |
| 3 kHz | −5.49 | −2.54 |
| 8 kHz | −45.54 | −57.43 |

| property | Orange | Brit |
| --- | --- | --- |
| −6 dB low corner (re its own 300 Hz) | **63.4 Hz** | 72.6 Hz |
| 1.2 kHz minus 200 Hz | **+3.29 dB** | −1.29 dB |
| spectral peak (M6.6) | 1.000000 | — |

**An honesty correction the measurement forced (kept from the first release).** The first
version of this section claimed "more 60 Hz than the Brit" and the test asserted it re each
cab's own 1 kHz — which the Orange **fails** (−9.88 vs −7.69 absolute at 60 Hz), because the
bark IS the normalization peak. The claim was replaced by the one that is actually true and
is a property of the box rather than of the voicing: its **−6 dB low corner reaches 9.2 Hz
lower**. The bark difference (**+4.58 dB** more 1.2 kHz relative to 200 Hz) is the other
load-bearing bar.

### 57.9 What the schematics refuted — including two of the first release's own findings

**1. `kInterstageScale` is 1.0, and it is an UN-FITTING, not a re-derivation.** The
corrected `OrangePreamp` ends at the F.A.C. network, which already terminates at the
driver's own grid node (68 n → Cfac → 100 k send + 100 k return → 1 M grid leak). Its output
therefore *is* the driver's grid voltage, in volts, and there is nothing left for a trim to
represent. It was still swept, because "the constant is unity" is only defensible if unity
is also where the amp behaves:

| scale | ≥5 % THD onset | cranked W into 8 Ω (0.15 / 0.30 / 0.50 V in) |
| --- | --- | --- |
| 0.02 | — | 1.8 / 7.9 / 21.2 |
| 0.06 | 1.00 | 18.0 / 61.8 / 70.8 |
| 0.12 | 1.00 | 58.2 / 77.0 / 84.1 |
| 0.20 | 0.90 | 72.8 / 86.8 / 87.7 |
| 0.40 | 0.70 | 88.3 / 89.2 / 92.5 |
| 0.70 | 0.60 | 90.8 / 90.2 / 93.3 |
| **1.00** | **0.50** | **90.8 / 90.6 / 94.0** |
| 1.50 | 0.40 | 90.1 / 91.3 / 94.5 |
| 2.00 | 0.35 | 90.1 / 91.7 / 94.8 |

**2. "`kInterstageScale` does NOT set the breakup onset" is REFUTED.** §57.9 used to record
that the onset sat at VOLUME 0.59 across a 5× sweep because the *preamp* clipped first. On
the corrected circuit the preamp loses ~35 dB in the James stack + GAIN pot + 330 p before
V1B, so it clips far later and the **power section** is the first thing to break up: the
onset now moves monotonically with the scale, **1.00 → 0.35** across the table. At unity it
lands at VOLUME 0.50 — the §46 no-master window is met **by the circuit** rather than by
choosing a constant.

**3. The amp does NOT reach its rated 120 W, at any scale.** The power section's own
ceiling measures **92.71 W with feedback / 93.56 W without**, so the §42 criterion
("smallest value at which a cranked amp reaches rated power") is **unsatisfiable** and was
not used. The shortfall is reported rather than tuned away, with its cause measured — see
§57.3. It is an open item.

**4. The cathodyne's clip is NOT "asymmetric by construction".** Measured, the transcribed
AC-coupled stage clips within **0.25 %** of symmetry, where the DC-coupled reconstruction
measured −132.7 / +67.3 V. §57.3 carries the corrected claim and the corrected assertion.

**5. The F.A.C.'s range is not "330 p to .047".** See §57.6.

**6. The biggest AUDIBLE consequence is a bass cut nobody predicted.** The transcribed
330 p between the GAIN wiper and V1B's grid corners at ≈436 Hz into the 1 M grid leak, and
on the composed amp the low E sits **−20.26 dB relative to 660 Hz** where the JCM sits at
−2.91. Nothing was done about it, because it is what the circuit does and because the BASS
control has ~12 dB of authority at 82 Hz to answer it (measured, at the network's output:
+4.19 dB at BASS max against −7.98 at BASS min, re noon at 1 kHz). It is also half of why
the composed mid-forward contrast improved. **If a field report says the corrected amp is
thin, the first thing to check is whether the 330 p is really in series with the grid on the
sheet — not to re-tune it.**

### 57.10 Test suite — `clipper_orange_tests`

Eleven blocks: DC operating points (Ohm's-law cross-checks on every plate load **and on
both transcribed 33 K droppers**), the cathodyne (anti-phase, balance, driver-vs-split-load
gain separation, compliance, and that the pair really is AC-coupled), the James+GAIN+330 p
network and the F.A.C. network each against their own `H(jω)`, **the mid-forward bar**,
**knob authority**, breakup-tracks-VOLUME + monotonicity, the F.A.C. ladder, NFB depth +
the H.F. Boost resonance and tilt, aliasing, DC on signal, the cab, and reset/ragged
blocking + the three denormal rest cases.

The schematic correction gives this target its **first XFAIL ledger**
(`orange-schematic-alias-44k1`, §57.7), so core ctest goes **28 → 29 entries** and the repo
carries **5** ledgers.

**Perturbation proofs** (patch one constant or one line of topology in a scratch copy,
`touch`, rebuild, confirm RED, restore FROM THE SCRATCH COPY, `touch`, rebuild, confirm
GREEN — never `git checkout --` and never `git stash`, both of which have destroyed work in
this repository):

| # | perturbation | result |
| --- | --- | --- |
| P1 | bass leg `kR3` 22 k → 100 k (the reconstruction's value) | RED — breakup onset window |
| P2 | BASS pot `kRB` 1 M → 10 k | RED — **`bassTravel > 8.0`**, the new knob-authority bar |
| P3 | **the signal ORDER reverted** to `V1A → V1B → F.A.C. → stack` | RED — THD monotonicity in VOLUME |
| P4 | the GAIN-wiper coupling `kCg` 330 p → 330 n | RED — the James network's denormal rest |
| P5 | cathodyne split loads made unequal (plate node 0.8·Vk) | RED — `ratio > 0.9999` |
| P6 | **the AC coupling removed** (`vgc_ = vpd`, i.e. the DC-coupled reconstruction) | RED — split-load gain window |
| P7 | H.F. Boost choke `Lboost` 2 mH → ~0 | RED — the boost branch's resting-state floor |
| P8 | global NFB disconnected (`Rfb` → 1 GΩ) | RED — THD monotonicity |
| P9 | F.A.C. ladder flattened (all six 4n7) | RED — `kCaps[0] == 0.0`, the straight-through click |
| P10 | the C+ dropper `kRdropCplus` 33 k → 0 | RED — `cPlus < rail − 49.5` |
| P11 | screen resistor `kRscreen` 1 k → 250 Ω | RED — the 2–6 V screen-drop window |
| **P12** | **`kC2` 1n5 → 330 pF, i.e. the POST-'74 treble cap** | **RED — `orangeNotch > 0.0`: the James measures −2.31 dB (a SCOOP) and the contrast falls 6.78 → 3.72 dB** |

**P12 is the one that proves the bar itself has teeth, and it is also a result.** Swapping
only the era-defining treble cap for the post-'74 value takes the tone network from a mid
BUMP to a mid SCOOP and drops the contrast against the FMV below the shipped 6.0 bound. The
early 1n5 cap is not a detail: on this metric it is *what makes an early OR120 mid-forward*.
It also means a post-'74 voice, if one is ever added (§57.13), cannot inherit this bar.

**Two bars this run had to ADD, because the perturbation transcript found them missing.**
`kRB` 1 M → 10 k and `kRdropCplus` → 0 and `kRscreen` → 250 all left the suite GREEN on the
first pass: the supply checks were Ohm's-law **identities** (the code computes the rail that
way, so the assertion could not fail) and nothing measured whether the tone pots did
anything. They were replaced by absolute windows derived from the transcribed resistors and
the physically-bounded currents, plus a knob-authority block (**BASS +11.96 dB at 82 Hz,
TREBLE +39.81 dB at 5 kHz**, bars 8 and 20). All three then go red.

### 57.11 Wiring — both fronts

**The schematic correction changes NO wiring.** Amp voice **4** (`kAmpOrange`), cab built-in
**2** (`kCabOrange412`) and the F.A.C.'s own param id **13** (`kAmpParamOrangeFac`) are all
unchanged, as are the web `AmpParams` shape, the worklet's opaque index pass-through, the
native `Params::orangeFac` and `CabChoice`, and the assistant's `'orange'` / `'orange412'` /
`'fac'` vocabulary. The whole correction is inside `core/`, so the front ends inherit it
through the rebuilt WASM artifact and the recompiled native engine.

For the record, the original wiring, unchanged:

* **C ABI**: voice 4, cab 2, and ONE param id — `kAmpParamOrangeFac = 13`, because no other
  voice has a six-position switch and reusing a knob slot would make a stale rig state
  silently mean something else. Everything else is the house reuse pattern: VOLUME (0),
  BASS (1), TREBLE (3), REVERB (9), and PRESENCE (11) → the **H.F. Boost** (the same slot
  the AC30 takes as TOP CUT). The 'middle' slot never reaches this voice.
* **Web**: `params.ts`, `rig.ts` (`AmpType`, `CabChoice`, the `fac` param + its 0.2 default
  + migration), `audio.ts`, `Amp.tsx` (`OrangeFace` — VOLUME · BASS · TREBLE · F.A.C. · HF ·
  REVERB, and **no** master/mid/bright, which is as load-bearing as what it has),
  `Board.tsx`, `App.tsx`, the `--accent-orange` token and its `amp.css` block.
* **Native**: `ClipperEngine`, `PluginProcessor` (`pid::orangeFac`), `PluginEditor`.
* **Assistant**: `set_amp` `'orange'`, `set_cab` `'orange412'`, `set_param` `'fac'`, and the
  OR120 section of the stable `SYSTEM_PROMPT` block.

**One deliberate divergence, and it is a session-safety decision.** The native `CabChoice`
enum already had `CAB_CUSTOM = 2`, and those values are stored in the APVTS `cabModel`
choice parameter and in saved sessions. Inserting the Orange cab at 2 to match the C ABI
would silently turn every saved session that says "Custom IR" into "Orange 4×12". So native
appends **`CAB_ORANGE412 = 3`** and the engine maps the two spaces **in code**
(`loadCurrentCabIntoPair`), never by assuming the integers agree. The popup-menu ids are a
third space again (the Orange is menu id 5).

**A naming note the Field Guide settles, and the UI has now caught up.** The panel reads
**Input – F.A.C. – Bass – Treble – H.F.Boost – Gain – Reverb Send – Reverb Return**: the
volume control is called **GAIN** and the presence control is called **H.F. BOOST**. The web
face said "Vol" and "HF"; that is a label change with no DSP content, so it was left out of
the correction slice deliberately and **landed separately (2026-08-01)** across the web face,
the native editor panel and the assistant's own vocabulary.

The same follow-up corrected a **factual claim the assistant was still making**: its prompt
described H.F. BOOST as "a top-end lift inside the feedback loop", which is the
reconstruction's mechanism, not this one's. Defect #8 replaced that with a 1 k pot + 2 mH
choke + 0.47 µF **series-resonant network at the driver's cathode** (5191.1 Hz, Q 8.15), so
the coaching now says a focused resonant peak rather than a broad shelf. **The PARAM IDS did
not move** — GAIN is still the shared `volume` slot 0 and H.F. BOOST still the shared
`presence` slot 11 — so the rig JSON, the testids, host automation and the C ABI are
untouched by the renaming; only printed text changed. `OrangeAmp::PARAM_HF_DRIVE` keeps its
identifier name deliberately: renaming a core enum would be a `core/` change and would force
a WASM rebuild for a cosmetic reason.

### 57.12 Scope check

**All five goldens UNCHANGED** (`rat_jcm800`, `sd1_twin_reverb`, `muff_twin`, `ts_ac30`,
`clean120_chorus`: ±0.00 dB), so **nothing was blessed and nothing needed to be** — the
Orange is in no golden rig, and a change confined to one voice cannot move another rig's
render. That is the scope check for a slice that rewrote most of two DSP files.

Core ctest **29/29** (5 XFAIL ledgers reported Skipped, one of them new). Native
`clipper_identical_core` / `clipper_chain_edit` / `clipper_cab_state` all green — the first
of those is the proof that the plugin's default state still renders bit-identically to a
hand-built core chain. Web: `tsc --noEmit` + `vite build` clean, Playwright **76 passed**.
Node suites 15 / 10 / 12 and electron 20. WASM artifact rebuilt (**77** hashed inputs) and
the staleness gate re-verified.

### 57.13 Named follow-ups

* **The ~93 W ceiling** against the rated 120 W (§57.3/§57.9). Attribution is measured; the
  next step is the EL34 grid network and the screen model, **not** a re-invented filter cap.
* **The 44.1 kHz alias floor**, XFAIL `orange-schematic-alias-44k1` (§57.7). ~~The
  candidate is one shared oversampling domain around the whole preamp+power cascade.~~
  **That candidate was built, run and REFUTED on 2026-08-01 (§57.7's amendment, §63.14):
  it takes this amp to −48.7 dB at 44.1 kHz, i.e. worse, while fixing the Rockerverb's
  twin defect outright.** Re-owned to the **cathodyne's compliance clip** (§57.3) — the
  hardest nonlinearity in the lineup, and the only one in this amp that does not soften
  as the domain widens. A shared domain is still worth doing here for the *latency*
  (216 → 72 samples) if and only if the alias cost is answered first.
* **The panel names**: GAIN and H.F. BOOST, per the Field Guide (§57.11).
* **A post-'74 voice.** The cross-check sheet is a complete second amp — 330 pF treble,
  DC-coupled cathodyne, 2K2 grid stoppers — and P12 shows it measures *materially*
  different. It would need its own bar, not this one.
* **Cathodyne grid conduction** is still not modelled (compliance clipping is). A real split
  load does conduct at slam; the EL34 grids carry the blocking mechanism as on the 2204.
* **OT core saturation** stays linear — the same documented deferral the other three amps
  carry — and `Raa`, the OT corners, the HT winding's Thévenin source and the −48 V bias are
  the values the sheets do **not** carry (§57.1).
* **A native `orange412` snapshot scene** for the headless screenshot suite.

## 58. The first FILTER pedal — a GCB-95-style wah with a derived sweep law, and the same tank driven by an envelope

The lineup's six pedals were all *dirt* (RAT / SD-1 / TS / Muff / GOLD) plus one
modulation box (the phaser, §22). This is the first **filter**: pedal type `wah`,
a Dunlop GCB-95-class Cry Baby whose POSITION is an ordinary automatable
parameter, plus a SENSITIVITY control that hands the same resonator to an
envelope follower — one resonant primitive covering both Cry Baby and
Mu-Tron-style envelope-filter territory. Owner-chosen option.

Trademark-safe per the §17 doctrine: wordmark **"Weeper"**, model line
`FILTER Nº7 · TREADLE`. No Dunlop/Cry Baby/Vox/Mu-Tron wording on any user
surface.

### 58.1 Research — what was sourced, and what could not be

**Proxy note, up front and honestly.** This session's egress policy returned
**403 for every one of the primary references**: `electrosmash.com` *and* its
archive mirror, `geofex.com`, `dafx.de`, `ccrma.stanford.edu`,
`guitarscience.net`, `web.archive.org`, `en.wikipedia.org`, `grokipedia.com`,
`cushychicken.github.io`, `delicious-audio.com` and every blogspot mirror of the
schematic. What *did* work was (a) web-search result summaries, which quote
those pages' text directly, and (b) `github.com` clones. So the numbers below are
sourced from search-returned quotations of the primary pages plus one primary
artefact fetched in full (the Faust library). **The full GCB-95 netlist was NOT
obtainable**; every place that matters is flagged below.

#### The tank

| Quantity | Value | Source |
| --- | --- | --- |
| `L1` inductor | 200 mH…1 H usable, **500 mH typical**, DCR 10–200 Ω (**15 Ω typ**) | ElectroSmash GCB-95 analysis (via search quotation) |
| `C` tank cap | **0.01 µF** | ElectroSmash GCB-95 analysis |
| `VR1` wah pot | **100 kΩ**, Dunlop "Hot Potz" | ElectroSmash; Amplified Parts "Potentiometer — Dunlop, Hot Potz II Crybaby, 100 kΩ" |
| Hot Potz taper | **logarithmic / audio**; an A-taper reads ~10 % of full value at 50 % rotation; the Vox-era part is the custom-audio "ICAR" taper | Reverb (Clarostat Hot Potz 1 listing); pot-taper references via search |
| `R7` across/into the tank | **33 kΩ**, "adjusts the sharpness of the resonant peak. Reducing its value, the Q factor is reduced, and the filter bell is spread" — the **"Vocal Mod"** raises it to 39 k / 68 k / 100 k | ElectroSmash GCB-95 analysis |
| Transistors | **MPSA18** (high-hFE small-signal NPN); ElectroSmash's designator set also lists an MPSA13 | ElectroSmash GCB-95 analysis |
| DC bias network around the gain stage | collector→base **470 kΩ**, base→ground **82 kΩ**, emitter **390 Ω** | ElectroSmash GCB-95 analysis (designator sets differ between mirrors — see the caveat below) |

**Caveat on designators.** Two different ElectroSmash designator sets came back
through search (one giving `R1 68K / R2 1.5K / R3 33K / R4 470 / R5 82K`, another
`R1 68K / R2 1.5K / R3 22K / R4 390 / R5 470K`), and the community itself warns
that "existing online schematics don't always correspond to the actual board with
100 % accuracy". The *values* recur across both sets; only the letters move. This
model therefore uses the values and does not cite a designator it could not
confirm.

#### The measured behaviour (the anchors this model is built on)

- **"The frequency response is characterized by a resonant peak centered in
  750 Hz (with the variable resistor VR1 at mid position), and the peak sweeps up
  and down from 450 Hz to 1.6 kHz."** — ElectroSmash.
- **"At toe down, the band is centered at 1.6 kHz, and at heel down, the band is
  centered at 450 Hz. These frequencies are boosted at somewhere around 18 dB
  while everything above and below is rolled off in a bell curve."** —
  Catalinbread, "Vox Cry Baby".
- Dunlop's own published spec for the Dimebag Cry Baby From Hell: **"filter
  center frequency 440 Hz at heel down to 1.5 kHz–2.2 kHz at toe down; max gain
  at fc 15 dB"**.
- **The mechanism, named verbatim:** "the resonant frequency of an LC filter made
  up of a fixed inductor L1 and a fixed capacitor C2 can be changed using a
  variable resistor VR1 … connecting a complementary reactance (inductor L1) will
  produce a resonant circuit which is **adjusted by tuning the apparent
  capacitance of C2**." — ElectroSmash. This sentence is the whole model.
- Geofex (Mark Hammer / R.G. Keen), "The Technology of Wah Pedals": "the
  inductor is connected to the base through a 33K resistor"; "all by itself, the
  inductor/capacitor series filter is very sharp, highly resonant, and by
  adjusting the series resistance we can tame this resonance down and broaden it".

#### The independent measurement used as the reference

`grame-cncm/faustlibraries` → `vaeffects.lib` → `crybaby`, the **CCRMA / Julius
Smith digitised CryBaby**, fitted to *three measured GCB-95 frequency responses*
(reference: `ccrma.stanford.edu/~jos/pasp/vegf.html`). Cloned in full from
GitHub, so this is a primary artefact rather than a quotation:

```faust
crybaby(wah) = *(gs) : fi.tf2(1,-1,0,a1s,a2s)
with {
  Q  = pow(2.0,(2.0*(1.0-wah)+1.0)); // Resonance "quality factor"
  fr = 450.0*pow(2.0,2.3*wah);       // Resonance tuning
  g  = 0.1*pow(4.0,wah);             // gain (optional)
  ...
```

So the measured reference says: **fr 450 Hz → 2216 Hz (2.30 octaves)**, and
**Q 8 → 4 → 2 from heel to toe** — the resonance is *broader at the toe*, not
sharper. That is the opposite of the "series-RLC damping" intuition, and it
decided the topology (§58.2).

#### The inductor question (Fasel / halo), and why this model has one number

The only measured comparison that came back is a PedalPCB forum thread ("Wah
Inductors. No hype. Just measurements."):

| Part | Measured |
| --- | --- |
| Dunlop **Red Fasel** (toroid) | 17.5 Ω / **565.2 mH** |
| Dunlop **Yellow Fasel** (cup core) | 14.7 Ω / *24.26 mH* — **almost certainly a transcription error** for ~542.6 mH; a 24 mH wah inductor would put the tank at 14.5 kHz. Recorded, not used. |
| **Whipple Halo** | 28.8 Ω / **580.7 mH** |
| **Sabbadius Soul Halo** | 30.3 Ω / **597.8 mH** |

So the real spread among "famous" inductors is **565 → 598 mH (0.5 dB of centre
frequency, 0.10 octaves)** and **17.5 → 30.3 Ω of DCR**. The audible difference
players report is therefore *not* mostly the inductance: it is the **core**
(toroid vs cup) saturating — "the Fasel inductor showed onset of
saturation-generated harmonics sooner than a Crybaby inductor, with a second
harmonic appearing with the third, and the fourth rising with the fifth,
demonstrating **asymmetric** clipping". **This model does not model core
saturation** — the tank is linear and the only nonlinearity is the transistor.
That is a named, deliberate omission (§58.7).

DCR is likewise not the thing that sets Q here: 17.5 Ω against `ω0·L ≈ 2.4 kΩ`
at 750 Hz is a Q of ~135 on its own, three orders above the measured 4. The
damping is circuit loading, not the inductor (§58.3).

#### The envelope follower (AUTO)

Geofex, "The Technology of Auto-Wahs / Envelope-Controlled Filters" (Mark
Hammer, 1999–2000), via search quotation: **"Most commercial
envelope-controlled products provide an envelope signal that responds with
maximum swing over a period of 50 msec or less, and drifts back to baseline over
a period of 500 msec or less."** and a typical precision-rectifier detector
"yielding an **attack time of about 10 ms and decay of around 500 ms**".
Mu-Tron III context: optocoupler-controlled, attack ≈ 30–40 ms on the slow
setting. Those are the numbers §58.5 is built on.

#### What could not be sourced (open gaps, recorded so the next slice does not re-fit them)

1. **The full netlist.** Not obtained. Consequently the *divider* that turns
   wiper position into apparent-capacitance multiplication is not derived from
   component values — its span is pinned to the published 450 Hz heel and its
   shape to one taper exponent (§58.2), and that exponent's honesty check is that
   it must land inside the documented audio-taper spec. It does.
2. **The split of the tank's damping** between `R7` and the base/feedback
   loading. §58.3 derives the *total* effective parallel damping and states the
   split it implies, but could not confirm it.
3. **Pot rotation vs treadle angle** (the rack-and-pinion geometry). POSITION is
   taken as pot rotation. Geofex has a paper on exactly this
   (`wahrocker.pdf`) — 403 here.
4. **Core saturation** of the inductor (see above).

### 58.2 The sweep law — DERIVED, and the one fitted number lands inside a published spec

The mechanism is ElectroSmash's sentence: the pot tunes the **apparent
capacitance** of the tank cap. So the tank's resonance is

```
    f0(p) = f_LC / sqrt( M(p) ),     M(p) = Ceff/C  (the apparent-capacitance multiplication)
    f_LC  = 1 / (2*pi*sqrt(L*C))
```

with `L = 500 mH` and `C = 0.01 µF` **straight off the published component list —
neither is fitted**:

```
    f_LC = 1 / (2*pi*sqrt(0.5 * 1e-8)) = 2250.7908 Hz
```

**That single number is the slice's first real result.** At full toe the pot
feeds back nothing, so `M = 1` and the peak sits at the bare LC resonance —
**2250.79 Hz derived from two published component values**, against the CCRMA
*measured* toe of **2216.06 Hz**: **+1.57 %**. Two entirely independent routes
(a component list and a measurement of a real pedal) landing 1.6 % apart is what
says the mechanism is right, and it is why this model's toe is 2250.79 Hz and not
ElectroSmash's frequently-quoted "1.6 kHz" (see §58.6 for that disagreement).

The bootstrap multiplies `C` by `1 + A*u(p)` where `u(p)` is the pot's normalised
wiper law (1 at heel, 0 at toe). Pinning the **published heel** (450 Hz — the one
figure ElectroSmash, Catalinbread and Dunlop's own CBFH spec all agree on, within
2 %) fixes the span:

```
    A = (f_LC / 450)^2 - 1 = 24.0175762      (heel multiplies C by 25.02x)
```

That leaves **exactly one free parameter — the pot taper** — and this is where the
honesty check lives. Modelling it as the standard log-pot family
`u(p) = (beta^(1-p) - 1)/(beta - 1)` and least-squaring `beta` in ln(f) against
the CCRMA measured fit over the whole travel gives

```
    beta = 23.537247    ->    u(0.5) = 0.17090
```

i.e. **the fitted taper reads 17.1 % of full pot resistance at half rotation** —
squarely inside the independently documented audio/log-taper spec ("an A-taper
pot is at 10 % of the pot value at 50 % rotation", and the Hot Potz is documented
as a log / custom-audio part). The one fitted number in the sweep law is a pot
taper, and it came out being a pot taper.

Shipped law vs the CCRMA measured reference (`450*2^(2.3p)`):

| POSITION | u(p) | M(p) | derived f0 (Hz) | CCRMA measured fit (Hz) | error |
| --- | --- | --- | --- | --- | --- |
| 0.000 | 1.00000 | 25.018 | 450.00 | 450.00 | +0.00 % |
| 0.125 | 0.65933 | 16.835 | 548.56 | 549.24 | -0.12 % |
| 0.250 | 0.42978 | 11.322 | 668.91 | 670.35 | -0.22 % |
| 0.375 | 0.27511 | 7.608 | 816.05 | 818.18 | -0.26 % |
| 0.500 | 0.17090 | 5.104 | 996.23 | 998.61 | -0.24 % |
| 0.625 | 0.10068 | 3.418 | 1217.45 | 1218.83 | -0.11 % |
| 0.750 | 0.05336 | 2.282 | 1490.10 | 1487.61 | +0.17 % |
| 0.875 | 0.02148 | 1.516 | 1828.09 | 1815.66 | +0.68 % |
| 1.000 | 0.00000 | 1.000 | 2250.79 | 2216.06 | +1.57 % |

**rms 0.46 %, worst +1.57 % at the toe.** Total travel **2.322 octaves**,
450.0 -> 2250.8 Hz. The expectation this slice was handed ("roughly 400 Hz-2.2 kHz
— confirm, don't assume") is **confirmed**.

**Why the law is not a plain log, and why that matters.** The circuit's own law
is `f ~ 1/sqrt(1 + A*u)`, which with a LINEAR pot would put the sweep almost
entirely in the last inch of treadle travel:

| | heel half | toe half |
| --- | --- | --- |
| linear pot (counterfactual) | **0.472 octaves** | **1.851 octaves** |
| shipped (audio taper) | 1.147 octaves | 1.176 octaves |

The audio taper is what *linearises* the sweep in octaves — the pot's compression
and the circuit's square root very nearly cancel. That cancellation is why
CCRMA's exponential fit works so well on a real pedal, and it is the single most
common thing a modelled wah gets wrong: ship the mechanism with a linear control
law and the pedal feels dead for three quarters of its travel. It is asserted as
its own test bar.

### 58.3 Q across the sweep — the topology decides, and the measurement agrees

Fixed damping resistance `Rp`, fixed inductance `L`, **variable capacitance** is a
parallel RLC whose bandwidth is `BW = 1/(2*pi*Rp*Ceff)`. Since `Ceff ~ 1/f0^2`:

```
    BW ~ f0^2        and        Q = f0/BW = Rp/(2*pi*f0*L)  ~  1/f0
```

**The resonance is SHARPEST at the heel and BROADEST at the toe.** That is
counter-intuitive (a toe-down wah *sounds* piercing) and it is what the
measurement says: CCRMA's fitted Q runs **8 -> 4 -> 2** heel -> mid -> toe, and its
implied bandwidth exponent `d(lnBW)/d(ln f0)` is **1.870** against this
topology's exact **2.000**. The alternative topology — a series LCR in the
degeneration path — predicts constant absolute bandwidth (`Q ~ f0`, exponent 0)
and is **refuted by the measurement**, so it is not what this model ships.

Scale: fitting the one constant `Q*f0` to CCRMA over the travel gives
`K = 3997.44 Hz`, hence

```
    Rp = K * 2*pi*L = 12558.32 ohm
```

Derived Q against the measured reference:

| POSITION | f0 (Hz) | Q derived | Q CCRMA | error | BW (Hz) |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 450.00 | 8.883 | 8.000 | +11.0 % | 50.7 |
| 0.25 | 668.91 | 5.976 | 5.657 | +5.6 % | 111.9 |
| 0.50 | 996.23 | 4.013 | 4.000 | **+0.3 %** | 248.3 |
| 0.75 | 1490.10 | 2.683 | 2.828 | -5.2 % | 555.5 |
| 1.00 | 2250.79 | 1.776 | 2.000 | -11.2 % | 1267.3 |

The +/-11 % at the ends is the exponent difference (-1.000 derived vs -0.870
measured) and is **reported, not fitted away** — bending Q to match would mean
abandoning the topology that produced the sweep law, on the strength of a
three-point fit.

**VOICE — the "Vocal Mod" as a knob.** ElectroSmash documents `R7 = 33 kOhm` as the
resistor that "adjusts the sharpness of the resonant peak" and the standard mod
as raising it to 39 k / 68 k / 100 k. Splitting the derived total damping gives
the rest of the loading, `Rother` such that `Rother || 33 kOhm = 12558.32 Ohm`, i.e.
`Rother = 20273.5 Ohm`, and the knob sweeps `R7` **log-centred on the stock value**:

```
    R7(v) = 10890 * (100000/10890)^v      v = 0.5  ->  33.000 kOhm exactly
    Rp(v) = R7(v) || 20273.5
```

| VOICE | R7 | Rp | Q heel | Q mid | Q toe |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 10.89 kOhm | 7084 Ohm | 5.011 | 2.264 | 1.002 |
| 0.50 (stock) | 33.00 kOhm | 12558 Ohm | 8.883 | 4.013 | 1.776 |
| 1.00 | 100.0 kOhm | 16856 Ohm | 11.923 | 5.386 | 2.384 |

VOICE moves **width only** — the centre frequency and the peak height are
untouched by construction (§58.4), which is exactly the published description of
the mod ("the filter bell is spread").

**Peak height is constant across the sweep**, and that is not an assumption: in
this topology the resonant gain is set by resistors that do not move, and the two
published measurements agree (Catalinbread quotes ~18 dB at *both* ends). The
shipped peak boost is the published **+18 dB (7.943x)**, and "the peak height
does not move across the sweep" is asserted as a test bar rather than assumed.

### 58.4 The implementation — a TPT state variable, because the coefficients move every sample

The resonator is a **topology-preserving-transform (Zavalishin) state-variable
filter**, not a direct-form biquad, for three reasons that are all load-bearing
here:

1. Its two integrator states **are** the physical variables — inductor current
   and capacitor voltage. It is the tank, discretised, not a curve fitted to one.
2. It is unconditionally stable under **per-sample coefficient modulation**,
   which is the whole point of a wah: `g = tan(pi*f0/fs)` and `2R = 1/Q` are
   recomputed EVERY SAMPLE from the smoothed POSITION and the envelope (the
   phaser's precedent, §22). A direct-form biquad re-derived per sample is not
   safe under fast modulation; this is.
3. Its BP output has peak gain exactly `Q` at `f0`, so the **unity-peak**
   bandpass is `2R*bp` — the peak height is decoupled from Q by construction,
   which is what §58.3's "VOICE changes width only" bar needs.

`flushDenormal` is applied in the **WHOLE-STATE** form (docs §56.4b): this is a
second-order recursion, and the house one-liner — guarding only the newest tap —
provably does not converge above first order. Both integrator states are tested
and zeroed as a unit.

**The output stage is a real transistor.** `BjtStage` (docs §24/§53) is
configured as the GCB-95's common-emitter MPSA18-class stage —
`Vcc 9 V, Rc 22 kOhm, Re 390 Ohm, Rf 470 kOhm (collector->base), Rbg 82 kOhm
(base->ground), Cin 10 nF`, no feedback cap — and runs inside a **4x oversampled**
domain like every other nonlinear stage in the project, with a measured alias
floor (§58.6).

The staging between the two has **no fitted constant**. The pedal's published
closed-loop resonant gain is +18 dB; the transistor stage's own small-signal gain
`G0` is *measured from the model itself* in `prepare()` (a 1 kHz probe, then
`reset()`); so the tank's insertion divider is forced:

```
    kTankDivider = 10^(18/20) / G0
```

The stage therefore contributes its real curvature, its real headroom and its
real clipping ceiling, and the pedal's small-signal resonant boost is the
published 18 dB by construction. Where the pedal starts to bark is then a
*prediction*, not a knob (§58.6).

**Ordering caveat, stated because it is a real departure.** In the GCB-95 the
tank sits in the transistor's feedback path — filter and gain are one stage. This
model splits them (filter -> divider -> transistor), which is what makes the
filter's coefficients cheap to modulate per sample and keeps the nonlinearity in
a small oversampled domain. The cost is that the tank does not see the
transistor's clipped output, so the resonance does not detune or damp when the
stage is slammed. Recorded as **ADR 018**, not fitted around.

### 58.5 AUTO — the same tank, driven by an envelope, and why SENSITIVITY is the mode

There is no second filter and no second law: SENSITIVITY hands the *same*
`f0(p)` law an envelope-driven position.

```
    posEff = pos + (1 - pos) * sens * env      env in [0,1]
```

- **SENS = 0 is EXACTLY the manual pedal** — the envelope term is multiplied by
  zero, so a treadle wah is bit-for-bit unaffected by the feature. That is a test
  bar, not a claim.
- With SENS > 0, POSITION becomes the **resting (heel) frequency** the note falls
  back to and the envelope opens upward from it — which is how a real envelope
  filter's "range" control behaves.

There is deliberately **no discrete mode switch hidden in a float parameter
slot**. A mode encoded as "slot 1 >= 0.5" is a control whose whole travel does
nothing, which the house rules forbid; a continuous SENSITIVITY is live
everywhere and is the mode.

**The follower, and why the time constants matter more than the filter.** A
one-pole peak follower on |x| with asymmetric constants, from Geofex's published
figures ("maximum swing over a period of 50 msec or less ... drifts back to
baseline over a period of 500 msec or less"; a typical detector "attack about
10 ms, decay around 500 ms"):

```
    tauAttack  =  10.0 ms      (Geofex's stated typical attack)
    tauRelease = 166.7 ms      (3*tau = 500 ms = Geofex's stated drift-back)
```

`env` is normalised through a fixed reference level so a normally picked note
opens the filter usefully; the acceptance number is the **measured octave
excursion of a real pluck** (§58.6), not the follower's own output — asserting
that the follower follows would be a tautology.

**Cross-slice note (2026-07-31):** a compressor slice was running in parallel on
its own branch and is also building an envelope follower. Nothing is shared
across in-flight branches, by design. Unifying the two followers was a named
follow-up for a later cleanup pass — not a drive-by edit here.

**SETTLED 2026-08-01 — and the answer is NO, with the measurement in §58.8 and
the decision in ADR 023.** M13.1's follower became a real component
(`SidechainDetector`, shared with M13.6a's gate — §61.2, ADR 021), the
substitution into this pedal was built and run, and it is a **threshold**
detector where a wah needs a **proportional** one: 1.95 dB of proportional range
on the compressor's own component values and 6.71 dB on the best wah-plausible
ones, against **19.09 dB and no threshold** for the one-pole above. Substituted,
a 0.10 V pick moves this filter **0.000 octaves**. The wah keeps its own
follower; `SidechainDetector` was **not** widened. Do not re-open this without
reading §58.8 — and if you do, run the substitution rather than comparing block
diagrams.

### 58.6 Validation — `clipper_wah_tests` (44.1 k and 48 k)

New ctest target, `clipper_add_test_flags()`-registered, **ctest 25 -> 26 entries**.
Every headline bar is a property of a RENDER; where a bar is only
implementation-vs-its-own-recipe it says so.

**Measurement note that changed how the suite is written.** The first draft
measured the response by rendering a steady tone per probe frequency — ~250
renders per POSITION point, which put the suite past a 10-minute wall clock. It
now renders ONE tiny impulse (1 mV) per knob setting and takes 200 Goertzel bins
off the tail, and the impulse's linearity is **proved rather than assumed**: two
renders a decade apart in level agree to **0.0007 dB**. (At 0.1 V they do not
agree — that is the pedal barking, and it is measured separately.)

**(a) Sweep law, rendered vs derived vs the independent measurement** (48 k):

| POSITION | derived (Hz) | RENDERED (Hz) | err | CCRMA measured (Hz) | err |
| --- | --- | --- | --- | --- | --- |
| 0.000 | 450.00 | 449.23 | -0.17 % | 450.00 | -0.17 % |
| 0.125 | 548.56 | 548.23 | -0.06 % | 549.24 | -0.18 % |
| 0.250 | 668.91 | 669.04 | +0.02 % | 670.35 | -0.20 % |
| 0.375 | 816.05 | 816.48 | +0.05 % | 818.18 | -0.21 % |
| 0.500 | 996.23 | 996.40 | +0.02 % | 998.61 | -0.22 % |
| 0.625 | 1217.45 | 1215.97 | -0.12 % | 1218.83 | -0.23 % |
| 0.750 | 1490.10 | 1489.13 | -0.06 % | 1487.61 | +0.10 % |
| 0.875 | 1828.09 | 1830.03 | +0.11 % | 1815.66 | +0.79 % |
| 1.000 | 2250.79 | 2248.97 | -0.08 % | 2216.06 | +1.48 % |

**Worst vs the model's own law 0.17 % (the discretisation bar). Worst vs the
independent CCRMA measurement 1.48 %, rms 0.59 % (the bar with teeth).**

**(b) Shape.** Travel **2.322 octaves**; halves **1.147 / 1.176** against the
linear-taper counterfactual's **0.472 / 1.851**, computed in the test rather than
quoted. The wiper reads **17.09 %** at half rotation — asserted to land inside the
published audio-taper window (10-20 %), which is the honesty check on the slice's
single fitted number.

**(c) Resonance height and width:**

| POSITION | peak (dB) | Q rendered | Q derived | Q CCRMA |
| --- | --- | --- | --- | --- |
| 0.00 | 17.90 | 8.653 | 8.883 | 8.000 |
| 0.25 | 17.90 | 5.956 | 5.976 | 5.657 |
| 0.50 | 17.91 | 3.965 | 4.013 | 4.000 |
| 0.75 | 17.91 | 2.685 | 2.683 | 2.828 |
| 1.00 | 17.91 | 1.804 | 1.776 | 2.000 |

**Peak boost 17.90-17.91 dB against the published 18.0, and its spread across the
whole 2.3-octave travel is 0.010 dB** — the topology's constant-peak prediction,
measured. Q vs derived worst **2.6 %**; vs the independent measurement worst
**9.8 %** (the exponent difference of §58.3, reported not fitted).

**(d) VOICE moves width only** (POSITION 0.5): Q **2.253 -> 3.965 -> 5.292** while
the peak frequency stays at **996.40 Hz on all three** and the height at
**17.91 dB on all three**.

**(e) AUTO tracking, as a player-observable property.** A real plucked D (146.8 Hz,
0.30 V) with POSITION parked at 0.10 (527.2 Hz):

| SENSE | rest (Hz) | peak (Hz) | octaves | t to peak | t back to within 10 % |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 527.2 | 527.2 | **0.000** | — | — |
| 0.25 | 527.2 | 686.4 | 0.381 | 82.7 ms | 843 ms |
| 0.50 | 527.2 | 894.3 | 0.762 | 82.7 ms | 841 ms |
| 0.75 | 527.2 | 1167.0 | 1.146 | 82.7 ms | 841 ms |
| 1.00 | 527.2 | 1526.6 | **1.534** | 82.7 ms | 840 ms |

The filter goes **up and comes back**, by an amount that scales with the knob.
**SENSE = 0 measures EXACTLY 0.000 octaves**, and separately the whole render at
SENSE 0 is **bit-identical (worst |diff| 0.000e+00)** to a model whose
sensitivity was never set.

**Honest note on the attack: 82.7 ms to the peak, against a 10 ms follower time
constant.** That is not a bug and it is not the coefficient: |x| of a 147 Hz note
passes through zero twice per cycle, so a peak detector gains only during the
rising part of each cycle and decays for the rest — exactly what a real
diode-and-cap detector does. It reads as the quack/swell boundary. On a higher
note it is faster.

**(f) No zipper, measured against its own control** (48 k, 6-20 kHz, carrier
harmonics excluded):

| stimulus | far-field floor |
| --- | --- |
| STATIC POSITION | **-322.8 dB** |
| pathological per-block 0<->1 slam | **-104.3 dB** |
| fast full-travel sweep (5 Hz) | -68.8 (3-6 k) / -75.0 / -79.0 / **-81.3** (14-20 k) |

**The sweep skirt is NOT coefficient stepping, and the slice proved that rather
than asserting it.** Re-aiming POSITION every 64 samples, every 8 samples, and
every SINGLE sample all measure **identically** (-68.8/-75.0/-79.0/-81.3), so the
control granularity contributes nothing; the floor DECAYS with frequency (a
stepping artifact is flat) and drops **8.7-14.4 dB at 96 kHz** — both signatures
of a discrete time-varying resonator, neither of stepping. The test asserts the
decay, not just the level.

**(g) Alias floor** (4186 Hz at 1.5 V into the toe, harmonics excluded):

| factor | floor | latency |
| --- | --- | --- |
| 1x | -73.0 dB | 0 |
| 2x | -118.8 dB | 64 |
| **4x (shipped)** | **-118.8 dB** | **72** |
| 8x | -156.0 dB | 76 |

**45.8 dB of improvement from 1x to 4x** — the bar is "it MOVES with the factor"
(the §54 lesson), not merely "it is low".

**(h) DC offset ON SIGNAL:** worst **0.054 % of peak** across POSITION 0/0.5/1,
against the shared 1 % bar. **Honest note recorded in the test:** the +0.1 V
input-offset stimulus that `support/DcOffset.h` exists for has **no extra teeth
here** — the resonator is a bandpass with a zero at DC, so an input offset is
removed before it can reach anything and the two rows are identical to six
decimals. The clean row is doing the work.

**(i) reset / guards:** NaN knobs -> **0/4096** non-finite samples; a NaN input
followed by `reset()` -> **0/4096**; silence in -> exact digital silence out;
one 8192-sample block vs 64x128 blocks **bit-identical (0.000e+00)**;
`maxAbsRestingState()` **exactly 0.0** after a 12 s silent tail. The 12 s is
measured, not padding: 2 s reads 2.7e-06, 6 s 1.0e-16, 10 s 3.9e-27, **12 s
0.0** — the envelope follower's 166.7 ms release needs 11.4 s to ring from a loud
note down through the 1e-30 floor.

**(j) Staging, reported not aimed at.** `G0 x kTankDivider = 7.9433` exactly
(the identity that says nothing was fitted): measured stage gain **39.9082x**,
divider **0.199039**. On real playing material the pedal is a **CUT, not a level
bomb** — a plucked low E measures **-6.6 dB RMS** at every input level from 0.1
to 0.5 V peak, and the output peak stays at ~0.99x the input peak, because the
+18 dB lives at fc while the fundamental and everything above roll off.
Breakup (THD at resonance, POSITION 0.5): **0.64 % at 0.05 V, 1.32 % at 0.10,
2.79 % at 0.20, 4.48 % at 0.30, 9.10 % at 0.50, 33.67 % at 1.0 V** — i.e. clean
on a normal pickup and barking when boosted, which is a prediction of the
component values rather than a knob.

**CPU:** **14.25x realtime / 7.0 % of one 48 k stream** — cheaper than the Muff
(3.2x) and far cheaper than the JCM800. Latency **72 samples** at the shipped 4x.

### 58.7 What the slice found, what it refuted, and what is left open

**(1) A 9.6 dB peak-height tilt, and it was the MODEL's coupling cap, not the
law.** The first build measured the resonant boost at **12.18 dB at the heel and
21.74 dB at the toe** — a 9.6 dB tilt across a travel where the topology predicts
none — and pushed the rendered centre frequency +2.82 % out at the toe. Bisected
to `BjtStage::Config::Cin`: at the 10 nF first guess the stage's input network
has a **~1.1 kHz corner sitting in the MIDDLE of the sweep**. In the real pedal
the tank's own capacitor DC-blocks the base and there is no second coupling
high-pass; `BjtStage`'s topology requires a `Cin`, so it is now 1 µF (corner
~11 Hz, two decades below the band). After: **17.90-17.91 dB across the whole
travel, spread 0.010 dB**, and the worst centre-frequency error 0.17 %. The
constant-peak-height bar is what holds this honest, and the perturbation run
confirms reverting `Cin` fails it.

**(2) A hypothesis of this slice's own, REFUTED by its own measurement.** The
POSITION smoother was made a cascaded 2-pole on the theory that the fast-sweep
far-field floor was the host's per-BLOCK control staircase. It is not: re-aiming
POSITION per-block, per-8-samples and PER-SAMPLE all floor identically. The
comment in the source says so. The second pole was **kept anyway, for a different
and measured reason** — it improves the pathological per-block slam by 23.7 dB
(-80.6 -> -104.3) for one extra multiply-add.

**(3) Two test bars that could not fail, found by the perturbation run and
fixed.** Writing the peak-height bar against `WahModel::peakBoostDb()` made it an
identity: moving the constant to 24 dB left the whole suite green and only
tripped an unrelated assertion three functions later. It is now a literal 18.0,
the published figure. And nothing pinned the **shipped 4x oversampling default**
— `testAliasing` sets the factor explicitly on every row, so dropping the default
to 1x left the suite green; the default is now asserted through
`latencySamples() == 72`. Both were caught by perturbing, not by review.

**Perturbation transcript** (patch -> `touch` -> rebuild -> run -> restore ->
`touch` -> rebuild -> run; six for six):

| perturbation | result |
| --- | --- |
| `kTaperBeta` 23.537247 -> 1.0000001 (a LINEAR pot taper) | FAIL `worstRef < 3.0` |
| Q law flipped parallel-RLC -> series-LCR (`Q ~ f0`) | FAIL `worstQRef < 0.16` |
| `Cin` 1 µF -> 10 nF (the coupling corner back in the sweep) | FAIL `worstOwn < 1.5` |
| `kPeakBoostDb` 18 -> 24 | FAIL the published-literal bar |
| envelope release 166.7 ms -> 10 s | FAIL "the filter comes back" |
| default oversampling 4x -> 1x | FAIL `latencySamples() == 72` |

The **web** spec was perturbation-proven too: removing the worklet's `wah`
dispatch (so a wah routes to the RAT) takes the heel-band energy from 5.4e-04 to
**9.0e-05** and the cross-over bar goes red.

**Open, and named rather than fitted:**

1. **The full GCB-95 netlist** (§58.1 gap 1). The divider that turns wiper
   position into apparent-capacitance multiplication is pinned to published
   frequencies, not derived from component values. A slice with the netlist
   should be able to DERIVE `kTaperBeta` and check it against 23.537247.
2. **The Q exponent**: derived -1.000, measured -0.870, so +-11 % at the ends.
   Closing it means finding what else in the loading moves with the pot.
3. **The tank is not inside the transistor's feedback path** (§58.4), recorded in
   **ADR 018**. Cost: the resonance does not detune or damp when the stage is slammed.
4. **Inductor core saturation is not modelled** — the measured Fasel-vs-halo
   difference is mostly core behaviour, not inductance (§58.1).
5. ~~**Two envelope followers now exist in this repo**~~ — **SETTLED 2026-08-01,
   and the answer is that they are different circuits.** See **§58.8** and
   **ADR 023**: the substitution was built and measured, and the wah keeps its
   own follower. This item is closed, not deferred.
6. **Duplicate instances**: the native engine is one-instance-per-type, so a wah
   before AND after the dirt works on the web and not in the plugin — the
   pre-existing `kMaxChain` limitation, and this pedal is the first one where
   wanting two of them is a normal request.

### 58.8 The follower vs the shared `SidechainDetector` — measured, and REFUSED

**Added 2026-08-01** (branch `claude/wah-detector-unify-6f557i`, plan
`docs/work/2026-08-01-wah-detector-unify.md`, **ADR 023**). §58.5's cross-slice
note, §58.7 item 5, ADR 019 and ADR 021 all named the same follow-up: three
envelope followers were written in one week on three parallel branches, §61
extracted two of them into a real component (`SidechainDetector`), and the wah's
was the last one standing. This subsection is the settlement.

**The outcome is (b): they are genuinely different circuits, and the wah keeps
its own.** ADR 021's prohibition — do not grow the component a parameter per
consumer until it is a union of three models — bound this slice, so the refusal
had to be earned with a measurement rather than a block diagram.

#### 58.8.1 The deciding metric, and why it is the right one

`SidechainDetector` is a **THRESHOLD detector**: a DC-restorer clamp into a
base-emitter junction, whose conduction is exponential and whose collector
current is orders of magnitude larger than the envelope resistor's pull-up.
§59 says so in its own words ("the threshold IS a Vbe") and §61.4 *builds* a
40 dB dB-linear threshold on top of exactly that steepness.

The metric is the **proportional range** — the input dynamic range, in dB, over
which the envelope travels from 10 % to 90 % of its own full swing. In player
terms: how much of a pick-strength range the control responds to proportionally.
Measured open loop on a 146.83 Hz tone, at 48 kHz for the wah row and 176.4 kHz
(the consumers' own detector rate) for the others:

| detector | envelope pair | 10 % at | 90 % at | proportional range |
| --- | --- | --- | --- | --- |
| M13.1 compressor | 10 µF / 150 kΩ | 0.45147 V | 0.56511 V | **1.950 dB** |
| M13.6a gate | 47 nF / 220 kΩ | 0.43749 V | 0.57566 V | **2.384 dB** |
| best wah-plausible config found | 10 µF / 16.67 kΩ | 0.54975 V | 1.19046 V | **6.711 dB** |
| **the wah's shipped follower** | ideal `\|x\|` + one-pole | — | — | **19.085 dB, no threshold** |

**The compressor's own detector is a switch too, and that is the finding that
settles it.** Its graded compression is the FEEDBACK LOOP, not the detector —
§59 measured the same fact from the other side (216:1 feed-back against 3.3:1
feed-forward). **A wah has no gain to reduce**, so it is feed-forward by
necessity and cannot borrow the loop.

The drive gain in front of the detector is a **pure level translation** — across
three decades (1 → 300) the range is identical to three decimal places — so with
the published 166.7 ms release pinned as `R_env·C_env`, the range is set purely
by the current balance. Pushing R_env down widens it, and it reaches the shipped
follower's figure only in a limit that is not a part:

| R_env | C_env | proportional range |
| --- | --- | --- |
| 16.7 kΩ | 10 µF | 6.639 dB |
| 5.0 kΩ | 33.3 µF | 10.672 dB |
| 1.67 kΩ | 100 µF | 13.812 dB |
| 556 Ω | 300 µF | 15.861 dB |
| 167 Ω | 1 000 µF | 17.085 dB |
| 55.6 Ω | 3 000 µF | 17.685 dB |
| 16.7 Ω | 10 000 µF | 18.094 dB |

(The 16.7 kΩ row reads 6.639 dB here against 6.711 in the table above: the
extension sweep uses a coarser bisection and a shorter settle so it could cover
seven decades. The 0.07 dB disagreement is the harness's, not the model's, and
neither figure is within 12 dB of the follower's.)

**The wah's ideal rectifier IS the R_env → 0 limit of this detector** — the limit
in which the transistor's exponential is swamped by the pull-up. That is a
different circuit, not a component value; and a 10 000 µF cap behind a 17 Ω
resistor across a 9 V rail is a power supply, still 1 dB short.

#### 58.8.2 The substitution, built and run

The refusal is not argued from the table above. `WahModel` was re-plumbed onto a
`SidechainDetector` in a scratch tree and given the best configuration the
topology offers: envelope pair 10 µF / 16.667 kΩ (release pinned to the published
166.7 ms; 10 µF is M13.1's own cap, the largest genuinely-a-part choice), clamp
network in the gate's op-amp precision-rectifier form (Geofex's cited detector
*is* a precision rectifier, so equal 1 kΩ leg impedances), and calibrated at ONE
point — the drive gain set so a steady 0.25 V note lands mid-transfer, the span
set so the same note reads the env01 the shipped follower gives it. Everything
below is then a prediction.

**Every §58.6 AUTO number moved** (48 kHz, plucked D at 0.30 V, POSITION 0.10):

| SENSE | octaves before | after | t_peak before | after | t_back before | after |
| --- | --- | --- | --- | --- | --- | --- |
| 0.00 | 0.000 | 0.000 | — | — | 0.0 | 0.0 |
| 0.25 | 0.381 | **0.239** | 82.7 ms | **144.0 ms** | 842.7 ms | **438.7 ms** |
| 0.50 | 0.762 | **0.478** | 82.7 | **144.0** | 841.3 | **438.7** |
| 0.75 | 1.146 | **0.718** | 82.7 | **144.0** | 841.3 | **438.7** |
| 1.00 | **1.534** | **0.958** | 82.7 | **144.0** | 840.0 | **438.7** |

44.1 kHz agrees to the decimal (0.239 / 0.478 / 0.718 / 0.958, t_peak 143.7 ms).
**0.958 octaves fails §58.6's own acceptance bar** (`prevOct > 1.0` — "a single
pluck at full SENSITIVITY must be worth more than an octave, otherwise the
control is decorative"), and it fails it before any new bar is reached. Note the
t_back moved 840 → 439 ms even though `R_env·C_env` is *exactly* the shipped
release τ: an identical time constant does not give an identical return, because
the excursion it is returning from is smaller and the discharge is nonlinear.

**The real damage is level dependence.** Sweep depth at SENS 1.00 against pick
strength — the shipped follower is exactly proportional across the whole
realistic range, the substitution is dead below a firm pick and railed above one:

| pick peak | shipped octaves | substituted octaves | shipped t_peak | substituted t_peak |
| --- | --- | --- | --- | --- |
| 0.02 V | 0.101 | **0.000** | 82.7 ms | 178.7 ms |
| 0.05 V | 0.254 | **0.000** | 82.7 | 104.0 |
| 0.10 V (the house clean probe, §11.1) | 0.508 | **0.000** | 82.7 | 56.0 |
| 0.15 V | 0.762 | **0.014** | 82.7 | 56.0 |
| 0.20 V | 1.018 | **0.207** | 82.7 | 89.3 |
| 0.30 V | 1.534 | 0.958 | 82.7 | 144.0 |
| 0.50 V | 2.094 (railed) | 2.094 (railed) | 29.3 | 74.7 |
| 0.80 V | 2.094 (railed) | 2.094 (railed) | 14.7 | 32.0 |

So a substituted wah **does not move at all** at the project's own clean probe
level, and its time-to-peak is neither constant nor monotone (56–179 ms). §58.6's
honest note said the 82.7 ms is *the rectifier's own behaviour* on a 147 Hz note
rather than the 10 ms coefficient — that attribution is confirmed here twice
over, and it cuts both ways: change the rectifier and the number moves.

**Two more consequences, both structural rather than voicing:**

* `maxAbsRestingState()` goes **exactly 0.0 → 2.675e-13** after a 20 s silent
  tail, failing the §33/ADR 006 bar the wah suite already asserts. This is not a
  bug in either design — it is a **formal contradiction between them**. The wah's
  `env` rests at zero and is therefore `flushDenormal`-guarded and asserted
  exact; the detector's envelope node rests at the SUPPLY RAIL and is explicitly
  *not* guarded, with the reasoning written into `SidechainDetector.h`. One
  object cannot be on both sides of ADR 006's scope rule.
* CPU rises ~27 % for the whole pedal: interleaved same-machine A/B, six pairs,
  median **0.484 s → 0.617 s** per 10 s of audio = **4.8 % → 6.1 %** of one
  48 kHz stream — a Newton solve per leg per sample where there were three flops.

**What survived:** SENS = 0 is still bit-identical (`worst |diff| = 0.000e+00`),
because the envelope term is multiplied by zero. That bar cannot distinguish the
two and was never going to.

#### 58.8.3 The bar that gives the refusal teeth

Prose rots; `clipper_wah_tests` gains `testFollowerLevelLaw`, three
player-observable bars measured on a RENDER (they watch the FILTER, not the
follower — reading the follower's own output would be a tautology):

| bar | shipped, 44.1 k / 48 k | why it exists |
| --- | --- | --- |
| a QUIET 0.05 V pick still opens the filter (`> 0.15 oct`) | **0.254 oct** | a Vbe threshold measures 0.000 |
| sweep depth is PROPORTIONAL (2× and 4× the pick → 2× and 4× the sweep, ±10 %) | **2.002 / 4.014** | the tank's `f0(p)` law is nonlinear, so proportionality in OCTAVES is a prediction about the FOLLOWER, not an identity |
| time-to-peak does not move with pick strength (≤ 2 measurement blocks) | **0.00 ms spread across 12 dB** | a current-starved discharge's attack does move — §59 measured 14/10/5/3 ms across SUSTAIN |

**Perturbations (each patched, `touch`ed, rebuilt, run; then restored,
`touch`ed, rebuilt, run — all restores GREEN):**

| # | patch | result |
| --- | --- | --- |
| P1 | the whole `SidechainDetector` substitution | **RED** — and it trips §58.6's *existing* `prevOct > 1.0` first. Re-run with the new test ordered ahead of it: **RED on bar 1**. Re-run again with bars 1–2 removed: **RED on bar 3**. All three bars proven independently. |
| P2 | a Vbe-style deadband on the ideal rectifier (`max(0, \|x\| − 0.06)`) | **RED** on bar 1 — the threshold, isolated from everything else the detector changes |
| P3 | attack coefficient scaled *down* by the envelope | **RED** on bar 1 |
| P3b | attack coefficient scaled *up* by the envelope (`×(1 + 40·env)`, clamped) | **RED** on bar 2 |
| P4 | a HALF-wave rectifier instead of `\|x\|` | **GREEN, and reported rather than hidden.** t_peak moves **82.7 → 90.0 ms** and depth scales by 0.783 uniformly (0.199/0.398/0.797 oct), but the spread stays **0.00 ms** and the ratios stay 2.001/4.010. That is correct: bar 3 asserts level-INDEPENDENCE, not the value 82.7 ms. The perturbation confirms §58.6's attribution of the constant to the rectifier while showing the *law* belongs to the linear follower. |

#### 58.8.4 What this does and does not license

**Does not close M13.3.** The optical compressor is still expected to reuse
`SidechainDetector` — it is a feed-back compressor with a gain cell, the case the
component is built for. The rule this slice adds is procedural, not a veto:
**build the substitution and measure it.** "Both blocks rectify and integrate" is
not a reason to share and not a reason to refuse.

**The honest reason the wah is different, stated plainly.** The GCB-95 has **no
envelope follower at all** — §58's AUTO mode is a synthesised feature, and its
constants come from a published *behavioural* spec (Geofex: ~10 ms attack,
~500 ms drift-back) rather than from a transcribed netlist. Forcing it onto
`SidechainDetector` would have meant inventing component values to reproduce a
published time constant, i.e. fitting parts to a target — the exact thing §57
spent a whole slice undoing.

## 59. M13.1 — the "Squash" OTA compressor (the first DYNAMICS processor)

The lineup's first pedal that is neither dirt nor modulation: an MXR Dyna Comp /
Ross-style **CA3080 OTA compressor**, shipping as pedal type `comp`. Two knobs,
because the real box has two — SUSTAIN and LEVEL. Files: shared
`core/include/clipper/dsp/CompressorEngine.h` + `core/src/dsp/CompressorEngine.cpp`;
thin `core/include/clipper/dsp/CompModel.h` + `core/src/dsp/CompModel.cpp`; tests
`core/tests/test_comp_model.cpp` (`clipper_comp_tests`); C ABI `comp_*`;
`--pedal comp` in the render CLI; a `squash` row in `clipper-bench`; worklet
`comp` dispatch; `rig.ts` / `Pedal.tsx` / `pedal.css` / `tokens.css` / assistant;
native `ClipperEngine` + APVTS + `PedalCard`. Trademark-safe throughout (no
MXR / Dyna Comp / Ross text on any user surface; the wordmark is "Squash").

**Built as a config-parameterized engine from the first line**, exactly as §21's
`OverdriveEngine` serves both the SD-1 and the TS. The optical / LA-2A-style
voice (M13.3) is a second config plus one `applyGainCell()` case; the noise gate
(M13.6) is this same detector feeding a different `ControlMap`. The seam is
written out in full in the `CompressorEngine.h` banner so the next slice does not
have to guess where it is, and the decision — including its named risk, that a
seam written before its second consumer exists can be the *wrong* seam — is
recorded in **ADR 019**
(`docs/decisions/019-compressor-engine-is-config-parameterized.md`).

### 59.1 Research — what was reachable, and what was not

**Reachable.** The primary source is a complete **LTspice transcription of the
whole pedal**, published by Nick Chesney (cushychicken) alongside his write-up
"Simulating the MXR Dyna Comp Compressor in LTSpice" (2020-11-02), and hosted on
GitHub at `Cushychicken/ltspice-guitar-pedals` (`mxr-dyna-comp/mxr-dyna-comp.asc`)
— which is why it was reachable at all (see the gap list below). His schematic is
in turn traced from **R. G. Keen's Geofex archive** (`dynacomp.gif`). The `.asc`
was parsed node by node from its `SYMBOL`/`WIRE`/`FLAG` records and the netlist
reconstructed by hand; that reconstruction is what `CompModel.cpp` encodes, and
every reference designator in that file follows the LTspice numbering.

Component values were **cross-checked against an independent source**: the
ElectroSmash "MXR Dyna Comp Analysis" component list, reached through search-result
extracts (the site itself 403s here — see the gaps). Its designators differ but
its values agree: the 500 kΩ sensitivity pot, the 50 kΩ output pot, the 2 kΩ
trimmer, the 27 kΩ bias-setting resistor, the 150 kΩ pair, the 10 µF caps, the
1 nF, the 1N914/1N4148 clamp diodes, the 2N3904s, the CA3080.

Two further cross-checks that landed, both worth recording because they validate
the *model*, not just the transcription:

* **The bias rail.** `CompModel.cpp` computes it as `9 V · 27 k/(56 k + 27 k) =
  2.9277 V`, from the divider. The schematic's own net for that node is named
  `V3P0`. Nothing was assumed.
* **The whole control loop, against an independent simulation.** Chesney's SPICE
  run reports that with the sensitivity pot at 10 kΩ the bias current starts at
  "just under 200 µA" and, on a 100 mV 1 kHz sine, "stabilizes at about 16 µA".
  This model, at the SUSTAIN position that gives the same 37 kΩ total, measures
  **199.85 µA idle and 16.54 µA settled**. That is a 4 % and 3 % agreement on a
  number produced by the gain cell, the load, the phase splitter, the rectifier,
  the envelope integrator and the control map all in series, from a simulator
  this code has never seen. It is the strongest absolute check available for a
  pedal with no published transfer curve.

**Not reachable from this environment, recorded as gaps.** The proxy in this
container permits **github.com only**; every other host returns 403 at the CONNECT
stage, so `WebFetch` failed uniformly and `WebSearch` result extracts were the
only other channel.

* **Kröger & Zölzer / DAFx-11, "Analysis and Simulation of an Analog Guitar
  Compressor"** — the state-space analysis of this exact pedal. `recherche.ircam.fr`
  and `dafx.de` both 403. This is the biggest gap: it is the one source that would
  have given an independent set of derived equations to check the gain cell and the
  detector against.
* **ElectroSmash's analysis page and the CA3080 datasheet / AN6668** — 403 direct;
  used only through search extracts. The OTA laws below (`gm = Iabc/2Vt`,
  `Iout = Iabc·tanh(Vd/2Vt)`) are therefore **derived from bipolar differential-pair
  theory**, not quoted, though the extracts do confirm the qualitative statement
  ("transconductance is directly proportional to the amplifier bias current").
* **coda-effects.com** — 403, as CLAUDE.md already records.
* **A published static compression curve or ratio for a real unit.** None found.
  Every curve in §59.4 is this model's own; there is no absolute reference to
  check the ratio against, and that is stated rather than papered over.
* **The SUSTAIN pot's taper letter.** Not sourced anywhere. Shipped as a plain
  linear rheostat; the measured consequence is in §59.4 and is reported, not fitted.
* **Attack/release figures.** A review measurement ("attack fixed at 5 ms,
  release ~1 second") was the only quantitative claim found, and it is a review,
  not a manufacturer spec. Treated accordingly: it is used as a soft anchor with a
  3–9 ms window, never as a fitting target.

### 59.2 The three things the brief said must be modelled, not approximated

**1. The CA3080 gain cell.** Not `gain = f(envelope)`. The cell is the bipolar
differential pair it physically is:

```
    Iout = Iabc · tanh( Vd / (2·Vt) )        Vt = 25.85 mV
    gm   = dIout/dVd |_0 = Iabc / (2·Vt)
```

`Iabc` is a real current computed from real node voltages (§59.3), and the tanh is
the CA3080's own soft limit, which is a large part of why the pedal colours the
tone. At the shipped SUSTAIN 0.5 idle, `Iabc` = 27.70 µA → `gm` = 535.7 µS.

**2. The detector time constants — derived, because they ARE the pedal.** There is
no attack knob and no release knob because there are no parts for them:

| what | components | derived | measured |
| --- | --- | --- | --- |
| release | `R18·C9` = 150 kΩ × 10 µF | **1.500 s** | **1.496 s** (SUSTAIN 0.5), 1.448 s (1.0) |
| attack | peak detector: `C7/C8` 10 nF into `R16/R17` 1 MΩ, clamped by `D1/D2`, driving `Q3/Q4`'s base | not a single RC — it is a *current-starved discharge* whose rate is set by how much base current the phase splitter can push through the clamp | **14 / 10 / 5 / 3 ms** at SUSTAIN 0.30 / 0.50 / 0.80 / 1.00 |
| clamp settling | `C7·R16` = 10 nF × 1 MΩ | 10 ms | (the DC-restorer corner) |

The attack is *not constant across the knob*, which falls out of the circuit
rather than being designed: more idle gain puts the detector over its turn-on
sooner. The published ~5 ms lands at SUSTAIN 0.80.

**3. Feed-forward vs feed-back — settled from the netlist.** The detector taps
`V_comp_out`, the phase splitter's emitter, which is **after** the gain cell.
The Dyna Comp is therefore **FEED-BACK**. This is encoded as a config field with a
measurement hook (`setDetectorTap`) rather than a comment, and
`clipper_comp_tests` proves the claim is load-bearing: at SUSTAIN 1.0 over 26 dB
of input, feed-back gives **216:1** and the same code tapping the cell's input
gives **3.3:1**.

That one fact explains the pedal's whole character. In a feed-back compressor the
loop drives the OUTPUT to wherever the detector turns on, so the threshold is an
absolute output level fixed by a base-emitter drop — it does not move for any
knob, and above it the ratio is enormous.

### 59.3 The circuit → the model, stage by stage

```
in -[C2 10n]-[R4 10k]- Q1 emitter follower -[C3 1u]-> CA3080 (-in)
                                                          |
    (+in) AC-terminated by R12 15k || C4 10n in series with C5 1u,
    bridged to (-in) by the 2k offset TRIMMER  <-- THE input attenuator
                                                          |
                             Iout = Iabc·tanh(Vd/2Vt)     v
                     R13 150k || C6 1n  (to the 2.93 V bias rail)
                                                          |
                                            Q2 phase splitter (R14 = R15 = 10k)
                                             /                          \
                              EMITTER = audio out              COLLECTOR (anti-phase)
                                             \                          /
                       [C7/C8 10n] -> nodes with [R16/R17 1M] to ground,
                       [D1/D2 1N4148] clamping the NEGATIVE excursion,
                       Q3/Q4 grounded-emitter, collectors on the envelope node
                                                          |
                                          C9 10u, R18 150k to +9 V
                                                          |
                              Q5 follower -> RV1 500k SUSTAIN rheostat -> R23 27k
                                                          |
                                                    Iabc (back to the OTA)
out <- RV2 50k LEVEL <- R20 10k <- C10 50n <- (the same splitter emitter)
```

**The finding that is in no prose source: the OTA sees only ~12 % of the signal,
and the network that does it is second order.** The 2 kΩ offset trimmer bridges
the two OTA inputs while the non-inverting input is AC-terminated in 15 kΩ, so the
*differential* drive is a divider, not the raw signal. Its transfer function is

```
  Zterm(s) = R8 || [ R12/(1 + s·R12·C4) + 1/(s·C5) ]
  H(s)     = −Rtrim / (Rtrim + Zterm(s))
```

with **real, hugely separated roots** — zeros at 0.157 Hz and 1077 Hz, poles at
9.29 Hz and 9105 Hz, `|H| → 1` at infinity and 0.002 at DC. `CompModel.cpp`
computes those roots from the component values rather than pasting them in.

This matters twice over:

* **Tonally.** `|H_d|` RISES with frequency (0.118 at 100 Hz, 0.160 at 1 kHz,
  0.492 at 5 kHz) while the 150 kΩ ∥ 1 nF load pole at **1210 Hz** FALLS. They
  very nearly cancel: the composed small-signal response is flat within **1.74 dB
  from 110 Hz to 5 kHz**. That is why a Dyna Comp reads as "not very bright"
  without sounding filtered, and it is why neither network may be simplified away
  without the other.
* **Numerically.** A 9.29 Hz pole at the oversampled 192 kHz sits at radius
  0.99970. Docs §56 measured exactly that shape emitting audible hiss out of a
  `float` direct form, and §56.4b showed the house `flushDenormal` one-liner
  cannot converge a direct form of order ≥ 2 *at all*. So the network ships as a
  **cascade of two first-order sections in `double`** — the cure §56 named,
  applied at the first opportunity rather than after the fact.

**Everything else, and where each number comes from.** The 2N3904 card is ON
Semi's SPICE model verbatim (`IS = 6.734f`, `BF = 416.4`, `ISE = 6.734f`,
`NE = 1.259`); the 1N4148 is the project's own post-§36 card (`Is = 2.52 nA`,
`n = 1.752`) — the ideality factor is **not** dropped back to 1.0 here either.
The **Gummel-Poon low-current (ISE/NE) term is modelled deliberately**, and it is
the one modelling choice in this slice that needed an argument: the detector
transistors' base current is what loads the clamp network, so their hFE at ~0.2 mA
is what sets the attack. With `betaF` alone a 2N3904 reports hFE = 416; with the
ISE term the *same published card* yields **hFE = 106 at 0.23 mA**, which is the
datasheet's own low-current figure, obtained with nothing fitted.

The CA3080's Iabc pin is taken as **0.7 V** above the negative rail (one
diode-connected mirror junction). Note for anyone comparing against the LTspice
transcription: it substitutes an LM13700, whose bias node sits at ~1.3 V, moving
the maximum control current by 8 %. The pedal has a CA3080.

Two documented simplifications, both named rather than buried:

1. **The discharge transistors' saturation** is a smooth `Vce/(Vce + 0.1)` factor
   rather than Ebers-Moll's reverse term. It only ever acts when the envelope node
   is already bottomed, and the measured worst-case discharge moves the node
   0.01 V per sample, so it effectively never binds.
2. **The supply compliance** is applied to the load node *after* its pole rather
   than inside the RC. It uses the existing `AsymSoftClipper` (ADAA), and it is
   genuinely asymmetric on a single supply: +5.07 V up, +1.93 V down around the
   2.93 V rail.

### 59.4 What SUSTAIN actually is — measured

**It is not a threshold.** It is a rheostat in the control-current path, so it
sets the gain cell's IDLE bias current, i.e. how much gain the loop has to give
away before it settles. The threshold is a base-emitter drop and no knob is in
front of it. Measured at 1 kHz, LEVEL 1.0, settled:

| SUSTAIN | idle Iabc | small-signal gain | settled output peak above the knee |
| --- | --- | --- | --- |
| 0.00 | 14.63 µA | **+11.25 dB** | 0.383 V |
| 0.30 | 20.41 µA | **+14.14 dB** | 0.386 V |
| 0.50 | 27.70 µA | **+16.79 dB** | 0.390 V |
| 0.80 | 59.89 µA | **+23.49 dB** | 0.396 V |
| 1.00 | 270.5 µA | **+36.58 dB** | 0.400 V |

**The gain column moves 25.33 dB. The output column moves 0.28 dB.** That pair of
numbers is the whole answer, and it is what a `gain = f(env)` fake would get
wrong. The 25.33 dB is the circuit's own span: `Iabc = 7.1 V/(R + 27 k)` over a
500 kΩ rheostat is a 19.5× range = 25.8 dB, and `gm ∝ Iabc`.

The static curve at SUSTAIN 0.50 (input dBV peak → output dBV peak):

| in | −53.98 | −46.02 | −40.00 | −33.98 | −26.02 | −20.00 | −13.98 | −7.96 | −1.94 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| out | −37.19 | −29.23 | −23.21 | −17.20 | −9.54 | −8.39 | −8.17 | −8.14 | −8.22 |
| gain | +16.79 | +16.79 | +16.79 | +16.78 | +16.48 | +11.61 | +5.81 | −0.18 | −6.28 |

Slope below the knee **0.9992** (linear, to four figures). Ratio over the top
18 dB of input **106:1**. The 3 dB-of-gain-reduction knee sits at **−20.0 dBV**.
It is a limiter, not a gentle studio compressor, and that is correct for the
circuit.

**The knob law, reported not fitted.** With the pot as a plain linear rheostat
(its taper letter could not be sourced), `Iabc = 7.1/(R + 27 k)` is strongly
top-heavy: 12.24 dB of the 25.33 dB total lives in the last 20 % of travel. That
is measured and left alone. If the owner wants it re-tapered, that is a §43-style
owner decision with a stated design equation, not something to fit here.

### 59.5 Sustain extension — the reason the pedal exists

Level-matched, because "it lasts longer" only means something at equal loudness:
a decaying 220 Hz note (0.30 V peak, τ = 0.7 s), the bypassed reference scaled so
both envelopes agree 0.3 s in, then the time each stays within 20 dB of that point.

| SUSTAIN | compressed holds | level-matched bypass | extension |
| --- | --- | --- | --- |
| 0.00 | 2.26 s | 1.90 s | **+0.36 s** (1.19×) |
| 0.30 | 2.46 s | 1.90 s | **+0.56 s** (1.29×) |
| 0.50 | 2.64 s | 1.90 s | **+0.74 s** (1.39×) |
| 0.80 | 3.14 s | 1.90 s | **+1.24 s** (1.65×) |
| 1.00 | 4.16 s | 1.90 s | **+2.26 s** (2.19×) |

### 59.6 The honest expectations — REPORTED, not designed out

A real Dyna Comp squashes the attack, hisses at full sensitivity and colours the
tone at every setting. None of that was "fixed"; a better-behaved compressor would
be a different pedal. Each is measured, and each is **asserted in the direction
that says this is still a Dyna Comp**, so a future slice that quietly improves one
turns the suite red and has to argue the change.

**Attack-transient loss** (1 kHz burst, the first 50 ms vs settled):

| SUSTAIN | 0.30 | 0.50 | 0.80 | 1.00 |
| --- | --- | --- | --- | --- |
| loss | **7.72 dB** | **10.00 dB** | **15.18 dB** | **20.00 dB** |

**Noise.** This project synthesises no noise anywhere, so what is measured is the
mechanism: the GAIN the pedal applies to whatever floor arrives. On a −84.75 dBFS
input noise floor at LEVEL 1.0 — SUSTAIN 0.00 **+8.22 dB**, 0.50 **+13.76 dB**,
1.00 **+33.56 dB**. Turning the knob up costs **25.34 dB** of noise gain, bought
with the same turn as the compression. That is the pedal's famous hiss, stated as
a number rather than simulated.

**Tone colour**, small-signal, dB re 1 kHz (SUSTAIN 0.5):

| Hz | 20 | 41 | 82 | 110 | 220 | 440 | 1000 | 2000 | 5000 | 10000 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| dB | −15.21 | −6.59 | **−2.42** | −1.57 | −0.65 | −0.32 | 0.00 | +0.17 | −0.58 | **−2.88** |

The low end really is thinned — three high-passes sit between input and output
(input 30.5 Hz, output 51.9 Hz, and the drive network's own 9.29 Hz pole), the
smallest coupling cap in the path being 10 nF. Low E is down 2.42 dB and 20 Hz is
down 15.21 dB. The top rolls off 2.88 dB by 10 kHz, which is the "keeps the white
noise down" characterisation of the Ross/Dyna Comp family, measured.

**And it inverts phase.** The signal drives the OTA's inverting input, so the
pedal is phase-inverting, like the real one. Documented, not corrected.

### 59.7 Hygiene, cost, and the numbers the conventions require

| property | measured |
| --- | --- |
| aliasing, single 9 kHz tone at 0.3 V, SUSTAIN 1.0 | 1× **−7.61 dB** → 2× −28.99 → **4× −62.37** → 8× −97.73 |
| DC offset ON SIGNAL (220 Hz 0.2 V, and with +0.1 V input offset) | 0.0000 % of peak both cases |
| block-size invariance (128 vs ragged 100/37/256/1/411) | **exactly 0.0** |
| rate independence (settled level, 44.1 / 48 / 88.2 / 96 kHz) | spread **0.014 dB** |
| latency | 72 samples at 4× (the shared oversampler figure) |
| `reset()` | re-parks at the cached quiescent point **exactly** (matches a fresh model to < 1e-9 V); one NaN poisons 245/256 samples, after `reset()` **0/48000** |
| denormals | `maxAbsRestingState()` **exactly 0.0** after a silent tail; output exactly 0.0f |
| knob zipper vs the signal's own slew | SUSTAIN **1.66×**, LEVEL **0.93×** |
| CPU, 4× at 48 kHz | signal **8.24× realtime / 12.1 % of a stream**; silence 16.5× / 6.1 % |

The aliasing measurement needed a correction worth recording, because the first
attempt was wrong in an instructive way: a **two-tone** stimulus reported −25 dB
flat across 1×…8×, which looked like a bad alias floor and was not aliasing at
all. A compressor's gain is modulated by its own detector, so two tones fill the
spectrum with legitimate intermodulation sidebands. The tell is exactly §54's:
**a real alias floor moves with the oversampling factor.** With a single tone the
envelope is constant and everything below the fundamental is fold-back — and the
floor then moves 54.76 dB from 1× to 4×.

**Silence is CHEAPER than signal** (6.1 % vs 12.1 %), which is the §33/§34 check:
the detector's Newton solve early-outs on its residual at the parked node instead
of burning its iteration budget, and the denormal policy is doing its job.

The denormal scope rule (ADR 006) was applied by measurement, not by pattern:

* **Guarded** — the six linear filter states (input and output coupling
  high-passes, the two drive-network sections, the load pole). All rest at exactly
  zero; `maxAbsRestingState()` covers exactly these and asserts 0.0.
* **NOT guarded, envelope node** — rests at **8.9684 V at SUSTAIN 0 / 8.6353 V at
  SUSTAIN 1.0**. A real DC operating point; it can never be subnormal.
* **NOT guarded, the two clamp caps and their Newton warm starts** — and this one
  is a finding rather than an application of the rule. Their *physical* rest is
  −4.90e-272 V (the node's equilibrium against the clamp diode's own reverse
  saturation current), which IS below the flush floor — but the **solver cannot
  resolve it**: Newton exits at `kNewtonResidualTolA`, so the node floors at
  `tol × (Rsrc + T/C)` ≈ **1.9e-11 V** and the cap settles inside that. Measured
  after a 40 s silent tail: ~1e-11 V, not decaying. That is 297 decades above the
  subnormal range, so a guard there would be unreachable code in the hottest loop
  in the file — the exact thing §33 found was the wrong thing to ship. Comment,
  not flush.

### 59.8 A bug this slice made, and how it was caught

The first implementation put the **collector** current in the clamp node's KCL.
Only base current flows into that node; the collector current flows from the
envelope cap to ground and never touches the base. The wrong version made the node
~β times too stiff, which starved the discharge by two orders of magnitude, and
the symptoms were: attack **500–650 ms** instead of 3–14 ms, the output never
properly pinned (ratio 9:1 at SUSTAIN 1.0 instead of >100:1), and — the tell — the
independent LTspice cross-check off by **3.6×** (58.4 µA settled against the
reference sim's 16 µA) while the *idle* current agreed to 4 %. An error that
shows up only under signal, and only against an outside number, is exactly what a
self-consistent test suite cannot catch; this is the case for keeping an absolute
reference in the loop.

### 59.9 Perturbation proofs

Every load-bearing bar was proven by reverting the thing it names in a scratch
copy of the source, rebuilding (with `touch` after **both** patch and restore —
docs §29's trap), and confirming the suite goes red. `git stash` was NOT used:
CLAUDE.md records that stashes are repo-global across worktrees and have already
caused one cross-agent collision.

| # | perturbation | result | first assertion to fail |
| --- | --- | --- | --- |
| baseline | — | GREEN | — |
| P1 | `detectorFromOutput = true → false` (feed-forward) | **RED** | `squashed < idle * 0.6` — the loop stops giving gain away |
| P2 | `C6` 1 nF → 1 pF (the load pole deleted) | **RED** | `hi - lo < 3.0` — the core band stops being flat, 1.74 → 6.6 dB |
| P3 | `C9` 10 µF → 1 µF (release 1.5 s → 0.15 s) | **RED** | `rel > 1.20 && rel < 1.80` |
| P4 | `ISE = 0` (Gummel-Poon low-current term removed; hFE 106 → 416) | **RED** | `attackMs > 3.0 && attackMs < 9.0` — attack 5.00 → 2.00 ms |
| P5 | `Rtrim` 2 kΩ → 2 Ω (the OTA input attenuator shorted) | **RED** | `pinSpread < 1.5` — the threshold stops being knob-independent |
| restore | — | GREEN | — |

P4 is worth a note: the first version of the suite **stayed green** under it,
because the wide 0.5–30 ms attack window accepted both 5 ms and 2 ms. Rather than
narrowing a bar around the perturbation, the fix was to add a bar against an
**absolute reference** — the published ~5 ms figure, with a 3–9 ms window that
reflects how weakly sourced that figure is (§59.1). The model measures 5.00 ms at
SUSTAIN 0.80, dead centre, and P4 then fails at 2.00 ms.

### 59.10 Integration and goldens

One param shape, additive registries: `rig.ts` `PedalType` + gear tray +
`COMP_KNOB_DEFAULTS` (SUSTAIN 0.5, LEVEL 0.4 — **0.4 measures unity**: a 0.15 V
peak 220 Hz note comes out at 0.1565 V, +0.37 dB); worklet `comp` dispatch behind
the `_comp` C-ABI prefix; `Pedal.tsx` FACES entry; `tokens.css` `--accent-comp`;
native `PEDAL_COMP = 6` (appended, never inserted — those integers are the packed
snapshot encoding, and the 4-bit slots §27's board widening bought are exactly the
headroom this seventh type needed), three APVTS parameters, a `PedalCard` face and
an `identical_core_test` case (`Squash -> Screamer -> Twin`). Assistant:
`add_pedal` gains `'comp'`, and the coach gets a real block — including that
SUSTAIN is not a threshold, that the pedal belongs FIRST in the chain (a
compressor after a distortion has nothing left to do, since distortion is already
a compressor, and will mostly raise the noise floor between notes), and the four
honest expectations stated as facts rather than apologies.

**Visual identity (doctrine §17).** Dark chassis both themes, **TEAL** accent, the
'stack' anatomy with exactly **TWO** knobs over a round stomp — two knobs where
every dirt box has three is the morphology cue. The real pedal is red; red is the
RAT's accent here and two red boxes on one board is a usability bug, so the colour
is chosen for distinguishability and the identity is carried by the face. Wordmark
"Squash", model line `DYNAMICS Nº7 · SQUASH`. No trademarks.

**Goldens: all five UNCHANGED at ±0.00 dB, and nothing was blessed.** The
compressor is in no golden rig, and it touches no other model.

## 60. M13.4 — the "Echoman" BBD analog delay (the lineup's FIRST DELAY)

The first pedal in the lineup that is not dirt, dynamics, modulation or filter:
an EHX Deluxe Memory Man-class **bucket-brigade analog delay**, shipping as pedal
type `delay`. Three knobs, because the three that matter on the real box are
DELAY, FEEDBACK and BLEND. Files: the reusable primitive
`core/include/clipper/dsp/DelayLine.h`; the model
`core/include/clipper/dsp/DelayModel.h` + `core/src/dsp/DelayModel.cpp`; tests
`core/tests/test_delay_model.cpp` (`clipper_delay_tests`); C ABI `delay_*`;
`--pedal delay` in the render CLI; an `echoman` row in `clipper-bench`; worklet
`delay` dispatch; `rig.ts` / `Pedal.tsx` / `pedal.css` / assistant; native
`ClipperEngine` + APVTS + `PedalCard` + an `identical_core_test` board case.
Trademark-safe throughout (no Electro-Harmonix / Memory Man / Deluxe text on any
user surface; the wordmark is "Echoman").

**It is a new DSP FAMILY, and the deliberate deliverable is the primitive.**
`DelayLine` is a plain interpolating ring buffer that knows nothing about bucket
brigades, feedback, companders or knobs; `DelayModel` is the BBD behaviour built
on top of it. M13.6's flanger and any future tape echo are cheap only if that
separation is right, so the primitive carries its own bars in this suite (§60.7)
rather than being an untested header that happens to work here. **ADR 022** records
that split, and the oversampling derivation below, as decisions rather than habits.

### 60.1 Research — what was reachable, and what was not

**Reachable, and used.**

* **The MN3005's electrical numbers**, via the Panasonic / Xvive datasheet through
  search-result extracts: **4096 stages**, an adjustable delay of **20.48 ms to
  204.8 ms**, a **10 kHz** clock giving 205 ms at a **75 dB** S/N, P-channel
  low-noise silicon gate process. Those endpoints are what fix the device law
  (§60.2) — and they fix it *independently*, because the law has to reproduce both
  of them from the stage count alone, which it does exactly.
* **The Deluxe Memory Man's published delay travel, 30 ms to 550 ms**, from EHX's
  own product pages (consistent across several retail listings). Also sourced: the
  DMM uses **two MN3005s** in series (the pair is what gives the 550 ms), an
  **NE570/NE571 compander** placed before and after the BBDs (Rev C carries the
  NE571, Rev D the NE570), and a **CD4047** as the clock oscillator. Its controls
  are Level, Delay, Blend, Feedback plus the chorus/vibrato section.
* **The NE570's compander law**, via the ON Semi / Philips datasheet and AN174
  through search extracts: each half has a **full-wave rectifier** whose output is
  averaged by an external `C_RECT` against an internal resistor, mirrored ×2 to
  become the gain-control current `I_G`; the gain cell in the op-amp's feedback
  path gives a **2:1 compressor** and in the forward path a **1:2 expander**;
  `I_B = 140 µA`; the rectifier input resistor is the internal **10 kΩ**
  (`I = 3 V/R1 = 3 V/10 k`), and the application notes' worked examples use
  `C_RECT` of 1–2 µF.
* **BBD charge transfer efficiency**, from the device literature through search
  extracts: *"transfer efficiencies of 0.9998 have been obtained at 5 MHz sampling
  rates while at lower rates an efficiency of 0.99995 is not uncommon."* Audio
  BBDs run at 10–200 kHz, i.e. squarely in the "lower rates" band.
* **A REAL, INDEPENDENT BBD IMPLEMENTATION, read in full.** `chowdsp_utils` clones
  from GitHub and carries `chowdsp::BBD` — Holters & Parker's DAFx-18 model
  (`BBDDelayLine.h`, `BBDFilterBank.h`) — including the **pole/root sets and the
  cutoffs of the JUNO-60 chorus's BBD anti-aliasing / reconstruction filter pair,
  9900 Hz in and 9500 Hz out**. That is the only real BBD filter design point
  reachable from here, and it is where this model's fixed cutoffs come from
  (§60.3, and see the gap below — they are the JUNO's, not the Memory Man's).
* A **secondary** BBD model, `ajmwagar/pedalkernel`'s `bbd.rs`, also cloned. It is
  useful only as a cross-check on the delay formula (it uses the same
  `N/(2·f_clk)`), and this slice **disagrees with it on one substantive point**:
  it delays the compressor's envelope and feeds the delayed envelope to the
  expander. A real NE570 has one rectifier per half, each looking at its own
  input, so no envelope is delayed anywhere — and that disagreement is the whole
  mechanism behind the breathing (§60.4). The netlist-level argument wins.

**Not reachable, recorded as gaps.** The proxy here permits **github.com only**;
every other host returns 403 at the CONNECT stage, so `WebFetch` failed uniformly
and `WebSearch` result extracts were the only other channel.

* **The Deluxe Memory Man's SCHEMATIC.** `ehx.com`'s own schematic thread,
  `electronicservicemanuals.com`, `freestompboxes.org`, `music-electronics-forum`
  and the Google-Sites teardown all 403. **This is the biggest gap and it has a
  specific consequence: the anti-alias and reconstruction filters' topology,
  order and cutoffs are NOT the DMM's.** They are the JUNO-60's, borrowed from an
  independent published BBD design, and stated as such rather than invented.
  Also unsourced for the same reason: the CD4047's actual timing network, the
  compander's `C_RECT` value in this pedal specifically, the BBD bias trimmer's
  operating point, and the feedback pot's taper.
* **The MN3005 datasheet PDF itself** (xvive.com, alldatasheet, jotrin, tubedepot)
  — 403 everywhere; its numbers above are from search extracts, not the document.
* **The NE570 datasheet and AN174 PDFs** (onsemi, aionfx, experimentalistsanonymous,
  digikey) — 403 everywhere; same caveat.
* **Raffel & Smith, DAFx-10 "Practical Modeling of Bucket-Brigade Device Circuits"
  and Holters & Parker, DAFx-18 "A Combined Model for a Bucket Brigade Device and
  its Input and Output Filters."** `dafx.de`, `dafx10.iem.at`, `academia.edu`,
  `researchgate.net` and `ccrma.stanford.edu` all 403. The papers were NOT read.
  What *was* read is a complete, working implementation of the second one
  (chowdsp_utils), which is arguably the better source anyway — but the
  derivations behind it are unseen here, and this model does not claim to be
  either paper's method.
* **A published frequency response or repeat-degradation measurement for a real
  DMM.** None found. One qualitative figure surfaced in an extract — the delay
  path "attenuates the high end significantly, from 55 Hz to 5.5 kHz, with a small
  bass cut and strong mid boost" — and it is recorded here as a sanity anchor, not
  used as a fitting target. **Every curve in §60.4 and §60.5 is this model's own.**
* **The MN3005's charge transfer efficiency specifically.** Not published anywhere
  reachable. `kCte = 0.9999` is the commonly quoted mid-range figure and sits
  inside the sourced 0.9998…0.99995 band; it is a **stated modelling parameter**,
  and §60.4 reports its consequence rather than tuning it toward a sound.

### 60.2 The device, and the law the DELAY knob obeys

An MN3005 is a **4096-stage** BBD. A charge packet advances one stage per clock
HALF-cycle, so an N-stage device holds N/2 **sample slots** clocked at `f_clk` and
its delay is

```
    t_delay = N / (2 · f_clk)
```

That is not asserted, it is **checked against the datasheet's own endpoints**:
`4096/(2·10 kHz) = 204.8 ms` and `4096/(2·100 kHz) = 20.48 ms`. Both exactly.

The DMM puts **two** of them in series: `kStages = 8192`, `kSlots = 4096`, and the
delay is `4096 / f_clk`. The DELAY pot varies the CD4047 astable — the **CLOCK** —
and nothing else in the delay path moves. Since the CD4047's frequency is
`1/(4.4·R·C)`, `f_clk ∝ 1/R` and the delay TIME is directly proportional to the
timing resistance, so a linear pot in series with a minimum resistor gives a delay
**linear in rotation**. That is what ships; it is a consequence of the oscillator,
not a taste choice.

**The clock runs off BOTH ends of the MN3005's spec sheet, and that is reported
rather than clamped away.** EHX's published 30–550 ms with 4096 slots means

| DELAY knob | delay (ms) | f_clk (Hz) | MN3005 rating |
| --- | --- | --- | --- |
| 0.000 | 30.0 | **136 533** | 36 % above the rated 100 kHz max |
| 0.125 | 95.0 | 43 116 | in spec |
| 0.250 | 160.0 | 25 600 | in spec |
| 0.375 | 225.0 | 18 204 | in spec |
| 0.500 | 290.0 | 14 124 | in spec |
| 0.625 | 355.0 | 11 538 | in spec |
| 0.750 | 420.0 | 9 752 | 2 % below the rated 10 kHz min |
| 0.875 | 485.0 | 8 445 | 16 % below |
| 1.000 | 550.0 | **7 447** | 26 % below |

The published TIME range is the stronger source, so it wins. A real DMM genuinely
runs its BBDs past both ends, which is a large part of why its long settings sound
the way they do.

### 60.3 The signal path, and where each number comes from

```
  in ─[input HP 60 Hz]─┬─→ COMPRESSOR (NE570, 2:1)
                       ↑            │
                  FEEDBACK          ↓
                       ↑    [anti-alias LPF, FIXED 9.9 kHz, 4-pole Butterworth]
                       │            │
                       │    [BBD compliance limit, tanh at 0.45 V]
                       │            │
                       │    [S/H at f_clk] → [Poisson charge smear] → [delay
                       │     = 4096/f_clk seconds, cubic fractional read]
                       │            │
                       │    [reconstruction LPF, FIXED 9.5 kHz, 4-pole]
                       │            │
                       └──── EXPANDER (NE570, 1:2) ─[output HP 60 Hz]─→ wet

  out = dry + BLEND · wet          (the dry stays at BASE rate and is untouched)
```

**1. The BBD is modelled as the SAMPLED DEVICE it is.** A clock-phase accumulator
runs at `f_clk`; on each tick the anti-aliased input is **held**, and the held
staircase is what enters the line. The read distance is the device's own law
expressed in internal samples, so it is exact, fractional, and it **glides** when
the clock does — which is why sweeping DELAY bends pitch exactly as a real BBD (or
a tape machine) does. The delay-line storage is `DelayLine`; the clock, the hold
and the smear are the BBD.

**2. The charge smear is the darkening, and it is DERIVED from the device.** A BBD
stage does not transfer all of its charge: a fraction ε is left behind and mixes
into the following packet. One stage is `y[n] = (1−ε)x[n] + ε y[n−1]`, and N of
them cascade to `[(1−ε)/(1−ε z⁻¹)]^N`. For small ε,

```
    log H = N[log(1−ε) − log(1−ε z⁻¹)] ≈ −N·ε·(1 − z⁻¹)
    ⇒ H(z) ≈ exp(−μ(1 − z⁻¹)),   a POISSON kernel h[k] = e^-μ μ^k / k!
```

with μ = N·ε in stage-transfer steps, i.e. **μ = N·ε/2 = 0.4096 sample slots**.
It is applied **once per slot, at the clock**, as an 8-tap FIR (the truncation is
1e-8 at this μ, renormalised so DC gain is exactly 1). Its −3 dB corner is
therefore a fixed FRACTION of `f_clk` — solve `exp(−2μ(1−cos ω)) = ½` for
ω = 1.4166 rad, i.e.

```
    f_-3dB = 0.2254 · f_clk
```

**and that is the whole darkening mechanism**: the filters are fixed, the clock
moves, so the corner falls in Hz as the delay lengthens. Measured against the
clock table above: **30.8 kHz at 30 ms → 1.68 kHz at 550 ms**.

**3. The compander is the NE570's actual law.** The gain cell sits in the op-amp's
feedback path for the compressor and in the forward path for the expander, which
makes the pair a square-root / square:

```
    compressor   G_c = sqrt(V_ref / env_in)        ⇒ env_out = sqrt(V_ref · env_in)   (2:1)
    expander     G_e = env_bbd / V_ref             ⇒ env_out = env_bbd² / V_ref       (1:2)
```

Composed with the same envelope those are **exactly unity at every level** — the
algebra is the datasheet's claim and §60.7 checks it against the rendered audio.
Each half has its own full-wave rectifier averaged with `τ = R·C = 10 kΩ × 1 µF =
10 ms`. **The two rectifiers see different signals** — the expander's is the
delayed, band-limited, charge-smeared one — so the envelopes cannot agree, and
that disagreement *is* the breathing. Nothing delays an envelope anywhere;
neither does the real circuit.

`kCompRefVolts = 0.15 V` is a **staging constant, declared as such**: the pair is
unity at every level by construction, so this only sets where the BBD's nominal
drive lands. It is a normal single-coil pluck peak, which puts the device at about
42 % of its compliance at that level and leaves the pick attack room.

**4. What bounds the feedback loop is the DEVICE, not a limiter.** The bucket has
finite capacity; past it the packet clips. `kBbdClipVolts = 0.45 V` is that
compliance, applied as a smooth odd `tanh` (odd, so it adds no DC). Removing it
takes the 30 s peak at FEEDBACK max from **1.14 to 8.04** — see P4 in §60.8.

**5. Fixed filters, borrowed cutoffs, and that is a named gap.** 4th-order
Butterworth (two cascaded biquads) at **9900 Hz** in and **9500 Hz** out — the
JUNO-60's figures out of the Holters-Parker implementation (§60.1). They are a
real BBD design point from a real instrument, they are NOT the Memory Man's, and
nothing here was tuned to them.

**Numerics (docs §56 / §56.4b, applied at the first opportunity rather than after
the fact).** These filters sit inside a FEEDBACK loop at up to 768 kHz, so their
state is `double`, not `float`; and a TDF2 biquad is a recursion of order 2, where
the house one-liner `z = flushDenormal(z)` **cannot converge** — so each section
tests BOTH taps and zeroes them as a unit.

### 60.4 What the shipped pedal measures

**Delay time vs knob**, rendered (impulse → first echo peak), against the device
law. Every row's `rendered − law` is the oversampler's own group delay and
nothing else:

| knob | device law (ms) | rendered (ms) | f_clk (Hz) | smear −3 dB (Hz) |
| --- | --- | --- | --- | --- |
| 0.000 | 30.00 | 31.750 | 136 533 | 30 776 |
| 0.125 | 95.00 | 96.688 | 43 116 | 9 719 |
| 0.250 | 160.00 | 161.688 | 25 600 | 5 771 |
| 0.375 | 225.00 | 226.708 | 18 204 | 4 104 |
| 0.500 | 290.00 | 291.667 | 14 124 | 3 184 |
| 0.625 | 355.00 | 356.688 | 11 538 | 2 601 |
| 0.750 | 420.00 | 421.729 | 9 752 | 2 198 |
| 0.875 | 485.00 | 486.771 | 8 445 | 1 904 |
| 1.000 | 550.00 | 551.792 | 7 447 | 1 679 |

The constant **~1.75 ms offset is the 8× oversampler's 76-sample group delay**,
and it is reported rather than compensated: the dry path never enters the
oversampled domain (that is what keeps BLEND 0 bit-identical), so the offset lands
on the wet path only. **The repeat SPACING carries no offset at all** — measured
gaps at knob 0.0 / 0.5 / 1.0 are 30.67 / 30.04 / 30.67 ms, 290.10 / 290.71 /
290.12 ms and 550.69 / 550.12 / 550.15 ms against laws of 30 / 290 / 550, the
residual being the peak-picker's own resolution on a 10 ms burst.

**REPEAT DEGRADATION — the bucket-brigade property.** A plucked burst (220 Hz +
1.5 kHz + 4 kHz partials), DELAY 0.5, FEEDBACK 0.7:

| repeat | rms | LF 100–400 Hz | HF 2–6 kHz | HF/LF (dB) |
| --- | --- | --- | --- | --- |
| 1 | 0.114705 | 0.040375 | 0.001291 | **−29.91** |
| 2 | 0.078984 | 0.028832 | 0.000688 | **−32.45** |
| 3 | 0.058732 | 0.021945 | 0.000380 | **−35.23** |
| 4 | 0.046020 | 0.017444 | 0.000234 | **−37.43** |
| 5 | 0.037122 | 0.014207 | 0.000145 | **−39.83** |

**9.92 dB of HF lost from repeat 1 to repeat 5, monotone, ~2.5 dB per pass.** That
is what "cumulative" means: the feedback returns to the COMPRESSOR'S INPUT, so
every repeat goes through the compander, both fixed filters, the sample-and-hold
and the charge smear again. A clean line with one filter in the loop converges
somewhere else — P1 and P2 in §60.8 measure exactly how much else.

**DARKENING WITH TIME.** The same burst, repeat 1 only, FEEDBACK 0, so the only
thing that changes across the table is the CLOCK:

| knob | delay (ms) | LF | HF | HF/LF (dB) | smear −3 dB (Hz) |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 30.0 | 0.043321 | 0.002463 | **−24.90** | 30 776 |
| 0.25 | 160.0 | 0.041863 | 0.001964 | **−26.57** | 5 771 |
| 0.50 | 290.0 | 0.040376 | 0.001390 | **−29.26** | 3 184 |
| 0.75 | 420.0 | 0.039551 | 0.001045 | **−31.56** | 2 198 |
| 1.00 | 550.0 | 0.038938 | 0.000786 | **−33.90** | 1 679 |

**8.99 dB darker at 550 ms than at 30 ms, on the FIRST repeat.** A fixed filter on
a variable-length digital line measures 0.00 dB here, by construction.

**THE COMPANDER CURVE**, 220 Hz, DELAY 0.3, FEEDBACK 0, wet measured with the dry
subtracted:

| in peak (V) | G_comp | G_exp | product | wet f0 | wet gain (dB) |
| --- | --- | --- | --- | --- | --- |
| 0.005 | 6.794 | 0.148 | 1.0019 | 0.004951 | **−0.09** |
| 0.015 | 3.923 | 0.255 | 0.9994 | 0.014773 | **−0.13** |
| 0.050 | 2.149 | 0.462 | 0.9924 | 0.048322 | **−0.30** |
| 0.150 | 1.240 | 0.781 | 0.9681 | 0.137580 | **−0.75** |
| 0.300 | 0.877 | 1.066 | 0.9347 | 0.255495 | **−1.39** |
| 0.600 | 0.620 | 1.414 | 0.8768 | 0.446452 | **−2.57** |

The compressor's gain FALLS and the expander's RISES, and their product is
**unity to 0.2 % over the bottom 30 dB** — the NE570 pair is an identity that the
BBD sits inside, not a compressor with a make-up gain bolted on. At a quiet input
the compressor really is working: at 0.005 V it puts **+16.64 dB** into the device
and the expander takes **−16.61 dB** back, which is 16 dB of BBD noise floor
pushed down — the reason the compander exists. The departure at the top (−2.57 dB
at 0.6 V) is the **bucket's own compliance**, not a limiter.

**FEEDBACK.** At FEEDBACK 0 there is **exactly one repeat**: echo 1 measures
6.14e-03 and echo 2 measures **1.70e-22 (−391 dB)** — what sits in the echo-2
window is the tail of the input filters' own ring-down arriving one delay later,
not a second pass. At FEEDBACK **max**, after a 0.6-peak strum (input peak 1.06),
over 30 s:

| DELAY | peak over 30 s | 15–22.5 s | 22.5–30 s |
| --- | --- | --- | --- |
| 0.0 (30 ms) | 1.135 | 0.000 | 0.000 |
| 0.5 (290 ms) | 0.920 | 0.291 | 0.294 |
| 1.0 (550 ms) | 0.920 | 0.349 | 0.349 |

Bounded, finite, and **converging rather than growing**. At long settings it
settles into a self-oscillating swirl at about a third of full scale — which a
real Memory Man does, and which the bucket's compliance is what caps. At the
short setting the loop passes 33 times a second, so the accumulated loss per
second wins and the repeats die out entirely. A 60 s run confirms the asymptote:
0.229 → 0.291 → 0.294 → 0.300 → 0.304 → 0.308 → 0.310 → 0.3116 over 5 s windows,
increments shrinking to 1e-4.

### 60.5 Oversampling is set by the DEVICE, and it is 8×, not the house 4×

The BBD's clock reaches **136.53 kHz** at the 30 ms end, so the internal rate has
to clear **2 × 136.53 = 273.07 kHz** or the DEVICE'S OWN clock images fold inside
the oversampled domain before the reconstruction filter can reach them. 4× at
44.1 kHz is 176.4 kHz and does **not** clear it; 8× is 352.8 kHz and does.

Measured, at DELAY 0.0 on a 3 kHz tone, worst non-harmonic product in the wet
signal (Hann-windowed, see the trap in §60.9):

| factor | internal rate (48 kHz base) | worst non-harmonic |
| --- | --- | --- |
| 1× | 48 kHz | −70.94 dB |
| 2× | 96 kHz | −66.08 dB |
| 4× | 192 kHz | −65.49 dB |
| **8×** | **384 kHz** | **−122.08 dB** |

**56.58 dB in one step**, and the step is exactly where the derivation says it
should be. CPU for it: **2.9 % → 5.4 %** of one 48 kHz stream. Latency does not
move at all, because the dry path never enters the oversampled domain. **ADR 022**
records this as a device-derived factor, NOT a licence to raise the house 4×
elsewhere: without a clock to clear, 1× → 4× moves only 5.5 dB here.

At the OTHER end of the travel the device aliases and **that is correct**: a
9 kHz tone into a 7.45 kHz clock (Nyquist 3.7 kHz) folds hard, and no amount of
host oversampling can or should remove it — a real DMM at 550 ms does the same
thing. Measured worst in-band product: **−9.66 dB at 1×, −16.23 dB at 4×**;
oversampling buys 6.6 dB of *implementation* accuracy there and nothing more.

### 60.6 Hygiene, cost, and the numbers the conventions require

| property | measured |
| --- | --- |
| alias floor, 3 kHz 0.3 V, DELAY 0.0, shipped 8× | **−122.1 dB** (4× −65.5, 1× −70.9) |
| DC offset ON SIGNAL, wet path (220 Hz 0.2 V, and with +0.1 V input offset) | **0.0000 %** of peak both cases (mean −9.4e-09) |
| block-size invariance (128 vs ragged 100/37/256/1/411) | **exactly 0.0** |
| rate independence (settled wet level, 44.1 / 48 / 88.2 / 96 kHz) | spread **0.0001 dB**; delay 238.000 ms at every rate |
| latency | **0 samples** — by design (see §60.4) |
| determinism (two fresh models) | worst |Δ| **0.000e+00** |
| `reset()` after one NaN | 1/256 samples poisoned → **0/96000** after reset; settled level within **0.016 dB** of a fresh model |
| denormals, `maxAbsRestingState()` after 40 s of silence | **exactly 0.0**; output exactly 0.0f |
| DELAY sweep, broadband HF floor 3–20 kHz | swept **−81.17 dB** re f0 vs static −86.37 (5.2 dB) |
| CPU, 8× at 48 kHz | signal **18.6× realtime / 5.4 %** of a stream; silence 23.2× / 4.3 % |

**Denormal scope (ADR 006), decided by measurement.** *Everything* recursive in
this model rests at exactly zero, the 550 ms delay LINE included — which is the
case the rule cares most about here, because a decaying feedback tail circulates
`float` subnormals through a quarter of a million samples of ring buffer, that is
invisible in the audio, and WASM has no flush-to-zero at all. `DelayLine::write`
therefore flushes; the ring has no recursion of its own, so the per-value contract
of `flushDenormal` is sufficient *there* and §56.4b's whole-state form is not
needed. It IS needed for the four Butterworth pairs, which are order-2 recursions
and zero both taps as a unit. The compander envelopes, the held sample and the
tick history all rest at zero and are guarded. There is no nonzero-resting state
anywhere in this pedal — no operating point, no rail — so unlike §59 there is
nothing here that should be left unguarded.

**`reset()` does NOT reproduce a fresh model bit-for-bit, and that is deliberate.**
It re-zeroes the BBD clock PHASE, which is a physical quantity with no canonical
value (a real device's clock has whatever phase it has). Two models differing only
in clock phase sample the input at different instants within one clock period, so
their echoes differ by a sub-clock-period timing — measured at ~2.4e-03 on a
0.15-peak echo. Asserting bit-identity there would be asserting an arbitrary
convention, so the bar is that the reset model recovers the same **steady-state
level**: 0.016 dB.

### 60.7 The `DelayLine` primitive, measured on its own

| property | measured |
| --- | --- |
| integer read (D = 50), vs the source samples | **0 mismatches of 350** — exact, no interpolation error at all |
| fractional read, |H| at 1 kHz, D = 100.00 / .25 / .50 / .75 | −0.0054 / −0.0046 / −0.0037 / −0.0028 dB |
| measured delay step per quarter-sample of requested delay | 0.2503 / 0.2502 / 0.2502 samples |
| 8 kHz droop at D = 100.5, **cubic** | **−0.2258 dB** |
| 8 kHz droop at D = 100.5, **linear** | **−1.2489 dB** |
| ring rest after silence | **exactly 0.0** |
| out-of-range reads (10 000 × capacity, −5, NaN) | all clamped, all finite |

The cubic-vs-linear gap of **1.02 dB at 8 kHz** is why `readCubic` is the default
read and why `DelayModel` uses it: a delay line whose read position sweeps under a
knob would otherwise lose an audible dB of top every time the knob moved, on top
of everything the device already takes. 4-point Lagrange is also **stateless**, so
a sweep that reverses direction leaves no ringing where an allpass interpolator
would smear it — the same argument `ChorusModel` already records.

### 60.8 Perturbation proofs

Every load-bearing bar was proven by reverting the thing it names in a scratch
copy of the source, rebuilding (with `touch` after **both** patch and restore —
docs §29's trap), and confirming the suite goes red. `git stash` was NOT used and
neither was `git checkout --`: CLAUDE.md records both destroying work in this
repo.

| # | perturbation | result | first assertion to fail | measured |
| --- | --- | --- | --- | --- |
| baseline | — | GREEN | — | — |
| P1 | `kCte` 0.9999 → 1.0 (charge transfer made perfect) | **RED** | `ratio < prevRatio - 1.0` | repeat 1 −26.11 dB, repeat 2 −26.13 — degradation collapses from 2.54 dB/pass to **0.02** |
| P2 | the sample-and-hold removed (`pre` written instead of `held`) | **RED** | `ratio < prevRatio - 1.0` | repeat 1 −24.84, repeat 2 −24.67 — the device stops being sampled and the degradation **reverses** |
| P3 | the compander bypassed (both gains pinned to 1) | **RED** | `gc * ge < prevProduct` | the product stops moving with level at all |
| P4 | the bucket compliance removed (`bucketLimit` returns `v`) | **RED** | `pk < 2.0` | 30 s peak at FEEDBACK max **1.14 → 8.04** — the loop runs away |
| P5 | BLEND gains a 0.001 wet floor | **RED** | `diff == 0` | **34 000 of 48 000** samples differ; hash `6276ea…` → `6a3a97…` |
| P6 | `kStages` 8192 → 4096 (one MN3005 instead of two) | **RED** | `DelayModel::bbdSlots() == 4096` | the clock travel doubles and the published 30–550 ms travel is unreachable |
| restore | — | GREEN | — | — |

**6/6 RED, restore GREEN.**

### 60.9 Two traps this slice found

**1. A rectangular-window Goertzel cannot measure an alias floor.** The first
version of the alias test reported a floor of **−55.8 dB that did not move with
the oversampling factor** and sat 409 Hz from the fundamental. That reads like a
DSP result and is not one: a rectangular window's leakage falls as 1/Δbin, so a
3 kHz tone read at 3409 Hz with 2 Hz bins leaks `1/(π·204) = −56 dB` — the number
measured, to the decimal. A Hann window's sidelobes fall as 1/Δbin³ and put that
leakage ~90 dB lower, and the real floor then appeared (−65.5 dB at 4×) **along
with the 56 dB step at 8× that the whole oversampling derivation turns on**. The
tell is §54's and it held: a floor that does not move with the factor is either
not aliasing or not being measured.

**2. "Exactly one repeat" is not "exactly zero".** At FEEDBACK 0 the echo-2 window
is not empty — it holds 1.70e-22, which is the input high-pass's own ring-down
arriving one delay later through a line that is fed continuously. The player-
observable statement is 100 dB down, not bit-zero, and writing the assertion the
strict way would have been asserting an accident of the filter's decay rate.

### 60.10 Integration and goldens

One param shape, additive registries: `rig.ts` `PedalType` + gear tray +
`DELAY_KNOB_DEFAULTS` (DELAY 0.35 = 212 ms, FEEDBACK 0.30, BLEND 0.35 — the
repeats behind the note, which is how a Memory Man is normally used); worklet
`delay` dispatch behind the `_delay` C-ABI prefix; `Pedal.tsx` FACES entry with a
new `bank` layout; `tokens.css` `--accent-delay` (already reserved); native
`PEDAL_DELAY = 8` (**pre-reserved** by the slot-reservation commit, so no slice had
to guess "next free" — the integers ARE the packed-snapshot encoding), four APVTS
parameters, a `PedalCard` face and an `identical_core_test` board case
(`RAT → Echoman → JCM800`, the first case carrying a long recursive state and a
feedback loop through the plugin's chunking). Assistant: `add_pedal` gains
`'delay'`, `PEDAL_PARAM` gains delay/time/feedback/repeats/blend/mix, and the
coach gets a real block — including that the DELAY knob is the clock so long
settings are darker *by design*, that the pedal belongs LAST in the chain, that
FEEDBACK max is a usable bounded swirl rather than a fault, and that sweeping
DELAY while the repeats ring bends their pitch like a tape machine.

**Visual identity (doctrine §17).** Dark chassis both themes, **DEEP BLUE** accent
— the furthest hue on the board from every dirt/mod/filter box. Web face is a new
`bank` layout: the widest chassis in the app (380 px) with the three knobs in a
single row, because a Memory Man is not a small box and that silhouette is the
morphology cue. Wordmark "Echoman", model line `DELAY Nº8 · BUCKET BRIGADE`. The
native card uses the shared Stack layout (native lays cards on a fixed rail, so
width is not a card-level property there) and carries the identity in the accent.
No trademarks anywhere.

**Goldens: all five UNCHANGED at ±0.00 dB, and nothing was blessed.** The delay is
in no golden rig and touches no other model.

## 61. M13.6a — the "Curfew" noise gate (the first UTILITY, and the second consumer of M13.1's detector)

The lineup's first pedal that is not a voice at all. It makes no sound; it takes
one away. Pedal type `gate`, slot **9** (reserved up front by the
slot-reservation commit — see `PEDAL_GATE`), accent **slate**, wordmark
"Curfew", model line `UTILITY Nº8 · GATE`. Files: shared
`core/include/clipper/dsp/SidechainDetector.h` (NEW — the extracted detector);
`core/include/clipper/dsp/GateModel.h` + `core/src/dsp/GateModel.cpp`; tests
`core/tests/test_gate_model.cpp` (`clipper_gate_tests`); C ABI `gate_*`;
`--pedal gate` in the render CLI; a `curfew` row in `clipper-bench`; worklet
`gate` dispatch; `rig.ts` / `Pedal.tsx` / `pedal.css` / `Board.tsx` / assistant;
native `ClipperEngine` + APVTS + `PedalCard` + a sixth `identical_core_test`
board. Trademark-safe throughout (no Boss / NS-2 / ISP / Decimator text on any
user surface).

### 61.1 Research — what was reachable, and what was not

**The reference is a Boss NS-2-class noise suppressor**, and this is the §57
situation rather than the §59 one: **NO SCHEMATIC WAS EVER READ.**

**Reachable, and used** (search-result extracts only — `WebFetch` returned 403
for every host tried, *including Wikipedia*, exactly as §57 and §59 record):

* **Boss's own product copy and the NS-2 owner's manual**: the pedal is built
  around "a high-quality VCA and high-speed envelope-detecting circuits", and
  "the expander starts its function when the volume of the instrument becomes
  lower than the threshold level". So it is a downward expander with a VCA and a
  fast detector — the topology, from the manufacturer.
* **The control complement**: THRESHOLD (how sensitive), DECAY (how fast the
  sound fades once the suppression engages), and a MODE switch (Reduction /
  Mute). Manual, plus Roland's own support article on the two modes.
* **One forum circuit description** (via extract): the input buffer Q1 feeds a
  gain stage around IC2a "where signal dynamics are sensed and transformed into a
  control signal via various stages with diodes", which then reaches pin 8 of an
  **M5207 dual VCA**. That is the only component-level statement obtained, and it
  matters: it says the sidechain is a **gain stage feeding a diode rectifier**,
  which is exactly the shape of M13.1's detector.
* **The gate literature, for the two things the reference does not document.**
  Hysteresis: "noise gates often implement hysteresis, that is, they have two
  thresholds: one to open the gate and another, set a few dB below, to close the
  gate", with **−6 dB the recommended starting point**. Hold: designers "have
  probably built a fixed hold time into the system, **usually about 20 to
  30 ms**", so a gate does not chase individual cycles of a low note under a fast
  attack. Guitar attack **0.1–1 ms**, release **~300 ms** as typical starting
  points.

**Not reachable, recorded as gaps.** Every one of these 403'd or is paywalled:
`freestompboxes.org` (the schematic thread), `ronsound.com` (sells the sheet),
`elecsprout.com`, `schematicsonline.com`, `eserviceinfo.com`, and the
`static.roland.com` manual PDF itself. A GitHub code/repo search for an NS-2
netlist or `.asc` found nothing — so unlike §59 there was no LTspice
transcription to parse.

Consequences, stated rather than buried:

* **Every component value in `GateModel.cpp` is a documented reconstruction**,
  chosen under stated constraints, not transcribed. §57's rule applies verbatim:
  *do not re-tune any of them toward a sound; find the schematic.*
* **The threshold range in dBV, the decay range in seconds and the VCA's
  off-isolation are all this model's own.** There is no published figure for any
  of them.
* **Whether the real NS-2 implements hysteresis at all could not be established.**
  It is in this model because the gate literature is unanimous that a single
  threshold chatters, and because this slice measures the chatter (§61.5). Its
  −6 dB nominal is that literature's own number, not a fit.
* The **device cards are NOT reconstructed**: the 2N3904 and 1N4148 come from
  `SidechainDetector.h`, i.e. they are the same published SPICE cards M13.1 uses.

### 61.2 THE SEAM — what ADR 019 got right, what it got wrong, and what was NOT done

ADR 019 built `CompressorEngine` config-parameterized from its first line and
named the noise gate as a consumer: *"a gate is this same `Sidechain` feeding a
different `ControlMap`"*. It also named the risk in terms — a seam written before
its second consumer exists can be the wrong seam, and the answer to that is *"a
finding to report and the header to correct, not a reason to quietly widen the
engine until both fit"*. This slice is that test. **The verdict: the CLAIM held;
the SEAM AS WRITTEN did not, and it was corrected rather than widened.**

**Right — the detector really is the shared part.** The gate uses the same
full-wave clamp/rectifier and the same envelope integrator, with **nothing
changed but component values**. No second envelope follower was written. The
diff, entirely in config:

| part | compressor (§59) | gate | why |
| --- | --- | --- | --- |
| clamp cap / resistor | 10 nF / 1 MΩ | 10 nF / 1 MΩ | the same DC restorer, 15.9 Hz — below the band, so it restores every note |
| envelope cap / resistor | 10 µF / 150 kΩ = **1.500 s** | 47 nF / 220 kΩ = **10.34 ms** | a compressor's release IS its envelope; a gate's audible release is the DECAY knob on the VCA ramp, so its detector only has to see a note stop |
| leg source impedances | 1.4 kΩ (emitter follower) / 10 kΩ (collector load) — deliberately ASYMMETRIC | 1 kΩ / 1 kΩ | the gate's rectifier is driven by an op-amp, so both legs see the same impedance and the full-wave rectifier is symmetric |
| supply / Vce,sat | 9 V / 0.1 V | 9 V / 0.1 V | same |

**Wrong (structure) — `Sidechain` was a struct with no code attached.** The
detector itself was `CompressorEngine::detectorLeg()` and `::advanceEnvelope()`,
both **private**, with their state (`clampP_`, `clampN_`, `vbP_`, `vbN_`,
`vEnv_`) in that class's members. There was nothing a second consumer could hold.
It is now the standalone **`SidechainDetector`**, which both pedals own by value.
The move is **bit-identical for the compressor** — two `clipper-render` renders
(a 2 s 80→6000 Hz sweep at SUSTAIN 0.70/LEVEL 0.50, and a 3 s 110 Hz pluck at
SUSTAIN 0.20/LEVEL 0.90) `cmp` byte-for-byte against the pre-move binary, and
`clipper_comp_tests` / `clipper_denormal_tests` / `clipper_nan_guard_tests` pass
unchanged.

**Wrong (`ControlMap`) — it is not a policy hook.** It is three resistor values
(`potOhms` / `seriesOhms` / `cellPinV`) that turn an envelope voltage into an
**OTA bias current**. A gate has no rheostat, no cell pin and no control current;
its control law is a Schmitt comparator plus an attack/hold/decay ramp into a
VCA. `GateModel` therefore carries its own `GateControl`, and **`ControlMap` was
not widened**. That is the ADR's instruction followed literally: an engine that
grows a field every time a voice disagrees has become a union of two models.

**Wrong (`detectorFromOutput`) — for a gate, feed-back is not a variant, it is
broken.** §59 proves by measurement that the Dyna Comp's detector tastes the gain
cell's OUTPUT, and that flipping it collapses the ratio 216:1 → 3.3:1. A gate
that tastes its own output **latches shut the first time it closes** — the
detector then sees only the VCA's off-isolation and can never see enough to
re-open. Measured, not argued (§61.7). The field stays OTA-only; the gate is
feed-forward by construction, with the flip kept as a test hook.

`CompressorEngine.h`'s seam banner now carries all four of those corrections, and
its rule for M13.3 is narrowed to: **reuse the DETECTOR, and expect the CONTROL
side to be your own.**

### 61.3 The knobs — TWO, and why the third was not shipped

The reference exposes THRESHOLD, DECAY and a MODE switch. The first two ship as
slots 0 and 2. **MODE deliberately does not ship**, and the reason is a finding
rather than a shortcut: per Roland's own support article, MODE does not change
the audio processing at all — it changes what the FOOTSWITCH does (in Reduction
the switch bypasses the suppression; in Mute the suppression is always on and the
switch becomes a mute). On this board the footswitch is **bypass on every pedal**,
so modelling MODE would either be inert (dead UI, forbidden) or would make one
pedal's footswitch mean something different from the other nine. Slot 1 is
carried and unused — the compressor's and the phaser's precedent.

### 61.4 THRESHOLD is a threshold — the table, and the contrast that matters

The THRESHOLD pot is a LOG (audio-taper) part attenuating the sidechain amp's
fixed ×400 gain over **40 dB**. Measured, shipped code, 220 Hz, 48 kHz — the
input peak at which a fresh gate ends up OPEN, bisected on the comparator's own
state:

| knob | opens at | the pedal's stated law | Δ |
| --- | --- | --- | --- |
| 0.00 | **−59.05 dBV** | −59.06 | +0.01 |
| 0.20 | −51.05 | −51.06 | +0.01 |
| 0.35 (default) | **−45.05** | −45.06 | +0.01 |
| 0.50 | −39.05 | −39.06 | +0.01 |
| 0.65 | −33.05 | −33.06 | +0.01 |
| 0.80 | −27.05 | −27.06 | +0.01 |
| 1.00 | **−19.05** | −19.06 | +0.01 |

Span **40.00 dB**, monotone, worst |measured − stated| **0.01 dB**. The SHAPE is
the property, not the offset: the detector sitting between the pot and the
comparator is **exponential**, so a dB-linear pot producing a dB-linear threshold
across 40 dB is a real prediction. (The one calibration constant in the file,
`kStatedTripVolts` = 0.4458 V — the sidechain drive at which the detector's mean
collector current reaches (V+ − vRef)/R_env = 8.30 µA — sets the offset only; it
is a conduction-angle integral with no closed form, and it is labelled as quoted
rather than derived.)

Two ABSOLUTE, player-observable windows at the default 0.35, asserted: a
realistic single-coil hiss floor (~−60 dBV) **stays shut**, and a normally played
note (0.15 V peak) **opens it**.

**THE CONTROL-LAW CONTRAST, which is the whole point of the pedal.** Across the
entire THRESHOLD travel the gain the pedal applies when it IS open:

| knob | 0.00 | 0.25 | 0.50 | 0.75 | 1.00 |
| --- | --- | --- | --- | --- | --- |
| gain when open | −0.0176 dB | −0.0176 | −0.0176 | −0.0176 | −0.0176 |

**40.00 dB of THRESHOLD travel against 0.0000 dB of GAIN travel.** §59.4 measured
the Dyna Comp's SUSTAIN doing the exact opposite: **25.33 dB of gain travel
against 0.28 dB of output travel**. That pair of numbers IS the difference
between a threshold and a sensitivity control, and both directions are asserted.
(The −0.0176 dB is the two 7.234 Hz coupling caps at 220 Hz — the gate is unity
when open, which is what makes it usable always-on.)

Threshold across sample rates (knob 0.35): 44.1 k **−45.05**, 48 k **−45.05**,
88.2 k **−45.06**, 96 k **−45.07** — spread **0.02 dB**.

### 61.5 Hysteresis and chatter — the two bars that carry this slice

**Where the hysteresis is taken is the design decision, and it was made by
measurement.** A Schmitt trigger is positive feedback; the obvious place is the
comparator's own input divider, i.e. a fixed VOLTAGE offset on the reference.
That does not work here, and the number says why: the detector's transfer is
exponential, so a fixed voltage offset at the comparator is worth **0.21 dB** of
input level. So the feedback path is the **sidechain attenuator** instead — while
the gate is open the sidechain sees 6 dB more level, which makes the gap a
defined number of dB independent of how steep the detector is.

Measured by ramping a 220 Hz tone from −70 dBV up over 8 s and back down over 8 s
(knob 0.35):

| | opens at | closes at | GAP |
| --- | --- | --- | --- |
| shipped (6 dB of sidechain feedback) | −44.98 dBV | −51.07 dBV | **6.09 dB** |
| the same code with the hysteresis set to 0 | −44.93 | −45.14 | **0.21 dB** |

**THE CHATTER TEST — the one that matters.** A decaying low E (82.41 Hz,
0.30 V peak, τ = 0.9 s, 6 s) crosses the threshold once on its way down. Counting
comparator EDGES over the whole render:

| THRESHOLD | shipped | hysteresis removed |
| --- | --- | --- |
| 0.35 | **1 open, 1 close** | 5 opens, 5 closes |
| 0.50 | **1 open, 1 close** | 6 opens, 6 closes |
| 0.65 | **1 open, 1 close** | 7 opens, 7 closes |

The mechanism is the detector's own envelope ripple at 2·f₀ — τ_env = 10.34 ms
against a 12.1 ms period — which is exactly why the stimulus is a LOW note.

### 61.6 The rest of the player-observable set

**The pick attack survives**, which is a gate's characteristic failure mode.
Measured against the SAME model with the gate held open (same code path, same
coupling caps, so the comparison isolates the decision):

| | gated | held open | lost |
| --- | --- | --- | --- |
| first 20 ms peak | 0.35866 | 0.35906 | **0.01 dB** |
| first 20 ms rms | 0.22008 | 0.22048 | **0.02 dB** |
| time to 90 % of the open peak | 3.44 ms | 3.44 ms | **0.00 ms** |

For contrast, §59.6 measured the compressor losing **7.72–20.00 dB** of the same
transient. A gate has no business losing any, and this one does not.

**Noise reduction**, on a real noise floor, gate closed: **80.01 dB** at a
−60 dBV floor and **80.01 dB** at −70 dBV. That number is the VCA's own
off-isolation (`kClosedGain` = 1e-4 = −80 dB — a reconstruction constant, since a
real gain cell does not reach zero), and the test asserts both that the reduction
clears 60 dB and that it does not exceed the cell's isolation.

**DECAY is the only time constant the player has.** Time for the VCA gain to fall
after the note stops:

| knob | −3 dB at | −20 dB at | span |
| --- | --- | --- | --- |
| 0.00 | 58.6 ms | 97.8 ms | 39.2 ms |
| 0.25 | 73.5 ms | 197.4 ms | 123.8 ms |
| 0.50 (default) | 120.8 ms | **512.4 ms** | 391.6 ms |
| 0.75 | 270.1 ms | 1508.5 ms | 1238.4 ms |
| 1.00 | 742.5 ms | 4658.6 ms | 3916.1 ms |

Monotone. ATTACK (**0.40 ms**) and HOLD (**30.0 ms**) are FIXED and have no knob,
because the reference exposes neither — the values are the gate literature's own
(0.1–1 ms for percussive guitar; 20–30 ms of built-in hold).

### 61.7 Two structural findings, both measured

**1. The detector MUST be feed-forward.** Same code, one flag:

| tap | settled peak on a 0.30 V note | open? |
| --- | --- | --- |
| feed-forward (shipped) | 2.993923e-01 (**−0.02 dB** re input) | yes |
| feed-back | 2.993923e-05 (**−80.02 dB**) | **no — latched** |

80 dB apart, and the feed-back build never opens at any level, because the only
thing its detector can see is the VCA's off-isolation.

**2. THE GATE IS NOT OVERSAMPLED, and that is a measured decision.** Its signal
path is a MULTIPLY; the only nonlinearity in the pedal is in the sidechain, and
the sidechain reaches the output only through a control ramp whose own corner is
400 Hz. On the worst stimulus available — a 9 kHz tone whose envelope is forced
open and shut at 4 Hz — the non-harmonic floor measures:

| factor | 1× | 2× | 4× | 8× |
| --- | --- | --- | --- | --- |
| floor re fundamental | **−176.93 dB** | −166.45 | −162.18 | −162.79 |

Oversampling makes it very slightly WORSE and costs **72 samples of latency** and
roughly double the CPU. The open threshold is likewise unmoved: at 82 Hz / 220 Hz
/ 1 kHz / 3 kHz / 6 kHz / 10 kHz, 1× and 4× agree within **0.09 dB**. So the gate
takes the phaser's arrangement — `setOversampling()` is accepted and IGNORED,
`oversampling()` returns 1, `latencySamples()` returns **0** — and
`clipper_gate_tests` asserts the no-op is real by rendering at 1/2/4/8× and
requiring the results be **bit-identical**. A utility pedal that put 1.5 ms on a
whole board for nothing would be a bug.

### 61.8 Hygiene, and the denormal scope decided by measurement

| property | measured |
| --- | --- |
| DC offset ON SIGNAL (220 Hz 0.30 V, and with +0.1 V input offset) | **0.0000 %** of peak, both cases |
| block-size invariance (128 vs ragged 100/37/256/1/411) | **exactly 0.0** |
| `reset()` vs a fresh model | **0.000e+00**; one NaN poisons 246/256 samples, after `reset()` **0/48000** |
| knob-move seam vs the signal's own slew | 8.610e-03 vs 8.622e-03 = **1.00×** |
| latency | **0 samples** (not oversampled — §61.7) |
| rate spread, 44.1–96 kHz | **0.02 dB** on the open threshold |

Denormal scope (ADR 006) applied by measurement, not by pattern:

* **Guarded** — the four coupling-cap states (input and output high-passes). They
  rest at exactly zero; `maxAbsRestingState()` covers exactly those and measures
  **0.000e+00** after an 8 s silent tail, with the output at **exact digital
  silence**.
* **NOT guarded, the detector's envelope node** — rests at the **9.0000 V** rail.
  A real DC operating point; it can never be subnormal.
* **NOT guarded, the VCA gain** — rests at the cell's off-isolation,
  **1.0000e−04**. Also a real operating point, 296 decades above subnormal.
* **NOT guarded, the detector's clamp caps and Newton warm starts** — §59's
  finding, unchanged by the extraction: they floor at the solver's residual
  tolerance (~1e−11 V), not at zero, so a guard there can never fire.

### 61.9 Perturbation proofs

Every load-bearing bar was proven by reverting the thing it names in a copy of
the source, rebuilding (with `touch` after **both** patch and restore — docs
§29's trap), and confirming the suite goes red. `git stash` was NOT used:
CLAUDE.md records that stashes are repo-global across worktrees.

| # | perturbation | result | first assertion to fail, with the measured number |
| --- | --- | --- | --- |
| baseline | — | GREEN | — |
| P1 | `kHysteresisDb` 6.0 → 0.0 | **RED** | `gap > 3.0 && gap < 10.0` — the gap collapses to **0.19 dB**, and the chatter test then reports 5/6/7 opens |
| P2 | `detectorFromOutput_` default false → **true** (feed-back) | **RED** | the threshold table's monotonicity — the bisection returns **+9.54 dBV** at knob 0.20 because the gate can no longer be opened at any sane level |
| P3 | `kThresholdRangeDb` 40 → 6 (a near-linear pot) | **RED** | the threshold table's `m > prev + 1.0` — the knob stops moving the threshold a usable amount per step |
| P4 | `kAttackSeconds` 0.4 ms → 40 ms | **RED** | `dbOf(pkR / pkG) < 1.0` — the pick edge is late by **6.15 ms** and the 20 ms peak is eaten |
| P5 | `kClosedGain` 1e-4 → 1e-2 | **RED** | `red > 60.0` — noise reduction **80.01 → 40.01 dB** |
| P6 | `kHoldSeconds` 30 ms → 0 | **RED** | the measured hold — unity survives **5.5 ms** instead of 35.8 ms after the comparator closes |
| restore | — | GREEN | — |

**P6 is worth a note, because its first version could not fail.** It was written
as `assert(control().holdSeconds >= 0.020 && <= 0.030)` — an identity that reads
the constant back, so moving the constant moves the bar with it. Replaced by the
player-observable form: how long the gain stays within 0.5 dB of unity after the
comparator lets go, measured from the audio at the FASTEST decay setting
(**35.8 ms** shipped, **5.5 ms** under P6). Same lesson as §58's and §57's
could-not-fail bars.

### 61.10 Two bugs found in the native face table on the way in

Filling `PEDAL_GATE = 9` meant touching `native/src/PedalCard.cpp`'s `kFaces`,
which was `const PedalFace kFaces[PEDAL_TYPE_COUNT]` **indexed by PedalType**.
Two defects fell out, both pre-existing on `main`:

1. **The wah's and the compressor's entries were in the wrong order.**
   `PEDAL_WAH = 6` and `PEDAL_COMP = 7`, but the table listed Squash before
   Weeper — so the native editor drew the Squash face (two knobs, teal) on a
   Weeper card and vice versa.
2. **The slot-reservation commit widened `PEDAL_TYPE_COUNT` to 11 while the table
   still had 8 entries**, so types 8/9/10 resolved to a value-initialized
   `PedalFace` with NULL `const char*` strings — and `showTrayMenu()` loops to
   `PEDAL_TYPE_COUNT`, so the gear tray was already offering three of them.

Both are impossible once the type is written down next to the face, so
`PedalFace` gains an explicit `int type`, `kFaces` is unsized, `pedalFace()` is a
keyed lookup with a RAT fallback, and a new `pedalHasFace()` filters the gear
tray and the swap menu so a RESERVED-but-unfilled type is never offered. The
delay's and the chorus's slots are untouched — they add a pair to the table when
their slices land.

### 61.11 Integration and goldens

One param shape, additive registries: `rig.ts` `PedalType` + `AVAILABLE_PEDAL_TYPES`
+ `GATE_KNOB_DEFAULTS` (THRESHOLD **0.35**, DECAY **0.5**); worklet `gate`
dispatch behind the `_gate` C-ABI prefix; `Pedal.tsx` FACES entry (the two-knob
'stack' geometry, SLATE accent — deliberately the most muted colour on the board,
because a gate is plumbing); native `PEDAL_GATE = 9` (pre-reserved), three APVTS
parameters, a `PedalCard` face and a sixth `identical_core_test` board
(`RAT → Curfew → JCM800` — the first case in which a ZERO-latency pedal follows a
nonzero-latency one, so the chain's latency accounting has to add exactly the
RAT's). Assistant: `add_pedal` gains `'gate'`, and the coach gets a real block —
including that **a gate goes AFTER the dirt** (the opposite of the compressor's
advice, and for the same reason: the noise a gate has to remove is made by the
pedals and the amp's gain, so a gate in front of a distortion has nothing to gate
yet), what to do when notes get chopped (lower THRESHOLD first), and the honest
limit that a gate cannot remove noise *under* a note.

**Goldens: all five UNCHANGED at ±0.00 dB, and nothing was blessed.** The gate is
in no golden rig; the detector extraction it required is bit-identical for the
compressor; and it touches no other model.

**Web-suite caveat, environmental not code — the same one §57 recorded.** The
Playwright run is unstable in this container: three full runs produced three
DIFFERENT failing sets (20 / 4 / 4), every one of them including pre-existing
specs this slice does not touch (`muff worklet`, `chain: reorder of two RATs`,
`gold worklet`, half of `amp.spec.ts`), and every failure passes on retry in
isolation. That is `playwright.config.ts`'s own documented Chromium
`OfflineAudioContext` flake — `audio.spec.ts` has created a great many contexts
by the time it reaches line 542, and the comp spec's own comment records the
engine starting to return silence past about six. The new `gate worklet` spec
measures **hiss drop 80.00 dB, note change 0.000 dB** every time it is run in
isolation, and its harness gate fails by name if a render comes back silent.
Also: **port 4173 was taken mid-session by a parallel agent's worktree**, so the
later runs were done on a throwaway config on another port (deleted afterwards).
Do NOT `pkill` a sibling's preview server.
## 62. M13.7 — the Boss CE-1 Chorus Ensemble: the JC-120's chorus, re-voiced

Pedal type `chorus`, slot `PEDAL_CHORUS = 10`, wordmark **"Ensemble"**, model line
`MODULATION Nº8 · ENSEMBLE`, MAGENTA accent. Three knobs: **RATE / DEPTH / MODE**.
Wired end to end in one slice — core, C ABI (`chorus_*`), worklet, web face +
tokens + assistant, native engine / APVTS / `PedalCard`.

### 62.1 The headline, stated plainly

**This is the JC-120's chorus circuit with the CE-1's knob ranges, its LFO
waveform and its mono output, and that is the honest description.** The Boss CE-1
(1976, Boss's first pedal and the world's first chorus pedal) *is* the Roland
JC-120 amplifier's chorus/vibrato circuit put in a floor box the year after the
amp shipped. This project has modelled that circuit since M6.3 (§11.2), so
`Ce1Model` **owns a `ChorusModel`** and drives it through a voicing seam. The
swept BBD delay line, the 4-point Lagrange interpolator, the LFO phase
accumulator and the ~8 ms control smoothing are the amp's validated code,
unmodified.

There are exactly **three** differences from the amp's voicing. Each is sourced.
Nothing else was invented to make the slice look bigger.

### 62.2 Research: sources, and what could not be obtained

The proxy here refuses nearly every audio site. `WebFetch` returned **403 for
every URL tried** — `djjondent.blogspot.com` (the most detailed CE-1 teardown
found), `help.uaudio.com` (the manual), `forum.fractalaudio.com`. GitHub clones
work, but there is **no CE-1 schematic or LTspice model on GitHub** (searched;
`jpcima/ensemble-chorus` is a string-ensemble model, not this circuit). So the
channel was **search-result extracts only**, exactly as §57's first Orange release.

**SOURCED (multiple independent extracts agreeing):**

| Fact | Value |
|---|---|
| Origin | The JC-120's chorus circuit in a floor box; Boss's first pedal, June 1976 – May 1984 |
| BBD | Panasonic **MN3002**, 512 stages (discontinued in 1984 — which is why the pedal was) |
| Chorus-mode LFO rate | **1.0 – 3.0 Hz**, midpoint reported as "about 1.75 Hz" |
| Vibrato-mode LFO rate | **3.2 – 11.6 Hz** |
| Non-overlap | Stated in prose: "the fastest LFO rate in Chorus mode is slower than the slowest LFO rate in Vibrato mode" |
| Delay window | **3–5 ms at minimum DEPTH, 3–7 ms at maximum** |
| LFO waveform | **Triangle in chorus, roughly sine in vibrato** — the pedal uses a different waveform per mode |
| Controls | Chorus mode has ONE knob (INTENSITY, ganged); vibrato has TWO (RATE, DEPTH) |
| INTENSITY | "The only noticeable effect is the RATE of modulation", though it moves depth too |
| Outputs | **Mono jack = wet and dry MIXED; stereo pair = dry on one jack, wet on the other** |
| Vibrato | "The wet-only sound of the chorus" |
| Character | "Dark, full and throaty"; "organic warmth" |

**A cross-check that landed.** The two rate ranges were sourced independently, and
they do not overlap — 3.0 < 3.2 — *and* the non-overlap is stated separately in
prose. Three facts agreeing is the strongest check available on this voice, and
§62.9 asserts it so a later edit cannot quietly break it.

**GAPS, RECORDED RATHER THAN FILLED:**

1. **No schematic was ever read.** Component values, the compander topology and
   the BBD clock scheme are all unsourced. Nothing in this model claims one.
2. **No vibrato-mode rate midpoint** was published, so that taper is *carried
   over* from the chorus law (§62.3) rather than fitted.
3. **The chorus mode's own delay excursion** was not separately sourced — the
   3–5 / 3–7 ms window is published for the *vibrato* DEPTH knob (chorus mode has
   no depth knob on the real panel). The same window is used for both, on the
   grounds that it is one BBD and one sweep generator with a switched range.
4. **No BBD bandwidth, filter corner or compander ratio.** See §62.7.

### 62.3 The rate law is DERIVED, and a linear pot is refuted

The endpoints are measured figures. The **taper** is the interesting part, because
the source publishes a third number: the chorus midpoint at ~1.75 Hz.

| Law | Prediction at knob 0.5 | Error vs the published 1.75 Hz |
|---|---|---|
| **log** — `rate = 1.0·(3.0/1.0)^k` | **1.7321 Hz** | **1.03 %** |
| linear — `rate = 1.0 + 2.0·k` | 2.0000 Hz | 14.29 % |

So the pot is exponential, and the published midpoint *chooses* it — a factor of
fourteen in the residual. Nothing was fitted. The same law is used for vibrato
(3.2 → 11.6 Hz): it is one LFO circuit with a switched range, so the pot's own
taper cannot plausibly change with the switch.

| RATE knob | CHORUS (Hz) | VIBRATO (Hz) |
|---|---|---|
| 0.00 | 1.0000 | 3.2000 |
| 0.25 | 1.3161 | 4.4155 |
| 0.50 | **1.7321** | 6.0926 |
| 0.75 | 2.2795 | 8.4068 |
| 1.00 | 3.0000 | 11.6000 |

Rendered LFO rate (measured from the pitch track of a 440 Hz probe in vibrato)
tracks the set rate to **1.5 %** or better across the travel, and is identical to
four decimal places at 44.1 / 48 / 88.2 / 96 kHz.

### 62.4 The delay window, read literally, and the floor that is not a bug

"3–5 ms at minimum, 3–7 ms at maximum" says the delay **floor is pinned at 3 ms**
and the **ceiling opens from 5 to 7 ms**. As a centred sweep:

```
centre D0 = (3 + ceiling)/2 = 4 + depth   ms
amplitude A = (ceiling - 3)/2 = 1 + depth  ms
```

both exactly linear in the knob, both straight off the source, no taper invented.

| DEPTH | base (ms) | sweep A (ms) | window (ms) | rendered peak deviation @3.2 Hz |
|---|---|---|---|---|
| 0.00 | 4.000 | 1.000 | 3.000 – 5.000 | **35.2 cents** |
| 0.50 | 4.500 | 1.500 | 3.000 – 6.000 | 53.0 cents |
| 1.00 | 5.000 | 2.000 | 3.000 – 7.000 | 71.0 cents |

Rendered agrees with the swept-delay physics (`peak = (1200/ln2)·A·2πf`) to
**within 2.1 %** at every point.

**THE DEPTH KNOB AT ZERO STILL MODULATES — 35 cents of it.** That is the circuit,
not an oversight, and it is the single most likely thing for a later slice to
"fix" by mistake. It is asserted. Turning the effect off is the pedal's own
bypass, not the depth knob.

At the extremes the numbers get large and are **reported, not tamed**: vibrato at
maximum RATE and DEPTH measures **272 cents** of peak deviation (2.7 semitones).
A CE-1's vibrato at full tilt genuinely is that wild.

### 62.5 Mono is the FACTORY output, and it is why this pedal is not the amp's

This is the difference that matters most, and it is the opposite of what a mono
host usually forces. **The CE-1 has both a stereo pair (dry on one jack, wet on
the other) and a MONO jack that mixes wet and dry internally.** The pedal chain
here is mono, so we ship the factory mono output — a real output on a real pedal,
not a sum we invented.

The audible consequence is the whole point. Mixing dry and wet **electrically**
produces **comb filtering**. The amp's chorus never combs, because `ChorusModel`
in CHORUS mode is a *split*: L is the bit-exact dry input and R is the wet one,
and the comb only ever happens acoustically in the room.

Measured, a 440 Hz tone at RATE 0.5 / DEPTH 1.0:

| | envelope swing |
|---|---|
| **CE-1 chorus (mono jack)** | **14.72 dB** |
| amp chorus, LEFT channel — what a mono host takes | **0.00 dB** (max abs difference from the input: **0.000e+00**) |

A mono host taking the amp's chorus gets **no chorus whatsoever**. That single row
is the justification for the pedal existing.

The comb is in the right place, and it moves. At DEPTH 0 the delay centres on
4.00 ms, so a dry+wet mix nulls first at `1/(2·4 ms) = 125 Hz` and peaks at
`1/4 ms = 250 Hz`:

| probe | DEPTH 0 | DEPTH 1 | note |
|---|---|---|---|
| 125 Hz | **0.02517** | 0.17751 | **+16.97 dB** — rises out of the null as the base moves to 5 ms |
| 250 Hz | 0.40825 | — | the peak; **24.20 dB** of comb against the 125 Hz null |
| 100 Hz | 0.15038 | 0.06140 | **−7.78 dB** — sinks toward the 5 ms delay's own first null |
| 125 Hz, VIBRATO | 0.42624 | — | **24.57 dB above** the chorus null: no dry path, so no comb at all |

**WHAT IS LOST:** the stereo image. A real CE-1 into two amps is wide; this is
not, and no mono model can be. Documented, not papered over.

### 62.6 The one deliberate departure from the real control layout

Recorded as **ADR 020** (`docs/decisions/020-ce1-ungangs-intensity.md`).

The real CE-1's panel is asymmetric: CHORUS mode has **one** knob (INTENSITY,
ganged — it moves rate and depth together, and the rate is the part you notice),
VIBRATO has **two** (RATE and DEPTH). Reproducing that gang would leave one of our
three slots doing **nothing** in chorus mode, and this codebase forbids dead UI in
terms ("Don't ship dead UI … or a knob whose top half does nothing").

**Decision:** RATE and DEPTH are independent in **both** modes. MODE selects which
rate RANGE and which WAVEFORM apply. A player who wants the factory chorus feel
moves RATE alone.

**Cost:** the chorus mode's knob feel is not the real pedal's. You can set a
combination (slow rate, deep sweep) that a real CE-1's single INTENSITY knob
cannot reach. Recorded here so a future slice does not "discover" the gang and
re-introduce it without knowing the trade was deliberate.

MODE itself is **genuinely discrete** — a real footswitch between two circuits —
so unlike §58's wah SENSE it is not smuggled into a continuous law. The threshold
is explicit (`< 0.5` chorus, `>= 0.5` vibrato) and both ends plus the boundary are
asserted.

### 62.7 What is NOT modelled, named rather than guessed

The CE-1 is a **bucket-brigade** circuit: an MN3002 behind a compander, with an
anti-alias filter in front and a reconstruction filter after. Sources describe the
result as "dark, full and throaty".

**None of that is modelled.** The wet path is a clean Lagrange-interpolated delay,
exactly as the amp's chorus is, and this pedal **inherits that approximation
rather than adding a new one**. No corner frequency, compander ratio or BBD
bandwidth figure could be sourced, and inventing one to chase an adjective is
precisely the fitting this project forbids (§57.1's rule). **It is the largest
known gap on this voice**, and the fix is a schematic, not a filter chosen by ear.

### 62.8 Two defects this slice's own tests found, and one numeric fact

**(1) Switching the LFO waveform mid-cycle STEPS the delay.** The first
implementation flipped a `waveform` flag. Sine and triangle share their zero
crossings and their sign, so switching *at* a zero crossing is seamless — but they
agree nowhere in between: at phase π/4 sine is 0.7071 and triangle is 0.5000, so
the flip **jumps the read pointer by 0.207·A**, about 14 samples at a 1.4 ms sweep
at 48 kHz. Audible click. Fixed by making the waveform a **smoothed blend** on the
same ~8 ms constant, with a `blend <= 0.0` fast path so every JC-120 caller still
evaluates `std::sin` and nothing else.

**(2) Forwarding the mode to `ChorusModel` CLICKED — measured at 17.02× the
signal's own slew.** `ChorusModel`'s mode is a *hard* switch by its own documented
convention, and switching it makes the L channel change meaning from dry to wet in
one sample while our dry mix weight is still mid-ramp at 0.5, so the output jumps
from `(dry+wet)/2` to `wet`. **Fixed by never switching it:** the owned
`ChorusModel` stays in `MODE_CHORUS` for both of our modes, and vibrato is that
same dry/wet pair with the dry weight taken to zero. Seam **17.02× → 0.95×**, and
it makes "vibrato is the wet path alone" structurally true rather than a second
code path that has to agree. Do not "simplify" it back.

**(3) `OnePoleSmoother` converges EXACTLY to a zero target and NOT to a nonzero
one.** Worth knowing before writing the next smoother test. Approaching zero the
value keeps halving until the residual drops under the 1e-30 guard, which then
snaps it — so the vibrato dry weight really does reach **exactly 0.0**, which is
what lets "the dry path is absent" be an `==` rather than a tolerance. Approaching
a *nonzero* target the guard cannot fire: the ramp stalls once the increment falls
below half a float ULP at the target, i.e. at a residual of about
`ULP/coeff = 1.19e-7 / 0.0026 ≈ 4.6e-5`. Measured: the wet weight settles at
**0.99998856**, 1.14e-5 short (−98.8 dB). Inaudible, but an `==` there would be a
flake.

### 62.9 The suite, and what it is pointed at

`clipper_ce1_tests` (core ctest **29 → 30 entries**, 30/30). Every bar is a
player-observable property; there is no comparison against an analytic expression
derived from the same code.

1. **Rate law** — endpoints against the teardown's measured figures, monotone in
   both modes, and the **derived taper**: within 3 % of the published midpoint,
   with the assertion that a linear pot would be >10 % off, so the refuted
   alternative stays refuted.
2. **The non-overlap cross-check** — fastest chorus < slowest vibrato, and the
   vibrato top is >3.5× the chorus top.
3. **Depth in CENTS** off a rendered pitch track, agreeing with the swept-delay
   physics to 15 %; both ends of the published window asserted; and the DEPTH-0
   floor asserted to still detune.
4. **Chorus vs vibrato as two measurable states** — the vibrato dry weight is
   `== 0.0` exactly, and on audio the envelope swings **15.12 dB** in chorus
   against **0.00 dB** in vibrato, while vibrato still moves the pitch by 137.8
   cents. A flat envelope with moving pitch is not something a gain change can
   fake.
5. **The comb** — position, depth, and that it MOVES with DEPTH (§62.5's table).
6. **Against the amp's chorus** — the §62.5 mono row, plus the range comparison.
7. Housekeeping: DC on signal (clean **0.026 %** of peak; with +0.1 V of input
   offset the offset **passes through**, because a linear delay with no coupling
   cap does not block DC — stated, bounded, not hidden), `reset()` (**0.000e+00**
   against a never-played model), the NaN guard (1/256 non-finite in →
   **0/48000** after reset; non-finite params rejected), no zipper (RATE slam
   **0.98× / 1.00×**, MODE switch **0.95×**), block-size invariance
   (**0.000e+00** ragged vs 128), rate independence (**0.00 %** spread over
   44.1–96 kHz), the ADR 006 resting state (delay line **exactly 0.0**), and the
   MODE mapping at both ends and across the threshold.

**Denormals (ADR 006).** The delay ring is a pure FIFO of the input with **no
recursion into it**, so on silence it reaches exactly 0.0 after one buffer length
— asserted, not assumed — and no flush is reachable. The two mix-weight smoothers
rest on their targets and `OnePoleSmoother` snaps exactly (see §62.8(3)). This
file adds **no new guard**, and that is a measured conclusion rather than an
omission.

**Latency 0 and no oversampling**, asserted: the effect is linear time-varying, so
there is nothing to alias, and its modulated delay IS the effect rather than a
compensable latency (the phaser precedent, §12).

### 62.10 The amp's chorus is BIT-IDENTICAL, proven twice

`ChorusModel` gained a voicing seam (`setBaseDelayMs` / `setSweepMs` /
`setRateHz` / `setWaveform`), a base-delay smoother and a waveform blend. All of
it defaults to the JC-120 values, so:

- **108 render hashes** (3 sample rates × 3 modes × 4 speeds × 3 depths, ragged
  block sizes, FNV-1a over both channels' raw float bits) are **identical** to the
  pre-slice source, re-verified after the waveform-blend change.
- **All five goldens UNCHANGED at ±0.00, and `clean120_chorus` — the one that
  would move if this slice had disturbed the amp — is among them.** Nothing was
  blessed.

Two implementation details carry that: `baseSamples` is an `OnePoleSmootherT<double>`
whose settled `next()` returns its target bit-exactly, and `lfo()` takes a
`blend <= 0.0` branch that is `std::sin(phase)` and nothing else.

### 62.11 Wiring, and a pre-existing bug found on the way

Slot `PEDAL_CHORUS = 10` (reserved before the slice, never renumbered), the
`_chorus` C-ABI prefix, `Pedal.tsx` FACES entry on the `plate` anatomy,
`tokens.css` `--accent-chorus` (magenta — the phaser owns orange and this is the
second modulation pedal), four APVTS parameters, a `PedalCard` face, and an
assistant block that covers what MODE actually does, the non-overlapping speed
ranges, the fact that DEPTH 0 does not switch it off, and that a chorus usually
goes **after** the dirt.

**A PRE-EXISTING BUG, FOUND AND FIXED — REPORT IT.** `PedalCard.cpp`'s `kFaces` is
a **positional** array indexed by `PedalType`, and the wah/compressor merge left
**Squash at index 6 and Weeper at index 7** while `PEDAL_WAH = 6` and
`PEDAL_COMP = 7`. So in the native plugin **a wah card drew the compressor's face
and a compressor card drew the wah's** — wrong wordmark, wrong accent, wrong knob
labels and wrong param attachments. `pedalMenuLabel` uses explicit `case` labels
and was right, which is exactly why the menu and the card disagreed. Corrected
into enum order here, because this slice extends that same array and shipping one
it had just edited while knowing it was wrong would be worse. Indices 8 and 9 are
left as **explicit empty faces**, reserved for the delay and gate slices to fill,
so the array stays index-aligned while they are in flight.


## 63. M10.7 — the Orange Rockerverb 100: the MODERN Orange (the sixth amp voice)

The Rockerverb 100's DIRTY CHANNEL joins the lineup as amp voice **5**
(`rockerverb`), sharing the OR120's `orange412` cab. It is EL34 push-pull like the
OR120 and the JCM800 and **reuses that power machinery wholesale** — the Koren
EL34 fit, the per-tube plate-load Newton, the grid-coupling/blocking solve, the OT
bandwidth pair, the rail/screen sag integrator, and the `LtpInverter` with audit
finding 7's tail reference (§42). No new device model was fitted for this voice.

It is the second half of the owner's *"or120 and rockverb. I'm an orange man."*
and the ROADMAP calls it the counterweight to the OR120 the way the OR120 is to
the JCM800. §63.4 and §63.5 are that bar, and they are hard asserts in dB.

**ONLY THE DIRTY CHANNEL SHIPS, and that is a research decision, not a shortcut.**
The netlist this voice is transcribed from is the dirty channel's; the clean
channel would be pure invention, and §57.1's rule — *"do not re-tune any of them
toward a sound; find the schematic"* — applies to inventing a whole channel too. A
footswitch in front of a made-up channel is worse than no footswitch. See §63.11.

### 63.1 Provenance — what is TRANSCRIBED and what is RECONSTRUCTED

**Read this before changing any constant in `RockerverbPreamp.h` /
`RockerverbPowerAmp.h`.**

**The channel that was open, and it is better than §57's was.** The proxy in this
build container permits **github.com only** — re-confirmed this slice:
`el34world.com` and `en.wikipedia.org` both fail `CONNECT tunnel failed,
response 403` under `curl`, and `WebFetch` returns 403 for
`el34world.com/.../Orange_rockreverb_50w.pdf` and for
`dirtboxlayouts.blogspot.com`. Orange's own site, prowessamplifiers,
music-electronics-forum and ampgarage were reachable only as search-result text.

But a GitHub code search found **a real netlist**: `Orange Rockerverb 50
Preamp.schx`, an example circuit shipped inside **LiveSPICE**
(`dsharlet/LiveSPICE`, `Tests/Examples/`; the same file is mirrored in four other
repositories). A `.schx` is a schematic FILE — every component with its value,
position, rotation and every wire — so the netlist is *recoverable exactly* rather
than eyeballed off a picture. It was parsed node by node: terminal offsets read
out of LiveSPICE's own `LayoutSymbol` implementations (`TwoTerminal` ±20,
`Potentiometer` anode/cathode/wiper, `Triode` plate/grid/cathode), the
rotation/flip transform out of `Circuit/Schematic/Symbol.cs`, the pot's own
`R(cathode→wiper) = R·P` convention out of `Potentiometer::Analyze`, and the wire
graph resolved by union-find with collinear-point merging. The full parsed netlist
is reproduced in `docs/work/2026-08-01-rockerverb.md`.

#### TRANSCRIBED — the dirty-channel preamp, from the `.schx` netlist

Designators are that file's.

| Element | Value | Note |
| --- | --- | --- |
| input → grid stopper | **68 k** (R23) | 1 M grid leak (R20) on S1 |
| S1 (V9) plate load | **100 k** (R19) → B2 | |
| S1 cathode | **1k5 ∥ 10 µF** (R2/C14) | fully bypassed |
| S1 → S2 coupling | **1 n** (C1) | a 398 Hz corner into the network below |
| GAIN-1 network | **R30 220 k** → (**R1 220 k** ∥ **C2 470 p**) ∥ **GAIN 1 M LOG** | |
| GAIN-1 bright cap | **100 p** (C16), top lug → **WIPER** | the 2204's trick (§47) |
| S2 (V10) plate load | **100 k** (R24) → B2, **100 p** (C4) across it | |
| S2 cathode | **1 k ∥ 10 µF** (R25/C3) | the hottest stage |
| S2 → S3 coupling | **2n2** (C13) | |
| GAIN-2 network | **R31 220 k** → **R32 470 k** ∥ **GAIN 1 M LOG** | NO bright cap |
| S3 (V11) plate load | **100 k** (R26) → B1, **100 p** (C6) across it | |
| S3 cathode | **2k2 ∥ 10 µF** (R27/C5) | |
| S3 → S4 | **4n7** (C7) → **470 k** (R6) → **220 k** (R5) to ground | a fixed 0.319 divider |
| S4 (V12) plate load | **100 k** (R4) → B1 | |
| S4 cathode | **1k5, NO bypass cap** (R3) | **the cold/tight stage** |
| tone stack | **560 p** (C24) · **39 k** slope (R43) · **22 n** (C25) · **22 n** (C26) | an **FMV** |
| TREBLE / BASS / MIDDLE | **250 k LIN** · **500 k LOG** · **25 k LIN** | |
| VOLUME | **1 M LINEAR**, AFTER the stack | |
| supply | **400 V** (V8) → **10 k** (R9) → B1 (22 µF) → **10 k** (R42) → B2 (22 µF) | |

**THE TWO GAIN POTS ARE ONE KNOB, AND THAT IS SOURCED, NOT ASSUMED.** Both carry
`Group="Gain"` in the schematic file — LiveSPICE's own gang marker — and both
default to the same wipe. One knob drives both wipers here.

**THE VOLUME POT IS LINEAR AND IT SHIPS LINEAR.** The file marks Gain and Bass
`Logarithmic` and Treble/Middle/Volume `Linear`, i.e. its author distinguished
them deliberately. A linear master is unusual and puts the useful range low on the
travel; that is MEASURED and REPORTED in §63.5 rather than "fixed" with an audio
taper this circuit gives no support for — the same call §59 made for the Dyna
Comp's SUSTAIN rheostat.

#### SOURCED as prose (search-result text only)

* Four EL34s in the 100 W head; four 12AX7 preamp valves; two 12AT7s for the
  reverb and the effects loop; a **two-stage clean channel and a four-stage dirty
  channel** (which the netlist independently confirms).
* **The phase-inverter valve is an ECC83 in its own socket** — a WHOLE dual triode
  dedicated to the PI. That is the reason this model uses a **long-tailed pair**:
  the OR120's Field Guide entry says in terms *"Phase Inverter: Cathodyne type:
  1/2 x 12ax7"* (HALF a valve), and a cathodyne cannot use the other half. This is
  an **inference from a sourced fact**, and it is labelled as one — it is a
  structural property this voice REPORTS (§63.6), never the bar.
* The front panel: Clean (Volume/Bass/Treble), Dirty (Gain/Volume/Bass/Middle/
  Treble), shared Reverb, a footswitchable Attenuator. **There is no presence
  control**, which is why this power section has no presence knob and its feedback
  loop carries no shaping filter.
* The reverb is real and valve-driven — it is in the amp's name, so unlike the
  JCM's (§19) and the OR120's (§57) it is not a usability add.

#### RECONSTRUCTED — named rather than hidden

Everything in `RockerverbPowerAmp.h`: the LTP's plate loads / tail / tail
reference, the EL34 fixed bias, `Raa`, the OT corners, the supply Thévenin source
and reservoir, the screen network, the PI→EL34 coupling and the feedback divider.
Also the **pots' taper LAWS** (the file gives the LETTER; LiveSPICE's own generic
log curve is `k = 2` and this project's house audio law is `k = 4`, §51 — the
house law ships, for consistency with every other log pot in the repo, and the
difference is a recorded gap), and the **PI grid leak** that loads the VOLUME pot
(1 M, the house value).

Each reconstructed constant is chosen against a PHYSICAL constraint — the triodes
in the project's window, the EL34s inside their 25 W plate rating, the amp
reaching its rated power — never against a tone. §57's rule applies verbatim:
**do not re-tune any of them toward a sound; find the schematic.**

**THE ATTENUATOR IS NOT MODELLED.** It is a post-OT load device, so it belongs
after this stage, not inside it.

### 63.2 The preamp — `RockerverbPreamp`

```
guitar in -> 68k grid stopper (1M leak)
  -> S1  V9   Ra 100k to B2, Rk 1k5 || 10uF          (bypassed)
  -> C1 1n  -> R30 220k -> [R1 220k || C2 470p] || GAIN-1 1M log
               with C16 100p bridging the pot's top lug to its WIPER
  -> S2  V10  Ra 100k to B2 (100p across it), Rk 1k || 10uF
  -> C13 2n2 -> R31 220k -> R32 470k || GAIN-2 1M log   (the SAME knob)
  -> S3  V11  Ra 100k to B1 (100p across it), Rk 2k2 || 10uF
  -> C7 4n7  -> R6 470k -> R5 220k                      (a 0.319 divider)
  -> S4  V12  Ra 100k to B1, Rk 1k5 UNBYPASSED          (the cold stage)
  -> FMV tone stack -> VOLUME 1M LINEAR -> the phase inverter's grid
B1 = 355.18 V, B2 = 331.49 V (solved through the transcribed 10k droppers).
```

Measured **preamp DC** (identical at 44.1 and 48 kHz):

| stage | Va | Vk | Ip (solver) | Ip (Ohm's law) | plate as % of B+ | rout |
| --- | --- | --- | --- | --- | --- | --- |
| S1 | 222.99 V | 1.627 V | 1.0850 mA | 1.0687 mA | 67.8 % | 32.8 k |
| S2 | 201.91 V | 1.296 V | 1.2958 mA | 1.2828 mA | 61.3 % | 30.8 k |
| S3 | 259.55 V | 2.104 V | 0.9564 mA | 0.9353 mA | 73.7 % | 35.1 k |
| S4 | 238.40 V | 1.752 V | 1.1678 mA | 1.1503 mA | 67.6 % | 32.4 k |

**These are ABOVE the project's 0.5–0.9 mA "gain-stage window", and that is
correct rather than a defect** — the same point §57.3 makes for the OR120's
driver. A 12AX7 on a **100 k** plate load with a 1–2.2 kΩ cathode from ~340 V is
the classic British gain stage; the 0.5–0.9 mA window belongs to 220 k-load
stages. What the test asserts instead is the solver's current against **Ohm's law
on the transcribed 100 k**, each cathode against its own transcribed resistor, and
absolute windows on the two dropper voltages (R9 must drop 30–60 V carrying four
triodes, R42 15–35 V carrying two). The dropper checks are deliberately NOT
Ohm's-law identities — §57.10 found three bars of exactly that kind that could not
fail.

Every network is driven from a **PLATE** (rout 30.8–35.1 kΩ). There is no cathode
follower anywhere in this preamp, which is a real part of why its tone stack is
softer than a 2204's.

**Discretization check** against each netlist's own complex nodal solve, three
knob points × 82 Hz…6 kHz for the interstage networks and five knob combinations
for the stack: worst |error| **0.3022 dB (44.1 kHz) / 0.2524 dB (48 kHz)** for the
interstage MNAs and **0.5457 / 0.4574 dB** for the tone stack, against a 0.60
bound that is the bilinear warp figure with margin. The standing §29 limitation
applies — an analytic reference derived from the same netlist validates the
DISCRETIZATION and structurally cannot catch a wrong topology. What validates the
topology here is §63.4.

**THE MID POT IS NOT A RHEOSTAT, and it is transcribed that way.** Both of its
sections sit between node C and ground, so the resistance C→GND is **25 k at every
knob position**; what the knob moves is where the 22 n cap TAPS INTO that 25 k. At
MIDDLE 0 the cap shunts the slope node almost to ground (everything but the treble
branch is pulled down); at MIDDLE 1 it lands on node C, the canonical FMV
position. It ships literally, and the test asserts the consequence — MIDDLE moves
**8.93 dB at 650 Hz** and only **1.85 dB at 20 Hz** — so nobody can quietly
"tidy" it into a rheostat.

### 63.3 ONE MODELLING DEPARTURE, stated up front

`TriodeStage` conflates its INPUT coupling and its OUTPUT coupling: one
`(Cc, Rgl)` pair serves both, so a cascade applies every interstage high-pass
**twice**. In a uniform cascade that is inaudible (the 2204's 22 n into 1 M is
7 Hz; the OR120's 68 n into 953 k is 2.5 Hz). Here the real interstage caps are
**1 n into 400 k = 398 Hz**, 2n2 into 540 k = 134 Hz and 4n7 into 690 k = 49 Hz,
so a doubled corner would be a gross error — a −12 dB/octave rolloff starting at
400 Hz, on an amp whose whole identity is its low mids.

So every real coupling cap lives **once**, inside the interstage MNA where its
divider is, and each `TriodeStage` carries a deliberately transparent 1 µF
coupling with `Rgl` set to the network's real resistive input (which is what loads
the plate) and `Rg` set to that grid's real series source impedance (the
transcribed 68 k stopper on S1, the 1 M pots' worst-case Thévenin of 250 k on
S2/S3, the transcribed 470 k ∥ 220 k = 149.8 k on S4).

**The cost, named:** preamp grid BLOCKING — the slow bias shift as a coupling cap
charges through grid current — is not modelled in this voice. Grid CONDUCTION
clamping is (through each grid's real source impedance), and the EL34 grid
blocking that dominates a cranked amp (§18) is fully present. The real interstage
τ here are 0.4 / 1.2 / 3.2 ms, so what is lost is fast and bass-dependent rather
than the 2204's 22 ms "farting". **Do NOT "fix" this by setting `Cc` to the real
cap — that re-introduces the doubled corner.** The proper fix is independent
input/output coupling configs on `TriodeStage` (§63.11).

### 63.4 THE BAR, HALF ONE — measurably NOT a re-skinned OR120

The metric is **§57.4's, verbatim**, so the two voices sit on ONE scale: at noon,
the minimum response across 300–800 Hz relative to the mean of the 100 Hz and
4 kHz responses. Negative = a mid SCOOP, positive = a mid BUMP. Scale-free, so the
two stacks' very different insertion losses cannot flatter either one.

The bar rests on a STRUCTURAL difference, because these two amps are the same
manufacturer, the same output valves and a shared lineage: the OR120's network is
a **JAMES / passive Baxandall** (two parallel shelving branches, nothing acting on
the middle) and the Rockerverb's is a **Marshall-lineage FMV** (a slope resistor
and a treble branch that between them notch the middle). That the modern Orange
uses a Marshall-family stack is the single most surprising thing the netlist says.

**(a) the tone NETWORKS, at noon, each from its own netlist:**

| | Rockerverb FMV | OR120 James |
| --- | --- | --- |
| mid-notch metric @ noon | **−6.01 dB** (a SCOOP) | **+0.75 dB** (a BUMP) |
| **contrast** | | **6.76 dB** against a **5.0** bar |

dB relative to each network's own 1 kHz:

| f | Rockerverb FMV | OR120 James |
| --- | --- | --- |
| 82 Hz | +6.80 | −2.98 |
| 220 Hz | +2.73 | −1.29 |
| 440 Hz | −0.99 | −0.27 |
| 660 Hz | −1.25 | −0.07 |
| 1 kHz | 0.00 | 0.00 |
| 2.2 kHz | +2.26 | +0.01 |
| 5 kHz | +3.06 | +0.01 |

The FMV peaks at BOTH ends and dips in the middle; the James is
flat-to-slightly-domed through the middle. Note that the Rockerverb's −6.01 dB
lands within 0.02 dB of the figure §57.4 measures for the actual Marshall FMV
(−6.03) — which is the point: on this axis the modern Orange is a Marshall and the
vintage Orange is not.

**Margin, stated plainly: the contrast bar ships at 5.0 against a measured 6.76,
so 1.76 dB. It was RECORDED, not snugged.** It is deliberately not set just under
the measurement, because §57.4 shipped 6.0 against 8.35 and a later component
correction took the measurement to 6.78 — a snugged bound would have failed for a
reason that was not a regression. The two SIGNS are asserted separately
(`rvNotch < −3`, `orNotch > 0`) so a change moving both together could not hide.

**(b) the COMPOSED amps, rendered** — both at tone knobs noon, a clean level, the
same input, deliberately NOT normalized at 1 kHz (the FMV's own notch minimum sits
near there, so normalizing there would hide the very thing being measured):

| f | Rockerverb (dB re its 660 Hz) | OR120 (dB re its 660 Hz) |
| --- | --- | --- |
| 110 Hz | −10.14 | −19.79 |
| 220 Hz | −3.16 | −10.79 |
| 330 Hz | −1.56 | −5.86 |
| 440 Hz | −1.02 | −2.97 |
| 660 Hz | 0.00 | 0.00 |
| 1 kHz | +1.98 | +1.78 |
| 2.2 kHz | +6.14 | +2.94 |
| 4.4 kHz | +8.47 | +2.53 |

| | Rockerverb | OR120 |
| --- | --- | --- |
| composed mid-notch | **−0.72 dB** | **+2.78 dB** |
| **contrast** | | **3.50 dB** against a **3.0** bar |

**The composed half is the TIGHTER of the two — 0.50 dB of margin — and it is
reported as such rather than widened.** §63.13's P4 (the GAIN gang broken)
measures it at **3.11 dB**, which is the composed metric behaving correctly
rather than being fragile: it is a COMPOSED measurement, so a change to the
gain structure moves it. That is exactly why the NETWORK half, measured from
the two netlists with no rendering in it at all, is the half with teeth. The composed figure is shallower than the
network's because the four-stage preamp's own coupling networks put a large
top-end tilt on this amp (+8.47 dB at 4.4 kHz against the OR120's +2.53), which
lifts the reference mean. The network half, measured from the netlists, is the
half with room in it.

### 63.5 THE BAR, HALF TWO — the MASTER VOLUME decouples drive from level

This is the property an amp with no master **physically cannot have at any
setting**, which is what makes it the right second half of a bar against a sibling
that shares this one's power machinery. The OR120 has ONE knob and its power
section IS the overdrive (§57.5, §46's convention). The Rockerverb's VOLUME sits
AFTER the tone stack.

Measured LEVEL-MATCHED: both amps bisected onto the same output RMS at the same
input (0.15 V peak / 220 Hz, tone knobs noon), then their THD compared.

| | knob found | output | THD |
| --- | --- | --- | --- |
| Rockerverb | GAIN 0.70, **VOLUME 0.0254** | −20.00 dBFS | **29.75 %** |
| OR120 | **VOLUME 0.2030** | −20.00 dBFS | **1.89 %** |
| **ratio at equal level** | | | **15.76× (23.95 dB)** |

**The bar ships at 5× against a measured 15.76× — 3.15× of margin, RECORDED not
snugged.** The bisection's own level match is asserted first (both within 0.5 dB
of the target), because the ratio means nothing otherwise.

…and the MECHANISM is asserted separately, so the ratio cannot be produced by an
amp that is merely dirty everywhere. With GAIN held at 0.70:

| VOLUME | output | THD |
| --- | --- | --- |
| 0.010 | −28.08 dBFS | 30.13 % |
| 0.015 | −24.57 | 30.01 |
| 0.020 | −22.09 | 29.89 |
| 0.030 | −18.56 | 29.63 |
| 0.050 | −14.10 | 29.24 |
| 0.080 | −10.02 | 29.05 |
| **0.100** | **−8.19** | **29.26** |
| 0.150 | −5.68 | 30.40 |
| 0.200 | −4.90 | 32.26 |
| 0.300 | −4.79 | 35.49 |

**19.89 dB of level for a THD ratio of 0.971 across VOLUME 0.01 → 0.10.** That is
a textbook master volume, and it is asserted as one (level span > 15 dB, THD ratio
in [0.90, 1.10], and THD already > 20 % at the bottom). A pre-stack volume pot —
the OR120's arrangement, and the §44 defect the Twin had — cannot satisfy it.

**THE LINEAR MASTER, REPORTED NOT FIXED.** Above ~0.15 the level stops moving
because the power valves are already flat out; the pot's useful range is the
bottom of its travel, which is what a **linear** 1 M master loaded by a 1 M grid
leak does. The tone-network law at 1 kHz measures −25.6 / −20.0 / −14.5 / −11.4 /
−7.3 / −4.3 / 0.0 dB at VOLUME 0.05 / 0.10 / 0.20 / 0.30 / 0.50 / 0.70 / 1.00. The
assistant coaches 5–15 for a room level rather than apologizing for it (§63.10).

**Breakup vs GAIN**, VOLUME 0.5, the same probe:

| GAIN | THD | RMS |
| --- | --- | --- |
| 0.05 | 1.312 % | −47.33 dBFS |
| 0.10 | 2.429 | −33.45 |
| 0.15 | 4.818 | −24.43 |
| **0.20** | **8.098** | −17.23 |
| 0.30 | 10.102 | −6.66 |
| 0.40 | 23.662 | −5.47 |
| 0.50 | 35.950 | −5.24 |
| 0.70 | 39.255 | −5.26 |
| 1.00 | 43.660 | −5.44 |

≥5 % THD onset at **GAIN 0.20**, monotone. **That is EARLY, and it is what the
netlist says rather than something to tune:** this is a four-stage preamp and the
GAIN pot sits AFTER the first stage, so S1's contribution is there at any knob
position. For comparison the JCM800 at the same probe onsets at 0.31 (§51) with
three stages and a COLD second one. The test's window is wide and absolute
(0.08 < onset < 0.35) — what it forbids is an amp that is either clean at half
gain or filthy at a tenth of it.

**`kInterstageScale` is an UN-FITTING to 1.0**, for the same reason the OR120's is
(§57.9): the preamp ends at the VOLUME wiper with the PI's 1 M grid leak already
stamped into the same matrix, so its output IS the phase inverter's grid voltage
in volts and there is nothing left for a trim to represent. It was still swept,
because "the constant is unity" is only defensible if unity is also where the amp
behaves:

| scale | ≥5 % THD onset (GAIN) | cranked W into 8 Ω (0.15 / 0.30 / 0.50 V in) |
| --- | --- | --- |
| 0.05 | 0.35 | 12.2 / 14.1 / 16.4 |
| 0.10 | 0.30 | 50.8 / 50.9 / 51.1 |
| 0.25 | 0.25 | 93.9 / 89.1 / 85.4 |
| 0.50 | 0.20 | 93.0 / 89.9 / 86.1 |
| **1.00** | **0.20** | **92.5 / 89.7 / 86.2** |
| 2.00 | 0.15 | 92.4 / 89.9 / 86.6 |
| 4.00 | 0.10 | 92.3 / 90.1 / 86.3 |

### 63.6 The power section — `RockerverbPowerAmp`

```
PI: 12AX7 LONG-TAILED PAIR, Ra1 100k / Ra2 120k, Rtail 10k to a -12 V reference
    B+ = 473.83 V (the main rail behind a 10k dropper)
    <- global NFB, 47k from the 8 ohm tap with 4k7 to ground (beta = 0.0909),
       into the SECOND grid. NO presence pot in that leg — this panel has none.
4x EL34, fixed bias -47 V, 22n/220k grid coupling, Raa 1.7k, solid-state supply
    -> OT (45 Hz / 14 kHz) -> the 8 ohm tap -> back to the PI
```

Measured **power-section DC**:

| node | value |
| --- | --- |
| EL34 rail / screen | **489.55 V** / 486.10 V |
| EL34 Ip / Ig2 per tube | **33.89 mA** / 3.45 mA |
| EL34 plate dissipation | **16.59 W = 66 % of the 25 W rating** |
| screen drop across the per-tube 1 k | 3.446 V |
| LTP B+ / Va1 / Va2 | 473.83 V / **390.91 (82.5 %)** / **384.70 (81.2 %)** |
| LTP tail / Ip1 | 3.719 V / **0.8291 mA** |

Every one of those is inside the project's documented windows (plates 70–85 % of
B+, 0.5–0.9 mA per triode, the EL34 inside its plate rating) — which is the
constraint the reconstructed constants were chosen against.

**THE PHASE INVERTER IS A LONG-TAILED PAIR, and its balance is a CALIBRATION where
the OR120's is a TOPOLOGY.** `Ra2` is asymmetric on purpose: a real LTP with equal
plate loads does not deliver equal legs, which is audit finding 8's whole subject
(§45). Swept on this amp's own rail and tail:

| Ra2 | tailRef −10 | −12 | −14 |
| --- | --- | --- | --- |
| 100 k | 0.8199 | 0.8280 | 0.8346 |
| 110 k | 0.8918 | 0.9007 | 0.9081 |
| **120 k** | 0.9620 | **0.9718** | 0.9800 |
| 130 k | 0.9703 | 0.9603 | 0.9520 |
| 140 k | 0.9110 | 0.9014 | 0.8935 |
| 150 k | 0.8596 | 0.8504 | 0.8428 |

120 k / −12 V ships: balance **0.971988**, legs **−25.4276 / +24.7153**, both
plates and the standing current in-window (the −14 V column balances marginally
better and puts Ip1 at 0.926 mA, outside the 0.9 ceiling — the window wins).
**That is the SAME Ra2 §45's independent sweep landed on for the 2204 — not a
coincidence and not a copy: same tube, same 10 k tail, a similar B+, so the same
compensation. Reported.**

The structural contrast against the OR120 is REPORTED, not asserted as the bar,
because the LTP is an inference rather than a transcription (§63.1):

| property | Rockerverb LTP | OR120 cathodyne (§57.3) |
| --- | --- | --- |
| leg balance | **0.971988**, and it took a resistor sweep | **0.999965**, *topological*, no calibration at all |
| leg gain | **×25.4 / ×24.7** — it AMPLIFIES | ×0.9733 — it cannot |
| clip mechanism | tail-steering cutoff | compliance (Vk pinned to [0, C+/2]) |

What IS asserted about it: the two legs are **anti-phase** (the signed product of
the leg gains must be negative — a property a magnitude bar cannot fake), the
balance clears **0.90** (§45's bar for the 2204), and both legs amplify by ≥ 10×.

**GLOBAL FEEDBACK IS FLAT, and that is the third structural difference.** The
2204's presence pot lifts its HF by several dB and the OR120's H.F. Boost R-L-C
peaks 6+ dB at 5.2 kHz; this amp's panel has no presence control, so its loop
carries no shaping filter at all. Measured loop depth **6.65 dB at 440 Hz**
(open-loop 0.29442 → closed 0.13689), and **6.45 dB at 220 Hz vs 6.18 dB at
5 kHz — a spread of 0.26 dB**, asserted under 1.0.

**THE AMP DOES NOT QUITE MAKE ITS RATED 100 W — reported, not tuned away.** The
composed cranked figures above top out at **93.9 W into 8 Ω**, and the power
section's own sine ceiling driven directly at the PI grid measures **82.15 W with
feedback / 84.78 W without**. Both are honest numbers on the same probes §57 used
(the composed one is peak-derived, so a squarer waveform reads higher than the
pure-sine ceiling). The §42 criterion "the smallest scale that reaches rated
power" was therefore **not usable** here either, and unity was justified by the
sweep above instead. Open item; **do not close it by re-inventing a screen filter,
softening the grid coupling or raising `kVsupply`** — the same instruction §57.3
leaves for the OR120's 93 W, and the same suspects.

`kFullScaleSecV = 42.99` is DERIVED by measurement on the composed amp (the §23
convention — every voice is normalized to its own cranked peak): cranked, a
220 Hz probe peaks at **0.8999**. The NFB tap reads the real secondary volts,
never this, so the loop gain is independent of it.

### 63.7 Knob authority — no dead UI

Added for the reason §57.10 found the hard way: the mid-notch metric alone cannot
see a collapsed pot.

| control | travel | at |
| --- | --- | --- |
| BASS | **+9.77 dB** | 82 Hz |
| MIDDLE | **+8.93 dB** | 650 Hz |
| TREBLE | **+10.80 dB** | 5 kHz |

GAIN across its travel, measured as preamp output on a 0.01 V input: **−39.7 /
−12.8 / +4.8 / +18.4 / +21.1 dB** at knob 0.1 / 0.3 / 0.5 / 0.7 / 1.0 — monotone,
and the span is that of TWO ganged dividers in series.

### 63.8 Antialiasing, DC and the rest

* **Alias floor**, the house composed probe (4186 Hz at 0.3 V into a fully cranked
  amp — the same stimulus `test_jcm800_power.cpp` and §57.7 use, so the numbers
  are comparable):

  | factor | 48 kHz | 44.1 kHz |
  | --- | --- | --- |
  | 1× | −27.6 dB | −24.6 dB |
  | 2× | −27.3 | −26.5 |
  | **4× (shipped)** | **−80.1** | **−52.7** |
  | 8× | −92.1 | −64.0 |

  **48 kHz passes the −56 dB bar with 24 dB to spare; 44.1 kHz fails it at −52.7
  and is an XFAIL (`rockerverb-alias-44k1`), not a loosened bound.** This is the
  SAME defect the OR120 registered in §57.7 (`orange-schematic-alias-44k1`,
  −50.8 dB at the same rate and factor), and two amps failing the same bar at the
  same rate — both running per-triode oversampling domains into a separately
  oversampled power section — is the architecture speaking. It is genuine foldover
  rather than §54's rail-clipping signature: it improves **11.3 dB going to 8×**
  and beats 1× by **28.1 dB**. The "4× must beat 1× by ≥ 12 dB" clause stays HARD
  at both rates. The named fix is one shared oversampling domain around the whole
  preamp+power cascade — §57.13's own candidate — **never a lower bar**.
  `clipper_rockerverb_tests` therefore registers a ledger: core ctest **32 → 34
  entries**, repo ledgers **5 → 6**.

  > **AMENDED 2026-08-01 — §63.14 FIXED THIS AND THE XFAIL IS GONE.** One shared
  > oversampling domain around the whole cascade takes the composed cranked 4×
  > floor to **−72.3 dB at 44.1 kHz and −84.9 dB at 48 kHz**, against the
  > UNCHANGED −56 dB bar, which is now a hard assert at BOTH rates. At an ordinary
  > crunch setting (GAIN 0.50 / VOLUME 0.10) it goes **−61.1 → −115.1 dB** at
  > 44.1 kHz. `clipper_rockerverb_tests` has **zero** known-bad properties and its
  > ledger registration came off: core ctest **35 → 34 entries**, repo ledgers
  > **6 → 5**. The paragraph above's "two amps … is the architecture speaking" is
  > **half right and was corrected by measurement**: the identical change makes the
  > OR120 *worse* and its XFAIL stays (§57.7's amendment).
* **DC offset ON SIGNAL** (§29 / `support/DcOffset.h`), GAIN 0.7 / VOLUME 0.7,
  220 Hz: **0.2152 % of peak** with a clean input and **0.2157 %** with +0.1 V of
  DC on the input — the +0.1 V case is the one that makes the assertion able to
  fail.
* **reset() + ragged blocking**: a whole-buffer render vs the same render after
  `reset()` in 128-frame blocks differs by **0.000e+00**; every sample finite.
* **Rate independence**: −5.236 / −5.237 / −5.242 / −5.243 dBFS over 44.1 / 48 /
  88.2 / 96 kHz — a spread of **0.007 dB**, THD 35.93–35.95 %.
* **Latency 360 samples (7.50 ms at 48 kHz)** — four per-stage preamp domains at
  72 each plus the power section's 72. This is the deepest cascade of any voice
  here, and it is the second reason the shared-OS-domain slice is worth doing.
  **AMENDED 2026-08-01 (§63.14): 360 → 72 samples (1.50 ms), the shared domain's
  own figure, and it is now a HARD ASSERT against `Oversampler`'s documented
  0 / 64 / 72 / 76 rather than read back from the amp** — so re-arming an inner
  domain fails `testOneOversamplingDomain` even if every spectral bar passes.
* **CPU**: see §63.12.
* **Denormals** (§33, ADR 006). The FMV stack's **six** cap companions and each
  interstage network's up-to-four all rest at exactly zero, so all are guarded;
  after being driven then silenced for 4 s, `maxAbsRestingState()` measures
  **exactly 0.0** on the stack and on all three interstage networks. The power
  section deliberately exposes no accessor, for the reason `Jcm800PowerAmp.h`
  gives verbatim: every state in it idles at a real operating point.

### 63.9 The cab — REUSED, and decided by measurement

The Rockerverb 100 is a head sold against the same PPC-style 4×12 the OR120 is, so
this voice ships **no new cab**: it defaults to `orange412` (built-in cab 2, §57.8)
and the app hints at it exactly as the OR120's does. That is a real decision, and
the measurement that supports it is §63.4(b): the composed contrast against the
OR120 is **3.50 dB through the SAME cab**, i.e. the difference between these two
amps lives entirely in the heads, and a second cab would only add a second,
unrelated EQ opinion on top of it. Nothing in the native `CabChoice` space moves,
so §57.11's divergence-at-2 trap is not touched at all.

### 63.10 Wiring — both fronts, and NO new param id

**This voice needs no new parameter.** Its GAIN and its post-tone-stack VOLUME
mean exactly what the JCM800's GAIN (10) and MASTER (12) mean to a player, and its
BASS/MIDDLE/TREBLE are the shared tone ids — it is the first Orange in this repo
with a mid control. §57 gave the F.A.C. a new id because no other voice had a
six-position switch; here the opposite reasoning applies, and the two together are
the house rule: **reuse a slot when the FUNCTION matches, take a new one when it
does not.**

* **C ABI**: voice **5** (`kAmpRockerverb`), cab 2. GAIN → `kAmpParamJcmGain`
  (10), VOLUME → `kAmpParamJcmMaster` (12), BASS/MIDDLE/TREBLE → 1/2/3,
  REVERB → 9. The `presence` slot (11) and the F.A.C. (13) never reach it, and it
  never reads slot 0.
* **Web**: `params.ts` (`AMP_MODEL_INDEX.rockerverb = 5`), `rig.ts` (`AmpType` +
  the migration + `AVAILABLE_AMP_TYPES`), `audio.ts`, `Amp.tsx`
  (`RockerverbFace` — GAIN · BASS · MIDDLE · TREBLE · VOLUME · REVERB),
  `Board.tsx`, `App.tsx` (the cab hint), `amp.css`. **The accent is the OR120's
  `--accent-orange`, deliberately** — it is the same manufacturer and the same
  tolex wink; the identity that matters is the control row.
* **Native**: `ClipperEngine` (no new `Params` field at all), `PluginProcessor`
  (`kAmpModelChoices` **appended** — the index is stored in host automation and in
  saved sessions, so inserting one would silently re-voice every saved rig),
  `PluginEditor` (`case 5`). `identical_core_test` gains a sixth board case,
  `TS → Squash → Rocker Verb`, at **0.000e+00** — and that case sets `volume` and
  `jcmPresence` to non-default values on purpose, because this voice must ignore
  both and the two renders would diverge if the wrap ever routed them.
* **Assistant**: `set_amp` `'rockerverb'`, plus a `SYSTEM_PROMPT` block that
  coaches what actually matters — that GAIN and VOLUME are independent, that the
  master is LINEAR so 5–15 is a room level, that the amp is gainy enough to crunch
  by 10–15, and that it has a MID where the OR120 does not.

**The PANEL WORD and the SLOT deliberately disagree in one place, and it is
documented in three:** the knob printed **VOLUME** binds to the **master** slot,
because it sits after the tone stack and is a master by function. Same doctrine
§57.11 records for the OR120 printing GAIN on slot 0 — the label is per voice, the
slot is not, so rig JSON, testids, host automation and the C ABI are untouched.

**Web spec**: the acceptance bar is NOT reproduced in Playwright, for §57's
measured reason (the worklet form needs many `OfflineAudioContext`s and Chromium's
documented silent-render flake makes such a bar a coin flip). The web spec asserts
the DELIVERY PATH instead — voice 5 reachable, and param id 12 moving the level
**> 6 dB at a fixed GAIN** (the core measures 19.89 dB) — behind a HARNESS GATE
that fails first and by name if a render comes back silent.

### 63.12 CPU, and where this voice sits

`clipper-bench`, 8 s riff at 48 kHz in 128-frame blocks, same session and same
machine (absolute columns are machine-dependent — §35's rule — so the only
defensible statement is the interleaved comparison):

| unit | × realtime | % of one 48 kHz stream |
| --- | --- | --- |
| **rockerverb** | **2.38×** | **42.04 %** |
| ac30 | 2.74× | 36.51 % |
| jcm800 | 2.79× | 35.89 % |

The Rockerverb is the most expensive amp in the lineup, and the reason is
structural rather than wasteful: it is the only voice with **four** oversampled
triode stages, each in its own 4× domain, feeding a fifth (the power section's).
That is the same fact behind its 360-sample latency and behind the 44.1 kHz alias
XFAIL, and the same slice fixes all three.

### 63.13 Perturbation proofs

Patch one constant or one line of topology in a scratch copy, `touch`, rebuild,
confirm RED, restore FROM THE SCRATCH COPY, `touch`, rebuild, confirm GREEN —
never `git checkout --` and never `git stash`, both of which have destroyed work
in this repository.

| # | perturbation | result |
| --- | --- | --- |
| P1 | tone-stack slope `kRslope` 39 k → 3.9 k | RED — `orNotch − rvNotch > 5.0`, the network bar |
| P2 | treble cap `kCt` 560 p → 47 n | RED — `rvNotch < −3.0`, the FMV's own sign |
| P3 | VOLUME pot `kRV` 1 M → 1 k (a collapsed master) | RED — the level-match gate inside the master-volume bar |
| P4 | **the GAIN GANG broken** (gang 2 pinned at noon) | RED — the breakup-onset window |
| P5 | S3→S4 divider `Rgnd` 220 k → 2M2 | **GREEN on the first pass — see below** |
| P5′ | the same, after the fix | RED — the absolute interstage-divider window |
| P6 | **LTP made SYMMETRIC** (`Ra2` 120 k → 100 k) | RED — `bal > 0.90` (measures 0.828) |
| P7 | MID pot made a RHEOSTAT (lower section shorted) | RED — the tone stack vs its own H(jω) |
| P8 | global NFB disconnected (`kFeedbackBeta` → 0) | RED — the composed mid-notch contrast |
| P9 | EL34 bias −47 → −56 V (a cold quad) | RED — the plate-dissipation window |
| P10 | supply dropper R9 10 k → 100 k | RED — the absolute dropper window |
| — | restore | GREEN |

**P5 IS THE RESULT, not the footnote.** Flattening the transcribed 470 k / 220 k
divider into S4's grid to 470 k / 2M2 — a 2.6× change in how hard the cold stage
is driven — left **every bar in the suite GREEN**. The reason is the one §29
states and §57.10 hit from a different angle: an `H(jω)` check compares a network
against its OWN netlist and therefore cannot see a wrong component value, and
every other bar in this suite is either scale-free (the mid-notch metrics) or
measured somewhere the change happened to land inside a window.

Fixed the way the convention requires — **by adding an ABSOLUTE reference, not by
narrowing a bar around the perturbation**. Each interstage network's mid-band
magnitude is now compared against a number **written out in the test from the
transcribed resistors** rather than read back from its `Config`:

| network | transcribed | measured | window |
| --- | --- | --- | --- |
| S1→S2 (R30 220 k · R1 220 k ∥ GAIN 1 M · 470 p · noon wiper) | — | **0.04598** | ±25 % |
| S2→S3 (R31 220 k · R32 470 k ∥ GAIN 1 M · noon wiper) | — | **0.06626** | ±25 % |
| S3→S4 (R6 470 k · R5 220 k, no pot) | 220/(35.1+470+220) = **0.3034** | **0.30293** | **±8 %** |

The two GAIN networks carry a pot and a taper, so their windows are loose; the
fixed divider has nothing free in it, so its window is tight. At `Rgnd` = 2M2 that
network reads **0.814** and at 22 k it reads **0.042** — both far outside.
### 63.11 Named follow-ups

* **THE CLEAN CHANNEL** and its footswitch. It needs a netlist; the `.schx` this
  voice is transcribed from covers the dirty channel only. Do not invent it.
* ~~**The 44.1 kHz alias floor**, XFAIL `rockerverb-alias-44k1` (§63.8).~~
  **CLOSED 2026-08-01 (§63.14)** by one shared oversampling domain. The OR120's
  twin entry stays and is re-owned to its cathodyne (§57.7's amendment).
* **`TriodeStage` independent input/output coupling configs** (§63.3), which would
  give this voice its preamp grid blocking back without the doubled corner.
  **NOT the cause of "squashed" — refuted by measurement in §63.14** (the amp
  reproduces 25.52 dB of a 26.0 dB input span at GAIN 0.30), so this is a fidelity
  item, not a field-report item. It is still shared with four amps and four
  goldens; do it on its own.
* **Per-voice amp knob DEFAULTS** (§63.14). This voice inherited
  `AMP_KNOB_DEFAULTS`' JCM values — GAIN 0.5 / master 0.4 — and both are past this
  amp's wall. Blocked on nothing but scope: it touches `rig.ts`, `Amp.tsx`, the
  native APVTS and the three literal rig-JSON Playwright fixtures, so it wants its
  own slice. The numbers to use are in §63.14's wiper table.
* **The ~93 W ceiling** against the rated 100 W (§63.6).
* **The Attenuator**, which is a post-OT load device and belongs after the OT.
* **The pots' taper laws** — the file gives the letter, not the curve; and the
  VOLUME pot's LINEAR marking is the file's, which is worth confirming against a
  factory sheet before anyone re-tapers it.
* A native `rockerverb` snapshot scene for the headless screenshot suite.

### 63.14 The 2026-08-01 OWNER FIELD REPORT — one fix, three refutations

*Date: 2026-08-01 · Branch `claude/rockerverb-field-report-6f557i` ·
`docs/work/2026-08-01-rockerverb-field-report.md`*

The report, verbatim, on a voice that was hours old:

> *"the rockverb is a bit tinny/squashed compared to the sunshine stack/orange
> clone in logic pro" · "a touch brittle almost. it does have orange
> characteristics overall" · "the gain needs scaling back too 100 is totally
> unusable" · "too noisy, too much gain."*

**RULE ZERO, applied throughout: the Sunshine Stack is another vendor's model, not
a reference unit.** It is used below only as a description of a symptom. §57's rule
governs — every change is justified by the circuit, the netlist or a measured
defect, and where none exists the answer is *"this is what the netlist does"*.

Everything was measured BEFORE anything was changed, because §43's field report
said "the RAT" and measured as the Muff, and §51's reported knob position was not
where the THD onset actually was.

#### 63.14.1 "brittle" + "too noisy" — ONE defect, and it is FIXED

This project synthesises no noise anywhere (§59 says so in terms), so "too noisy"
can only be foldover, unbounded HF products, or amplified input noise. It is the
first: `rockerverb-alias-44k1` (§63.8), and Logic's default grid is 44.1 kHz.

**The fix is the one §63.8 and §57.13 both named — ONE shared oversampling domain
around the whole preamp+power cascade, never a lower bar.** `RockerverbAmp` now
owns a single `Oversampler`; the preamp and the power section are prepared **at the
oversampled rate** with their own resamplers set to **1×**, which `Oversampler.h`
documents as an exact pass-through with no filtering and no delay. That is why the
change needed **no** edit to `TriodeStage`, `RockerverbPreamp` or
`RockerverbPowerAmp` — they are simply clocked at 192 kHz instead of 48 kHz — and
therefore **no shared code was touched and no golden could move**. The three
interstage MNAs and the FMV stack come inside the domain with them, which is a
strictly better discretization for them, not a worse one.

Composed cranked alias floor (the house probe, 4186 Hz / 0.3 V):

| factor | 44.1 kHz before → after | 48 kHz before → after |
| --- | --- | --- |
| 1× | −24.6 → −24.6 | −27.6 → −27.6 |
| 2× | −26.5 → −27.3 | −27.3 → −27.8 |
| **4× (shipped)** | **−52.7 → −72.3** | **−80.1 → −84.9** |
| 8× | −64.0 → −88.6 | −92.1 → −78.6 |

…and at an ORDINARY CRUNCH SETTING, which is where a player lives and where the
five-domain arrangement was quietly worst — GAIN 0.50 / VOLUME 0.10, 4×:

| | 44.1 kHz | 48 kHz |
| --- | --- | --- |
| before | −61.1 dB | −71.5 dB |
| after | **−115.1 dB** | **−96.2 dB** |

**The 10.4 dB rate penalty at a normal setting REVERSES.** The −56 dB bar was not
touched; it is now a **hard assert at both rates**, plus a new absolute −80 dB bar
on the realistic setting, plus the unchanged "4× must beat 1× by ≥ 12 dB" clause.
`rockerverb-alias-44k1` XPASSed, was **deleted**, and its property is asserted for
real. `clipper_rockerverb_tests` registers no ledger: core ctest **35 → 34
entries**, repo ledgers **6 → 5**.

**Latency 360 → 72 samples (7.50 → 1.50 ms at 48 kHz)** — five 4× domains became
one. That is asserted too, against `Oversampler`'s own documented 0 / 64 / 72 / 76
rather than read back from the amp, and the shipped 4× default is pinned (§58.7
found a bar that could not fail because nothing pinned a default factor).

#### 63.14.2 THE SAME CHANGE MAKES THE OR120 WORSE — §57.7's attribution refuted

§57.7 concluded that "two amps failing the same bar at the same rate … is the
architecture speaking". **Half of that is now measured to be false.** The identical
change was built and run on `OrangeAmp` as well:

| composed cranked 4× | 44.1 kHz | 48 kHz |
| --- | --- | --- |
| Rockerverb before → after | −52.7 → **−72.3** | −80.1 → **−84.9** |
| OR120 before → after | −50.8 → **−48.7** | −73.0 → **−67.8** |

So the OR120 half was **REVERTED** — `core/src/dsp/OrangeAmp.cpp` is byte-identical
to its pre-slice state; only its header banner and the XFAIL's `fix` string
changed, both to record this. **The mechanism, measured rather than assumed:**
removing the intermediate band-limitings lets each stage's own products reach the
NEXT nonlinearity unfiltered. That is a large win when the later stages are triodes
(this amp's four) and a loss when the dominant nonlinearity is a hard rail — and
the OR120's cathodyne clips on **compliance** (Vk pinned to [0, C+/2], §57.3), the
hardest clipper in the lineup. `orange-schematic-alias-44k1` is re-owned to the
**cathodyne**, not to the domain layout. Still never a lower bar.

#### 63.14.3 "squashed" is NOT §63.3's missing grid blocking — REFUTED

§63.3 names preamp grid blocking as this voice's one modelling departure, and
"squashed" is exactly what an un-bloomed preamp sounds like. It is not the cause.
Dynamic range, 220 Hz, VOLUME 0.10, input swept 0.02 → 0.40 V (a **26.0 dB** span):

| GAIN | output span | pick attack re sustain (input 2.37 dB) |
| --- | --- | --- |
| 0.30 | **25.52 dB** | 3.38 dB |
| 0.50 | 10.45 dB | 0.24 dB |
| 0.70 | **1.01 dB** | 0.19 dB |
| 1.00 | **−0.04 dB** | — |

**The amp has essentially perfect touch sensitivity at GAIN 0.30 and none at all at
0.70.** The squash is not a missing mechanism, it is where the knob puts you; grid
blocking would put a bloom-and-recover on top of a brick wall, not restore 25 dB of
range. **`TriodeStage` was therefore NOT touched**, which is also why the four
valve goldens were never at risk. §63.11's entry for it is re-scoped to a fidelity
item.

#### 63.14.4 "too much gain / 100 unusable" — CONFIRMED as a symptom, and the taper CANNOT fix it

Measured in **wiper** space, which is taper-independent (220 Hz / 0.15 V, VOLUME
0.10; the knob column is where the shipped k = 4 law puts each wiper):

| wiper | knob (k=4) | THD % | RMS dBFS | dyn range (26 dB in) |
| --- | --- | --- | --- | --- |
| 0.002 | 0.026 | 1.06 | −72.50 | 25.99 |
| 0.010 | 0.107 | 1.46 | −44.54 | 26.01 |
| 0.030 | 0.240 | 4.89 | −25.38 | 26.33 |
| 0.050 | 0.326 | 9.84 | −16.26 | 23.98 |
| 0.080 | 0.416 | 15.86 | −10.22 | 17.02 |
| 0.120 | 0.501 | 26.41 | −9.00 | 10.33 |
| 0.200 | 0.615 | 29.57 | −8.33 | 2.63 |
| 0.500 | 0.831 | 32.36 | −8.34 | 0.04 |
| 1.000 | 1.000 | 34.31 | −8.55 | −0.04 |

**Above wiper 0.20 the output moves 0.22 dB** — so with the shipped taper, knob
0.615 → 1.00 (**38 % of the travel**) is level-dead, and by a 1 dB level-ceiling
criterion it is dead from knob **0.50**. The complaint is real and this table is it.

**But the taper is not the lever, and that is a measured refutation of the obvious
§51-style fix:**

* The house law `(e^{kx}−1)/(e^k−1)` puts the wiper at `1/(e^{k/2}+1)` at half
  rotation. **k = 4 → 11.9 %, inside the 10–20 % audio-taper spec §58 established
  as this repo's reference.** k = 8 → **1.8 %**, which is not an audio pot. The
  most the spec allows is k = 4.394 (10.0 %), which moves the level ceiling from
  knob 0.500 to **0.537** — nothing.
* The geometry is fixed and no monotone map changes it: clean→onset is **23.5 dB**
  of the useful span and onset→saturated is **16.5 dB**, so any taper that puts
  saturation at the top of the travel puts the ≥5 % THD onset at ~59 % of it.
  Measured: k = 8 → onset **0.56**, k = 10 → **0.65**, k = 12 → **0.71**. A
  Rockerverb dirty channel that is clean at GAIN 6 is a worse model than one that
  is dirty at 3, and §63.5's own shipped window (0.08 < onset < 0.35) forbids it.

**So no taper change ships, and §51's method does not apply here.** §51 could take
the owner's sentence as the design equation because its constraint (`taper(1) = 1`,
"100 is perfect") was compatible with the circuit; here the sentence is *"100 is
unusable"*, and the circuit says 100 is a wall because **two ganged 1 MΩ log pots**
in front of a **four-stage** preamp are what the netlist has.

**What IS wrong, and is NOT this slice's to fix: the DEFAULTS.** The voice inherited
`AMP_KNOB_DEFAULTS`' JCM values wholesale — **GAIN 0.5 / master 0.4** — although
§63.5 documents that its VOLUME is a **linear** master whose useful range is 5–15,
and the table above puts GAIN 0.5 at 26 % THD with 10 dB of dynamic range left.
**The amp opens at the wall**, and that is the single best explanation for a first
impression of "too much gain / squashed". Per-voice defaults touch `rig.ts`,
`Amp.tsx`, the native APVTS and three literal rig-JSON Playwright fixtures, and
`setAmpType` deliberately does not reset knobs — so it is named as its own slice
(§63.11) rather than smuggled in here. **The assistant prompt was extended in the
meantime** (the §48 / §63.10 precedent): it now coaches the measured GAIN geometry
alongside the VOLUME 5–15 it already carried.

#### 63.14.5 "tinny" is a BASS DEFICIT, not a treble excess — and it is the netlist

Composed spectral tilt, clean level, tone knobs noon, dB re each amp's own 660 Hz:

| f | Rockerverb | OR120 | JCM800 |
| --- | --- | --- | --- |
| 82.41 Hz | **−14.79** | −23.33 | **−3.36** |
| 110 Hz | −10.07 | −20.17 | −3.14 |
| 220 Hz | −3.09 | −11.13 | −2.66 |
| 660 Hz | 0.00 | 0.00 | 0.00 |
| 2.2 kHz | +6.05 | +3.15 | +7.15 |
| 4.4 kHz | **+8.37** | +2.81 | **+10.15** |
| 6 kHz | +8.88 | +1.96 | +10.64 |

**The Rockerverb is LESS bright than the JCM800 at the top** and **11.4 dB thinner
at low E.** A preamp-only probe puts all of it in the preamp (−14.82 dB at 82 Hz at
GAIN 0.3), i.e. the transcribed **1 n / 2n2 / 4n7** interstage cascade — corners at
398 / 134 / 49 Hz, whose analytic product at 82 Hz re 660 Hz is ≈ −19 dB before the
FMV's own bass lift gives some back. **Not touched.** The BASS control has
**+9.77 dB** of authority at 82 Hz to answer it (§63.7) — the same disposition §57
records for the OR120's 330 p.

#### 63.14.6 The netlist was RE-VERIFIED, independently, because two complaints rest on it

`dsharlet/LiveSPICE` was re-cloned and `Tests/Examples/Orange Rockerverb 50
Preamp.schx` re-parsed from scratch:

* **`C1 = 1 nF` — CONFIRMED.** The single largest audible consequence in the voice
  is transcribed correctly, so §63.14.5's bass deficit is the amp's and not a parse
  error.
* **The MIDDLE pot is NOT a rheostat — CONFIRMED**, by re-deriving the terminal
  geometry rather than trusting the first pass: a LiveSPICE `Potentiometer` has
  anode (−10,−20), cathode (−10,+20) and wiper (+10,0); `Middle` sits at (470,100)
  with `Rotation="-10"` (≡ 180°) and `Flip="true"`, and the flip resolves as
  y → −y, which puts its anode at (480,80) on the BASS pot's cathode, its cathode
  at (480,120) on the ground/output-reference node, and its **wiper** at (460,100)
  on C26. §63.2's transcription stands.
* **A consequence of that, newly measured and REPORTED:** because C→GND is 25 k at
  every position instead of a canonical FMV's rheostat, the stack's mid-band
  insertion loss is **−13.24 dB at 1 kHz** (−14.55 at 660 Hz) where a canonical
  Marshall FMV at noon is ~−20 dB. **That is ~7 dB of "too much gain", and it is
  the netlist's, not a fit.**
* **Both GAIN pots carry `Wipe="0.6"`** in the file where every other pot carries
  0.5 — the file's own saved position. Noted as weak evidence; not used.
* **Still open and unresolvable from this container:** the file is a Rockerverb
  **50** preamp and this voice ships as a **100**. No reachable source settles
  whether the two preamps are identical. Recorded, not guessed.

#### 63.14.7 CPU: NO measurable change, and the reason is honest

Interleaved same-machine A/B (§35's rule — absolute columns are machine-dependent),
`clipper-bench`, 6 alternating pairs of the same binary pair, % of one 48 kHz stream:

| | runs | median |
| --- | --- | --- |
| AFTER (one domain) | 46.03 / 46.13 / 46.91 / 39.88 / 39.46 / 47.04 | **46.08 %** |
| BEFORE (five domains) | 47.93 / 39.68 / 48.19 / 47.36 / 48.98 / 46.22 | **47.65 %** |

The 1.6-point difference sits inside a **7.6-point within-binary spread**, so the
defensible statement is **no measurable change**. That is not a disappointment, it
is the mechanism: eight halfband passes were deleted, and the three interstage 4×4
MNAs and the FMV stack's 8×8 now run at 192 kHz instead of 48 kHz. The two roughly
cancel. Latency, which does move 5×, is the win.

#### 63.14.8 Perturbation proofs

Patch in a scratch copy, `touch`, rebuild, confirm RED, restore FROM THE SCRATCH
COPY, `touch`, rebuild, confirm GREEN. Never `git checkout --`, never `git stash`.

| # | perturbation | result |
| --- | --- | --- |
| P1 | `RockerverbAmp` reverted to the five-domain arrangement (the whole fix) | **RED** — the realistic-setting bar first (48 kHz reads −71.5 dB against −80) |
| P1b | the same, with the realistic assert lifted so the next bar can be reached | **RED** — `f4 < -56.0` at 44.1 kHz, measuring **−52.7 dB** |
| P2 | the shared domain KEPT but the inner power-section domain re-armed (`power_.setOversampling(2)`) | **RED** — `testOneOversamplingDomain` (latency 64 at 1×, expected 0). **Every alias bar still PASSES** (44.1 kHz 4× reads −75.5, realistic −117.1), which is the isolation: the latency bar catches a domain-layout regression the spectrum cannot see |
| — | restore | **GREEN** |

P1 and P2 between them prove all three new/changed bars independently. The bar that
would have been the "could not fail" candidate — the latency assert — is written
against `Oversampler`'s **documented** 0 / 64 / 72 / 76 rather than read back from
`RockerverbAmp`, which is exactly the identity §58.7 and §57.10 each caught once;
P2 confirms it fails when it should.

#### 63.14.9 Scope, and what did NOT move

* **ALL FIVE GOLDENS UNCHANGED at ±0.00** — `rat_jcm800`, `sd1_twin_reverb`,
  `muff_twin`, `ts_ac30`, `clean120_chorus`. Nothing blessed, nothing written. The
  change touches no shared class, which is why this is a construction guarantee
  rather than luck.
* Every other Rockerverb bar is unmoved: the two acceptance halves (network
  contrast 6.76 dB, composed 3.50 dB; master-volume ratio 15.76×), the LTP balance
  0.971988, knob authority, loop depth 6.65 dB / 0.26 dB spread, the breakup table,
  rate spread **0.007 dB** over 44.1–96 kHz. DC on signal moved 0.2152 % →
  **0.3645 %** of peak (and 0.3646 % with +0.1 V of input offset) — still an order
  of magnitude inside its bar.
* **Not done, deliberately:** the taper (§63.14.4), `TriodeStage` (§63.14.3), the
  interstage caps (§63.14.5), the OR120 (§63.14.2), and per-voice defaults
  (§63.11). `RatModel` / `DiodeClipperADAA` / §66 / ADR 027 belong to a parallel
  slice and were not touched.

## 64. M13.3 — the "Lumen" optical compressor (the SECOND dynamics voice)

The lineup's second compressor, and deliberately not the first one with different
knobs: a Teletronix LA-2A-style **electro-optical leveling amplifier** in pedal
form, shipping as pedal type `opto`. Three controls, because the reference has
three that all change the audio. Files: `core/include/clipper/dsp/OptoCell.h`
(NEW — the photocell as a shared component), `core/include/clipper/dsp/OptoModel.h`
+ `core/src/dsp/OptoModel.cpp`; tests `core/tests/test_opto_model.cpp`
(`clipper_opto_tests`); C ABI `opto_*`; `--pedal opto` in the render CLI; a `lumen`
row in `clipper-bench`; worklet `opto` dispatch; `rig.ts` / `Pedal.tsx` /
`pedal.css` / `tokens.css` / assistant; native `ClipperEngine` + APVTS +
`PedalCard` + a ninth `identical_core_test` board. Trademark-safe throughout (no
Teletronix / UREI / Universal Audio / LA-2A text on any user surface; the wordmark
is "Lumen", the model line `DYNAMICS Nº2 · LEVELER`).

### 64.1 Research — what was sourced, what was reconstructed

Same channel as §59 and §61: **the proxy permits github.com only**, `WebFetch`
returned 403 for every other host tried (`proreplicas.com` was the probe), and a
GitHub code search for an LA-2A netlist or `.asc` found nothing usable. So there
was no LTspice transcription to parse the way §59 had one, and **NO SCHEMATIC WAS
READ.** The channel was search-result extracts.

**SOURCED — used as given, and each one is load-bearing somewhere below:**

| # | fact | where it lands |
| --- | --- | --- |
| S1 | The T4B module is an **EL panel plus two Clairex CL-505L CdS photoconductive cells** (one for gain reduction, one for the meter). Only the GR cell is modelled. | `OptoCell` |
| S2 | The panel is driven by the **audio itself**, filtered and stepped up to **no more than ~90 V peak**. | `kPanelClampV`, and the reason the panel is the rectifier |
| S3 | The CL-505L's resistance runs **under 1 kΩ lit to over 1 MΩ dark**. | `rLightOhms`, `rDarkOhms` |
| S4 | The **photocell is the bottom leg of a voltage divider**; lower cell resistance = lower signal. | the whole gain-reduction topology |
| S5 | A **2.2 MΩ bleeder** sits across both legs of the EL panel. | `kPanelBleedHz` |
| S6 | CdS resistance follows illumination as a **log-log power law**, `R ∝ E^−γ`, with γ between **0.92 (dim) and 0.58 (bright)**; datasheets define γ over a 10:1 lux decade. | `cellGamma` |
| S7 | EL brightness "increases in proportion to the **second to the third power** of the voltage". | `elExponent` |
| S8 | Published performance: **attack 10 ms**; **release ~0.06 s for 50 %, 0.5 to 5 s for complete release depending upon the amount of previous reduction**; **0 to 40 dB gain limiting**; response +0/−1 dB from 30 Hz; under 0.5 % THD. | the acceptance bars |
| S9 | The **COMPRESS/LIMIT switch changes the sidechain SOURCE**: COMPRESS shorts the sidechain to the output (100 % feed-back), LIMIT feeds it roughly **1/25 of the input plus 24/25 of the output**. *(This one is a forum reading of the schematic, not a factory sheet — weaker than the rest, and flagged as such.)* | `kLimitFeedForward`, and the reason MODE ships |
| S10 | The unit's widely-quoted ratio is **"about 3:1"**, and it is program dependent rather than switchable. | the ratio prediction's check |
| S11 | The manual also describes the static curve as compressing from a **−30 dB breakaway up to −20 dB**, above which "input increases of an additional 20 dB result in output increases of less than 1 dB". | **NOT reproduced — see §64.5** |

**RECONSTRUCTED — this model's own, chosen under stated constraints, and §57's
rule applies verbatim: do not re-tune any of them toward a sound; find the
schematic.**

* Every resistor in `OptoModel.cpp` except the two derived ones. `kSeriesOhms` and
  the panel ceiling are **derived from published figures in the open** (S3 + S8
  give Rs = 1 kΩ·(10^2 − 1) = 99 kΩ → the 100 kΩ standard value); `kAmpGain`,
  `kHeadroomV`, `kGainRangeDb`, `kSidechainMaxGain`, `kPeakRangeDb`, the two
  coupling corners and the panel's 20 nF are not.
* **The tube stages are not modelled.** The reference is a 12AX7 voltage amp into
  a 12BH7A cathode follower with input and output transformers, and a 6AQ5 driving
  the panel. This model ships them as a fixed linear gain plus one soft (ADAA)
  ceiling. Named simplification, §64.6.
* **The entire dynamic model of the cell.** No published T4B response curve — step
  response, recovery curve, or anything measurable — was reachable. The
  **structure** is standard photoconductor physics (a fast free-carrier branch, a
  slow trap-limited branch, and a trap occupancy that retards the slow one); the
  **constants** are pinned to S8's published behaviour. **Every curve in §64.4 is
  this model's own.**
* **No independent simulation was available to check against.** §59's strongest
  asset was an outside SPICE run that agreed to 4 % and caught that slice's one
  real bug. Nothing equivalent exists here, and the substitute is weaker: the
  ratio is *predicted* from S6 and S7 and checked against S10 (§64.5). That is one
  number, not a whole loop, and it is the biggest gap in this section.

### 64.2 The circuit, and why it has no envelope capacitor

```
in ─[Cin]──[Rs 100k]──┬──► x A0 ──[headroom]──┬──► GAIN pot ─[Cout]─► out
                      │                       │
                 R_cell (T4B)                 │  the sidechain taps HERE,
                      │                       │  BEFORE the GAIN pot
                     gnd                      ▼
        ▲                            MODE: COMPRESS = this node (feed-BACK)
        │                                  LIMIT    = 24/25 of it + 1/25 of the
        │                                             pedal's own INPUT
        │                                      │
        │                            PEAK REDUCTION pot -> sidechain amp
        │                                      │        -> step-up (<= 90 V pk)
        │                                      ▼
        └──────── OptoCell ◄── EL panel (2M2 bleeder across it)
```

**The panel is the rectifier and the cell is the integrator.** An EL panel emits
on both polarities of its drive, so `|v|` is physics rather than a modelling
convenience — and the consequence is that **this pedal has no rectifier, no
envelope capacitor and no envelope resistor anywhere in its control path.** The
time constants are the photocell's. That is the whole voice, and it is why §64.3
refuses the shared detector.

`OptoCell` (a header-only component, held by value) is:

```
    p       = elExponent * cellGamma                       (= 1.875 shipped)
    K       = (1/rLight - 1/rDark) / panelMaxV^p           DERIVED from S2+S3
    dG_ss   = K * |v_panel|^p                              the steady-state target
    dG      = g_fast + g_slow      g_fast -> a*dG_ss, g_slow -> (1-a)*dG_ss
    tau_slow_release = tauSlowRelease * (1 + (memMult - 1) * m)
    m_ss    = dG_ss / (dG_ss + dG_half)                    trap kinetics (Langmuir)
```

**The trap target's SATURATING form is load-bearing and was found by measuring the
first draft.** Written as a linear `dG/dG_max`, the occupancy sits at ~1e-3 at any
playable level (the cell's limit corresponds to the full 90 V drive), and the
program dependence measures **1.083×** — i.e. none. The Langmuir form
`m_ss = dG/(dG + dG_half)` is what trap kinetics actually give
(`dn/dt = c(N−n)·n_free − r·n`), it needs one constant (`trapHalfFraction`, the
occupancy half-point as a fraction of the cell's light limit) instead of an
arbitrary normalization, and it produces **both** dependences the reference is
described by: DEPTH, because `m_ss` rises with the light, and DURATION, because
`tauMemChargeSeconds` is of the order of a second.

### 64.3 THE TWO SEAM REFUSALS, both measured (ADR 025)

ADR 019 predicted M13.3 would be "a config of `CompressorEngine` plus one
`applyGainCell()` case". ADR 021 narrowed that to "reuse the DETECTOR, expect the
CONTROL side to be your own". ADR 023 added the procedure: **build the
substitution and measure it.** Both were done. Both come out **no**, and both are
reported rather than fixed by widening someone else's type.

**(a) It is not a config of `CompressorEngine`.** Seam by seam, against the
banner's own list of what M13.3 would "reuse as-is":

| engine struct | what it is | applies here? |
| --- | --- | --- |
| `InputStage` | a coupling corner + an emitter follower's gain | partially — a corner, no follower |
| `DriveNetwork` | the CA3080's differential input attenuator (2 poles, 2 zeros) | **no** — no such network exists |
| `GainCellSpec` | `Iout = Iabc·tanh(Vd/2Vt)`, returning a **current** | **no** — an optical cell is a **resistor in a divider** |
| `LoadStage` | what the cell's output current develops its voltage across, plus the CA3080's compliance | **no** |
| `Splitter` | the Dyna Comp's phase splitter | **no** — there is none |
| `Sidechain` | the clamp/rectifier + envelope integrator | **no** — see (b) |
| `ControlMap` | rheostat + series resistor + OTA bias pin | **no** (ADR 021 already said so) |
| `OutputStage` | coupling + level pot | **yes** |

One of eight. Making it fit would need `applyGainCell()` to stop returning a
current, `LoadStage`/`Splitter`/`compliance_` to be bypassed by a flag, and the
detector to become a kind — four axes whose two settings share no component, which
is the union-of-N-models ADR 021 forbids in terms. **`OptoModel` is therefore a
standalone model that HOLDS the component that really is shared** — which is
exactly the shape M13.6a's `GateModel` already took, one slice earlier, for the
same reason. `CompressorEngine` is the OTA pedal's engine; that is not a defect,
it is what it is, and the banner now says so.

**(b) `SidechainDetector` is not this pedal's detector — and the substitution was
BUILT AND RUN.** A replica of the shipped loop was written with one block
switchable (validated against the shipped model first: settled gain reduction
agrees to **0.05 dB** at −24 / −12 / 0 dBV), and given the detector's best shot:
the clamp network in the gate's op-amp precision-rectifier form, the envelope pair
pinned to the published 60 ms 50 %-release (`R_env·C_env` = 86.6 ms, using M13.1's
own 10 µF cap), and calibrated at ONE point (a −12 dBV tone landing at the shipped
gain reduction). Everything after that is a prediction.

ADR 023's metric first — the **proportional range**, the input dB span over which
the control travels 10 % → 90 % of its own swing, open loop on a 146.83 Hz tone,
all rows at the same 192 kHz rate:

| block | proportional range |
| --- | --- |
| M13.1's detector (10 µF / 150 kΩ) | **2.031 dB** *(§58.8 measured 1.950 on its own harness — the instrument agrees)* |
| the best opto-plausible detector config found (10 µF / 8.66 kΩ) | **8.901 dB** |
| **this pedal's EL panel + CdS cell** | **14.323 dB** *(10.179 dB analytic small-signal, `20·log10(9^{1/p})`; the difference is the sine's own rectification)* |

Then the damage, on the replica:

| in dBV | shipped GR | substituted GR | shipped ratio | substituted ratio |
| --- | --- | --- | --- | --- |
| −30 | 2.66 | **0.09** | 1.34 | 1.00 |
| −24 | 5.01 | 0.82 | 1.64 | 1.14 |
| −18 | 7.96 | 5.86 | 1.97 | **6.26** |
| −12 | 11.29 | 11.29 *(calibration point)* | 2.25 | **10.40** |
| −6 | 14.84 | 16.62 | 2.45 | 9.03 |
| 0 | 18.53 | 21.72 | 2.60 | 6.66 |
| +6 | 22.30 | 26.51 | 2.69 | 4.96 |

A smooth monotone rise toward the predicted 2.875:1 becomes a **non-monotone
10:1 wall** with **0.09 dB of gain reduction at −30 dBV** — the level S11 says the
reference has already started compressing at. And with the detector standing in
for the cell's dynamics as well, the acceptance property is simply gone: complete
release **0.364 s after a 150 ms stab and 0.364 s after a 6 s passage — 1.000×**,
the same figure M13.1 measures, because an RC is an RC.

So: **`SidechainDetector` was not widened, and this pedal does not consume it.**
The rule ADR 023 left ("both blocks rectify and integrate" is neither a reason to
share nor a reason to refuse) was followed literally in the other direction from
§58.8: this one refuses because the measurement says so, not because the block
diagrams differ.

**(c) What IS shared: `OptoCell`.** ADR 021's lesson applied on the way in rather
than after the fact — the photocell is a component that owns its own state, held
by value, with ROADMAP **M13.5's photocell-driven Uni-Vibe** as its named future
consumer. Do not grow it a `lightSourceKind` axis for that: a lamp's drive law is
a different function of a different quantity, and the thing to extract then is the
lamp.

### 64.4 THE ACCEPTANCE BAR — program dependence, and the OTA voice as the control

The bar is the property an RC detector cannot have at any coefficient. Identical
stimulus, identical measurement, both models: a 220 Hz tone at 0.30 V for 150 ms
and for 6 s, then silence; gain reduction traced every 1 ms; "complete release" is
the time from the end of the burst until the reduction falls under 0.5 dB — the
point at which a player hears the pedal let go.

| model | 150 ms burst | 6 s burst | ratio |
| --- | --- | --- | --- |
| **Lumen (PEAK 0.70)** | peak GR 12.17 dB, releases in **1.00 s** | peak GR 12.24 dB, releases in **2.88 s** | **2.892×** |
| Squash (SUSTAIN 0.80) | peak GR 18.93 dB, releases in 4.11 s | peak GR 18.95 dB, releases in 4.11 s | **1.000×** |

The two stimuli reach the same depth to **0.07 dB**, so the comparison is about
memory and not about level. The mechanism is asserted alongside the symptom: trap
occupancy at the end of the burst **0.0686 (short) vs 0.6163 (long)**.

**The bar is `> 2.0` for the optical voice and `< 1.25` for the OTA one.** The
margin is **recorded, not snugged**: 2.892 against 2.0 is 45 % of head-room, and
2.0 was chosen as "unambiguously more than measurement noise" before the number
was known to be 2.9. The cell's two release constants
(`tauSlowReleaseSeconds` 0.18 s, `slowReleaseMemMult` 15) were pinned to S8's
published 0.5–5 s window, **not** to this ratio, and the ratio is what fell out.

**The DEPTH axis, which is what S8's sentence literally describes** ("depending
upon the amount of previous reduction"). 4 s burst, PEAK REDUCTION swept:

| PEAK | peak GR | trap occupancy | 50 % release | complete release |
| --- | --- | --- | --- | --- |
| 0.30 | 3.89 dB | 0.2697 | 46 ms | **1.04 s** |
| 0.50 | 7.72 | 0.4468 | 75 ms | 2.04 s |
| 0.70 | 12.24 | 0.5903 | 291 ms | 2.82 s |
| 0.90 | 17.09 | 0.6965 | 638 ms | 3.42 s |
| 1.00 | 19.58 | 0.7378 | 823 ms | **3.67 s** |

Every point is inside the published 0.5–5 s, the span across the operating space
is **3.53×**, and both are asserted.

**50 % release: 47 ms** on a short burst at PEAK 0.50, against the published 60 ms,
asserted inside a 40–80 ms window. It lengthens with depth exactly as the two-stage
description says it should.

**Attack, and the bar that is a prediction rather than a constant read back.**
Measured as the manufacturer's spec means it — a 1 kHz tone stepping on, and the
time for the cycle-peak envelope to settle within 1 dB of its final value, with the
oversampler's latency skipped:

| PEAK | overshoot | attack |
| --- | --- | --- |
| 0.30 | +3.12 dB | **9.0 ms** |
| 0.50 | +6.20 | 10.0 |
| 0.70 | +8.89 | 10.0 |
| 1.00 | +11.17 | **9.0 ms** |

The absolute window (5–20 ms against a published 10 ms) is the weaker of the two
bars, because the cell's own attack constant is that number. The bar with teeth is
the **spread: 1.0 ms across the whole knob**, asserted `< 3.0`. §59.2 measured
M13.1's attack moving **14 → 3 ms** across SUSTAIN, because its attack is a
current-starved discharge; an optical attack is the cell's and does not move. A
3 ms window cannot contain the OTA behaviour.

### 64.5 The ratio is a PREDICTION from two published device exponents

Deep in reduction the divider's sensitivity `d(GR_dB)/d(V_panel_dB)` saturates at
`p = n·γ` — the EL exponent times the cell's gamma — so a feed-back loop settles at
a ratio of **1 + n·γ**. With the shipped mid-range values (n = 2.5 from S7's
"second to third power", γ = 0.75 from S6's 0.58–0.92) that is **2.875:1**, and the
published bracket the two source ranges allow is **2.16:1 … 3.76:1**, which
contains S10's "about 3:1".

Rendered static curve, PEAK 1.00 / GAIN 1.00, 220 Hz, 2 s settle:

| in dBV | −54 | −48 | −42 | −36 | −30 | −24 | −18 | −12 | −6 | 0 | +6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| out dBV | −34.46 | −29.16 | −24.67 | −21.02 | −17.98 | −15.30 | −12.86 | −10.55 | −8.33 | −6.15 | −4.02 |
| GR dB | 0.44 | 1.16 | 2.68 | 5.05 | 8.02 | 11.36 | 14.92 | 18.61 | 22.39 | 26.21 | 30.06 |
| ratio | — | 1.13 | 1.34 | 1.64 | 1.97 | 2.25 | 2.45 | 2.60 | 2.70 | 2.76 | **2.81** |

**Measured deep ratio 2.81:1 against the 2.875 prediction — 2.2 %.** A soft knee
that stiffens with depth, which is what players describe and what the divider's own
shape gives: sensitivity rises from 0 (a dark cell barely loads the divider) and
saturates at `p`.

**WHAT IS MISSED, reported per the honesty gate and NOT fitted:** S11's manual
description of the curve going "horizontal" above −20 dB, with 20 dB in giving
under 1 dB out (i.e. >20:1). This model does not do that at any setting — its
ratio ceiling is `1 + n·γ` and no choice inside the published exponent ranges
reaches 20:1. The two published claims (S10's ~3:1 and S11's near-brick-wall)
are not obviously consistent with each other either, and the model reproduces the
one the topology predicts. If a schematic turns up and the sidechain has a gain
stage that steepens with level, this is where it goes.

The divider's own ceiling is **40.09 dB** (Rs 100 kΩ over a 1 kΩ lit cell, the
`Rs` that S3 + S8 derive), and it is asserted twice — once analytically, and once
by **rendering a slammed input** and checking the reduction really gets there,
because the analytic line is an Ohm's-law identity on two constants and by itself
could not fail (§57.9's finding).

### 64.6 The three controls, and the honest expectations

**PEAK REDUCTION is a threshold; GAIN is a level.** Measured, and the contrast is
the three-way table this lineup can now print:

| pedal | its "amount" control moves… | …and its other axis moves |
| --- | --- | --- |
| Squash (§59.4) | GAIN by **25.33 dB** | settled output by 0.28 dB |
| Curfew (§61.4) | THRESHOLD by **40.00 dB** | open-state gain by 0.00 dB |
| **Lumen** | THRESHOLD by **45.44 dB** (the 3 dB-of-reduction point, +4.38 → −41.06 dBV, monotone) | — |
| **Lumen's GAIN** | output by **32.00 dB** | gain reduction by **0.0000 dB** |

The last row is the reference's arrangement and is a real property rather than an
identity: the sidechain taps *before* the GAIN pot, and moving the tap after it
makes that assertion fail.

**MODE ships, and §61.3 is why that needed a number.** The noise gate's MODE switch
was deliberately not shipped because it changes what the footswitch does rather
than the audio. This one changes the audio: LIMIT mixes S9's 1/25 of the pedal's
own input into the sidechain, so under reduction the sidechain still sees the
un-reduced peak. Measured at PEAK 1.00 / GAIN 1.00:

| in dBV | −24 | −12 | 0 | +6 |
| --- | --- | --- | --- | --- |
| COMPRESS out | −15.30 | −10.55 | −6.15 | −4.02 |
| LIMIT out | −15.86 | −12.27 | −10.98 | −11.73 |
| difference | −0.55 | −1.72 | −4.82 | **−7.71 dB** |

Asserted to be worth more than 3 dB somewhere, and to be **monotone in sign** —
LIMIT may never clamp *less* than COMPRESS.

**The honest expectations, measured and asserted in the direction that keeps
them.** They are largely the OPPOSITE of §59.6's, which is the point of having two:

* **The pick attack survives.** First 50 ms against settled: **+0.93 / +1.07 /
  +1.22 dB** at PEAK 0.50 / 0.70 / 1.00, i.e. the transient gets *through*. §59.6
  measured M13.1 losing **7.72–20.00 dB** of the same thing. The overshoot table in
  §64.4 says the same from the other side (+3 to +11 dB of un-compressed front).
* **It does not invert phase.** A divider into a non-inverting amplifier. M13.1
  does invert (its signal drives the OTA's inverting input), so a player stacking
  the two should know they disagree; the test correlates the output against the
  latency-aligned input and asserts the sign.
* **Turning it up does not raise the noise floor.** Idle gain on a −84.75 dBFS
  floor is **+4.69 dB at PEAK 0.00, 0.50 and 1.00 — 0.00 dB of travel**, because an
  opto at rest is a near-unity divider and the make-up gain lives on its own knob.
  §59.6 measured M13.1's moving **25.34 dB**, which is its famous hiss.
* **It is nearly tone-neutral**, and that is a claim worth pinning because it is
  what a leveling amplifier is for. Small-signal, dB re 1 kHz: 20 Hz **−1.29**,
  41 Hz −0.32, 82 Hz −0.08, 220 Hz −0.01, 5 kHz −0.03, 10 kHz **−0.12**. Worst
  deviation from 82 Hz up: **0.12 dB**. (§59.6's Dyna Comp: −2.42 dB at 82 Hz and
  −2.88 dB at 10 kHz.) The published +0/−1 dB from 30 Hz is met.
* **It is slow to let go**, by design — see §64.4. Fast staccato playing can hear
  the level still returning under the next note. That is the pedal.

**The documented simplification:** the reference's tube stages and transformers are
a fixed +20 dB gain plus one soft ADAA ceiling at 6 V. Its published distortion is
under 0.5 % THD, i.e. the amplifier is not meant to be a voice, so what is lost is
the transformer's low-end behaviour and the tube's own asymmetry at the top of the
swing. Stated here rather than buried; it is the largest modelling departure in the
file after the cell's dynamics.

### 64.7 Oversampling — the FIRST thing checked, and the answer is 4x

§61.7's gate declined oversampling on measurement, so this slice checked whether it
could do the same before writing the loop. It cannot: **the gain-reduction divider
is a multiply by an audio-rate signal.** The panel is the rectifier, so the cell's
conductance carries a residual ripple at 2·f₀, and `x · div(t)` generates sum and
difference products that fold.

Single 9 kHz tone at 0.30 V, PEAK 1.00, worst non-harmonic component in band:

| factor | 1× | 2× | 4× | 8× |
| --- | --- | --- | --- | --- |
| floor | **−86.16 dB** | −103.19 | **−113.09** | −114.07 |
| latency | 0 | 64 | **72** | 76 |

It **moves 27 dB with the factor**, which is §54's tell for real aliasing rather
than the legitimate IMD a compressor's own gain modulation produces (§59.7's trap:
a two-tone stimulus reports a flat floor at every factor and measures nothing —
this test uses a single tone for exactly that reason). 4× is the knee; 8× buys
1 dB. The ADAA headroom limiter's own half-sample averaging is the second reason:
its droop at 10 kHz is **−1.99 dB at base rate and −0.12 dB at 4×**.

Shipped **4×, latency 72 samples**, and the suite asserts the shipped factor
directly — §58.7's could-not-fail finding was that a test setting the factor per
row does not guard the default.

### 64.8 Hygiene, and the denormal scope

| property | measured |
| --- | --- |
| DC offset ON SIGNAL (220 Hz 0.30 V, and with +0.1 V of input offset) | **0.0000 %** of peak, both |
| block-size invariance (128 vs ragged 100/37/256/1/411) | **exactly 0.0** |
| `reset()` vs a fresh model | **0.000e+00** |
| rate spread, 44.1 / 48 / 88.2 / 96 kHz | **0.0032 dB** |
| knob-move seam vs the signal's own slew | **1.00×** |
| latency | **72 samples** at 4× |
| CPU, 4× at 48 kHz | **2.97 % of one stream** (`clipper-bench`, 33.6× realtime — against the OTA voice's 7.92 %), 3.0 % signal / 1.5 % silence on a standalone probe |
| the shipped defaults (PEAK 0.50, MODE compress, GAIN 0.62) | a 0.15 V 220 Hz note out at **−16.52 dBV** against an input of −16.48 = **−0.04 dB**, i.e. unity |

Denormal scope (ADR 006), decided by measurement:

* **Guarded** — the three coupling high-passes (input, output, panel bleeder) and
  **the cell's three states** (`gFast_`, `gSlow_`, `mem_`). All rest at exactly
  zero; `maxAbsRestingState()` covers exactly these. Each is FIRST ORDER, so the
  house one-liner is the right form (§56.4b's warning is about direct forms of
  order ≥ 2).
* **NOT guarded** — the loop's cell resistance, which rests at the cell's **dark
  value, 1.0e7 Ω**. A real operating point. This puts `OptoCell` on the **opposite
  side of the scope rule from `SidechainDetector`**, whose envelope node rests at a
  supply rail — one of the several reasons the two are not the same block.
* **The settle time is long and it is reported rather than worked around.** The
  output reaches exact digital silence within 5 s, but `maxAbsRestingState()`
  reaches **exactly 0.0 at 137 s**, because the trap occupancy's own time constant
  is 2 s and it has to fall ~30 decades to reach the 1e-30 flush floor. Nothing is
  denormal in the meantime (the states are ~1e-5 at 10 s), so the long tail is
  about proving convergence, not about a cost; the test uses 160 s.

### 64.9 Perturbation proofs

Every load-bearing bar was proven by patching what it names in the source,
rebuilding (with `touch` after **both** patch and restore — docs §29's trap), and
confirming the suite goes red. `git stash` was NOT used, and neither was
`git checkout --` (CLAUDE.md records what each has already destroyed here).

| # | perturbation | result | first assertion to fail, with the number |
| --- | --- | --- | --- |
| baseline | — | GREEN | — |
| P1 | `slowReleaseMemMult` 15 → 1 (the trap memory does nothing) | **RED** | THE ACCEPTANCE BAR — program dependence 2.892 → **1.000×**, i.e. exactly the OTA voice's figure |
| P2 | `trapHalfFraction` 0.01 → 1.0 (the linear normalization the first draft had) | **RED** | THE ACCEPTANCE BAR — **1.249×** |
| P3 | `elExponent` 2.5 → 1.0 (the EL law flattened) | **RED** | the published 50 %-release window (the curve moves under it). Re-run with `testRatioCurve` ordered first: **RED** on the knee shape, deep ratio **1.73:1** — see the note below |
| P4 | `kSeriesOhms` 100 k → 10 k | **RED** | the published 50 %-release window, then the ceiling |
| P5 | `kLimitFeedForward` 1/25 → 0 (MODE does nothing) | **RED** | MODE authority — 7.71 → **0.00 dB** |
| P6 | the cell's attack coefficient scaled by the drive (M13.1's current-starved shape) | **RED** | the attack SPREAD — 1.0 → **4.0 ms**, i.e. the attack starts moving with the knob |
| P7 | the sidechain tapped AFTER the GAIN pot | **RED** | the control-law contrast — GAIN now moves the gain reduction by **11.8587 dB** (and the output travel collapses 32.00 → 20.80 dB, which trips first) |
| P8 | the shipped oversampling default 4 → 1 | **RED** | the TONE bar first — the ADAA limiter's own half-sample droop puts 10 kHz at **−1.99 dB** against a 1.0 dB in-band bound — and then the shipped-factor assert |
| restore | — | GREEN | — |


**One honest note on P3, because it exposes a weak bar.** `testRatioCurve`
compares the rendered deep ratio against `predictedDeepRatio()` — but that
prediction is computed from the SAME two device constants, so with `elExponent`
moved to 1.0 the measurement (1.73:1) still agrees with the prediction (1.750) to
1 %. That comparison therefore validates the discretization and the loop, and
CANNOT catch a wrong exponent — the §29 warning about a reference derived from the
same netlist, in miniature. The bars that do catch it are the PUBLISHED bracket
(2.16:1 … 3.76:1, from S6 and S7's own ranges) and the knee shape, and both are in
the same test. Left as is, with the weakness named, because the identity is still
worth having: it is what would catch a broken loop.

### 64.10 Integration and goldens

One param shape, additive registries: `rig.ts` `PedalType` + `AVAILABLE_PEDAL_TYPES`
+ `OPTO_KNOB_DEFAULTS` (PEAK REDUCTION **0.50**, MODE **0** = compress, GAIN
**0.62** = unity); worklet `opto` dispatch behind the `_opto` C-ABI prefix;
`Pedal.tsx` FACES entry (the 'compact' anatomy — a levelling amplifier is a studio
box rather than a stomp — with the PERIWINKLE accent, a T4B's blue-green panel glow
pushed to blue-violet so it separates from the delay's deep azure and the Muff's
magenta-violet); native `PEDAL_OPTO = 11` (appended, never inserted — those
integers are the packed snapshot encoding), four APVTS parameters, a keyed
`PedalCard` face and a ninth `identical_core_test` board (`Lumen → RAT → Twin` —
the first case in which a pedal's MIDDLE slot carries a real control, so it is the
one that proves slot 1 is plumbed end to end rather than silently dropped the way
every two-knob pedal's is). Assistant: `add_pedal` gains `'opto'`, and the coach
gets a real block — including the choosing-between-the-two-compressors paragraph,
which is the question a player will actually ask.

**Goldens: all five UNCHANGED at ±0.00 dB, and nothing was blessed.** The optical
compressor is in no golden rig and touches no other model; the Dyna Comp and the
noise gate are **bit-identical by render hash** (`clipper-render` renders compared
byte for byte before and after the slice).

