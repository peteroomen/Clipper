#!/usr/bin/env bash
# Idempotently install and activate the Emscripten SDK used to build the WASM
# core. Safe to re-run: it skips work that is already done.
#
# Result: after sourcing $EMSDK/emsdk_env.sh (which build-wasm.sh does for you),
# `emcc` is on PATH.
set -euo pipefail

EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"

# PINNED, deliberately. `latest` made the committed WASM artifact irreproducible:
# two machines with the same source could not agree on the bytes, so nothing could
# be said about whether the committed artifact matched the committed source.
# 6.0.4 is the version verified to reproduce the committed
# web/public/generated/clipper.js byte-for-byte (173337 bytes). If you bump this,
# rebuild the artifact in the same slice and say so — the stamp records the emcc
# version and check-artifact.mjs will warn on the mismatch until you do.
EMSDK_VERSION="${EMSDK_VERSION:-6.0.4}"

# 1. Already have emcc on PATH? Nothing to install — but say so if it is not the
#    pinned version, because a build with it will not reproduce the committed
#    artifact and the difference is not your source change.
if command -v emcc >/dev/null 2>&1; then
    echo "emcc already on PATH: $(command -v emcc)"
    ON_PATH_VERSION="$(emcc --version | head -1)"
    echo "$ON_PATH_VERSION"
    case "$ON_PATH_VERSION" in
        *"$EMSDK_VERSION"*) ;;
        *)
            echo "WARNING: this is not the pinned emsdk $EMSDK_VERSION. A rebuild will" >&2
            echo "         produce different artifact bytes than the committed one." >&2
            echo "         Force the pin with: EMSDK_DIR=~/emsdk-$EMSDK_VERSION bash scripts/setup-emsdk.sh" >&2
            ;;
    esac
    exit 0
fi

# 2. Clone emsdk if we don't have it yet.
if [ ! -d "$EMSDK_DIR" ]; then
    echo "Cloning emsdk into $EMSDK_DIR ..."
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"

# 3. Install + activate the pinned version. Try activate first: it succeeds only
#    if that exact version is already installed, which makes this both idempotent
#    and version-aware. (The old check — "does upstream/emscripten/emcc exist" —
#    was version-blind: an emsdk left at some other version skipped the install
#    and then failed to activate the pin.)
if ! ./emsdk activate "$EMSDK_VERSION" >/dev/null 2>&1; then
    echo "Installing emsdk $EMSDK_VERSION ..."
    ./emsdk install "$EMSDK_VERSION"
    ./emsdk activate "$EMSDK_VERSION"
fi

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh"
echo "emsdk ready:"
emcc --version | head -1
