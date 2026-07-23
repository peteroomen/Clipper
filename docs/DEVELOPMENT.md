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
| `PARAM_DISTORTION` (0) | 0 dB | +54 dB | linear-in-dB pre-clip gain | plus a fixed pre-clip high-shelf (see below) |
| `PARAM_FILTER` (1) | 20 kHz (bright) | 500 Hz (dark) | log-swept one-pole LP cutoff | RAT convention: clockwise = darker |
| `PARAM_LEVEL` (2) | 0.0 | 1.0 | identity linear gain | audio-taper law is a future refinement |

### Circuit model & assumptions (circuit-informed, NOT SPICE-accurate)

Reference level: input float `1.0f == 1.0 V` at the diode stage (a hot humbucker
DI peaks ~0.3 V), so pre-gain must lift the signal past the diode knee to clip —
as the real LM308 stage does.

1. **Gain / shaping (LM308 non-inverting amp).** Variable pre-gain (0…+54 dB;
   the real RAT reaches ~+66 dB via `1 + P1/Rg`, P1 = 100 k Distortion pot, Rg ≈
   47 Ω — capped lower here since we do not model the LM308's slew limiting).
   The RAT feedback network (47 Ω + 2.2 µF leg to ground, ~100 pF across the
   feedback) makes the stage gain **rise toward the mids/highs** with corners
   roughly in the 100–800 Hz band — this is the RAT's tightness. Modeled as a
   first-order high-shelf: unity above ~320 Hz, bass shelved to 0.30 (≈ −10.5 dB).
   *Assumption:* a single shelf approximates the two-pole feedback transfer; exact
   component-accurate EQ and op-amp slew limiting are future refinements.
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

## Notes / conventions

- `core/` must never include platform/OS/browser/Emscripten headers. The only
  Emscripten touch point is `EMSCRIPTEN_KEEPALIVE` in `src/clipper_c_api.cpp`,
  guarded by `#if defined(__EMSCRIPTEN__)` so the file still builds natively.
- Gain smoothing (one-pole, ~5 ms) lives in the core, not in JS, so parameter
  changes are click-free regardless of the host.
- Parameter ids are mirrored and must stay in sync. The **RAT** ids
  (`PARAM_DISTORTION=0`, `PARAM_FILTER=1`, `PARAM_LEVEL=2`) live in
  `core/include/clipper/dsp/RatModel.h` (`clipper::dsp::RatModel::ParamId`),
  `web/src/params.ts`, and `web/worklet/clipper-processor.js`. The M0 gain id
  (`clipper::ParamId::PARAM_GAIN=0` in `Processor.h`) still exists in the WASM
  module but the live app no longer uses it.
```
