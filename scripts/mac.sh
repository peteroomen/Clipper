#!/usr/bin/env bash
# Build and launch the Clipper Mac app in one shot.
#
#   bash scripts/mac.sh          # build web + unpacked arm64 .app, then launch it
#   bash scripts/mac.sh --dmg    # also produce the installable .dmg (slower)
#   bash scripts/mac.sh --dev    # skip packaging, run electron directly (fastest loop)
#
# The compiled DSP engine (web/public/generated/) is committed to git, so a
# plain `git pull` before this script is all that's needed to stay current.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: this script builds/launches the macOS app and must run on a Mac" >&2
  exit 1
fi

# Rosetta trap: x64 Node fetches Intel Electron -> audio lag under Rosetta.
if [[ "$(node -p process.arch)" != "arm64" ]]; then
  echo "error: Node is $(node -p process.arch), not arm64." >&2
  echo "Install Apple Silicon Node (e.g. via nvm/brew), then: rm -rf electron/node_modules && re-run." >&2
  exit 1
fi

[[ -d web/node_modules ]]      || (cd web && npm install)
[[ -d electron/node_modules ]] || (cd electron && npm install)

echo "==> Building web app"
(cd web && npm run build)

mode="${1:-}"
if [[ "$mode" == "--dev" ]]; then
  echo "==> Launching electron (dev)"
  cd electron && exec npx electron .
fi

echo "==> Packaging arm64 app"
if [[ "$mode" == "--dmg" ]]; then
  (cd electron && npx electron-builder --mac --arm64)
  echo "==> DMG in electron/dist-app/"
else
  (cd electron && npx electron-builder --mac --arm64 --dir)
fi

app="electron/dist-app/mac-arm64/Clipper.app"
if [[ ! -d "$app" ]]; then
  # electron-builder's output dir name can vary by version; find the .app
  app="$(find electron/dist-app -maxdepth 2 -name '*.app' -type d | head -1)"
fi
[[ -n "$app" && -d "$app" ]] || { echo "error: built .app not found under electron/dist-app" >&2; exit 1; }

echo "==> Launching $app"
osascript -e 'tell application "Clipper" to quit' >/dev/null 2>&1 || true
sleep 0.5
open "$app"
