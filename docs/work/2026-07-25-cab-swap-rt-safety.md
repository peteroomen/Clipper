# Cab swap: get the IR synthesis and FFT partitioning out of `process()`

**Date:** 2026-07-25
**Branch:** fix/cab-swap-rt-safety (stacked on `fix/nan-parameter-guard`)
**Roadmap item:** 2026-07-24 audit, finding 2 (shipping-blocker) — plus the two
adjacent defects the same finding calls out (stale `HEAPF32` across the commit;
a second edit inside one fade window committing at non-zero gain).

## Goal

A cab change (built-in select or user IR upload) must not run allocation, IR
synthesis or FFT partitioning inside `process()`. The audio-thread step becomes
an O(1), allocation-free index flip; the heavy work moves into the `cab` message
handler, i.e. **between** render quanta rather than mid-block.

## Approach

**Double-buffer at the C ABI level, not inside `CabConvolver`.** `fix/cab-block-size`
is rewriting `core/src/dsp/CabConvolver.{h,cpp}` in a separate open PR, so this
slice does not touch that file at all. Instead `AmpChain` holds **two** convolver
pairs plus an active index — exactly the pattern `amp_set_model` already uses for
the four amp voices — and the ABI splits in two:

- `amp_prepare_cab_builtin(h, which)` / `amp_prepare_cab_custom(h, ir, len)` —
  synthesise / copy / peak-normalize the IR and run `CabConvolver::prepare` on the
  **inactive** pair. Heavy, allocating, **not** RT-safe. Returns 1 on success.
- `amp_commit_cab(h)` — `active ^= 1`. One integer write. No allocation. Safe to
  call from `process()`.

`amp_set_cab_builtin` / `amp_load_custom_ir` stay as prepare+commit wrappers so the
ABI is purely additive and existing callers (core tests, any FFI) keep working.

**Fidelity: this is a strict refactor, and the claim is bit-identity, not
"close".** The activated convolver is prepared by the same `CabConvolver::prepare`
call with the same IR bytes and the same 128 partition, and `prepare()` zeroes the
FDL/overlap — which is precisely what the pre-fix in-place `prepare()` on the live
convolver did. So the swapped-in cab must produce bit-identical output to the
pre-fix path. The test asserts that against a reference pair built the *old* way.

**Worklet.** Three changes in `web/worklet/clipper-processor.js`:

1. The `cab` message handler calls `_amp_prepare_cab_*` immediately (and frees the
   IR scratch immediately), then stages `_pendingCab = { ready: true }`.
   `_commitPending()` only calls `_amp_commit_cab`.
2. **Defect (a):** re-fetch `HEAPF32` after `_commitPending()` before reading the
   output block. (With the prep moved out, nothing in the commit path grows the
   heap any more — the re-fetch is the belt to that braces.)
3. **Defect (b):** stop the "commit the previous edit immediately" behaviour. A
   second edit arriving mid-fade now **merges into the pending set** and rides the
   fade that is already running, so every topology swap still lands at output zero.
   `_prepareChain` must therefore diff against the *pending* node list when one
   exists, and carry forward the previous edit's `removed` list so no handle leaks.

**Honest scope limit.** `AudioWorkletProcessor.port.onmessage` runs on the audio
**rendering thread**, so this does not make the work "off the audio thread". It
converts a guaranteed multi-quantum stall in the middle of a block into, at worst,
one late quantum at the instant of a deliberate user action — and removes the
allocation from `process()` itself entirely. A fully correct fix needs the
partitioned spectra computed on the main thread and copied in; that is a bigger
architectural change and is explicitly out of scope.

## Steps

- [x] `core/src/clipper_c_api.cpp`: `CabPair cabs[2]` + `activeCab`; add
      `amp_prepare_cab_builtin` / `amp_prepare_cab_custom` / `amp_commit_cab`;
      re-point `amp_latency_samples`, `amp_process`, `amp_process_stereo`,
      `amp_reset` at the active pair (reset clears **both** pairs, same reasoning
      as resetting all four inactive amp voices).
- [x] `scripts/build-wasm.sh`: add the three new exports to `EXPORTED_FUNCTIONS`.
- [x] `web/worklet/clipper-processor.js`: prepare-in-onmessage, O(1) commit,
      `HEAPF32` re-fetch, merge-don't-commit for a second edit in one fade.
- [x] New ctest `clipper_cab_swap_tests` (`core/tests/test_cab_swap.cpp`):
      allocation counting via a replaced global `operator new`, wall-clock
      commit-vs-prepare-vs-process, bit-identity vs the pre-fix construction.
