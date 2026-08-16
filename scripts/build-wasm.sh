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

# The emcc source list and flags live in ONE marked region because
# web/scripts/artifact-stamp.mjs hashes it out of this file, verbatim, as part of
# the build stamp. Recording the flags INTO the stamp instead would be useless:
# self-reported flags cannot detect their own staleness (edit one without
# rebuilding and the stamp still agrees with itself). Blank lines and comment-only
# lines inside the region are stripped before hashing, so commenting freely here
# does not fire the staleness check — but moving a flag OUT of the region hides it
# from the stamp, so don't.
#
# --- STAMP:EMCC-ARGS BEGIN ---
EMCC_ARGS=(
    "$CORE_DIR/src/Processor.cpp"
    "$CORE_DIR/src/clipper_c_api.cpp"
    "$CORE_DIR/src/dsp/RatModel.cpp"
    "$CORE_DIR/src/dsp/OverdriveEngine.cpp"
    "$CORE_DIR/src/dsp/SdModel.cpp"
    "$CORE_DIR/src/dsp/TsModel.cpp"
    "$CORE_DIR/src/dsp/BjtStage.cpp"
    "$CORE_DIR/src/dsp/MuffModel.cpp"
    "$CORE_DIR/src/dsp/GoldModel.cpp"
    "$CORE_DIR/src/dsp/CompressorEngine.cpp"
    "$CORE_DIR/src/dsp/CompModel.cpp"
    "$CORE_DIR/src/dsp/GateModel.cpp"
    "$CORE_DIR/src/dsp/OptoModel.cpp"
    # M13.5 — the Uni-Vibe. LampDrive.h and OptoCell.h are header-only, so this
    # one .cpp is the whole voice. Inside the STAMP:EMCC-ARGS markers, because
    # docs §60 and §64 both record a new model being missed here.
    "$CORE_DIR/src/dsp/VibeModel.cpp"
    "$CORE_DIR/src/dsp/PhaserModel.cpp"
    "$CORE_DIR/src/dsp/WahModel.cpp"
    "$CORE_DIR/src/dsp/DelayModel.cpp"
    "$CORE_DIR/src/dsp/AmpModel.cpp"
    "$CORE_DIR/src/dsp/ChorusModel.cpp"
    # M13.7 — the CE-1 chorus pedal. It OWNS a ChorusModel (above), so it must be
    # listed after it for readability; link order does not matter here.
    "$CORE_DIR/src/dsp/Ce1Model.cpp"
    "$CORE_DIR/src/dsp/ReverbModel.cpp"
    "$CORE_DIR/src/dsp/CabConvolver.cpp"
    "$CORE_DIR/src/dsp/CabIR.cpp"
    "$CORE_DIR/src/dsp/TriodeStage.cpp"
    "$CORE_DIR/src/dsp/Jcm800Preamp.cpp"
    "$CORE_DIR/src/dsp/Jcm800PowerAmp.cpp"
    "$CORE_DIR/src/dsp/Jcm800Amp.cpp"
    "$CORE_DIR/src/dsp/OptoTremolo.cpp"
    "$CORE_DIR/src/dsp/TwinPreamp.cpp"
    "$CORE_DIR/src/dsp/TwinPowerAmp.cpp"
    "$CORE_DIR/src/dsp/TwinAmp.cpp"
    "$CORE_DIR/src/dsp/Ac30Preamp.cpp"
    "$CORE_DIR/src/dsp/Ac30PowerAmp.cpp"
    "$CORE_DIR/src/dsp/Ac30Amp.cpp"
    "$CORE_DIR/src/dsp/OrangePreamp.cpp"
    "$CORE_DIR/src/dsp/OrangePowerAmp.cpp"
    "$CORE_DIR/src/dsp/OrangeAmp.cpp"
    "$CORE_DIR/src/dsp/RockerverbPreamp.cpp"
    "$CORE_DIR/src/dsp/RockerverbPowerAmp.cpp"
    "$CORE_DIR/src/dsp/RockerverbAmp.cpp"
    -I "$CORE_DIR/include"
    -isystem "$WDF_INC"
    -O3
    -msimd128
    -std=c++17
    # Make the artifact PATH-INDEPENDENT so two people (or two worktrees) building
    # the same source produce the same bytes.
    #
    # The link is -O3 with NO -DNDEBUG, so assert() is live in the shipped engine
    # and every one of them bakes its __FILE__ into the WASM. Those are ABSOLUTE
    # paths, so the committed artifact recorded whichever directory the builder
    # happened to be in: main's artifact embedded four strings pointing at
    # `/home/user/Clipper/.claude/worktrees/agent-ab1bbfef070dfac5b/...`, an
    # ephemeral agent worktree, because that is where it was last rebuilt. Same
    # source, same emcc, 64 differing bytes — which is why the "reproduces
    # byte-for-byte" claim only ever held from an identically-named directory.
    #
    # -ffile-prefix-map rewrites those to repo-relative, which makes a future
    # rebuild-and-compare CI check possible. It does NOT remove the asserts: that
    # is -DNDEBUG, a change to what the audio engine does at runtime, and it wants
    # a deliberate decision rather than a drive-by (docs §31).
    -ffile-prefix-map="$REPO_ROOT/="
    -ffile-prefix-map="$CORE_DIR/=core/"
    -s MODULARIZE=1
    -s EXPORT_ES6=1
    -s SINGLE_FILE=1
    -s ENVIRONMENT=web,worker
    -s ALLOW_MEMORY_GROWTH=1
    # Emscripten DROPS any exported symbol not named here, so this list must stay
    # in step with clipper_c_api.cpp. The *_reset entries are the recovery path for
    # audit finding 1 (docs §28): without them a poisoned engine can only be
    # recovered by tearing the whole worklet graph down.
    -s EXPORTED_FUNCTIONS='["_clipper_create","_clipper_destroy","_clipper_set_param","_clipper_process","_rat_create","_rat_destroy","_rat_set_param","_rat_set_oversampling","_rat_latency_samples","_rat_process","_sd_create","_sd_destroy","_sd_set_param","_sd_set_oversampling","_sd_latency_samples","_sd_process","_ts_create","_ts_destroy","_ts_set_param","_ts_set_oversampling","_ts_latency_samples","_ts_process","_muff_create","_muff_destroy","_muff_set_param","_muff_set_oversampling","_muff_latency_samples","_muff_process","_gold_create","_gold_destroy","_gold_set_param","_gold_set_oversampling","_gold_latency_samples","_gold_process","_comp_create","_comp_destroy","_comp_set_param","_comp_set_oversampling","_comp_latency_samples","_comp_process","_gate_create","_gate_destroy","_gate_set_param","_gate_set_oversampling","_gate_latency_samples","_gate_process","_opto_create","_opto_destroy","_opto_set_param","_opto_set_oversampling","_opto_latency_samples","_opto_process","_vibe_create","_vibe_destroy","_vibe_set_param","_vibe_set_oversampling","_vibe_latency_samples","_vibe_process","_phaser_create","_phaser_destroy","_phaser_set_param","_phaser_set_oversampling","_phaser_latency_samples","_phaser_process","_wah_create","_wah_destroy","_wah_set_param","_wah_set_oversampling","_wah_latency_samples","_wah_process","_chorus_create","_chorus_destroy","_chorus_set_param","_chorus_set_oversampling","_chorus_latency_samples","_chorus_process","_delay_create","_delay_destroy","_delay_set_param","_delay_set_oversampling","_delay_latency_samples","_delay_process","_amp_create","_amp_destroy","_amp_set_param","_amp_set_model","_amp_latency_samples","_amp_process","_amp_process_stereo","_amp_set_cab_builtin","_amp_load_custom_ir","_amp_prepare_cab_builtin","_amp_prepare_cab_custom","_amp_commit_cab","_clipper_reset","_rat_reset","_sd_reset","_ts_reset","_muff_reset","_gold_reset","_comp_reset","_gate_reset","_opto_reset","_vibe_reset","_phaser_reset","_wah_reset","_chorus_reset","_delay_reset","_amp_reset","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["HEAPF32","HEAPU8"]'
)
# --- STAMP:EMCC-ARGS END ---

emcc "${EMCC_ARGS[@]}" -o "$OUT_FILE"


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

# The build stamp. It records a content hash over everything that affects the
# artifact so that web/scripts/check-artifact.mjs can prove, with no toolchain
# installed, that the committed artifact was built from the sources in the tree.
# Commit it in the SAME commit as the artifact — see CLAUDE.md, "The committed
# WASM artifact".
if command -v node >/dev/null 2>&1; then
    node "$REPO_ROOT/web/scripts/artifact-stamp.mjs" --write \
        --repo-root "$REPO_ROOT" \
        --emcc-version "$(emcc --version | head -1)" \
        --artifact "$OUT_FILE"
else
    echo "ERROR: node not found — cannot write web/public/generated/.build-stamp.json." >&2
    echo "       The artifact was built but is UNSTAMPED; check-artifact.mjs will fail." >&2
    exit 1
fi
