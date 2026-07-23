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
   (1N914-ish: Is = 2.52 nA, Vt = 25.85 mV, one diode/side → ±0.6 V knee), built
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

- **Transfer curve match:** `f(x) = Vk·tanh(x/Vk)`, `Vk = 0.35`. The WDF diode
  node's *static* curve (measured by settling the WDF at DC) has small-signal
  slope ≈ 1 (diodes off) and a soft knee whose output saturates around
  0.33–0.39 V under realistic overdrive (it keeps rising ~logarithmically rather
  than hard-limiting at 0.6 V). `tanh(x/Vk)·Vk` matches the unity origin slope
  exactly and limits at ±Vk; Vk = 0.35 places the ceiling in the WDF's measured
  mid/high-drive band. The tanh flattens where the diode keeps creeping up — a
  documented approximation; the comparison is about aliasing, not an exact curve.
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
  a frequency-domain delay line, `Y = Σ FDL[k+1]·H_k` (the `+1` defers the newest
  block one cycle), inverse-FFT, take the last P samples.
- **Latency = exactly one partition = 128 samples.** The `+1` offset realizes it;
  an impulse comes out as the IR delayed by 128 samples (`latencySamples()`
  matches the measured impulse delay; the impulse reproduces the IR to ~3e-17 —
  the FFT partitioning is exact). If the IR sample rate ≠ engine rate it is
  linearly resampled at load (fine for the smooth synthetic IR; a documented
  compromise for future real IRs).
- `process()` allocates nothing (all FFT scratch, the FDL, and the IR spectra are
  sized in `prepare()`). The amp+cab chain processes 1 s of audio in ~6 ms (44.1
  k) / ~12 ms (96 k) — far under real time.

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

## Built DSP artifacts are committed

`web/public/generated/` (the Emscripten-built WASM engine + the worklet copy)
is **checked into git** so that `git pull` alone updates the audio engine —
no local Emscripten needed to build the web or Mac app. If you change
`core/` or `web/worklet/`, run `bash scripts/build-wasm.sh` and commit the
regenerated artifacts alongside the source change (a stale artifact means
new UI bound to an old engine — trim knobs that do nothing, etc.).

## Mac app (Electron)

**One-shot build & launch (on the Mac):** `bash scripts/mac.sh` — builds the
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
  self-`listen`s on a fixed port and serves no statics; the ~40 lines of http
  glue that `serve.mjs` and it share are the only duplication.)
- **API key resolution** (`electron/config.mjs`), first hit wins: (1)
  `ANTHROPIC_API_KEY` env, (2) `config.json` in the app's `userData` dir, (3)
  neither → a minimal in-app prompt to paste a key, saved to that `config.json`.
  **`MOCK=1` runs keyless** (canned `[mock]` responses, no key, no spend).
- **Key storage — v1 tradeoff:** the key is saved as **plain text** in
  `~/Library/Application Support/Clipper/config.json` (mode `0600`), **not** the
  macOS Keychain. Documented here as a known v1 limitation.
- **Mic permission:** on startup the app calls
  `systemPreferences.askForMediaAccess('microphone')`; the built app declares
  `NSMicrophoneUsageDescription` (guitar input) in its Info.plist.
- **Paths are `app.isPackaged`-aware:** in dev, `web/dist` and `server/` sit next
  to the repo; when packaged, electron-builder copies them into the `.app`'s
  `Resources/` (`web-dist/` and `server/`) and `main.mjs` resolves them via
  `process.resourcesPath`.

### Dev loop (any OS with Electron)

```bash
cd web && npm run build        # the shell serves web/dist — build it first
cd ../electron && npm install  # downloads Electron for your platform
MOCK=1 npm run dev             # keyless demo window (or set ANTHROPIC_API_KEY)
```

`npm test` (in `electron/`) runs the main-process unit suites with plain Node —
no Electron needed: `serve.test.mjs` (ephemeral port, statics, SPA fallback,
`/api` wired to the real handler, keyless-mock + no-key-500, path-traversal
guard — 10 tests) and `config.test.mjs` (key resolution order — 6 tests).

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

- **Verified in CI/container:** both Node unit suites (16 tests) pass, and
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

## Notes / conventions

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
  `AMP_PARAM_CAB=5` handled by the C ABI wrapper) are likewise mirrored in
  `web/src/params.ts` and the worklet. The M0 gain id
  (`clipper::ParamId::PARAM_GAIN=0` in `Processor.h`) still exists in the WASM
  module but the live app no longer uses it.
```
