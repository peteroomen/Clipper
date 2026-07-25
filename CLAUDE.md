# Clipper / Tonesmith — Agent & Development Guidelines

This file is read by Claude Code at the start of every session. It captures conventions that keep human and AI contributions consistent.
**Do not skip it. Do not start writing code before completing the pre-session checklist below.**

**Ship small slices. One slice = one plan = one branch = one PR = one session.** This is the core working rhythm, shared across all of Peter's projects (riff, budget-app, pixel-horizons). See the **Pre-Session Checklist**, **Plan File Format**, and **Post-Session Checklist** below — they are not optional.

**One rule specific to this project, above all others: audio quality is the product.** A change that speeds something up or tidies something must be provably fidelity-neutral, or it is a tone change and needs to be argued as one. "Measured" beats "sounds fine to me" — see **Measure, Don't Assert**.

---

## Tech Stack

| Layer            | Choice                                                                                  |
| ---------------- | --------------------------------------------------------------------------------------- |
| DSP core         | Portable C++17, zero platform/OS/browser deps (`core/`) — CMake ≥ 3.16, clang++ or g++   |
| WDF library      | `chowdsp_wdf`, header-only, pinned by commit SHA via CMake `FetchContent`                |
| Web engine       | Emscripten → WASM ES module + `AudioWorklet` (`web/worklet/`, `web/public/generated/`)   |
| Web app          | Vite 5 + React 18 + TypeScript                                                          |
| Assistant        | Anthropic Claude via a zero-dependency Node proxy (`server/`), SSE streaming + tool use  |
| Desktop          | Electron 33 wrapper running the same proxy in-process (`electron/`)                      |
| Native plugin    | JUCE 8.0.4 (pinned tag, FetchContent) — Standalone + VST3 + AU (`native/`)               |
| Tests            | `ctest` (plain-assert, no framework) · Playwright `OfflineAudioContext` · `node --test`  |
| Package manager  | npm (per-workspace: root, `web/`, `electron/`)                                           |

---

## Key Docs

| Doc              | Path                            | Purpose                                                                 |
| ---------------- | ------------------------------- | ----------------------------------------------------------------------- |
| Roadmap          | `ROADMAP.md`                    | Build order and milestone definitions — the source of truth for "next"   |
| Engineering log  | `docs/DEVELOPMENT.md`           | Numbered sections (§N) — the long-form record of every milestone         |
| Work logs        | `docs/work/`                    | Per-session plan files — read the most recent before starting            |
| Decisions        | `docs/decisions/`               | Architecture decision records (ADRs)                                     |
| Audits           | `docs/audits/`                  | Read-only review passes; findings feed the roadmap                       |

`docs/DEVELOPMENT.md` is the deep record and is cited by section number throughout the code (e.g. "see docs §25"). When you change behaviour that a section documents, update that section in the same slice — a stale §N is worse than none, because the code cites it as justification.

---

## Current State

> **Update this section at the end of every session.**

- **Current phase:** Post-v1.1. The gear lineup is six audio pedals (RAT, SD-1, TS, Muff, Phaser, Gold) plus four amp voices (Clean 120, JCM800 2204, blackface Twin, AC30 top boost), two cabs + user IR upload, tuner, and the conversational assistant. Web and native both ship a dynamic, reorderable pedal board.
- **Last shipped:** the scrolling native pedal board on a milled rail · the GOLD "Myth" overdrive (sixth pedal type, parallel clean/dirt blend + germanium WDF) · the AC30 gain-structure fix (starved phase inverter) · M11 Player Expectations Suite.
- **Just landed:** a full project audit — `docs/audits/2026-07-24-project-audit.md`. It is the current source of truth for what to fix next and supersedes ad-hoc bug lists. Three findings are shipping-blockers (NaN parameters permanently brick audio; cab swap runs 11–46 ms of allocation inside `process()`; `CabConvolver` corrupts its stream on non-128-multiple host block sizes). **Audit finding 3 is fixed** (`fix/cab-block-size`, 2026-07-25): `CabConvolver` now has a sample-accurate input/output FIFO and is bit-identical for any block size, including sizes that vary call to call — see `docs/work/2026-07-25-cab-block-size.md`. Findings 1 and 2 are still open.
- **Artifact debt:** `core/` changed in `fix/cab-block-size`, so `web/public/generated/` needs `bash scripts/build-wasm.sh` regenerating and committing. Do not build new web work on the current artifact assuming it carries the convolver fix.
- **Known gaps (from the audit, not yet actioned):** no CI at all · `check-artifact.mjs` only checks existence, not staleness · the valve amps have no parameter smoothing · per-triode oversampling costs 7.5 ms of latency on the JCM800 · the AC30 "sag" is a static saturator and its tone stack has a structural ~37 dB mid notch · a recurring class of test asserts identities or the implementation against a reference derived from the same code (one instance of this, the vacuous `testConvolverChunking`, is now a real test).
- **Still open on the native side of finding 3:** the *convolver* is now correct for whatever block size the host hands it, but `native/src/ClipperEngine.cpp:372` still only chunks when `numFrames > maxBlock_`, so the rest of the native chain sees raw host block sizes. The related audit item is control-rate parameter sampling at large DAW blocks (audit line 274).
- **Repo hygiene note:** project history lived on a long-running feature branch for a while and `main` held only a README. That is being corrected — `main` is now the trunk. Do not start new work from anything but `main`.
- **Env note:** Node 18+ for the web app and tests; CMake ≥ 3.16 and a C++17 compiler for the core; Emscripten only for the WASM rebuild (`scripts/setup-emsdk.sh`).

