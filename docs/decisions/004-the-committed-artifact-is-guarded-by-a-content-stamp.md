# ADR 004: The committed WASM artifact is guarded by a source-content stamp, not by trust

Date: 2026-07-25
Status: Accepted

## Context

`web/public/generated/clipper.js` is committed build output. That is a deliberate choice — a
`git pull` updates the engine without anyone installing Emscripten — and it comes with a contract
(CLAUDE.md → "The committed WASM artifact"): change `core/` or `web/worklet/`, rebuild, commit the
artifact in the same commit. A stale artifact means new UI bound to an old engine, which presents
as knobs that do nothing rather than as a build error.

Nothing enforced the contract. `web/scripts/check-artifact.mjs` called `existsSync` on two paths
and passed for an arbitrarily old artifact, and it was the *sole* guard — wired into `prebuild`,
`npm test`, and (once CI existed) the web job. The 2026-07-24 audit flagged this, noting the two
artifacts happened to be in sync "but by luck".

The luck ran out on 2026-07-24. Two PRs each changed `core/` and each rebuilt `clipper.js`. The
merge conflicted on the binary. Taking either side would have produced a `main` whose committed
engine contained **one** of the two fixes while the committed source contained **both**, and no
check in the repo — not `ctest`, not Playwright, not `check-artifact.mjs` — could have detected it.
The correct artifact, rebuilt from merged source, was 173337 bytes; the two conflicting inputs were
165971 and 172290.

Three properties were needed of any fix:

1. It must detect a `core/` change with no rebuild — the actual failure above.
2. It must run with **no Emscripten toolchain**, because the CI job that would catch this has no
   emsdk and installing one there would cost minutes per run.
3. It must not fire on unrelated edits. A guard that cries wolf on every test edit gets deleted,
   and this one is load-bearing.

## Decision

`scripts/build-wasm.sh` writes `web/public/generated/.build-stamp.json` — committed build output,
like the artifact itself — recording a SHA-256 over the **contents** of every input that affects
the artifact, plus a per-input hash map. `check-artifact.mjs` recomputes that hash from the working
tree and fails when it differs, naming the inputs that changed. Both directions call one shared
module, `web/scripts/artifact-stamp.mjs`.

Hashed: every C/C++ source and header under `core/src/` and `core/include/`;
`web/worklet/clipper-processor.js`; and the emcc source-list + flag region of `scripts/build-wasm.sh`.
Sorted by path, contents only, never mtimes — so the hash is deterministic and order-independent.

Four decisions inside that:

- **The emcc flags are hashed out of `build-wasm.sh`, from between
  `# --- STAMP:EMCC-ARGS BEGIN/END ---` markers, rather than recorded into the stamp.** Recorded
  flags cannot detect their own staleness: change `-O3` to `-O2` without rebuilding and a
  self-reported stamp still agrees with itself. Hashing the region means the checker derives the
  flags from the same single source of truth the build uses. Blank and comment-only lines inside
  the region are stripped first, so commenting freely there does not fire the check.
- **`core/tools/` and `core/tests/` are excluded, and that was measured rather than assumed.**
  `g++ -std=c++17 -MM` over the exact 26 translation units `build-wasm.sh` hands to `emcc`, with
  the same `-I core/include`, yields a 62-file closure containing zero files from either directory.
  The same measurement showed the emcc source list is *exactly* `find core/src -type f` (26 of 26),
  and that 36 of the 37 headers in `core/include/` are in the closure.
- **The emcc version is recorded but never hashed.** Hashing it would make the source hash
  unrecomputable without a toolchain, defeating requirement 2. It is compared only when `emcc`
  happens to be on `PATH`, and a mismatch is a **warning**, not a failure: a newer emcc does not
  make the artifact stale relative to the source, it only means a rebuild would produce different
  bytes.
- **The worklet copy is compared byte-for-byte, not by hash.** `build-wasm.sh` produces it with
  `cp`, so exact equality is the true invariant and a hash would only obscure it.

Separately, `EMSDK_VERSION` in `scripts/setup-emsdk.sh` is pinned from `latest` to **6.0.4**.
`latest` meant two machines with identical source could not agree on the artifact bytes, so nothing
could be said about reproducibility at all.

## Consequences

**Easier.** The failure mode that nearly shipped is now a build failure with the fix in the
message. A merge that conflicts on `clipper.js` now also conflicts on `.build-stamp.json`, and that
conflict is *legible* — two different `sourceHash` values in a text file, which reads as "rebuild
from the merged source" instead of "pick a side of a binary". The check costs ~70 ms and no
toolchain.

**Harder / the costs.**

- A rebuild is now mandatory rather than conventional. Changing one byte of `core/` and not
  rebuilding fails `npm run build`, `npm test`, and CI. That is the point, but it means a
  source-only PR that cannot rebuild (no emsdk) is blocked rather than merely discouraged.
- `core/include/clipper/dsp/OutputLimiter.h` is hashed but is *not* compiled into the artifact —
  the one header in `core/include/` outside the closure. Editing it forces a rebuild that changes
  nothing. Special-casing it was rejected: the hash's file set should be a rule a human can state
  ("everything under `core/src` and `core/include`"), not a list with an exception, and the header
  is a plausible future member of the artifact anyway.
- **The pinned `chowdsp_wdf` commit is not covered.** Bumping the `FetchContent` SHA in
  `core/CMakeLists.txt` changes the artifact and the stamp will not notice. Hashing the whole
  `CMakeLists.txt` would fire on every unrelated test-target edit; the honest fix is a second
  marked region around the pin, deferred rather than done, because this slice already introduces
  one marker contract and two is a pattern worth deciding on deliberately.
- **The stamp attests source content, not artifact bytes.** It says "these sources built the
  artifact next to me"; it cannot say "this WASM contains that code". Byte-level attestation would
  need a reproducible rebuild in CI — which the pin now makes conceivable, but see below.
- **A discovery that blocks that follow-up: the artifact is not path-independent.** Rebuilding at
  the pinned 6.0.4 during this slice produced a file of exactly the same size that differed in
  exactly 64 bytes, all of them inside absolute build paths embedded in the WASM. They come from
  `__FILE__` in live `assert()`s — the emcc link has `-O3` but no `-DNDEBUG`, so the asserts in
  `Oversampler.h`, `RatModel.cpp`, `GoldModel.cpp` and `OverdriveEngine.cpp` ship in the engine
  along with the absolute path of whoever built it. So "6.0.4 reproduces the artifact byte-for-byte"
  holds only when building from an identically-named directory, and any future rebuild-and-`cmp`
  CI job needs `-ffile-prefix-map` (and a decision about whether asserts belong in the shipped
  audio engine at all) first. Both are out of scope here and recorded in
  `docs/work/2026-07-25-artifact-staleness.md`.
- The stamp records `generatedAt`, so every rebuild produces a diff even when the source hash is
  unchanged. Accepted: it makes "when was this engine built" answerable from the tree.