- [x] `web/tests/cab.spec.ts` test 3: rewrite to land the swap **mid-render** via
      `ctx.suspend()`/`resume()`, and swap toward the *brighter* cab so lower slew
      is not free.
- [x] `web/tests/cab.spec.ts`: new test — multiple edits inside one fade window; the
      whole batch must commit at output zero. (Landed as *four* edits, and using
      pedal removals + an amp-power stomp rather than a reorder — see below; a
      reorder measured as non-discriminating.)
- [x] **Not planned, added:** a `'hold'` declick phase covering the swapped-in
      convolver's dead partition (see "What actually happened", item 4).
- [x] Docs: `docs/DEVELOPMENT.md` §30, ADR 003, CLAUDE.md Current State.

## How this will be measured

`core/tests/test_cab_swap.cpp` prints and asserts:

1. **Allocation count** (replaced global `operator new`/`delete`, counted around
   the call only): `amp_commit_cab` == **0** allocations; `amp_prepare_cab_builtin`
   >> 0. This is a direct assertion of "allocation-free", not a proxy for it.
2. **Wall clock** (`std::chrono::steady_clock`, best-of-N): mean `amp_commit_cab`
   must be below one `amp_process_stereo(128)` call, and at least 100× cheaper
   than `amp_prepare_cab_builtin`.
3. **Bit-identity**: 4 s of pink-ish noise through `amp_process_stereo` after a
   prepare+commit swap to the Brit 4×12, versus the identical amp voice driven
   into a locally-owned `CabConvolver` pair prepared the pre-fix way
   (`prepare(sr, generateBrit4x12IR(sr), …, 128)`). `max|a-b|` must be exactly 0.
   Same for a synthetic custom IR through the peak-normalize path.

## Manual test steps

- [ ] `npm run dev`, plug in, play a sustained chord, switch Clean 2×12 → Brit 4×12
      mid-chord. Expect the tone to change with no dropout and no click.
- [ ] Upload a WAV IR mid-chord (the heavier 45.7 ms path). Same expectation.
- [ ] Edge case: drag-reorder two pedals and immediately (<6 ms) switch the cab —
      both edits must land, and neither may click.
- [ ] Edge case: switch the cab back and forth ~10× rapidly. No dropout, no leak,
      the final cab matches the UI selection.
- [ ] Edge case: a rig whose custom IR is missing still falls back to clean212.

## Out of scope for this session

- `core/src/dsp/CabConvolver.{h,cpp}` — owned by `fix/cab-block-size` (finding 3).
- Moving the partitioned spectra computation to the **main** thread (the fully
  correct fix). Ledgered as a follow-up.
