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
gesture to start), then move the **Gain** slider (0..2). Status text shows the
AudioContext state and sample rate. The graph is:
`OscillatorNode (220 Hz sine) -> AudioWorkletNode (WASM gain) -> destination`.
You cannot hear it inside the container; the audio proof is the Playwright test
below.

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

## Notes / conventions

- `core/` must never include platform/OS/browser/Emscripten headers. The only
  Emscripten touch point is `EMSCRIPTEN_KEEPALIVE` in `src/clipper_c_api.cpp`,
  guarded by `#if defined(__EMSCRIPTEN__)` so the file still builds natively.
- Gain smoothing (one-pole, ~5 ms) lives in the core, not in JS, so parameter
  changes are click-free regardless of the host.
- Parameter ids are mirrored in three places and must stay in sync:
  `core/include/clipper/Processor.h` (`clipper::ParamId`),
  `web/src/params.ts` (`PARAM_GAIN`), and
  `web/worklet/clipper-processor.js` (`PARAM_GAIN`).
```
