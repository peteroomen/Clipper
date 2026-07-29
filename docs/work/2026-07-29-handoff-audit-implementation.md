# HANDOFF — audit implementation, session of 2026-07-25 → 2026-07-29

**Read this before starting the next session.** It is the state of the 2026-07-24 audit
backlog: what is merged, what is on a branch and how far along it is, what must NOT be
done, and the traps this session hit so the next one doesn't. The audit itself
(`docs/audits/2026-07-24-project-audit.md`) remains the source of truth for the backlog;
this file is the source of truth for *where each item stands*.

The session ended on the **weekly usage limit**, twice — six agents were killed mid-work
the first time, three the second. Everything was preserved: every branch below is pushed,
every local HEAD matched its remote at handoff, and no worktree had uncommitted changes.

---

## 1. Merged to `main` (done, verified, do not redo)

Ten audit findings plus the process block. In merge order: CI (`ci.yml`), proxy loopback
(finding 17), NaN parameter guard + reset tree (finding 1, §28), cab swap off the render
path (finding 2, §30), CabConvolver any-block-size (finding 3), artifact staleness stamp +
golden re-bless ritual (§31, ADR 004), vacuous-test sweep (§29), halfband no-modulo
(perf 2, §32 — bit-identical), denormal guards (finding 11, §33), Muff Newton early-out
(finding 12, §34), **native CI job made blocking** (#13), **diode ideality** (finding 15,
§36, ADR 008 — `rat_jcm800` re-blessed on owner approval), **valve-amp smoothing**
(finding 6, §35), **Muff output DC blocker** (finding 16 DC half, §37, ADR 009 —
`muff_twin` re-blessed, −0.03 dB broadband).

The engineering log reads §28–§37 in order. ADRs 001–009 assigned. **§38/§39/§40/§41 and
ADRs 010/011/012/013 are RESERVED** for the four open branches below — do not renumber.

## 2. Open branches, closest-to-done first

### `test/web-render-silence-guard` — §41, ADR 013. ~80 % done, HIGHEST PRIORITY
Fixes the intermittent all-zero Playwright renders and the `retries: 2` hole.
**The diagnosis is DONE, CORRECT, and written in `web/tests/support/render-guard.ts`'s
header — do not re-derive it.** It is a race between the `AudioWorkletProcessor` being
installed onto the offline render thread and `startRendering()`; the `ready`/`latency`
messages order against the worklet, not the renderer. It is **NOT** context accumulation
(disproved 16/16, and `OfflineAudioContext.prototype.close` does not exist in this
Chromium) — that was the orchestrator's hypothesis and it was walked back. The fix
(`installOfflineRenderBarrier`) is a real happens-before: suspend at frame 0, start
rendering, await the suspension, resume. Wall-clock barriers measured insufficient.
Done: barrier + silence guard adopted across amp/audio/cab/expectations/tuner specs,
ADR 013 ("a flake is a failure"), `zzprobe.spec.ts` deleted, `PW_PORT` env override.
**Remaining:** confirm what `retries` is actually set to (brief demands 0); flake rate
before → after across ≥5 full-suite `--retries=0` runs under comparable load; the
perturbation proof (break setup, guard fires with its clear message, restore, re-green);
squash the `wip:` commits onto `origin/main`; fill the plan file; PR.

### `fix/web-input-and-a11y` — §38, ADR 010. ~70 % done
Knobs (focus ring, fine-adjust, Home/End/PageUp), popover Escape/click-outside/focus-trap,
Upload IR keyboard-reachable, the `.pedal` light-theme contrast pin, chat scroll pinning +
aria-live, tuner rAF gating, `Board.tsx` zero-DOMRect cable fix. New `Menu.tsx`,
`a11y.spec.ts`.
**Known problem, found by the finishing agent right before it died: the draft's
focus-ring "before" contrast figure (2.83:1, and 6.35:1 in two CSS comments) does NOT
reproduce.** Re-derive every contrast number programmatically in both themes before
trusting anything in the draft's comments; the agent's browser cross-check matched its
own computation, so the machinery exists in its transcript work. It was also checking
readout opacity when killed.
**Remaining:** re-derive the contrast table (before → after, per readout, both themes vs
4.5:1); measured tuner rAF rate; measured knob px-per-unit; run tsc + Playwright;
verify its earlier `CLAUDE.md`/`DEVELOPMENT.md` edits are not stale (main has moved a
lot); squash; PR.

