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
    EMSDK_DIR="${EMSDK_DIR:-/home/user/emsdk}"
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

emcc \
    "$CORE_DIR/src/Processor.cpp" \
    "$CORE_DIR/src/clipper_c_api.cpp" \
    -I "$CORE_DIR/include" \
    -O3 \
    -msimd128 \
    -std=c++17 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s SINGLE_FILE=1 \
    -s ENVIRONMENT=web,worker \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_clipper_create","_clipper_destroy","_clipper_set_param","_clipper_process","_malloc","_free"]' \
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