---

## ⚠️ Pre-Session Checklist — Complete Before Writing Any Code

Complete every step in order. Do not proceed to code until the plan file exists and has been confirmed.

**1. Orient**

- [ ] Read the **Current State** section above + the most recent file in `docs/work/` — know what shipped last and what was deferred
- [ ] Identify today's slice from `ROADMAP.md` (or the open findings in `docs/audits/`)
- [ ] If touching a circuit model, read its `docs/DEVELOPMENT.md` section first — it records the component values, the measurements, and *why* a constant is what it is
- [ ] If touching the signal chain, read **both** `web/worklet/clipper-processor.js` and `native/src/ClipperEngine.h` — the chain is implemented twice and parity is a hand-maintained invariant

**2. Clarify**

- [ ] If the task is ambiguous, ask one focused clarifying question before proceeding. Do not make assumptions and build the wrong thing.
- [ ] For a tone change, establish up front how it will be judged — a measurement, a golden, or an explicit "this is a taste call and here is the A/B"

**3. Plan**

- [ ] Write a plan file to `docs/work/YYYY-MM-DD-{slug}.md` using the format below
- [ ] Plan must include a **Manual test steps** section — happy path + at least one edge/failure case
- [ ] For DSP work the plan must name the **measurement** that will show it worked (alias floor, THD, DC offset, latency, CPU, golden diff)
- [ ] Present the plan as a summary to the user and get explicit confirmation before writing code
- [ ] **Do not write a single line of application code until the plan is confirmed**

**4. Branch**

- [ ] Run `git status` to confirm you are NOT on `main`, then `git checkout -b feat/{slug}` (or `fix/` `chore/` `docs/` `perf/`)

---

## Plan File Format

Filename: `docs/work/YYYY-MM-DD-{short-slug}.md`
Example: `docs/work/2026-07-24-nan-parameter-guard.md`

```markdown
# {Feature / Task Name}

**Date:** YYYY-MM-DD
**Branch:** feat/{slug}
**Roadmap item:** {milestone / audit finding / deferred item}

## Goal

One sentence: what does "done" look like for this session?

## Approach

How will this be built? Key technical decisions made upfront.
Call out anything non-obvious or where multiple approaches were considered.
For DSP: state whether this is fidelity-neutral or a deliberate tone change.

## Steps

- [ ] Step 1
- [ ] Step 2
      (Be specific — vague steps lead to vague output)

## How this will be measured

The number that proves it worked, and the tool that produces it
(`clipper-bench`, `clipper-render --alias-report`, a new ctest, a golden diff).
"It sounds fine" is not a measurement.

## Manual test steps

How to verify this works end-to-end after the code is written.
Cover the happy path and at least one failure/edge case.

- [ ] Test step 1 (e.g. plug in, set X, expect Y)
- [ ] Edge case: what happens if …

## Out of scope for this session

Explicitly list anything related but not being done today.

---

<!-- Fill in below during/after the session -->

## What actually happened

(decisions made, approaches changed, surprises)

## Measured results

(the actual numbers, before → after)

## Files created / modified

(list key files)

## Deferred to next session

(anything punted — be specific so next session picks it up cleanly)

## Status

- [ ] In progress
- [ ] Complete
- [ ] Partial — see deferred
```

