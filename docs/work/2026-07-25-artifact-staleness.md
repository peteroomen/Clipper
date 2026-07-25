# Artifact staleness guard + a deliberate golden-blessing ritual

**Date:** 2026-07-25
**Branch:** chore/artifact-staleness
**Roadmap item:** 2026-07-24 audit → "Test & process integrity", findings 2 and 3 of that section
(`check-artifact.mjs` only calls `existsSync`; `update-goldens.sh` blesses any regression in one command)

## Goal

Two process holes closed: a committed WASM artifact that is stale relative to `core/` or
`web/worklet/` fails the build with an actionable message, and re-blessing a golden requires a
clean tree, a printed dB summary, a human at a terminal, and a written justification.

## Approach

### 1. Artifact staleness — a content stamp, recomputable without a toolchain

`scripts/build-wasm.sh` writes `web/public/generated/.build-stamp.json` recording a SHA-256 over
the *contents* of everything that actually affects `clipper.js`. `check-artifact.mjs` recomputes
that hash from the working tree and fails if it differs.

The hash inputs, and why each one:

| Input | Why |
| --- | --- |
| every `.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx/.inl/.ipp` under `core/src/` | the artifact's translation units |
| the same under `core/include/` | the header closure those TUs pull in |
| `web/worklet/clipper-processor.js` | the file `build-wasm.sh` copies into `generated/` |
| the marker-delimited emcc source-list + flag region of `scripts/build-wasm.sh` | a flag change (`-O3`, `-msimd128`, `EXPORTED_FUNCTIONS`) changes the artifact as surely as a source change |

Deliberately **excluded**: `core/tools/`, `core/tests/`, `core/CMakeLists.txt`, `core/build/`.
Verified empirically rather than assumed — see "How this will be measured".

Two decisions worth calling out:

- **The flag region is hashed out of `build-wasm.sh` itself**, from between
  `# --- STAMP:EMCC-ARGS BEGIN/END ---` markers, not self-reported into the stamp. Self-reported
  flags cannot detect their own staleness: edit a flag without rebuilding and the stamp would
  still agree with itself. Hashing the region means the checker derives the flags from the same
  single source of truth the build does. Comment-only and blank lines inside the region are
  stripped before hashing so a comment edit does not fire the check.
- **One shared hashing module** (`web/scripts/artifact-stamp.mjs`) is used by both the writer
  (`build-wasm.sh`, via `node`) and the reader (`check-artifact.mjs`). Sharing the *algorithm* is
  not a self-consistency hole — the *inputs* are the real files on disk in both directions. Node
  is already required for the web workspace, so this also avoids `sha256sum` vs `shasum -a 256`.

Sub-checks, all reported together rather than first-failure-wins:

1. `clipper.js` and `clipper-processor.js` exist (the old behaviour, kept).
2. `generated/clipper-processor.js` is **byte-identical** to `web/worklet/clipper-processor.js`.
   `build-wasm.sh` copies it with `cp`, so an exact comparison is possible and a hash would be a
   worse answer.
3. The stamp exists, parses, and its `sourceHash` matches a fresh recomputation. On mismatch the
   per-file map in the stamp is diffed against the tree so the message *names* the files.
