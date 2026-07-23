#!/usr/bin/env bash
# A/B evidence for the M6.1 RAT re-voice + gain change.
#
# Renders the SAME signals through the OLD pedal model (the committed HEAD:
# single 320 Hz / -10.5 dB pre-clip shelf, +54 dB max gain) and the NEW model
# (two-corner RAT feedback voicing, +66 dB max gain), and dumps peak/RMS +
# magnitude spectra so you can see the difference. It also renders the NEW model
# with a +12 dB "input trim" (emulated via input amplitude) to show the
# calibration recovering drive at real interface levels.
#
# Nothing here is committed: all output goes to an UNTRACKED scratch dir. The
# OLD binary is built by swapping ONLY RatModel.{cpp,h} for their HEAD versions
# into a scratch copy of the current tree (so both binaries share the same
# render tool, incl. the `pluck` generator), reusing the already-fetched
# chowdsp_wdf checkout so no network is needed.
#
# Usage:  bash core/scripts/ab_render.sh  [OUT_DIR]
#   OUT_DIR defaults to core/.ab-scratch (git-ignored — see below).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORE="$REPO_ROOT/core"
OUT="${1:-$CORE/.ab-scratch}"
CHOW="$CORE/build/_deps/chowdsp_wdf-src"

mkdir -p "$OUT"

echo "== Building NEW render tool (current working tree) =="
cmake -S "$CORE" -B "$CORE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$CORE/build" --target clipper-render >/dev/null
NEW="$CORE/build/clipper-render"

echo "== Building OLD render tool (HEAD RatModel swapped in) =="
OLDTREE="$OUT/core_old"
rm -rf "$OLDTREE"
mkdir -p "$OLDTREE"
git -C "$REPO_ROOT" archive HEAD core | tar -x -C "$OLDTREE"   # HEAD's core tree
# Overwrite just the two changed pedal files with the current working-tree main.cpp
# (so the OLD binary also has the `pluck` generator) — the RatModel already IS the
# HEAD version inside the archive, which is exactly the OLD behavior we want.
cp "$CORE/tools/render/main.cpp" "$OLDTREE/core/tools/render/main.cpp"
if [ ! -d "$CHOW" ]; then
  echo "ERROR: chowdsp_wdf not found at $CHOW — run 'cmake -B build' in core/ first." >&2
  exit 1
fi
cmake -S "$OLDTREE/core" -B "$OLDTREE/core/build" -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_CHOWDSP_WDF="$CHOW" >/dev/null
cmake --build "$OLDTREE/core/build" --target clipper-render >/dev/null
OLD="$OLDTREE/core/build/clipper-render"

# Cranked distortion, dark-ish filter, unity level; 4x oversampling (the default).
DIST=1.0; FILT=0.3; LEVEL=0.9
# Real-world interface level (guitar DI often 0.01..0.05) and a +12 dB "trim".
AMP_REAL=0.03; AMP_TRIM=0.12

echo
echo "== A/B renders -> $OUT =="
for SIG in "pluck:82.4:2.0" "sine:82.4:2.0" "sweep:20:20000:4.0"; do
  TAG="$(echo "$SIG" | tr ':.' '__')"
  echo "--- signal $SIG (dist=$DIST filter=$FILT level=$LEVEL) ---"
  echo "[OLD 54dB/shelf @ amp $AMP_REAL]"
  "$OLD" --gen "$SIG" "$OUT/${TAG}_OLD.wav" --amp "$AMP_REAL" \
         --distortion "$DIST" --filter "$FILT" --level "$LEVEL" \
         --spectrum "$OUT/${TAG}_OLD.csv" | sed 's/^/    /'
  echo "[NEW 66dB/voicing @ amp $AMP_REAL]"
  "$NEW" --gen "$SIG" "$OUT/${TAG}_NEW.wav" --amp "$AMP_REAL" \
         --distortion "$DIST" --filter "$FILT" --level "$LEVEL" \
         --spectrum "$OUT/${TAG}_NEW.csv" | sed 's/^/    /'
  echo "[NEW 66dB/voicing @ amp $AMP_TRIM  (= +12 dB input trim)]"
  "$NEW" --gen "$SIG" "$OUT/${TAG}_NEW_trim.wav" --amp "$AMP_TRIM" \
         --distortion "$DIST" --filter "$FILT" --level "$LEVEL" | sed 's/^/    /'
done
echo
echo "Done. WAVs + spectra in: $OUT  (untracked; delete freely)"