---

## Post-Session Checklist

Do not close the session without completing these steps:

- [ ] Fill in the "What actually happened", "Measured results", "Files changed", and "Deferred" sections of the plan file
- [ ] Update the **Current State** section of this file (`CLAUDE.md`)
- [ ] Update the relevant `docs/DEVELOPMENT.md` section if behaviour it documents changed
- [ ] Add an ADR to `docs/decisions/` if a significant architectural decision was made
- [ ] **If `core/` or `web/worklet/` changed:** run `bash scripts/build-wasm.sh` and commit the regenerated artifacts (see **The Committed WASM Artifact**)
- [ ] Run the core suite: `cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`
- [ ] Run `cd web && npm run build` (this includes `tsc --noEmit`) and `npm test` (Playwright)
- [ ] Run `npm run test:server && npm run test:history` at the root, and `cd electron && npm test`
- [ ] Commit with a conventional commit message and push the branch
- [ ] Open a PR — even for the smallest slice, always go through PR review

---

## Branching Strategy

### Branch model: **trunk-based with short-lived feature branches**

```
main          ← the trunk; always buildable and always green
  └─ feat/<slug>        new features, e.g. feat/gold-overdrive
  └─ fix/<slug>         bug fixes, e.g. fix/nan-parameter-guard
  └─ perf/<slug>        fidelity-neutral performance work
  └─ chore/<slug>       deps, config, tooling
  └─ docs/<slug>        documentation only
```

### Rules

1. **`main` is the trunk and is always buildable.** Core tests, web build and web tests must pass on it.
2. **Never push directly to `main`.** All changes go through a feature branch + PR, no exceptions. There is no "trivial fix" exception.
3. **Branch early, merge fast.** Branches should live < 2 days. Prefer small, focused PRs. Long-running branches are how this project ended up with 100 commits off-trunk.
4. **One slice = one branch = one PR = one session.** Don't bundle a bug fix with a new feature.
5. **Branch names** are lowercase, hyphen-separated: `feat/spring-reverb`, `fix/cab-block-size`.
6. **Commit messages** follow Conventional Commits:
   - `feat:` new user-facing feature (a new pedal, a new amp voice, a UI capability)
   - `fix:` bug fix
   - `perf:` fidelity-neutral speed/latency work — state the measured before → after in the body
   - `refactor:` internal restructure, no behaviour change
   - `chore:` deps, tooling, CI
   - `docs:` documentation only
   - `test:` tests only
   - `style:` formatting, naming — no logic change
   - For a deliberate **tone** change, use `feat:` or `fix:` and say in the body what changed audibly and what measurement backs it.
7. **No force-push to `main`** under any circumstances.

### GitHub branch protection (owner must configure once)

In **Settings → Branches → Add rule** for `main`:

- ✅ Require a pull request before merging
- ✅ Require approvals: 0 (solo project — self-merge is fine, just needs a PR)
- ✅ Require status checks to pass (once CI exists — see **Automated Checks**)
- ✅ Do not allow bypassing the above settings
- ✅ Restrict who can push to matching branches (remove direct-push access)

### Agent-specific rules (Claude Code)

- **Always work on a branch.** Before making any code change: `git checkout -b feat/<slug>`. Even a one-line fix.
- Before starting any task, run `git status` to confirm you are NOT on `main`. If you are, branch first.
- Never run `git push origin main` or `git push --force`. Push your feature branch and open a PR.
- Never commit secrets, `config.json`, or an API key. The Electron key store lives at the OS config path, never in the repo.
- Binary blobs: the committed WASM artifact and the golden `.wav` files are the **only** sanctioned binaries. Don't add others.
- Always run the core `ctest` suite and `cd web && npm run build` before committing.
- Co-author commits with: `Co-Authored-By: Claude <noreply@anthropic.com>`
- Never bypass git hooks with `--no-verify`. If a hook fails, fix the issue.

---

## Automated Checks

> **Status: not yet wired up.** The 2026-07-24 audit's top process finding is that this project has no CI — every suite listed below exists, is good, and passes, and nothing runs it automatically. Standing this up is a `chore/ci` slice. Until it exists, the Post-Session Checklist commands are the gate and you must actually run them.

### Intended CI pipeline (GitHub Actions, on every PR to `main` and push to `main`)

