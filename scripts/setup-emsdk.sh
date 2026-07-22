#!/usr/bin/env bash
# Idempotently install and activate the Emscripten SDK used to build the WASM
# core. Safe to re-run: it skips work that is already done.
#
# Result: after sourcing $EMSDK/emsdk_env.sh (which build-wasm.sh does for you),
# `emcc` is on PATH.
set -euo pipefail

EMSDK_DIR="${EMSDK_DIR:-/home/user/emsdk}"
EMSDK_VERSION="${EMSDK_VERSION:-latest}"

# 1. Already have emcc on PATH? Nothing to do.
if command -v emcc >/dev/null 2>&1; then
    echo "emcc already on PATH: $(command -v emcc)"
    emcc --version | head -1
    exit 0
fi

# 2. Clone emsdk if we don't have it yet.
if [ ! -d "$EMSDK_DIR" ]; then
    echo "Cloning emsdk into $EMSDK_DIR ..."
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"

# 3. Install + activate the requested version (idempotent; re-activating is cheap).
if [ ! -f "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
    echo "Installing emsdk $EMSDK_VERSION ..."
    ./emsdk install "$EMSDK_VERSION"
fi
./emsdk activate "$EMSDK_VERSION"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh"
echo "emsdk ready:"
emcc --version | head -1
