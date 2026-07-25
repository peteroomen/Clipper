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
- **Just landed (2026-07-25, in merge order):** CI at last (`.github/workflows/ci.yml` — core ctest, web tsc+vite+Playwright, node suites, a dependency-free Conventional Commits gate, JUCE identical-core advisory) · `fix/proxy-loopback` — the audit's Security & app layer cleared: the proxy binds `127.0.0.1` by default (`HOST` opts into wider exposure, loudly), the banner reports the address actually bound, the Electron window gets a CSP (with `'wasm-unsafe-eval'` — without it WASM never compiles and the app makes no sound), `will-navigate`/`setWindowOpenHandler` denial, a media-only permission handler, an explicit `chmod 0600` on a pre-existing `config.json`, a real path-containment check, and `req.destroy()` on body-limit overflow (node suites 27 → 35) · **audit finding 1 fixed** — the non-finite parameter guard + engine reset path (docs §28, ADR 002): one shared NaN-rejecting clamp (`core/include/clipper/dsp/ParamGuard.h`) replaces ~14 broken copies, every `*_set_param` C ABI export hard-rejects non-finite, and `reset()` exists down the whole tree (`clipper_reset` / `rat_reset` / `sd_reset` / `ts_reset` / `phaser_reset` / `muff_reset` / `gold_reset` / `amp_reset`). Measured 48000/48000 → **0/48000** non-finite samples after one NaN, on all ten units; `Jcm800Amp::reset()` is ~302 000× cheaper than `prepare()`. New ctest target `clipper_nan_guard_tests`.
- **ALL THREE audit shipping-blockers are now fixed.** **Finding 2 — the cab swap is off the render path** (docs §30, ADR 003): split into `amp_prepare_cab_*` + `amp_commit_cab`, double-buffered at the ABI, so the audio-thread step is **0.000001 ms and 0 allocations** against a prepare of 10.97–42.66 ms. Worst render block over a 4096-tap IR load: 104.92 ms (39 quanta missed) → **0.42 ms (none)**. Bit-identical to the pre-fix audio.
- **Honest residual on finding 2 — it is NOT "real-time safe".** `amp_prepare_cab_*` runs in the worklet's `onmessage`, which **is** the audio rendering thread; this moves the work *between* quanta rather than off-thread. A guaranteed multi-quantum stall becomes at worst one late quantum on a deliberate action. The real fix is main-thread spectra. Also still on the audio thread: `_destroyPedal`'s `free()` inside `_commitPending`, and `_postLatency()` posting from `process()`.
- **Found while fixing finding 2, not yet fixed:** `_prepareChain` mutates `engaged` on the *live* node, so an engaged-flag change arriving in a `chain` message is not declicked (a stomp via `_pendingBypass` is). `CabConvolver::prepare()` zeroing the FDL means a swapped-in cab is silent for up to two partitions — handled with a 256-sample `'hold'` declick phase rather than by touching the convolver. The `removed`-list union also fixed a **WASM handle leak that was live on `main`**, masked there by the eager commit.
- **Still open from the audit:** `docs/audits/2026-07-24-project-audit.md` remains the source of truth. Nothing from Phases 2–5 has been actioned — no circuit defect is fixed.
- **The artifact staleness gate is real now** (docs §31, ADR 004). `web/public/generated/.build-stamp.json` is committed build output: `build-wasm.sh` writes a SHA-256 over the *contents* of the 65 inputs that actually affect `clipper.js`, and `check-artifact.mjs` recomputes it with **no toolchain**, naming the file that changed. This was not theoretical — two PRs each rebuilt `clipper.js`, the merge conflicted on the binary, and taking either side would have shipped an engine holding one of the two fixes while the source held both. **Do NOT hand-write `.build-stamp.json`**, and don't move an emcc flag outside the `STAMP:EMCC-ARGS` markers: both turn the guard into decoration.
- **Re-blessing a golden is now a ritual, not a command** — clean tree, a printed dB table measured against the *previous* goldens, a confirmation `yes |` cannot answer even through a pty, and a justification in `core/tests/goldens/GOLDENS.md` that CI requires. Measured on a deliberately wrong golden: the old path reported **0.00 dB** and blessed it; the new one reports **17.35 dB @ 800 Hz** and writes nothing.
- **§ and ADR numbers are assigned centrally.** Two slices reached for ADR `001`, and two reached for docs `§29`. Ask rather than guess.
- **Also just landed (2026-07-25): `test/assert-real-properties`** — the audit's *systemic* finding, sequenced before the circuit work so the fidelity fixes are measured rather than asserted (docs §29). Tests + `core/CMakeLists.txt` + docs only, **zero DSP change**, goldens untouched. Highlights: **`-UNDEBUG` is no longer behind `if(NOT MSVC)`** (on MSVC the entire 1129-line expectations suite compiled to a no-op `main` that printed its success banner with zero goldens present) and `core/tests/support/AssertsLive.h` makes it a **build error** if `NDEBUG` ever survives in a test TU again · the phase inverter is measured properly for the first time (plate as a **fraction of B+**, standing current from **Ohm's law on the plate load**, and the **leg-gain ratio** — shared across all three amps in `core/tests/support/LtpProbe.h`) · the JCM800 push-pull test's `f(V+v) − f(V−v)` **algebraic identity** is gone · DC offset is asserted **on signal**, with a +0.1 V input-offset case because deleting a coupling cap changes nothing on a clean input · block B's `tol` 2e-5 → **0.0 (bit-identical)** plus a **ragged 100-frame** pass, the only segmentation that can catch a block-size bug · **`OptoTremolo` has a test** (`clipper_opto_tremolo_tests`) · the three amp-swap "no pop" tests land the swap **mid-render** via `ctx.suspend()`/`resume()` · the three perf-smoke tautologies (`performance.now() delta > 0`) now assert real audio · `audio.spec.ts` gets a finiteness guard covering **every** render, channel and sample. Core ctest **24/24**.
- **The XFAIL ratchet (read this before "fixing" a red suite).** `core/tests/support/Xfail.h`: a known-bad property is measured, its real number printed, the finding named — and **an XPASS is a hard failure**. If you fix finding 7/8/16 or the tremolo and the suite goes red with `[XPASS]`, that is the design working: delete the XFAIL and assert the property for real, in the same slice. Never `#if 0` a property or loosen a bound to go green — that is exactly how the starved AC30 phase inverter shipped. Open XFAILs are visible in a plain `ctest` run as `<target>_xfail_ledger ... ***Skipped`; run `./build/<target> --xfail-ledger` for the detail. Currently **11 XFAILs across 6 ledger entries**, covering findings **7, 8, 16** and two Medium/DSP items (the opto tremolo's speed sag, control-rate parameter sampling).
- **Known gaps (from the audit, not yet actioned):** **the proxy still has no auth and no rate limit** (its own slice; loopback binding is the mitigation until then) · the valve amps have no parameter smoothing · per-triode oversampling costs 7.5 ms of latency on the JCM800 · the AC30 "sag" is a static saturator and its tone stack has a structural ~37 dB mid notch · the SIGNAL path is still NaN-transparent (`OutputLimiter::clamp1`) and `native/src/ClipperEngine` has no reset seam · **`web/playwright.config.ts:34` sets `retries: 2`**, so a fault appearing in under a third of runs is retried away — the last remaining way for a real fault to vanish silently (recommendation in docs §29; deliberately not changed unilaterally).

- **Also just landed (2026-07-25): `perf/muff-newton-earlyout`** — **audit finding 12 fixed** (docs §34): the Muff cost ~3× more CPU when you were NOT playing. `BjtStage`'s damped Newton had no residual-based exit, so at the parked operating point the backtracking line search was **unsatisfiable by construction** (nothing can strictly decrease a residual already at the floating-point floor) and burned all 30 backtracks every iteration — **31 Ebers-Moll evaluations, 124 `exp` calls, per stage per sample**, to reproduce the answer it already had. A residual early-out (`kNewtonResidualTolA`, a **current in amps**, scaled by the existing `tubeSolverTolScale()`) takes idle from **31.00 → 1.00 evaluations per solve**: silence **6.9 s → 0.50 s** per 10 s of audio (**~13.7×**), silence/signal ratio **2.58–2.87× → 0.23–0.27×** (idle is now *cheaper* than playing), `clipper-bench` **3.9× → 4.9× realtime / 25.4 % → 20.3 %**. Also fixes `dampedNewton` returning `it + 1`, which really did make `lastMaxNewtonIterations()` report **61** against a cap of 60.
- **Finding 12's headline caveat: it is NOT bit-identical, and that is a measured result, not a shortcut.** The pre-fix solver drove the residual to the floating-point floor, so **any** early-out that fires declines refinement it performed — a tolerance sweep against the pre-fix binary shows bit-identity only at 1e-19, where the early-out never fires and the pathology returns. 1e-17 is the tightest value that still fires: worst difference **−127.4 dBFS absolute / −134.1 dB relative**, i.e. **7.4 dB inside the project's own −120 dBFS solver gate** (§25), and 53 femtovolts of node error per solve. The **DC operating-point solve deliberately opts out** (`tol = 0.0`) — it runs once in `prepare()` and seeds the quiescent point every render references. Two new gates pin both directions: `clipper_muff_tests` fails if the early-out stops firing, `clipper_tube_solver_tests` (now covering the Muff) fails if it starts costing accuracy — both confirmed red under perturbation.
- **Two traps from that slice, worth knowing before you write a pedal harness:** `MuffModel`'s **`PARAM_VOLUME` smoother defaults to 0**, so a Muff driven only through `PARAM_SUSTAIN` renders **digital silence** — that made an entire 25-render bit-identity sweep vacuous until an `assert(peak > 0.01)` caught it. And the worst case for solver accuracy was **not** hot DI (−228 dBFS) but a **loud** input (−127 dBFS, 19 dB worse), so a hot-DI-only test would have reported a number 100 dB better than the truth.
- **Newly found, now an XFAIL (`muff-slam-exhausts-newton-cap`):** widening the old ±10 V single-oversampling slam test to **±20 V across every rate × factor** showed **6 of 16 combinations exhaust the 60-iteration cap** (2× at all four base rates; 4× at 88.2 and 96 kHz). Output stays finite and bounded, so it is not the old cascade blow-up, but the solve has not converged there. **Pre-existing** — identical counts measured on the pre-early-out solver. The shipped desktop path (4× at 44.1/48 kHz) converges in 17–18. Its own slice; **do not raise `kMaxNewtonIter`**, that buys iterations rather than convergence.
- **The XFAIL ratchet (read this before "fixing" a red suite).** `core/tests/support/Xfail.h`: a known-bad property is measured, its real number printed, the finding named — and **an XPASS is a hard failure**. If you fix finding 7/8/16 or the tremolo and the suite goes red with `[XPASS]`, that is the design working: delete the XFAIL and assert the property for real, in the same slice. Never `#if 0` a property or loosen a bound to go green — that is exactly how the starved AC30 phase inverter shipped. Open XFAILs are visible in a plain `ctest` run as `<target>_xfail_ledger ... ***Skipped`; run `./build/<target> --xfail-ledger` for the detail. Currently **12 XFAILs across 6 ledger entries**, covering findings **7, 8, 16**, two Medium/DSP items (the opto tremolo's speed sag, control-rate parameter sampling) and one found in-flight by a slice widening its own test (`muff-slam-exhausts-newton-cap`, docs §34).
- **Known gaps (from the audit, not yet actioned):** **the proxy still has no auth and no rate limit** (its own slice; loopback binding is the mitigation until then) · `check-artifact.mjs` only checks existence, not staleness · the valve amps have no parameter smoothing · per-triode oversampling costs 7.5 ms of latency on the JCM800 · the AC30 "sag" is a static saturator and its tone stack has a structural ~37 dB mid notch · the SIGNAL path is still NaN-transparent (`OutputLimiter::clamp1`) and `native/src/ClipperEngine` has no reset seam · **`web/playwright.config.ts:34` sets `retries: 2`**, so a fault appearing in under a third of runs is retried away — the last remaining way for a real fault to vanish silently (recommendation in docs §29; deliberately not changed unilaterally).
- **Still vacuous, deliberately deferred:** every tone-stack test compares the discrete MNA against an analytic `H(jω)` **derived from the same netlist** — it validates the discretization but structurally cannot catch a wrong topology or component value, which is exactly how audit finding 5 (the AC30's ~37 dB mid notch) and the JCM800 preamp flatness shipped. Fixing it needs published response curves per amp; that is a research slice.
- **Also just landed:** the audit's **"Test & process integrity"** artifact + goldens holes (docs §29, ADR 004). `web/public/generated/.build-stamp.json` is a new piece of committed build output: `build-wasm.sh` writes a SHA-256 over the *contents* of the 65 inputs that actually affect `clipper.js` (all of `core/src` + `core/include`, the worklet, and the emcc flag region of `build-wasm.sh` itself), and `check-artifact.mjs` recomputes it with **no toolchain** and fails naming the file that changed. This was not theoretical: two PRs each rebuilt `clipper.js`, the merge conflicted on the binary, and taking either side would have shipped an engine holding one of the two fixes while the source held both. `EMSDK_VERSION` is pinned `latest` → **6.0.4**. Separately, re-blessing a golden is now a ritual (clean tree, printed dB table vs the *previous* goldens, a confirmation `yes |` cannot answer even through a pty, a justification recorded in `core/tests/goldens/GOLDENS.md`), and the `--update-goldens` path no longer compares each render against the file it just wrote. Measured on a deliberately wrong golden: the old code reported **0.00 dB** and blessed it; the new report says **17.35 dB @ 800 Hz** and writes nothing.
- **Do NOT hand-write `.build-stamp.json`.** A hand-made stamp is a lie about which sources built the artifact, and it is the only thing standing between a source change and a silently stale engine. If the check fails, rebuild.

- **Also just landed (2026-07-25): `perf/halfband-no-modulo`** — the audit's fidelity-neutral perf item 2, and **bit-identical** (docs §32, ADR 005). `HalfbandFilter.h` indexed its ring as `ring_[(w_ - p + M) % M]` with a runtime `M_`, so the compiler emitted a hardware integer **division per tap** — 64 per output sample on the interpolator's tight stage, 65 on the decimator's, at the *oversampled* rate, for every oversampled pedal and once per triode stage in every valve amp. Now a **doubled ring buffer** (`2*N` floats, every write mirrored `N` apart, unwrapped `ring_[w_ + N - p]` reads, compare-advance) plus parallel tap arrays in the decimator. Measured **2.72× on the oversampler alone** (new `os2x`/`os4x`/`os8x` bench units: 2.90× / 2.72× / 2.65×), 2.2× on SD-1 / Screamer, 1.8× on RAT / Gold, and **the JCM800 from 60.6 % to 53.3 % of one realtime stream**; every non-oversampled control row unmoved. **0 differing bits** out of ~1.4 M samples vs a copy of the old implementation (both filter classes, the full cascade at 1×/2×/4×/8× over ragged block sizes, up *and* down, plus after a NaN-poisoned `reset()`); `--alias-report` identical to the digit; goldens untouched. New ctest target `clipper_halfband_tests` (core ctest 24 → **25**).
- **Two things `perf/halfband-no-modulo` pinned that you must not "tidy":** the tap loop walks `p` **upward** (reading memory backwards) — reversing it into a forward-streaming loop is mathematically identical and changes 38 % of samples by ~1 ULP, which a tolerance test cannot see, so the comparison is a `memcmp`; and **every** write to the ring must mirror both halves, `reset()` included, or stale history reappears one filter length later. No SIMD in that loop without a fidelity argument — vectorising a float reduction reassociates it.
- **New, quantified, still open:** `Oversampler::latencySamples()` **over-reports** the true round-trip delay by 0.5 / 0.75 / 0.875 base samples at 2× / 4× / 8× (measured 63.500 / 71.250 / 75.125 vs reported 64 / 72 / 76). The decimator emits on the *second* sample of each pair, and `(2M) >> (s+1)` cannot express the half. Pre-existing (bit-identity proves it), inaudible (18 µs), errs safe — but it is the number the UI and the plugin report, so fixing it is a plumbing slice. Also observed: docs **§7's `--alias-report` table is stale** (pre-existing, dates from the §11.1 RAT re-voice).

- **Also just landed (2026-07-25): `fix/denormal-guards`** — **audit finding 11** fixed (docs §33, ADR 006). Fidelity-neutral, **goldens untouched**, core ctest 24/24. §25 wrote the anti-denormal policy and applied it to two shared primitives; the tool meant to watch for the rest, `scripts/denormal_bench.cpp`, **only exercised those same two classes**, so it reported a clean ~1.00× cliff for months while a RAT ran **1.97× slower on silence than on a riff**. Per 10 s of silence, silence-time before → after (each now meeting its hardware-FTZ floor): **RAT 647 → 309 ms**, **GOLD 629 → 305 ms**, **AC30 1982 → 1603 ms**, **`Processor` at GAIN 0 23 ms → 4 ms against a 2 ms FTZ floor**. Subnormal output samples emitted **downstream into the rest of the chain**: **GOLD 393 607 → 0**, **SD-1 429 236 → 0**, **Screamer 428 761 → 0**, **`Processor` 427 187 → 0**, AC30 1588 → 2 (a bounded ring-down transient). Guarded: the two output DC blockers' `dcY1_`, the `chowdsp` WDF capacitor in RAT + GOLD, all three valve tone stacks' cap companions (**68.2×** and **18.6×** isolated — the two worst cliffs in the core), the AC30 power section's OT pair, `TwinPreamp::brightS_`, and `Processor`'s hand-rolled smoother. Whole-lineup A/B at knobs min/noon/max: **max |Δ| = 1.0151e-30** (−600 dB), all of it in silent tails; eight of eleven units bit-identical everywhere. New: `Denormal.h` gains `flushDenormalWdfCapacitor` / `isSubnormal(double)` / `maxAbsState`; `denormal_bench` gains a whole-unit section 2; `clipper_denormal_tests` gains block 2, perturbation-checked 7/7.
- **Reset exports exist but nothing calls them yet** — a worklet watchdog (detect non-finite in `process()`, schedule a declick-bracketed reset) is the follow-up, not a UI button.
- **ADR numbers are assigned centrally.** Two parallel slices both reached for `001`; if you are working a slice, ask rather than guess.
- **Also just landed:** **audit finding 3 fixed** — `CabConvolver` is exact for ANY host block size, not just multiples of 128. It zero-padded a partial block but advanced the FDL by a full partition and discarded the remainder, so the stream stayed misaligned forever; at 100-sample blocks the error EXCEEDED the signal (0.1636 vs a 0.1521 peak). Now feed → maybe-one-block → drain, with the deferral moved out of the FDL indexing and into an output FIFO, so it costs **no** extra latency and the 128-aligned path stays bit-identical (goldens untouched). Measured 0.0 error at every block size from 1 to 4096.
- **Still open on the native side of finding 3:** the *convolver* now handles any block size, but `native/src/ClipperEngine::process` still only chunks when `numFrames > maxBlock_`, so the rest of the native chain still sees raw host block sizes. Its own slice.
- **CI native job:** the `native` job's `ctest` inherits the core's registered tests (the native CMakeLists pulls the core in) and reported them Not Run because only the two native targets are built, so the job failed on every run with `continue-on-error` hiding it. Fixed with `-R 'clipper_identical_core|clipper_chain_edit'`. It stays advisory until it has been observed green — the load-bearing identical-core test had **never** actually run in CI before this.
- **Repo hygiene note:** project history lived on a long-running feature branch for a while and `main` held only a README. That is being corrected — `main` is now the trunk. Do not start new work from anything but `main`.
- **Env note:** **Node 22+** for the web app and tests — `npm run test:server` / `test:history` / `test:scripts` pass a quoted glob to `node --test`, and native glob support landed in Node 22, so on Node 20 they fail with "Could not find …/*.test.mjs". (Documented as 18+ until CI proved otherwise.) CMake ≥ 3.16 and a C++17 compiler for the core; Emscripten only for the WASM rebuild (`scripts/setup-emsdk.sh`), now pinned to **emsdk 6.0.4**. The artifact is byte-reproducible **from any directory as of 2026-07-25** — it was not before. The emcc link is `-O3` with no `-DNDEBUG`, so live `assert()`s bake their absolute `__FILE__` into the WASM, and the committed artifact recorded whichever directory the builder was in (main's briefly embedded an ephemeral agent-worktree path). `-ffile-prefix-map` makes those repo-relative; verified byte-identical across two different build directories. **Asserts still ship inside the audio engine** — removing them is `-DNDEBUG`, a runtime-behaviour change, deliberately not done here (docs §30).

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
- [ ] Run `npm run test:server && npm run test:history && npm run test:scripts` at the root, and `cd electron && npm test`
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

> **Status: wired up** (`.github/workflows/ci.yml`, landed 2026-07-25 — this note said "not yet" until then). Runs on every PR to `main` and every push to `main`. The **native job stays advisory** (`continue-on-error`) until it has been observed green. The Post-Session Checklist commands are still the local gate — run them before you push, don't outsource that to CI.
>
> **What CI cannot catch, and you must therefore still think about:** `web/playwright.config.ts` sets `retries: 2`, so a fault appearing in under a third of runs is retried away; `check-artifact.mjs` only checks the WASM artifact *exists*, not that it is current; and the goldens are `.wav` files no reviewer can read in a diff. See docs §29.

### The CI pipeline (GitHub Actions)

1. **Core:** `cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`
2. **Web:** the artifact staleness gate, then `cd web && npm ci && npm run build` (includes `tsc --noEmit`) then `npx playwright test`
3. **Node suites:** `npm run test:server`, `npm run test:history`, `npm run test:scripts`, `cd electron && npm test`
4. **Artifact staleness:** `node web/scripts/check-artifact.mjs` — a real content-hash check against `web/public/generated/.build-stamp.json`, needing no emsdk. See **The committed WASM artifact**.
5. **Goldens changelog:** a PR that changes a `.wav` under `core/tests/goldens/` must also change `core/tests/goldens/GOLDENS.md`.
6. **Commit messages:** a dependency-free Conventional Commits regex over the PR's first-parent subjects.
7. **Native (advisory):** the JUCE `identical_core_test` and `chain_edit_test`.

### Useful commands

| Command                                        | Purpose                                                       |
| ---------------------------------------------- | ------------------------------------------------------------- |
| `ctest --test-dir build --output-on-failure`   | The core DSP suite (20 targets + 6 `_xfail_ledger` entries reported Skipped — 26 ctest entries) |
| `build/denormal_bench`                          | Silence-vs-signal-vs-hardware-FTZ cost per unit (docs §33). Build: `c++ -std=c++17 -O2 -I core/include scripts/denormal_bench.cpp build/libclipper_dsp.a build/libclipper_core.a -o build/denormal_bench` |
| `./build/<target> --xfail-ledger`               | List that binary's known-bad properties (exits 77 = Skipped)  |
| `build/clipper-bench`                          | Per-unit CPU cost table (× realtime, % of one 48 k stream)     |
| `build/clipper-render --alias-report`           | Alias floor vs oversampling factor                            |
| `build/clipper-render --gen sweep:20:20000:4 …` | Render any gear to a WAV for listening / spectrum             |
| `bash scripts/build-wasm.sh`                    | Rebuild the committed WASM artifact + worklet copy            |
| `bash scripts/update-goldens.sh -m "why"`       | Re-bless the golden renders — **deliberate act only**, and the script enforces it |
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
- A parameter that reaches the engine must be **finite**. **Use `clampParam01` / `clampParam` / `paramToInt` from `core/include/clipper/dsp/ParamGuard.h` — never write a clamp by hand.** `v < 0 ? 0 : (v > 1 ? 1 : v)`, `std::clamp(v, 0, 1)` and `Math.min(1, Math.max(0, v))` all pass NaN straight through, and one NaN latches permanently in recursive state (audit finding 1, fixed 2026-07-25 — docs §28, ADR 002). At the C ABI and the worklet boundary a non-finite value is **rejected outright**, not clamped.
- **Every unit has a `reset()`, and it must never re-solve.** `reset()` clears recursive state and re-parks at the *cached* DC operating point (`TriodeStage`/`BjtStage` `cachePark()`, the power amps' `parkState()`). `prepare()` is not a recovery path — it settles ~50 k silent samples per tube stage (87.6 ms for a `Jcm800Amp`). If you add recursive state to a model, add it to that model's `reset()` **and** to the park snapshot, or `clipper_nan_guard_tests` block C will fail.

### Denormals: guard every recursive state **whose rest value is zero**

`core/include/clipper/dsp/Denormal.h` is the policy. **WASM has no flush-to-zero at all** — the runtime cannot be asked to flush — so a state that asymptotes toward zero sticks in the subnormal range forever and becomes a permanent audio-thread denormal generator. Every recursive accumulator (filter state, smoother value, feedback node, cap voltage) gets `flushDenormal`, in both the `float` and `double` overloads as appropriate. The guard only acts below −600 dB, so it is bit-transparent.

**Refined by audit finding 11 (fixed 2026-07-25 — docs §33, ADR 006). Read this before adding or removing a flush:**

- **The scope rule:** guard a state iff its value **at rest (silent input) can be zero**. A state that rests at a **nonzero DC operating point** — a cathode cap at its bias voltage, a B+ rail, a screen node, a sag envelope at idle draw, a Newton warm start at 100–300 V — can never be subnormal, and a guard there is unreachable code in the hottest loops in the codebase. Those sites get a **comment naming the measured resting value** instead. `TriodeStage.cpp` deliberately has no flush and does not include `Denormal.h`: `vCc_` measures 8.15e-4 V after 20 s of silence (the audit expected it to decay to zero — it does not).
- **Measure which, don't guess.** The AC30's cliff was in the OT bandwidth states, *not* the TOP CUT states that look like the obvious candidates; the JCM800 and Twin power sections had **no** cliff at all. Only bisection showed that. A flush kept where it cannot fire must be **labelled a guard-rail**, not cited as a fix.
- **To tell a denormal cost from any other cost**, compare a unit's silence time against **its own silence time with hardware FTZ+DAZ forced on** — never against its signal time. The Muff is 3.01× dearer on silence with *zero* denormal problem (`BjtStage`'s Newton iterates more near the quiescent point); its hwFTZ column shows the same 3.01×. `build/denormal_bench` section 2 does this per unit and prints both.
- **A double subnormal is invisible in the audio** (`float(1e-310)` is exactly `0.0f`), so an output-only test cannot see it — that is how this survived a green suite *and* a benchmark reporting a clean 1.00× cliff, because the benchmark only covered the two classes already fixed. Classes with zero-resting `double` state expose `double maxAbsRestingState() const` and the tests assert it is **exactly 0.0** after a silent tail. Don't add one to a class with no zero-resting state — the assertion would have no teeth.
- **`chowdsp` WDF capacitor state** is guarded via `flushDenormalWdfCapacitor()`, which reaches the library's private `z` through its own public API (`wdf.a` mirrors it; `incident(0)` zeroes both) — no fork of the SHA-pinned dependency. Call it **after** reading the sample's output voltage, never before.

### Oversampling

4× is the measured default for every nonlinear stage (2× measures at ~−21 dB alias floor, 4× at ~−104 dB). Nonlinearities go *inside* the oversampled domain; linear filters and volume networks stay at base rate. Prefer **one** oversampling domain around a cascade over one per stage — per-stage oversampling stacks group delay and repeats band-limiting (audit performance item 1).

The halfband filters under all of it (`HalfbandFilter.h`) are the hottest loop in the project and are held to a **bit-identical** bar: performance work there must produce `0` differing bits against the previous implementation (`clipper_halfband_tests` keeps a copy of it), which rules out SIMD, `-ffast-math`, and any reordering of the tap sum — float addition is not associative (docs §32, ADR 005). Their delay line is a **doubled** ring buffer; every write must update both halves, `reset()` included.

### The chain exists twice — keep parity

The rig graph is implemented in `web/worklet/clipper-processor.js` (JS) and `native/src/ClipperEngine.cpp` (C++). They must stay behaviourally identical: same order, same declick discipline, same latency accounting. `native/tests/identical_core_test.cpp` proves the *plugin* matches a hand-built core chain, but nothing currently proves *web* matches *native* — so when you change one, change the other in the same slice and say so in the PR body.

### The committed WASM artifact

`web/public/generated/clipper.js`, `web/public/generated/clipper-processor.js` and `web/public/generated/.build-stamp.json` are **committed build output**, so a `git pull` updates the engine without an Emscripten toolchain. The contract: **if you change `core/` or `web/worklet/`, run `bash scripts/build-wasm.sh` and commit all three regenerated files in the same commit.** A stale artifact means new UI bound to an old engine — knobs that do nothing.

This is now **enforced** (docs §29.1, ADR 004). `check-artifact.mjs` — wired into `prebuild`, `npm test` and CI — recomputes a SHA-256 over the contents of the 65 inputs that affect the artifact and fails naming the file that changed. What it covers: everything under `core/src/` and `core/include/`, `web/worklet/clipper-processor.js`, and the emcc flag region of `scripts/build-wasm.sh` (hashed *out of the script*, between its `STAMP:EMCC-ARGS` markers — don't move a flag outside them). What it deliberately doesn't: `core/tools/`, `core/tests/` (measured: zero files from either are in the artifact's compile closure) and the pinned `chowdsp_wdf` SHA in `core/CMakeLists.txt` (a known gap). The served worklet is additionally compared **byte-for-byte** with its source, because `build-wasm.sh` copies it with `cp`. It all runs without emsdk, by design.

### Measure, don't assert

This is the project's hardest-won convention, and the audit found it is the one most often skipped. A test must assert a **player-observable property**, not an identity:

- Don't compare the implementation against an analytic reference **derived from the same netlist** — that validates the discretization (which is fine) and can never catch a wrong topology or a wrong component value.
- Don't assert an algebraic tautology (`f(v) − f(−v)` is odd) and call it a test of the amp.
- Don't assert `performance.now() delta > 0` and call it a perf test.
- Don't check DC offset on **silent** input only — asymmetric clipping produces DC on *signal*.
- For a "no click" test, land the change **mid-render** (`ctx.suspend()` / `resume()`), not before rendering starts — otherwise an entirely removed declick still passes.
- Prefer an absolute reference where one exists: a published curve, a datasheet limit, an insertion loss at 1 kHz, a measured unit.

`core/tests/test_player_expectations.cpp` is the model to follow — it drives gear the way a player does (default knobs, min knobs, realistic levels, realistic noise, the worklet's exact calling conventions) and pins what they must hear.

**Shared test-support headers exist to make this convention enforceable rather than aspirational (docs §29 — read it before writing a test):**

- **`core/tests/support/AssertsLive.h`** — include it from every new test `.cpp`, and call `clipper::test::requireAssertsLive()` first in `main`. It is a **compile error** if `NDEBUG` reaches a test translation unit, because `assert()` would then compile away and the whole binary would pass unconditionally. Register new targets with `clipper_add_test_flags()`, never by hand-rolling the flags.
- **`core/tests/support/Xfail.h`** — the ONLY sanctioned way to leave a correct-but-currently-failing assertion in the tree. `expectXfail(measuredProperty, decl, detail)` prints the real number and names the finding; `reportXfails()` is the return value of `main`. **An XPASS fails the suite**, so a fixed defect cannot leave a stale XFAIL behind. Add `clipper_add_xfail_ledger(<target>)` in CMake so it shows as `***Skipped` in a plain `ctest` run. Do **not** `#if 0` a property, and do **not** loosen a bound to go green.
- **`core/tests/support/LtpProbe.h`** / **`DcOffset.h`** — the phase-inverter and DC-offset properties, defined once so the three amps and four pedals cannot drift on what "healthy" means. Extend these rather than re-deriving a bar per file.
- **`web/tests/support/finite-output.ts`** — two lines in a `test.beforeEach` and every offline render in that spec is scanned for non-finite samples, in every channel. Adopt it in any spec that renders audio.

And when you rewrite a test, **prove it has teeth**: perturb the constant or topology it names in a scratch copy, confirm it fails, revert, and record the perturbation in the PR body. A rewritten test that still cannot fail has not been fixed. (Perturbation harnesses must `touch` the file after both patch and restore — restoring a backup preserves the backup's mtime, `make` skips the rebuild, and you measure stale code. This produced one false reading in the §29 slice.)

### Goldens

Blessed renders live in `core/tests/goldens/`. `scripts/update-goldens.sh` re-blesses them and is a **deliberate act only**: re-blessing is how a regression becomes canon, and a reviewer cannot see the drift in a `.wav` diff. The script now enforces that rather than asking nicely — clean working tree, a printed per-golden dB table measured against the *previous* goldens, a confirmation typed at a terminal (`/dev/tty`, so `yes |` and CI cannot answer it), and a justification appended to `core/tests/goldens/GOLDENS.md` and staged with the goldens. CI fails a PR that changes a `.wav` there without a changelog entry. Say in the PR body what changed audibly and why it is correct, too.

Do not reach for `clipper_player_expectations_tests --update-goldens` directly — it is what the script drives, and using it raw skips every one of those gates. `--golden-report` measures without writing, which is the safe way to see where you stand.

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
| `PORT`              | optional | Proxy port (default 8787; `0` = ephemeral)                              |
| `HOST`              | optional | Proxy bind address (default `127.0.0.1`). The proxy is an unauthenticated relay to your key — only widen this on a network you trust |
| `MOCK`              | optional | Serve canned assistant responses — no API key, no network               |
| `EMSDK_VERSION`     | optional | Emscripten version for `scripts/setup-emsdk.sh`. **Pinned: `6.0.4`.** `latest` made the committed artifact irreproducible; if you bump it, rebuild the artifact in the same slice |

The Electron app stores the key in the OS user-config directory at mode `0600`, resolved by `electron/config.mjs`. It is never written into the repo and never reaches the renderer.

---

## Deferred / Post-v1.1

- **The 2026-07-24 audit backlog** — `docs/audits/2026-07-24-project-audit.md` is the ordered list. Its own suggested order: the three shipping-blockers (findings 1 and 2 done 2026-07-25; finding 3 remains), then bind the proxy to loopback, then stand up CI *before* the circuit work (so fidelity changes are measured rather than asserted), then the AC30 sag + stack topology + the shared PI tail reference.
- **Get the cab IR spectra off the audio thread properly** — `amp_prepare_cab_*` still runs on the audio *rendering* thread (that is where `onmessage` lives). The real fix computes the partitioned spectra on the **main** thread and copies them into the inactive pair; needs an ABI exposing the partition layout (or a main-thread WASM instance used purely as an IR compiler). Docs §29.
- **Wire up the reset exports** — `*_reset` exists in the C ABI but nothing calls it. Wanted: a worklet watchdog that notices non-finite output in `process()` and schedules a declick-bracketed reset, and/or an explicit affordance. Also `ClipperEngine::reset()` for native parity.
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
- Don't add a recursive state whose rest value is zero without a `flushDenormal` guard — WASM has no FTZ. Conversely, don't guard a state that rests at a nonzero DC operating point; comment it with the measured resting value instead (docs §33, ADR 006)
- Don't judge a denormal cost by the silence/signal time ratio — compare against hardware FTZ on the same unit, or you will "fix" a Newton-iteration cost with a flush
- Don't change `core/` or `web/worklet/` without rebuilding and committing the WASM artifact
- Don't change the chain in one front-end without changing the other
- Don't write a test that asserts an identity, a tautology, or the implementation against a reference derived from the same code
- Don't `#if 0` a failing property, and don't loosen a bound to go green — use `expectXfail` from `core/tests/support/Xfail.h` so the defect is measured, printed and named. An **XPASS is a hard failure**: if fixing a defect turns the suite red, delete the XFAIL in the same slice rather than reverting the assertion
- Don't add a test target without `clipper_add_test_flags()` — asserts must be live on every platform, and `support/AssertsLive.h` will fail the build if they are not
- Don't re-bless goldens to make a failing test pass — that is how a regression becomes canon
- Don't hand-write or hand-patch `web/public/generated/.build-stamp.json`, and don't move an emcc flag outside `build-wasm.sh`'s `STAMP:EMCC-ARGS` markers — both turn the staleness guard into decoration
- Don't calibrate a new constant to compensate for a suspected error elsewhere; find the error (this is how `kFullScaleSecV` ended up absorbing two separate factor-of-2 mistakes)
- Don't claim a performance win is fidelity-neutral without a measurement showing it
- Don't hardcode colours, and don't paint a light-theme accent on the dark pedal chassis
- Don't update React state at pointer-move or meter rate
- Don't ship dead UI (controls wired to nothing, or a knob whose top half does nothing)
- Don't commit secrets, `config.json`, or binary blobs other than the sanctioned WASM artifact and goldens
