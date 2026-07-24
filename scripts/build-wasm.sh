#!/usr/bin/env bash
# Build the portable Clipper core to a WASM ES module that the AudioWorklet can
# statically import.
#
# Chosen approach (documented in docs/DEVELOPMENT.md):
#   MODULARIZE=1 EXPORT_ES6=1 SINGLE_FILE=1
#   - SINGLE_FILE embeds the .wasm as a base64 data URI inside the .js, so the
#     worklet needs NO fetch/XHR (neither exists in AudioWorkletGlobalScope).
#   - EXPORT_ES6 emits an ES module with a default-export factory the worklet
#     imports statically (Chromium supports static imports in worklet modules).
#   - This avoids SharedArrayBuffer / COOP-COEP entirely.
#
# Output: web/public/generated/clipper.js  (single self-contained ES module).
#
# Why public/ and not src/: the AudioWorklet module and this WASM module are
# loaded by absolute URL at runtime and are NOT run through Vite's bundler.
# Files under public/ are served verbatim in `vite dev` and copied as-is into
# dist/ by `vite build`, so the worklet's static `import './clipper.js'`
# resolves identically in dev, preview, and production. This sidesteps the
# well-known fragility of bundling AudioWorklet modules and their imports.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_DIR="$REPO_ROOT/core"
OUT_DIR="$REPO_ROOT/web/public/generated"
OUT_FILE="$OUT_DIR/clipper.js"
WORKLET_SRC="$REPO_ROOT/web/worklet/clipper-processor.js"

# Make emcc available. Prefer an already-active emcc; otherwise source emsdk.
if ! command -v emcc >/dev/null 2>&1; then
    EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
    # Fall back to the CI/container install location if the default is absent.
    if [ ! -f "$EMSDK_DIR/emsdk_env.sh" ] && [ -f /home/user/emsdk/emsdk_env.sh ]; then
        EMSDK_DIR=/home/user/emsdk
    fi
    if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
        # emsdk_env.sh references unset vars; relax nounset while sourcing it.
        set +u
        # shellcheck disable=SC1091
        source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
        set -u
    fi
fi
if ! command -v emcc >/dev/null 2>&1; then
    echo "ERROR: emcc not found. Run scripts/setup-emsdk.sh first." >&2
    exit 1
fi

echo "Using: $(emcc --version | head -1)"
mkdir -p "$OUT_DIR"

# --- Locate chowdsp_wdf headers (needed by the M2 RAT model's WDF diode stage).
# They are fetched by CMake FetchContent into core/build/_deps. If they are not
# there yet, run a cmake configure to populate them (needs network the first
# time), then fail loudly with an actionable message if still absent — never let
# emcc surface a mysterious "chowdsp_wdf/chowdsp_wdf.h: No such file" error.
WDF_INC="$CORE_DIR/build/_deps/chowdsp_wdf-src/include"
if [ ! -d "$WDF_INC" ]; then
    echo "chowdsp_wdf headers not found under core/build/_deps; running cmake" \
         "configure to fetch them (FetchContent, needs network the first time)..."
    if ! cmake -S "$CORE_DIR" -B "$CORE_DIR/build" -DCMAKE_BUILD_TYPE=Release; then
        echo "ERROR: cmake configure failed — could not fetch chowdsp_wdf." >&2
        echo "       Run 'cmake -B build' in core/ manually and re-run this." >&2
        exit 1
    fi
fi
if [ ! -d "$WDF_INC" ]; then
    echo "ERROR: chowdsp_wdf headers still missing at:" >&2
    echo "       $WDF_INC" >&2
    echo "       Configure the native core first (see docs/DEVELOPMENT.md §6):" >&2
    echo "         cd core && cmake -B build -DCMAKE_BUILD_TYPE=Release" >&2
    exit 1
fi
echo "Using chowdsp_wdf headers: $WDF_INC"

emcc \
    "$CORE_DIR/src/Processor.cpp" \
    "$CORE_DIR/src/clipper_c_api.cpp" \
    "$CORE_DIR/src/dsp/RatModel.cpp" \
    "$CORE_DIR/src/dsp/OverdriveEngine.cpp" \
    "$CORE_DIR/src/dsp/SdModel.cpp" \
    "$CORE_DIR/src/dsp/TsModel.cpp" \
    "$CORE_DIR/src/dsp/AmpModel.cpp" \
    "$CORE_DIR/src/dsp/ChorusModel.cpp" \
    "$CORE_DIR/src/dsp/ReverbModel.cpp" \
    "$CORE_DIR/src/dsp/CabConvolver.cpp" \
    "$CORE_DIR/src/dsp/CabIR.cpp" \
    "$CORE_DIR/src/dsp/TriodeStage.cpp" \
    "$CORE_DIR/src/dsp/Jcm800Preamp.cpp" \
    "$CORE_DIR/src/dsp/Jcm800PowerAmp.cpp" \
    "$CORE_DIR/src/dsp/Jcm800Amp.cpp" \
    "$CORE_DIR/src/dsp/OptoTremolo.cpp" \
    "$CORE_DIR/src/dsp/TwinPreamp.cpp" \
    "$CORE_DIR/src/dsp/TwinPowerAmp.cpp" \
    "$CORE_DIR/src/dsp/TwinAmp.cpp" \
    -I "$CORE_DIR/include" \
    -isystem "$WDF_INC" \
    -O3 \
    -msimd128 \
    -std=c++17 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s SINGLE_FILE=1 \
    -s ENVIRONMENT=web,worker \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_clipper_create","_clipper_destroy","_clipper_set_param","_clipper_process","_rat_create","_rat_destroy","_rat_set_param","_rat_set_oversampling","_rat_latency_samples","_rat_process","_sd_create","_sd_destroy","_sd_set_param","_sd_set_oversampling","_sd_latency_samples","_sd_process","_ts_create","_ts_destroy","_ts_set_param","_ts_set_oversampling","_ts_latency_samples","_ts_process","_amp_create","_amp_destroy","_amp_set_param","_amp_set_model","_amp_latency_samples","_amp_process","_amp_process_stereo","_amp_set_cab_builtin","_amp_load_custom_ir","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["HEAPF32","HEAPU8"]' \
    -o "$OUT_FILE"

echo "Built WASM ES module: $OUT_FILE"
ls -la "$OUT_FILE"

# Copy the authored worklet next to the WASM module so its `import './clipper.js'`
# resolves as a sibling in the same served directory.
if [ -f "$WORKLET_SRC" ]; then
    cp "$WORKLET_SRC" "$OUT_DIR/clipper-processor.js"
    echo "Copied worklet: $OUT_DIR/clipper-processor.js"
else
    echo "WARNING: worklet source not found at $WORKLET_SRC" >&2
fi
