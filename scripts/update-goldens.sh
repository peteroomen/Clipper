#!/usr/bin/env bash
# Regenerate the M11 golden "first five minutes" rig renders (core/tests/goldens/).
#
# DELIBERATE ACT ONLY: the golden gate exists so voicing drift is a conscious
# decision. Run this only when a rig's voice was INTENTIONALLY changed, then
# commit the golden diff together with the change and a one-line justification.
#
# Usage: scripts/update-goldens.sh [build-dir]   (default: ./build)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -S core
cmake --build "$BUILD" -j --target clipper_player_expectations_tests
"$BUILD/clipper_player_expectations_tests" --update-goldens
echo
echo "Goldens rewritten under core/tests/goldens/ — review the diff, listen to the"
echo "renders, and commit them WITH the voicing change they bless."