1. **Core:** `cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`
2. **Web:** `cd web && npm ci && npm run build` (includes `tsc --noEmit`) then `npx playwright test`
3. **Node suites:** `npm run test:server`, `npm run test:history`, `cd electron && npm test`
4. **Artifact staleness:** `node web/scripts/check-artifact.mjs` — currently existence-only; see below
5. **Native (nice-to-have):** the JUCE `identical_core_test` and `chain_edit_test`

### Useful commands

| Command                                        | Purpose                                                       |
| ---------------------------------------------- | ------------------------------------------------------------- |
| `ctest --test-dir build --output-on-failure`   | The core DSP suite (16 targets)                               |
| `build/clipper-bench`                          | Per-unit CPU cost table (× realtime, % of one 48 k stream)     |
| `build/clipper-render --alias-report`           | Alias floor vs oversampling factor                            |
| `build/clipper-render --gen sweep:20:20000:4 …` | Render any gear to a WAV for listening / spectrum             |
| `bash scripts/build-wasm.sh`                    | Rebuild the committed WASM artifact + worklet copy            |
| `bash scripts/update-goldens.sh`                | Re-bless the golden renders — **deliberate act only**         |
| `npm run server`                                | The assistant proxy (needs `ANTHROPIC_API_KEY`)               |

---

## Project Structure

```
core/                     Portable C++17 DSP core — ZERO platform/OS/browser deps
  include/clipper/dsp/    Public headers; each carries the circuit rationale + component values
  src/dsp/                Pedal, amp, modulation and cab models
  src/clipper_c_api.cpp   The C ABI consumed by WASM and any FFI
  tests/                  Plain-assert ctest targets + goldens/ (blessed .wav renders)
  tools/                  render / bench / measure harnesses (native only, never in the WASM build)
web/
  src/                    React UI, audio.ts (engine lifecycle), rig.ts (rig state), assistant/
  worklet/                AudioWorkletProcessor — THE audio thread. Authored here.
  public/generated/       COMMITTED build output: clipper.js (WASM) + a copy of the worklet
  tests/                  Playwright OfflineAudioContext audio verification
server/                   Zero-dependency Node proxy for the assistant (SSE + tool use)
electron/                 Desktop shell — runs the proxy in-process, serves the built web app
native/                   JUCE plugin: Standalone + VST3 + AU, wrapping the same core
  src/ClipperEngine.*     The native chain — mirrors web/worklet/clipper-processor.js
  tests/                  identical_core_test (load-bearing), chain_edit_test
scripts/                  setup-emsdk.sh, build-wasm.sh, update-goldens.sh, native.sh, mac.sh
docs/                     DEVELOPMENT.md (engineering log), work/, decisions/, audits/
```

---

## Key Design Conventions

### The core is portable, and that is load-bearing

`core/` must compile with a plain C++17 compiler and has **zero** platform, OS, browser or Emscripten includes. That is what makes every model offline-testable and every audio bug attributable to the shell rather than the model. Don't reach for a platform header in `core/` — if you need one, the design is wrong.

### The audio thread allocates nothing, locks nothing, and never blocks

No `malloc`/`free`, no locks, no I/O, no unbounded work inside `process()` or anything it calls — in the C++ core, in the worklet, or in the native engine. Scratch buffers are sized in `prepare()`. This rule is currently violated by the cab-swap path (audit finding 2); that is a bug, not a precedent.

**Corollary:** "it happens at the declick zero so it's inaudible" is not a real-time safety argument. Output-zero prevents a step discontinuity; it does nothing about missing the render deadline.

### Every parameter is smoothed, and every topology change is declicked

