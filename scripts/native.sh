#!/usr/bin/env bash
# Build and launch the NATIVE (JUCE/CoreAudio) Clipper standalone in one shot.
# This is the low-latency path — prefer it over the Electron app for playing,
# especially with the tube amps (heaviest DSP; native runs them fastest).
#
#   bash scripts/native.sh          # configure+build the Standalone, then launch
#   bash scripts/native.sh --plugins  # also build VST3 + AU (for Logic etc.)
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: this script builds/launches the macOS native app and must run on a Mac" >&2
  exit 1
fi
if [[ "$(uname -m)" != "arm64" ]]; then
  echo "warning: not an Apple Silicon host — build will match the host arch" >&2
fi

echo "==> Configuring native build"
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release >/dev/null

if [[ "${1:-}" == "--plugins" ]]; then
  echo "==> Building Standalone + VST3 + AU"
  cmake --build native/build --config Release -j
  echo "==> Plugins in native/build/Clipper_artefacts/Release/{VST3,AU}"
  echo "    AU into Logic: cp -R native/build/Clipper_artefacts/Release/AU/Clipper.component ~/Library/Audio/Plug-Ins/Components/ && auval -v aufx Clp1 Clpr"
else
  echo "==> Building Standalone"
  cmake --build native/build --config Release -j --target Clipper_Standalone
fi

app="$(find native/build -path '*Standalone*' -name '*.app' -type d | head -1)"
[[ -n "$app" && -d "$app" ]] || { echo "error: built Standalone .app not found" >&2; exit 1; }

echo "==> Launching $app"
osascript -e 'tell application "Clipper" to quit' >/dev/null 2>&1 || true
sleep 0.5
open "$app"
echo "First run: grant microphone access, then Options -> Audio/MIDI Settings to pick your interface + a small buffer (64/128)."
