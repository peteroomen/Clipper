#!/usr/bin/env bash
# A/B evidence for the M6.1 RAT re-voice + gain change, and (appended below) the
# M6.5 fizz fixes: the LM308 op-amp model (ideal vs modelled) and the clean-path
# gain re-staging (OLD +6 dB / 0.9 limiter vs NEW unity / 0.97 limiter).
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
# The HEAD RatModel predates the M6.5 setIdealOpAmp() API; the OLD binary is
# inherently "ideal op-amp" (no LM308 model), and it is only used for the RAT
# M6.1 A/B and the clean-chain (--chain clean, AmpModel) A/B — never with
# --ideal-opamp. Drop those two calls so the copied main.cpp builds against the
# HEAD RatModel.
sed -i '/setIdealOpAmp/d' "$OLDTREE/core/tools/render/main.cpp"
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

# ---------------------------------------------------------------------------
# M6.5 fizz A/B — same NEW binary, LM308 op-amp model OFF (ideal, fizzy) vs ON.
# High-dist RAT on a high-E pluck and a high-E sine (the classic fizz probes):
# the ideal op-amp passes razor edges to the clipper; the LM308 (closed-loop BW
# collapse + slew) rounds them. Listen: ideal = fizzy/harsh, LM308 = thicker.
# ---------------------------------------------------------------------------
echo
echo "== M6.5 fizz A/B: LM308 op-amp OFF (ideal) vs ON -> $OUT =="
for SIG in "pluck:329.6:2.0" "sine:329.6:2.0"; do
  TAG="fizz_$(echo "$SIG" | tr ':.' '__')"
  echo "--- $SIG dist=1.0 filter=0.15 (bright) level=0.9, amp $AMP_TRIM ---"
  echo "[ideal op-amp (LM308 OFF) — fizzy]"
  "$NEW" --gen "$SIG" "$OUT/${TAG}_idealopamp.wav" --amp "$AMP_TRIM" \
         --distortion 1.0 --filter 0.15 --level 0.9 --ideal-opamp \
         --spectrum "$OUT/${TAG}_idealopamp.csv" | sed 's/^/    /'
  echo "[LM308 op-amp ON — thick]"
  "$NEW" --gen "$SIG" "$OUT/${TAG}_lm308.wav" --amp "$AMP_TRIM" \
         --distortion 1.0 --filter 0.15 --level 0.9 \
         --spectrum "$OUT/${TAG}_lm308.csv" | sed 's/^/    /'
done

# ---------------------------------------------------------------------------
# M6.5 clean-path A/B — pedal BYPASSED chain (amp default + cab + output limiter).
# OLD binary (M6.1 staging: +6 dB volume ceiling, 0.9 limiter) soft-clips every
# cycle at a hot -3 dBFS input = "fizzy". NEW binary (M6.5: unity ceiling, 0.97
# limiter) keeps the limiter dormant = clean. Input amp ~0.7 => ~-3 dBFS peak.
# (Both binaries share the current main.cpp, so both have --chain/--limiter-thresh;
# the amp volume taper difference comes from OLD vs NEW AmpModel.)
# ---------------------------------------------------------------------------
echo
echo "== M6.5 clean-path A/B: pedal-bypassed chain, OLD(0.9/+6dB) vs NEW(0.97/unity) =="
CLEAN_AMP=0.7
for SIG in "pluck:82.4:2.0" "sine:220.0:2.0"; do
  TAG="clean_$(echo "$SIG" | tr ':.' '__')"
  echo "--- $SIG  (amp $CLEAN_AMP ~ -3 dBFS input peak) ---"
  echo "[OLD staging: +6 dB ceiling, limiter 0.9 — soft-clips = fizzy]"
  "$OLD" --gen "$SIG" "$OUT/${TAG}_OLD.wav" --amp "$CLEAN_AMP" \
         --chain clean --limiter-thresh 0.9 | sed 's/^/    /'
  echo "[NEW staging: unity ceiling, limiter 0.97 — dormant = clean]"
  "$NEW" --gen "$SIG" "$OUT/${TAG}_NEW.wav" --amp "$CLEAN_AMP" \
         --chain clean --limiter-thresh 0.97 | sed 's/^/    /'
done

echo
echo "Done. WAVs + spectra in: $OUT  (untracked; delete freely)"