- **Knob moves** are smoothed in the core with `OnePoleSmoother` (~5–8 ms) and applied **per sample**, not once per block. The chain layer explicitly relies on this and does not bracket knob moves. (The valve amps currently don't do this — audit finding 6.)
- **Topology changes** (chain edit, pedal engage, cab swap, amp voice, amp power) are bracketed by the ~6 ms raised-cosine declick fade, with the swap landing exactly at the output zero. Both the worklet and `ClipperEngine` implement this; keep them in step.
- A parameter that reaches the engine must be **finite**. Clamp helpers must reject NaN explicitly — `v < 0 ? 0 : (v > 1 ? 1 : v)` passes NaN straight through, and one NaN latches permanently in recursive state (audit finding 1).

### Denormals: guard every recursive state

`core/include/clipper/dsp/Denormal.h` is the policy. **WASM has no flush-to-zero at all** — the runtime cannot be asked to flush — so a state that asymptotes toward zero sticks in the subnormal range forever and becomes a permanent audio-thread denormal generator. Every recursive accumulator (filter state, smoother value, feedback node, cap voltage) gets `flushDenormal`, in both the `float` and `double` overloads as appropriate. The guard only acts below −600 dB, so it is bit-transparent.

### Oversampling

4× is the measured default for every nonlinear stage (2× measures at ~−21 dB alias floor, 4× at ~−104 dB). Nonlinearities go *inside* the oversampled domain; linear filters and volume networks stay at base rate. Prefer **one** oversampling domain around a cascade over one per stage — per-stage oversampling stacks group delay and repeats band-limiting (audit performance item 1).

### The chain exists twice — keep parity

The rig graph is implemented in `web/worklet/clipper-processor.js` (JS) and `native/src/ClipperEngine.cpp` (C++). They must stay behaviourally identical: same order, same declick discipline, same latency accounting. `native/tests/identical_core_test.cpp` proves the *plugin* matches a hand-built core chain, but nothing currently proves *web* matches *native* — so when you change one, change the other in the same slice and say so in the PR body.

### The committed WASM artifact

`web/public/generated/clipper.js` and `web/public/generated/clipper-processor.js` are **committed build output**, so a `git pull` updates the engine without an Emscripten toolchain. The contract: **if you change `core/` or `web/worklet/`, run `bash scripts/build-wasm.sh` and commit the regenerated artifacts in the same commit.** A stale artifact means new UI bound to an old engine — knobs that do nothing. `check-artifact.mjs` currently only checks the files *exist*, so this contract is unenforced and rests on you (audit finding: process hole).

### Measure, don't assert

This is the project's hardest-won convention, and the audit found it is the one most often skipped. A test must assert a **player-observable property**, not an identity:

- Don't compare the implementation against an analytic reference **derived from the same netlist** — that validates the discretization (which is fine) and can never catch a wrong topology or a wrong component value.
- Don't assert an algebraic tautology (`f(v) − f(−v)` is odd) and call it a test of the amp.
- Don't assert `performance.now() delta > 0` and call it a perf test.
- Don't check DC offset on **silent** input only — asymmetric clipping produces DC on *signal*.
- For a "no click" test, land the change **mid-render** (`ctx.suspend()` / `resume()`), not before rendering starts — otherwise an entirely removed declick still passes.
- Prefer an absolute reference where one exists: a published curve, a datasheet limit, an insertion loss at 1 kHz, a measured unit.

`core/tests/test_player_expectations.cpp` is the model to follow — it drives gear the way a player does (default knobs, min knobs, realistic levels, realistic noise, the worklet's exact calling conventions) and pins what they must hear.

### Goldens

Blessed renders live in `core/tests/goldens/`. `scripts/update-goldens.sh` re-blesses them and is a **deliberate act only**: re-blessing is how a regression becomes canon, and a reviewer cannot see the drift in a `.wav` diff. If you re-bless, say in the PR body what changed audibly and why it is correct.

### UI conventions

- **Design tokens** live in `web/src/styles/tokens.css`. Never hardcode a colour — it must work in both themes or come from a token. Note that `.pedal` pins its chassis to the dark tokens on every theme; anything painted on that chassis must pin its accent to match (audit UI finding).
- **Knobs** are the most numerous control in the app: vertical drag, pointer capture, double-click to default, keyboard operable with a visible focus ring, and `role="slider"` with live `aria-valuenow`/`aria-valuetext`.
- **Never update React state at pointer-move rate** from the audio thread or a drag. Meters and needles are driven imperatively from a ref + rAF (`Tuner.tsx` is the pattern); persistence is debounced, not written per move.
- **Destructive actions** (remove pedal, swap pedal) need an undo path. There is none today — don't add more destructive surface without one.
- The native editor (`ClipperLookAndFeel`) is the light-bench sibling of the web UI. Widgets should derive from `juce::Button`/`juce::Slider` so keyboard operability and accessibility come for free.

### Assistant

- Tool arguments are **allowlisted and clamped** before they reach the engine (`web/src/assistant/tools.ts`, re-validated in `App.tsx`). This is the best-defended surface in the codebase — keep it that way. Add a tool only with an explicit param allowlist and a range clamp that rejects non-finite values.
- The system prompt is cached via `cache_control` on the stable block; volatile rig state goes in the user turn so it never breaks the cache prefix. Don't move rig JSON into the system block.
- `thinking` is sent explicitly. Don't add `temperature`/`top_p`/`top_k`/`budget_tokens` — they are rejected.
- Keep the tool surface small and typed. The assistant gets smarter through prompting and rig-state context, not more tools.

---

## ADR Format

When a slice makes a significant architectural decision, record it at `docs/decisions/NNN-{title}.md`:

```markdown
# ADR NNN: {Title}

Date: YYYY-MM-DD
Status: Accepted

## Context

Why did this decision need to be made?

## Decision

What was decided?

## Consequences

What are the trade-offs? What does this make easier or harder?
```

Circuit-modelling decisions belong here too — especially a deliberate departure from the real circuit. Record *what* was simplified, *why*, and *what it costs audibly*, so the next session doesn't "fix" it back or calibrate another constant against it.

---

## Environment Variables

| Variable            | Required | Description                                                             |
| ------------------- | -------- | ----------------------------------------------------------------------- |
| `ANTHROPIC_API_KEY` | ✅       | Claude API key — **server-side only**, never shipped to the browser      |
| `MODEL`             | optional | Override the assistant model id                                         |
| `MAX_TOKENS`        | optional | Override the response cap                                               |
| `PORT`              | optional | Proxy port (default 8787)                                               |
| `MOCK`              | optional | Serve canned assistant responses — no API key, no network               |
| `EMSDK_VERSION`     | optional | Emscripten version for `scripts/setup-emsdk.sh` (defaults to `latest`)  |

The Electron app stores the key in the OS user-config directory at mode `0600`, resolved by `electron/config.mjs`. It is never written into the repo and never reaches the renderer.

---

## Deferred / Post-v1.1

- **The 2026-07-24 audit backlog** — `docs/audits/2026-07-24-project-audit.md` is the ordered list. Its own suggested order: the three shipping-blockers, then bind the proxy to loopback, then stand up CI *before* the circuit work (so fidelity changes are measured rather than asserted), then the AC30 sag + stack topology + the shared PI tail reference.
- **Native tuner** — display-only pedal; needs a pitch tap + needle widget in JUCE. Web-only for now (a fake one would be worse than none).
- **Duplicate pedal instances** — the native engine is one-instance-per-type (`kMaxChain = PEDAL_TYPE_COUNT`), so a Screamer into a Screamer works on the web and not in the plugin. Options written up in `docs/DEVELOPMENT.md` → **Duplicate pedal instances**.
- **Native dark theme** — the editor is light-bench only.
- **CLAP** plugin format.
- **Presets & sharing** — the rig-state JSON is already the format; needs named slots and an undo ring.
- **BD-2 Blues Driver**, **DD-3 delay** (new DSP family: delay lines).
- **Riff integration** — Clipper's rig as riff's practice-tone engine. The assistant patterns already converge (both grew an "applied chip" chat UI).

---

## Things Not To Do

- Don't start writing code before the plan file exists and is confirmed
- Don't push directly to `main` or force-push — ever
- Don't bundle multiple concerns into one branch/PR — one slice per PR
- Don't skip the PR step — every slice gets a PR, no matter how small
- Don't allocate, lock, or do unbounded work on the audio thread — and don't accept "it's at the declick zero" as a justification
- Don't let an unsmoothed parameter or a non-finite value reach the engine
- Don't add a recursive state without a `flushDenormal` guard — WASM has no FTZ
- Don't change `core/` or `web/worklet/` without rebuilding and committing the WASM artifact
- Don't change the chain in one front-end without changing the other
- Don't write a test that asserts an identity, a tautology, or the implementation against a reference derived from the same code
- Don't re-bless goldens to make a failing test pass — that is how a regression becomes canon
- Don't calibrate a new constant to compensate for a suspected error elsewhere; find the error (this is how `kFullScaleSecV` ended up absorbing two separate factor-of-2 mistakes)
- Don't claim a performance win is fidelity-neutral without a measurement showing it
- Don't hardcode colours, and don't paint a light-theme accent on the dark pedal chassis
- Don't update React state at pointer-move or meter rate
- Don't ship dead UI (controls wired to nothing, or a knob whose top half does nothing)
- Don't commit secrets, `config.json`, or binary blobs other than the sanctioned WASM artifact and goldens