- `native/src/ClipperEngine` cab swapping: native has **no** built-in cab
  selection and no custom-IR path at all (it loads the default 2×12 once in
  `prepare()`), so there is no native counterpart to keep in parity. Audit
  finding 13 (native doesn't declick cab/power/voice changes) is its own slice.
- Deferring the pedal-handle `free()` in `_commitPending` (`_destroyPedal`).
- Wiring the `*_reset` exports to anything (deferred from the previous slice).

---

## What actually happened

Implemented as planned, plus four things the plan did not anticipate.

**1. The prepare-before-every-activation invariant.** With a single pair,
`prepare()` on the live convolvers zeroed the FDL as a side effect, so a swap always
started the new IR from silence. Double-buffering preserves that only because
`amp_prepare_cab_*` always runs on the pair about to be activated — the *outgoing*
pair keeps its FDL history. So no extra `reset()` was needed, but the prepare must
never be skipped as an "optimisation" when the requested IR already matches what sits
in the inactive pair. Written into the ABI banner as a named invariant.

**2. `amp_prepare_cab_*` returns `int`.** The worklet has to know the prepare actually
happened before staging a commit; otherwise a rejected IR (null pointer, zero length)
would stage a flip to a pair holding the *previous* IR plus stale history.

**3. Defect (b) was deeper than "commit at the wrong gain".** Removing the
commit-first also required `_prepareChain` to diff against the pending nodes **and**
to union the `removed` sets. Without the union, staging chain edit A (removes pedal X)
then any second chain edit before the fade zero leaked X's WASM handle forever,
because B's `removed` was computed from a node array that no longer contained X. That
leak exists on `main` too, masked by the eager commit.

Also: the worst case for (b) is not "mid-ramp" as the plan assumed. When both messages
land in the same audio-thread message drain, no render has advanced the ramp, so the
first edit commits at **full gain** — verified by instrumenting `_commitPending` on the
pre-fix worklet (two chain edits committing from inside the message handler at
`_declickGain == 1.0`).

**4. The slice exposed a pre-existing artifact and I fixed it too.**
`CabConvolver::prepare()` zeroes the FDL and overlap, so a swapped-in cab emits exact
silence for up to two 128-sample partitions, and because the swap lands at an arbitrary
offset in the convolver's partition grid that gap was landing in the *middle* of the
6 ms fade-in — stepping from ~59 % gain straight to zero and back, a 128-sample hole.
Present in shipped v1.1; invisible to the old test because it swapped before rendering
started. Once (b) was fixed and more change landed at the fade zero, the measured step
in a four-edit batch got *worse* than pre-fix (0.0355 vs 0.0086), so leaving it would
have meant shipping a regression by the project's own metric. Fixed with a new `'hold'`
phase that parks the output at zero for two partitions (`CAB_SWAP_DEAD_SAMPLES = 256`)
after a cab commit. Needs no `CabConvolver` change. Flagged for review as scope beyond
the brief.

**Surprises in the tooling.** Two of my three originally-designed browser scenarios did
not discriminate at all — a single cab swap was *already* correctly declicked pre-fix,
and a driven pedal chain's natural slew masked the step. I found that only by building
a Node harness that stubs `AudioWorkletGlobalScope` and running both worklets against
one rebuilt artifact, then scanning five candidate scenarios for one with real
separation. Without that I would have shipped two tests that pass on the broken code.

## Measured results

### Core: `clipper_cab_swap_tests`, Release, x86-64 Linux, 48 kHz / 128

```
[cab swap] render deadline (48k/128)   : 2.6667 ms
[cab swap] amp_process_stereo(128)     : 0.023476 ms/call, 0 allocations
[cab swap] COMMIT (audio-thread step)  : 0.000001 ms/call, 0 allocations
[cab swap] PREPARE builtin (off-path)  : 10.967303 ms/call
[cab swap] PREPARE custom 4096 (off-p) : 42.659175 ms/call, 13 allocations
[cab swap] prepare/commit time ratio   : 9321816x
[cab swap] commit vs one render block  : 0.000050x
[cab swap] commit vs the deadline      : 0.0000 %
```

The audio-thread step went from **10.97 ms** (built-in) / **42.66 ms** (4096-tap
upload) of allocating work — 411 % / 1600 % of the deadline — to **~1 ns and zero
allocations**. These native figures closely reproduce the audit's WASM measurements
(11.4 / 45.7 ms).

Allocation counts come from a **replaced global `operator new`**, so "allocation-free"
is asserted directly rather than inferred from a clock.

### End to end in the worklet (worst single render block)

| | worst steady block | worst block spanning a 4096-tap IR load |
|---|---|---|
| pre-fix | 0.40 ms (15.2 %) | **104.92 ms — 3935 % of deadline, 39 quanta missed** |
| fixed | 0.36 ms (13.5 %) | **0.42 ms — 15.7 %, 0 quanta missed** |

### Fidelity: bit-identity, not tolerance

`max|new − pre-fix reference| == 0.0` on **both** channels over **128 000 samples per
side**, across a scripted session: play → brit412 → play → 4096-tap custom IR (through
`peakNormalizeIR`) → play → back to clean212 → play → brit412 → play. The reference is
the pre-fix construction written out longhand (one `AmpModel`, one convolver pair,
re-prepared in place), not the new exports. Reported latency unchanged (128 → 128).
Goldens untouched and still passing.

### Test-fails-before-fix — verified in both directions

- **`clipper_cab_swap_tests` with `-DCLIPPER_CAB_TEST_LEGACY_ABI=1`** (commit routed
  through the old monolithic `amp_set_cab_builtin`): **FAILS.**
  `COMMIT 10.745495 ms/call, 5 allocations, 402.96 % of deadline, 459.4× a render
  block` → trips `assert(commitAllocs == 0)`. Block B (bit-identity) **passes** in both
  modes, which is the intended split: B is the guard that the speed-up cost no fidelity.
- **`cab.spec.ts` test 3 (rewritten).** The audit's "vacuous" claim confirmed
  empirically: the **old** body passes even with the declick entirely removed
  (`0.001848` against a limit of `0.012806`). The new mid-render body measures
  **1.00× baseline (PASS)** on the fixed worklet and **33.40× (FAIL)** against a
  declick-stripped worklet.
- **`cab.spec.ts` test 3b (new)**, four edits in one message drain:
  **pre-fix step 0.222230 = 7.79× baseline → FAIL; fixed 0.028545 = 1.00× → PASS.**
- The cab dead-partition hold: across five multi-edit scenarios the edit-window step
  went to exactly **1.00× the steady-state slew baseline** (from 1.55×–8.27×, with the
  four-edit case failing outright at 7.23× before the hold).

### Suites

Core **ctest 18/18** green. `web`: `tsc --noEmit` + `vite build` clean.
`test:server` 11/11, `test:history` 10/10, `electron` 16/16.

**Playwright was NOT run** — no browser binary was installable in this environment
(`npx playwright install chromium` fails to download). Instead the real worklet was
driven under Node with a stubbed `AudioWorkletGlobalScope`, pre-fix and fixed sharing
one locally rebuilt WASM artifact; every browser-side number above comes from that. The
two new/rewritten specs still need a genuine Playwright run once the artifact is
committed.

## Files created / modified

- `core/src/clipper_c_api.cpp` — `CabPair cabs[2]` + `activeCab` + `active()`/
  `inactive()`; new `amp_prepare_cab_builtin` / `amp_prepare_cab_custom` /
  `amp_commit_cab`; `amp_set_cab_builtin` / `amp_load_custom_ir` become wrappers;
  active-pair routing in `amp_process`, `amp_process_stereo`, `amp_latency_samples`;
  `amp_reset` clears both pairs; `amp_create` prepares both pairs.
- `core/tests/test_cab_swap.cpp` — new suite (allocation counting via a replaced global
  `operator new`, wall clock, bit-identity, degenerate sequences).
  `-DCLIPPER_CAB_TEST_LEGACY_ABI=1` reproduces the pre-fix failure.
- `core/CMakeLists.txt` — `clipper_cab_swap_tests`, the 18th ctest target.
- `web/worklet/clipper-processor.js` — prepare in the `cab` handler, O(1)
  `_commitPending`, `HEAPF32` re-fetch after the commit, `_stageEdit` (merge instead of
  force-commit), `_prepareChain` diffs against pending + unions `removed`, and the new
  `'hold'` declick phase for the cab's dead partition.
- `scripts/build-wasm.sh` — three new `EXPORTED_FUNCTIONS`.
- `web/tests/cab.spec.ts` — test 3 rewritten (mid-render, toward the brighter cab);
  new test 3b (four edits in one fade window).
- `docs/DEVELOPMENT.md` — new §30.
- `docs/decisions/003-cab-double-buffer-at-the-abi.md` — new ADR.
- `CLAUDE.md` — Current State.

## Deferred to next session

- **`web/public/generated/*` is NOT rebuilt in this branch** (the owner rebuilds the
  artifact centrally). `core/` **and** `web/worklet/` both changed, so
  `bash scripts/build-wasm.sh` must run and the artifacts be committed before the
  Playwright suite means anything — the new specs call exports that do not exist in the
  currently committed `clipper.js`. Verified reproducible with emsdk in this
  environment.
- **The fully correct fix:** compute the partitioned IR spectra on the **main** thread
  and copy the finished spectra into the inactive pair, so `onmessage` does no synthesis
  either. Needs an ABI exposing the partition layout (or a main-thread WASM instance
  used purely as an IR compiler) plus a transferable spectra buffer.
- **`_destroyPedal`'s `free()` still runs inside `_commitPending`**, i.e. inside
  `process()`. Bounded and microseconds, but still `free()` on the audio thread. Wants a
  deferred-destroy queue drained in `onmessage`.
- **`_prepareChain` mutates `existing.engaged` on the live node object**, so an
  engaged-flag change arriving in a `chain` message takes effect at the next render
  quantum instead of at the fade zero. M11 fixed this for `bypass` (stomp) messages via
  `_pendingBypass`; the `chain` path still has the hole. Found while designing test 3b.
- **The amp-voice swap has its own dead/settling time.** In the scan, `ampModel` →
  JCM800 plus a cab swap measured 3.39× baseline (passing, but the highest of any
  scenario). Probably the JCM's oversampling group delay and the latency renegotiation
  the audit flags in finding 13. Not investigated.
- Audit finding 3 (`CabConvolver` on non-128-multiple block sizes) — `fix/cab-block-size`.
- Audit finding 13 (native declicks neither cab nor power nor voice).

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
