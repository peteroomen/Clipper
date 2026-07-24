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
mono in ──► × input trim (−12..+24 dB) ──► RAT (if on) ──► SD-1 (if on)
        ──► AmpModel.processStereo (tone stack + volume + bright + JC-120
            chorus/vibrato split: mono → stereo) ──► per-side CabConvolver
            (cabL / cabR, if cab on) ──► OutputLimiter.processStereo ──► stereo out
```

- **`native/src/ClipperEngine.{h,cpp}`** — the whole DSP chain, using the core
  C++ classes **directly** (`RatModel`, `SdModel`, `AmpModel` + its owned
  `ChorusModel`, two `CabConvolver`s, `OutputLimiter`) — **not** the `clipper_c_api`
  C ABI. This mirrors `web/worklet/clipper-processor.js` sample-for-sample. It has
  no JUCE dependency, so the console test can drive it standalone.
- **`native/src/PluginProcessor.{h,cpp}`** — the JUCE `AudioProcessor`. Owns an
  `AudioProcessorValueTreeState` (the param store + state save/restore) and one
  `ClipperEngine`. Pure host glue: no DSP. Mono-in → stereo-out bus layout (also
  accepts a stereo track and takes channel 0 as the mono source).
- **`native/src/PluginEditor.{h,cpp}`** — a tidy **flat** custom editor (dark
  panels, JUCE sliders/toggles grouped **Pedals | Amp | Chorus | Output**, no
  image assets). The neumorphic web design is a later native pass. The **build
  git hash** is shown bottom-right (configured via CMake as `CLIPPER_GIT_HASH`).

**Fixed two-pedal chain (v1):** RAT then SD-1, both default off **except** RAT.
Drag-reorder / arbitrary-length chains remain a web/UI feature for now; the native
shell ships the fixed order. Mono in → stereo out so the chorus bloom works in
Logic.

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
| `ampOn` | bool | true | chain: amp power (off ⇒ stereo passthrough) |
| `volume` / `bass` / `middle` / `treble` | float | 0.4 / 0.5 / 0.5 / 0.6 | `AmpModel` id 0 / 1 / 2 / 3 |
| `bright` | bool | false | `AmpModel` id 4 |
| `cab` | bool | true | chain-level cab on/off (per-side `CabConvolver`) |
| `chorusMode` | choice Off/Chorus/Vibrato | Off | `AmpModel` id 8 (0/1/2) |
| `chorusSpeed` / `chorusDepth` | float | 0.3 / 0.5 | `AmpModel` id 6 / 7 |
| `oversampling` | choice 1x/2x/4x/8x | 4x | `RatModel`/`SdModel` `setOversampling` |

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
latency = (ratOn  ? RatModel::latencySamples() : 0)   // OS group delay
        + (sdOn   ? SdModel::latencySamples()  : 0)   // OS group delay
        + (ampOn && cab ? 128 : 0)                    // CabConvolver partition
        + 64                                          // OutputLimiter lookahead
```

At the default 4× oversampling each dirt pedal reports **72** samples, the cab
partition is **128**, and the limiter lookahead is **64**, so the full default
rig (RAT + SD-1 + cab + limiter) reports **336** samples. The pedal group delay
tracks the oversampling factor via each model's `latencySamples()` accessor; the
value is re-published whenever `cab` or `oversampling` changes.

### Oversampling

Fixed **4×** default, exposed as an optional `oversampling` choice (1/2/4/8)
mirroring the web select. A change routes to each pedal's `setOversampling`
(resets only the oversampling filter state), never a full re-prepare, so it is
realtime-safe.

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
| **Identical-core** console test (bit-exact) | ✅ 0.0 diff L+R, latency 336=336 |
| Core ctest (5 suites) still green | ✅ unchanged |
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