4. Advisory only: if `emcc` is on `PATH`, its version is compared to the recorded one. Absent →
   the sub-check is skipped with a printed notice (CI's web job has no emsdk and must still pass).
   A mismatch is a **warning**, not a failure: a newer emcc does not make the artifact stale
   relative to the source, it only means a rebuild would produce different bytes.

`EMSDK_VERSION` in `scripts/setup-emsdk.sh` is pinned from `latest` to `6.0.4`, the version known
to reproduce the current artifact byte-for-byte.

### 2. Goldens — blessing becomes a ritual

`core/tests/test_player_expectations.cpp` currently does `if (update) writeGolden(r); compareGolden(r);`
— it compares against the file it just wrote, so the ±1.5 dB gate can only ever see 16-bit
quantisation (≤0.11 dB). Fixed by measuring against the **previous** golden before writing, and
by adding a report-only mode so the summary can be shown before anything is overwritten:

- `--golden-report`: measure vs the committed goldens, print one machine-readable
  `GOLDEN-DELTA <name> <status> <rmsDb> <worstDb> <worstHz> <bands>` line each, write nothing,
  assert nothing.
- `--update-goldens`: measure vs the previous golden, print the same lines, *then* write. After
  writing it round-trips the file it wrote against a ≤0.15 dB tolerance — kept, but labelled as a
  write-path check, not the voicing gate.

`scripts/update-goldens.sh` becomes: clean tree → build → `--golden-report` → print the per-golden
before/after table → abort if nothing exceeds the quantisation floor → confirmation read from
`/dev/tty` requiring the exact phrase `bless <N> goldens` → justification (≥ 20 chars, `-m` or
prompted) → `--update-goldens` → append an entry to `core/tests/goldens/GOLDENS.md` → `git add`
the goldens and the changelog together.

`/dev/tty` rather than stdin is what makes `yes | scripts/update-goldens.sh` fail: a pipe on stdin
never reaches a `read < /dev/tty`, and in CI there is no controlling terminal at all, so the script
aborts. The required phrase carries the count of changed goldens, so it cannot be answered without
having read the table.

## Steps

- [x] Measure what is actually compiled into the artifact (`g++ -MM` closure over the emcc TU list)
- [x] `web/scripts/artifact-stamp.mjs` — file walk, per-file SHA-256, aggregate hash, marker-region
      extraction, stamp read/write, `--write` CLI
- [x] `scripts/build-wasm.sh` — flags into a marked `EMCC_ARGS` array, stamp written after the build
- [x] `web/scripts/check-artifact.mjs` — the four sub-checks, `--repo-root` seam, actionable message
- [x] `web/scripts/check-artifact.test.mjs` — `node --test` cases against synthetic repo trees
      (the five the brief asked for, plus seven more)
- [x] `package.json` — `test:scripts`
- [x] `scripts/setup-emsdk.sh` — pin `EMSDK_VERSION` to `6.0.4`, warn on a mismatched on-PATH emcc
- [x] `core/tests/test_player_expectations.cpp` — compare against the previous golden; add
      `--golden-report`
- [x] `scripts/update-goldens.sh` — clean tree, summary, `/dev/tty` confirmation, justification
- [x] `core/tests/goldens/GOLDENS.md` — the changelog, seeded with the current state
- [x] `.github/workflows/ci.yml` — rename the artifact step, run `test:scripts`, add a
      goldens-changelog gate (staying out of the `native:` job)
- [x] Docs: plan file, ADR 004, `docs/DEVELOPMENT.md` §29, CLAUDE.md Current State

## How this will be measured

1. **The stamp's coverage claim, verified not asserted.** `g++ -std=c++17 -MM` over the exact 26
   TUs `build-wasm.sh` passes to `emcc`, with the same `-I core/include`, gives the artifact's real
   file closure. The numbers that decide what goes in the hash:
   - files under `core/src/` compiled into the artifact: **26 of 26** (the emcc list is exactly
     `find core/src -type f`)
   - `core/include/` headers in the closure: **36 of 37** (only `OutputLimiter.h` is not)
   - `core/tests/` or `core/tools/` files in the closure: **0**
2. **The staleness check fires for the right edit and not the wrong one.** Five `node --test` cases
   at minimum, run against the pre-fix `check-artifact.mjs` as well as the new one: matching stamp passes; a
   `core/src` edit fails; a diverged worklet copy fails; a missing stamp fails; an edit under
   `core/tests/` or `core/tools/` does **not** fire.
3. **The golden gate measures the right thing.** Before: `--update-goldens` reports worst-band
   Δ ≤ 0.11 dB no matter what changed (it re-reads its own write). After: the reported Δ is against
   the committed golden, so a deliberate voicing change shows a real number in the table.
4. **`yes | scripts/update-goldens.sh` does not bless anything.**

## Manual test steps

- [ ] `cd web && node scripts/check-artifact.mjs` on a clean tree → passes, prints the stamp's
      source hash and a "no emcc, skipping the toolchain sub-check" notice
- [ ] Touch a comment in `core/src/dsp/RatModel.cpp` → the check fails and names that file
- [ ] Edge case: touch `core/tests/test_player_expectations.cpp` → the check still passes
- [ ] Edge case: `echo x >> web/public/generated/clipper-processor.js` → the check fails on the
      byte-identity sub-check, naming `cp`'s source and destination
- [ ] Edge case: `rm web/public/generated/.build-stamp.json` → the check fails with
      "run bash scripts/build-wasm.sh"
- [ ] `yes | bash scripts/update-goldens.sh` → aborts without writing a golden
- [ ] …and again under a real pty (`yes | script -qec … /dev/null`), so the answer actually reaches
      the prompt → still aborts on the phrase
- [ ] `bash scripts/update-goldens.sh` on a dirty tree → aborts before building anything

## Out of scope for this session

- Actually re-blessing any golden. The ritual is built; no `.wav` changes in this slice.
- The audit's sibling finding that `core/CMakeLists.txt:255` guards `-UNDEBUG` behind `if(NOT MSVC)`,
  so the whole expectations suite is a no-op on MSVC. Same section of the audit, different fix.
- Hashing the pinned `chowdsp_wdf` commit SHA into the stamp (see ADR 004 → Consequences).
- Any DSP source change, and any change to the `native:` CI job (a parallel slice owns that line).

---

## What actually happened

Followed the plan. Five things worth recording:

1. **The audit's "`core/tools` and `core/tests` are not compiled in" claim is true, and now
   measured.** The `g++ -MM` closure over the 26 emcc TUs contains zero files from either
   directory. It also turned up that the emcc source list is *exactly* `find core/src -type f`,
   so "hash everything under `core/src/`" over-covers by nothing at all, and that
   `core/include/clipper/dsp/OutputLimiter.h` is the single header in `core/include/` that the
   artifact does not pull in — a one-file over-fire surface, kept deliberately (excluding it would
   mean the hash's file set no longer matches a rule a human can state).

2. **emcc 6.0.4 turned out to be installed** at `/root/emsdk` — not on `PATH`, but exactly where
   `build-wasm.sh` already looks (`EMSDK_DIR=${EMSDK_DIR:-$HOME/emsdk}`). So the stamp could be
   bootstrapped here rather than punted to the orchestrator. But the rebuild was **not
   byte-identical**, and the reason turned out to be the most interesting thing in the slice:

   The rebuilt `clipper.js` was exactly the same size (173337 B) and differed in exactly **64
   bytes** — four 16-character runs. All four are inside *absolute build paths embedded in the
   WASM*: the committed artifact carries `…/worktrees/agent-ab1bbfef070dfac5b/core/src/dsp/RatModel.cpp`
   where the rebuild carries this worktree's id. Same length, hence same file size, hence a
   difference `ls -la` could never show. They are `__FILE__` strings from live `assert()`s — the
   emcc link is `-O3` but never defines `NDEBUG` — so `assert()` in `Oversampler.h`,
   `RatModel.cpp`, `GoldModel.cpp` and `OverdriveEngine.cpp` ships inside the audio engine
   together with the build directory of whoever compiled it.

   Consequences, all recorded rather than acted on: CLAUDE.md's "6.0.4 reproduces the committed
   artifact byte-for-byte" is true only when building from an identically-named directory; a
   future rebuild-and-`cmp` CI job needs `-ffile-prefix-map`; and whether `assert()` belongs in a
   shipped real-time engine is an open question. Since the brief said not to change the artifact,
   the committed bytes were **restored** and only `.build-stamp.json` is added — which is sound,
   because the stamp attests *source content*, and the source content is identical either way
   (the same `sourceHash` came out of both trees).

3. **The `` `# comment` `` trick inside the old emcc argument list had to go.** Moving the flags
   into a bash array meant those backtick-comment words became array elements evaluating to the
   empty string; unquoted they vanish, so it happened to still work, but it is a trap. They are
   now ordinary `#` comment lines inside the array literal, which bash accepts.

4. **The pre-fix `check-artifact.mjs` demonstration is honest rather than notional.** The new test
   file resolves the script under test from `CLIPPER_CHECK_ARTIFACT_SCRIPT`, so the *same* cases
   could be pointed at `git show origin/main:web/scripts/check-artifact.mjs` — copied into
   `web/scripts/` so its `resolve(here, '../public/generated/…')` still hit the real repo, which is
   what makes the old script exit 0. **8 of 12 fail** against it. Of the five cases the brief
   asked for, three fail before the fix (`core/src` edit, worklet divergence, missing stamp) and
   two pass vacuously (a healthy tree; a `core/tests`/`core/tools` edit) — the old script has no
   notion of staleness to get wrong, so those two exist to pin the *absence* of false positives
   now that it does.

   One pre-fix result needs an asterisk, and it is mine not the old script's: the "artifact itself
   is missing" case also fails pre-fix, but only because the old script ignores
   `--repo-root` and went on checking the real repo's `web/public/generated/`, which exists. The
   old script did have a working existence check. That case is a regression guard, not evidence.

5. **The golden self-comparison was worse than "measures quantisation".** `compareGolden` also
   asserts `frames == rig.audio.size()`, and in update mode it read back the file it had just
   written — so a change that altered the render *length* would also have sailed through. The
   report/compare split fixed both at once.

## Measured results

**Artifact staleness**

| | before | after |
| --- | --- | --- |
| `check-artifact.mjs` sub-checks | 1 (existence) | 4 (existence, byte-identity, source hash, toolchain advisory) |
| inputs whose contents are covered | 0 | **65** = 26 `core/src` + 37 `core/include` + the worklet + the emcc flag region |
| detects a `core/src` edit without a rebuild | no | yes, and names the file |
| detects a diverged worklet copy | no | yes |
| detects an emcc flag edit (`-O3` → `-O2`) | no | yes |
| runs without emsdk | yes | yes (toolchain sub-check skipped with a printed notice) |
| fires on a `core/tests` / `core/tools` edit | n/a | no |

- Compile-closure measurement (`g++ -std=c++17 -MM` over the 26 emcc TUs): **62-file closure**,
  **26 of 26** `core/src` files compiled in, **36 of 37** `core/include` headers reachable
  (`OutputLimiter.h` is the exception), **0** files from `core/tests/` or `core/tools/`.
- Committed `clipper.js`: 173337 bytes, `sha256 7cdf82d6…`. Unchanged by this slice; the 6.0.4
  rebuild differed in 64 path bytes (see above) so the committed bytes were restored, and the
  stamp records that same hash.
- New-test verdict against the pre-fix script: **8 of 12 fail**; of the five the brief asked for,
  3 fail and 2 pass vacuously. Against the new script: **12 of 12 pass**.
- Node suites 45 → **57**: `test:server` 15, `test:history` 10, `electron` 20, new `test:scripts`
  12 — the 5 the brief asked for, plus a `core/include` edit, a worklet-source edit, a malformed
  stamp, an emcc flag edit, a missing artifact, and two pinning the toolchain sub-check as
  advisory (a mismatched emcc warns and does not fail; an absent emcc is a skip note).
- The check itself costs ~70 ms and no toolchain.

**Goldens**

- `--update-goldens` worst-band Δ reported, before: **≤ 0.11 dB by construction** for any change
  whatsoever, because it re-read its own write. After: measured against the committed golden.
  On the current source all five rigs report `UNCHANGED` —

  | golden | RMS Δ | worst band Δ | bands |
  | --- | --- | --- | --- |
  | `rat_jcm800` | +0.00 dB | 0.00 dB @ 1008 Hz | 10 |
  | `sd1_twin_reverb` | +0.00 dB | 0.07 dB @ 317 Hz | 13 |
  | `muff_twin` | +0.00 dB | 0.00 dB @ 252 Hz | 13 |
  | `ts_ac30` | +0.00 dB | 0.00 dB @ 1008 Hz | 7 |
  | `clean120_chorus` | −0.00 dB | 0.11 dB @ 252 Hz | 7 |

  which is a null result for this slice *and* the calibration for the 0.15 dB `UNCHANGED`
  threshold: the worst observed storage/windowing floor is 0.11 dB.
- **The direct before/after on the self-comparison bug**, measured by planting a genuinely wrong
  golden (`rat_jcm800.wav` replaced with `muff_twin.wav` — same format, same length, different
  audio) on a throwaway commit and then discarding it:

  | | reported worst band Δ | outcome |
  | --- | --- | --- |
  | pre-fix `--update-goldens` (write, then re-read the write) | **0.00 dB** @ 1008 Hz | silently rewrote the golden and printed "within the ±1.5 dB voicing gate" |
  | post-fix `--golden-report` (measure vs the previous golden) | **17.35 dB** @ 800 Hz, RMS +2.59 dB | flagged `CHANGED`, wrote nothing |

  A 17 dB voicing error reported as 0.00 dB is the sharpest possible statement of what
  "compare against the file you just wrote" was worth.
- Dirty-tree run: aborts **before `cmake` is invoked** (verified — no build output).
- Both confirmation defences exercised on a clean tree with that differing golden present:

  | attempt | result |
  | --- | --- |
  | `yes \| bash scripts/update-goldens.sh` (no controlling terminal — the CI case) | prints the table, then aborts: "requires an interactive terminal" |
  | `yes \| script -qec 'bash scripts/update-goldens.sh' /dev/null` (a **real pty**, so `yes` genuinely reaches the prompt) | reaches the prompt, `y` ≠ `bless 1 goldens`, aborts |

  In both cases: **0 goldens written**, golden SHA-256 unchanged, nothing staged. The second is the
  stronger test — the phrase requirement, not just the absence of a tty, is what stops an automated
  answer.
- Nothing in `core/tests/goldens/*.wav` changed in this slice (`git diff origin/main --
  core/tests/goldens/` is `GOLDENS.md` only).

**Core suite:** 17/17 ctest targets pass (the brief said 18; 17 is the count on this `main` —
`clipper_player_expectations_tests` and `clipper_nan_guard_tests` included). Web: `tsc --noEmit` +
`vite build` green, Playwright **70/70**.

## Files created / modified

Created:

- `web/scripts/artifact-stamp.mjs` — the shared stamp/hash module + `--write` CLI
- `web/scripts/check-artifact.test.mjs` — 12 `node --test` cases
- `web/public/generated/.build-stamp.json` — new committed build output (7.7 KB)
- `core/tests/goldens/GOLDENS.md` — the blessing changelog
- `docs/decisions/004-the-committed-artifact-is-guarded-by-a-content-stamp.md`
- `docs/work/2026-07-25-artifact-staleness.md` (this file)

Modified:

- `web/scripts/check-artifact.mjs` — existence-only → four sub-checks
- `scripts/build-wasm.sh` — marked `EMCC_ARGS` region; writes the stamp
- `scripts/setup-emsdk.sh` — `EMSDK_VERSION` pinned to `6.0.4`; warns on a mismatched on-PATH emcc
- `scripts/update-goldens.sh` — clean tree, summary table, `/dev/tty` confirmation, justification
- `core/tests/test_player_expectations.cpp` — compare against the previous golden; `--golden-report`
- `package.json` — `test:scripts`
- `.github/workflows/ci.yml` — artifact step renamed and now a staleness gate; `test:scripts` in
  `node-suites`; new PR-only `goldens` job
- `CLAUDE.md`, `docs/DEVELOPMENT.md` (§29)

## Deferred to next session

- **The `chowdsp_wdf` pin is not in the stamp.** Bumping the pinned commit SHA in
  `core/CMakeLists.txt` changes the artifact and the check will not notice. Hashing all of
  `core/CMakeLists.txt` would fire on every unrelated test-target edit; the honest fix is a
  marked region around the `FetchContent` pin, the same trick used for the emcc flags. Cheap, but
  it is a second marker contract and this slice already introduces one.
- **`OutputLimiter.h` is hashed but not compiled in** — one spurious-fire surface, documented in
  ADR 004 rather than special-cased.
- **The audit's MSVC `-UNDEBUG` hole** (`core/CMakeLists.txt:255`) is untouched: on MSVC the
  expectations suite still compiles to a no-op that prints success with zero goldens present.
- **`assert()` ships inside the WASM audio engine, with absolute build paths attached.** The emcc
  link is `-O3` with no `-DNDEBUG` (unlike the ctest build, which deliberately forces `-UNDEBUG`).
  Two separate questions fall out, neither answered here: should a real-time audio engine abort on
  a failed assert in the browser, and should the artifact carry `-ffile-prefix-map` so it stops
  embedding the builder's directory? The second is a prerequisite for any reproducible-build CI
  job. Note that adding `-DNDEBUG` would be a **behaviour change to the shipped engine**, not a
  tidy-up, so it needs arguing as one.
- **Nothing proves the *committed* artifact matches the *current* stamp's own inputs on a fresh
  clone** beyond the hash — i.e. the stamp attests "these sources built the artifact next to me",
  not "this `.wasm` contains that code". Byte-level attestation would need a reproducible-build
  step in CI (emsdk 6.0.4 pinned, rebuild, `cmp`). That is a bigger, genuinely valuable slice now
  that the version is pinned.
- **`update-goldens.sh` stages what it writes** (`git add`) to make "the commit must include the
  changelog" hard to get wrong. The CI `goldens` job enforces the same rule at PR level. Neither
  can force the two into the *same* commit; that would need a pre-commit hook.

## Status

- [x] Complete