### `fix/native-input-and-a11y` — §40, ADR 012. ~60 % done
Mouse-up-inside + left-button-only + drag-off-abort (right-click must reach the host),
widgets re-based onto `juce::Button`/`juce::Slider`, `NeuKnob::setName` shadowing fix,
repaint-storm work, `paint_bench.cpp` harness.
**The paint bench had JUST built when the agent died — NO repaint measurement exists.**
**Remaining:** run the bench (before → after: DropShadow passes + offscreen allocations
per second in an edge-drag, ValueTree writes per drag-reorder); the abort-path proof
(drag-off and right-click must not toggle); keep `clipper_identical_core` green (it is a
BLOCKING CI job; use `ctest -R 'clipper_identical_core|clipper_chain_edit'`); decide the
1040 px minimum width; squash; PR.
**Trap:** `native/build-bench/` (497 MB) is uncommitted and `.gitignore` only lists
`native/build/` — a careless `git add -A` commits half a gigabyte. Add it to `.gitignore`
in this slice.

### `feat/undo-ring` — §39, ADR 011. Untouched draft, DELIBERATELY held
Undo ring over rig snapshots + assistant cancel (`AbortSignal` through `runAssistant`) +
focus not stolen mid-turn. Held because it shares `Chat.tsx` with `fix/web-input-and-a11y`
— **land that first, then rebase this on top.** ADR 011 draft exists
(`011-undo-is-a-ring-of-rig-snapshots.md`).
**Security check required before merge:** the draft edits `web/src/assistant/tools.ts`,
the best-defended surface in the codebase. A snapshot restored from persistence is
untrusted input — verify the undo path re-validates through the allowlist/clamp.
Measurements owed: undo entries per 3-second knob drag before/after coalescing; memory
per ring entry; the three Playwright properties (dialled-in values actually restored;
one assistant turn = one undo unit; cancel leaves rig unchanged and focus not on body).

### `fix/muff-dc-and-bass` — the deferred bass half. DO NOT MERGE AS-IS
Holds the working 47 kΩ series-base-resistor implementation, the low-end assertions, the
slam-convergence fix, and `kOutputTrim` 0.45. ADR 009 (on `main`) names the required
order: **(1)** settle why the clip-stage base node measures ~1.8 kΩ against the real
stage's ~4 kΩ; **(2)** then choose Rb — ideally the schematic value; **(3)** then
re-derive `kOutputTrim` against the *combination* (the DC-only slice already measures
2.0096 V at SUSTAIN 1.0, over the 2.0 V ceiling — do not stack trims; that is the
`kFullScaleSecV` failure mode). Expect the golden to move ~−5.85 dB broadband / 26 dB @
1270 Hz when this lands — it is the audit's largest tone change and needs its own bless
authorisation. Two XFAILs on `main` (`finding16-muff-almost-no-bass`,
`muff-slam-exhausts-newton-cap`) will XPASS when it lands — delete them in the same
slice, per the ratchet.

## 3. Not started

- **The valve-amp physics cluster — findings 4, 5, 7, 8, 9, 10.** The largest remaining
  block; most of the "doesn't sound like the real amp" surface. MUST be sequential (shared
  files + calibration constants). Suggested lead: **finding 7** (the shared PI tail
  reference, `LtpProbe.h` already measures it) since it fixes all three amps from one
  place; then 8 (`Ra2`), 9 (plate load line), 10 (screen fits), then 4 (AC30 sag) and
  5 (AC30 tone stack) with the re-derive of `kFullScaleSecV`/`kInterstageScale`
  (`fix/amp-calibration` in the original plan). Findings 7/8 and the tremolo have open
  XFAILs — fixing them XPASSes the suites; delete the XFAILs in the same slice.
