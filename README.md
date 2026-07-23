# Tonesmith
An AI-assisted Guitar Rig Sim

## Mac desktop app

A native macOS wrapper lives in [`electron/`](electron/): it runs the same proxy
as `npm run server` in-process and loads the built web app in an Electron window
(the same Chromium the test suite uses). Build a `.dmg` on your Mac with
`cd web && npm run build && cd ../electron && npm install && npm run dist:mac`.
See the **"Mac app (Electron)"** section of
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for the dev loop, API-key setup, and
the unsigned-app first-launch note.