- Proxy auth + rate limit (loopback is the stopgap) · assistant history poisoning
  (finding 18) · main-thread saturation (finding 19) · native declick parity (13) ·
  `prepare()` discarding parameter targets (14) · perf items 1 and 4–9 ·
  `Oversampler::latencySamples()` half-sample over-report · worklet reset watchdog
  (exports exist, nothing calls them) · native `ClipperEngine::process` chunking ·
  most of the Medium list.

## 4. Owner decisions on record (do not re-ask)

- **"From now on merge at will"** — standing authority to merge green PRs.
- **Golden re-blessing:** owner approved answering `update-goldens.sh`'s `/dev/tty`
  prompt via pty **on explicit per-golden authorisation**; each bless so far records that
  provenance in `GOLDENS.md`. A tone-change bless still gets its per-band dB table put to
  the owner first (the Muff bass half explicitly will).
- **`-DNDEBUG`: keep asserts live** in the shipped engine. Decided; don't flip it.
- **RAT +5 dB louder: accepted, un-compensated.** Downstream level staging is its own
  future slice — do not trim the RAT to hide it.
- **Muff split:** approved; bass half deferred pending the impedance question.
- **`retries`:** orchestrator decision, user accepted — land 0 (or 1 only with a measured
  irreducible rate in a comment). Never restore 2.

## 5. Traps this session hit (cost real time; avoid)

1. **Merge output read through `tail` hid a conflicted file** — a conflict marker reached
   a commit; only the C++ compiler caught it. Always end a merge with
   `grep -rl '^<<<<<<< ' --include=...` over the tree.
2. **Backgrounded `cd` does not persist** — a restore landed in the wrong worktree.
   Absolute paths for every worktree operation.
3. **The committed WASM artifact conflicts on EVERY pair of in-flight core slices** — by
   construction. Never take a side: `git checkout --ours` both files, run
   `bash scripts/build-wasm.sh` on the merged tree, commit all three regenerated files.
   Five consecutive PRs needed this.
4. **`wip:` commits fail CI's Conventional Commits gate.** Squash preserved drafts onto
   `origin/main` before opening a PR; verify the tree is byte-identical after squashing
   (`git diff <old-head> HEAD` empty) so measurements carry over.
5. **The "inert guard" class — three instances found:** the advisory native job (failed
   100 % of runs, hidden), the existence-only artifact check, and `clipper_muff_tests`
   losing its `ledgerMain` while CMake still registered the ledger (which would report
   the XFAIL ledger as Passed). *A guard that is present but inert is worse than absent,
   because it reads as coverage.* When touching any gate, prove it can still fail.
6. **Absolute bench numbers do not transfer across machines** — the JCM800 reads ~58 %
   here vs §32's recorded 53.3 %, and `main` measures the same ~58 % in the same session.
   CPU claims only as interleaved same-machine A/B, ≥3 runs, discard the warm-up run.
7. **Some agent worktrees lack `node_modules`** — surfaces as
   `TS2688: Cannot find type definition file for 'vite/client'`. Run `npm ci` in `web/`.
8. **Port 4173 collisions between concurrent suites** — the silence-guard branch adds a
   `PW_PORT` override; until it merges, check the port before running Playwright.
9. **mtime trap on perturbation tests** — `touch` after both patch and restore or the
   build silently skips and you measure stale code (documented in §29, bit once anyway).

## 6. Owner actions outstanding (not agent-doable)

- Add the **`native`** job to required checks in branch protection
  (Settings → Branches → `main`). CLAUDE.md records this as outstanding.
- Optional: delete the 13 merged remote branches (squash-merged, so GitHub won't flag
  them as merged); left alone deliberately.

## 7. Suggested next-session order

1. Finish + merge `test/web-render-silence-guard` (makes every later web verification
   trustworthy).
2. Finish + merge `fix/web-input-and-a11y`, then rebase and finish `feat/undo-ring`.
3. Finish + merge `fix/native-input-and-a11y`.
4. Start the physics cluster at finding 7, one slice at a time, each with its golden
   bless put to the owner with per-band numbers.
5. Resolve the Muff base-node impedance question; then land the bass half.
