# CabConvolver: sample-accurate FIFO for arbitrary host block sizes

**Date:** 2026-07-25
**Branch:** fix/cab-block-size
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` finding 3 (shipping-blocker)

## Goal

`CabConvolver::process()` produces the *same* output stream for any host block
size — 64, 96, 100, 128, 441, or a size that varies call to call — bit-identical
to today's output for 128-aligned blocks, with no audio-thread allocation and no
change to the reported latency.

## Approach

**The bug.** `process()` loops in `partition_`-sized chunks and, for a final
chunk shorter than a partition, zero-pads it into a stack buffer and calls
`processBlock()` anyway. `processBlock()` unconditionally advances the
frequency-delay line and overwrites `overlap_` with the *padded* block, then
`partition_ - n` computed output samples are thrown away. The stream is
permanently misaligned from that point on: the convolver believes it has seen
`partition_` samples when it has seen `n`, and every later call inherits it.

**The fix.** Give the convolver a sample-accurate input FIFO and output FIFO and
never call `processBlock()` on anything but a genuinely full partition.

The one non-obvious part is that a FIFO normally *adds* a partition of latency,
which would both change the reported latency and break the aligned goldens. It
does not have to here, because the existing FDL already carries a free partition
of slack: `processBlock()` at input block *m* sums `X_{m-1-k} · H_k`, i.e. output
block *m* only depends on input blocks up to *m-1*. So the deferral that creates
the one-partition latency can be moved *out* of the FDL indexing and *into* the
output FIFO:

- The FDL sum offset changes from `fdlHead_ - 1 - k` to `fdlHead_ - k`, so
  completing input block *m* now immediately computes what used to be emitted as
  output block *m+1*.
- The output FIFO is seeded at `reset()` with exactly `partition_` zeros — which
  is precisely the old output block 0 (an all-zero accumulator IFFTs to `+0.0`).

The emitted stream is therefore *unchanged*: `[P zeros] ++ Z_0 ++ Z_1 ++ …` is
term-for-term the old `y_0 ++ y_1 ++ y_2 ++ …`. Both schemes accumulate the same
complex products in the same `k` order into the same double accumulator, so this
is **bit-identical for aligned blocks**, not merely close. Latency stays
`partition_` and `latencySamples()` stays correct for every block size.

Availability proof (why the output FIFO can never underflow): after *K* total
input samples the convolver has produced `P + floor(K/P)·P` output samples and
emitted `K`, so `available = P - (K mod P) ≥ 1`. Segmenting each call at
`partition_ - inFill_` bounds the FIFO at `2P` and runs at most one block per
segment — so every buffer is `prepare()`-sized and nothing allocates.

Also in this slice (same finding): the `float tmpIn[4096]` / `tmpOut[4096]` stack
arrays go away — 32 KB of stack indexed by the caller-supplied `partition_`, so
`partitionSize > 4096` was a stack overflow. And `numFrames <= 0` returns early.

This is **fidelity-neutral for the 128-aligned path** (bit-identical) and a
straight bug fix everywhere else.

## Steps

- [ ] Add `inFifo_`/`inFill_`, a ring `outFifo_`/`outRead_`/`outCount_`, and a
      `blockOut_` scratch to `CabConvolver`, all sized in `prepare()`
- [ ] Rewrite `process()` as feed → maybe-one-block → drain, segmented at
      `partition_ - inFill_`; early-return on `numFrames <= 0`
- [ ] Change the FDL sum offset to `fdlHead_ - k` and seed the output FIFO with
      `partition_` zeros in `reset()`
- [ ] Delete the stack `tmpIn`/`tmpOut` padding path
- [ ] Rewrite the header algorithm comment to document the FIFO + where the
      one-partition deferral now lives
- [ ] Replace the vacuous `testConvolverChunking` in
      `core/tests/test_amp_model.cpp` with a real segmentation test
- [ ] Verify the new test FAILS on unmodified `CabConvolver.cpp` (stash the
      source fix, keep the test) and passes after

## How this will be measured

`max |ref - chunked|` where `ref` is the 128-aligned render, for block sizes
64, 96, 100, 128, 441 and a pseudo-random varying size, on both an impulse and a
musical (decaying pluck) input. Target: `0.0` for 128 (bit-identical) and
`< 1e-6` for every other size. The audit's measured "before" for blocks of 100 is
`0.1636` against a reference peak of `0.1521` — error larger than the signal.

The five golden-render ctests plus the whole 16-target core suite are the
bit-identity guard for the aligned path.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — all 16 targets green, the
      golden renders in particular (they prove the aligned path did not move)
- [ ] Edge case: block size 1 and block size 4096 through the new test's helper
- [ ] Edge case: `numFrames == 0` and a negative `numFrames` do nothing
- [ ] Edge case: in-place operation (`in == out`) still correct at odd sizes —
      this is how `amp_process` calls it
- [ ] Failure case: revert `CabConvolver.cpp` only; the new test must abort on
      the 100-sample case

## Out of scope for this session

- Audit finding 2 (cab-swap allocation inside `process()`) — separate slice
- The `ClipperEngine` / `PluginProcessor` chunking policy: this fix makes the
  convolver correct for whatever the host hands it, but the wider question of
  whether the *whole* native chain should be quantised to 128 is its own slice
- The real-FFT (Hermitian) 4× performance win noted in the audit
- The committed WASM artifact rebuild (`scripts/build-wasm.sh`) — handled
  centrally by the orchestrator, but it **is** required because `core/` changed

---

## What actually happened

Implemented as planned; the "move the deferral from the FDL into the output FIFO"
idea held up exactly, and the aligned path came out bit-identical (`maxErr` is a
hard `0.0`, asserted with `==`, not a tolerance).

Two things worth recording:

- The old `processBlock()` had a load-bearing comment about capturing `overlap_`
  from `in` *before* writing `out`, because callers process in place. That hazard
  is now gone by construction: `processBlock()` reads from `inFifo_` (a copy the
  convolver owns) and writes to `blockOut_`, so `in` and `out` are never aliased
  inside it. The ordering is kept anyway and the comment now explains why the
  hazard cannot recur.
- The FDL depth stays `numPartitions_ + 1` even though the new indexing only
  reads `numPartitions_` slots. Shrinking it is a separate, unrelated change and
  the spare slot costs one `fftSize_` complex block.

The new test also covers block size 1, block size 4096 (larger than the FDL/
partition and larger than the old 4096-element stack arrays), `numFrames == 0`,
`numFrames < 0`, and in-place (`in == out`) rendering at every block size.

Two findings from writing the test that are worth keeping:

- **Two probes are necessary.** Block size 441 is *exact for an impulse* on the
  broken code and off by `0.55` on the pluck. A single-probe segmentation test
  could plausibly have shipped and still missed it.
- **`testConvolverImpulse` was already calling `process()` with a non-multiple
  length** (`irLen + 2*128 + 64`) and passing, because the impulse sits at sample
  0 and the corruption landed in the silent tail. That is the shape of near-miss
  the "measure, don't assert" convention is about.

## Measured results

`max |128-aligned reference − chunked|` over a 4873-sample render of the default
2x12 IR, probes = an impulse and a decaying 220 Hz pluck. Reference peaks:
44.1 k impulse `0.1521` / pluck `0.3260`; 48 k `0.1423` / `0.3261`; 96 k
`0.0726` / `0.3482`. Measured with a standalone harness compiled against the old
and the new `CabConvolver.cpp` side by side.

BEFORE:

| block size | 44.1 k impulse | 44.1 k pluck | 48 k impulse | 48 k pluck | 96 k impulse | 96 k pluck |
| ---------- | -------------- | ------------ | ------------ | ---------- | ------------ | ---------- |
| 1          | 0.1521         | 0.3253       | 0.1423       | 0.3256     | 0.0726       | 0.3483     |
| 64         | 0.1535         | 0.4826       | 0.1442       | 0.5331     | 0.0757       | 0.4400     |
| 96         | 0.1609         | 0.4241       | 0.1519       | 0.4151     | 0.0736       | 0.4897     |
| 100        | 0.1636         | 0.4140       | 0.1533       | 0.4365     | 0.0791       | 0.4786     |
| 128        | 0.0000         | 0.0000       | 0.0000       | 0.0000     | 0.0000       | 0.0000     |
| 441        | 0.0000         | 0.5517       | 0.0000       | 0.4206     | 0.0006       | 0.3745     |
| 4096       | 0.0000         | 0.0000       | 0.0000       | 0.0000     | 0.0000       | 0.0000     |
| varying    | 0.1556         | 0.6048       | 0.1464       | 0.5013     | 0.0760       | 0.4647     |

AFTER: **every cell is exactly `0.0000`**, at all three rates, both probes, in
place and out of place. Not "within tolerance" — bit-identical, which is why the
test asserts `== 0.0`.

Two things the before-table shows that matter:

- The error is **larger than the signal**: 100-sample blocks at 44.1 k give
  `0.1636` against a `0.1521` reference peak (matching the audit's figure), and
  the pluck at 441 gives `0.5517` against a `0.3260` peak.
- **128 and 4096 were already exact** (both are multiples of the partition) —
  which is precisely why the goldens, `identical_core_test` and the old
  `testConvolverChunking` never saw any of this. `441` is exact for the *impulse*
  and badly wrong for the pluck, so a single-probe test could have missed it too;
  hence two probes.

Test-fails-before-fix confirmed: with the new test in place and only
`CabConvolver.{h,cpp}` reverted (`git stash push` of those two files),
`clipper_amp_tests` prints
`[!!] block=1 inPlace=0 impulse maxErr 1.52e-01 (ref peak 1.52e-01), pluck maxErr 3.25e-01 (ref peak 3.26e-01)`
and aborts on
`Assertion 'eImp == 0.0 && "convolver output depends on block segmentation"' failed`.
Restoring the fix, it passes.

Bit-identity of the aligned path: the full 16-target `ctest` suite passes
unchanged (253 s), including the golden renders and the −152 dB direct-convolution
null test. The convolver's own aligned reference render is also asserted to still
honour the one-partition latency contract (silence for `128 + impulseIndex`
samples) inside the new test, so a FIFO that silently added latency would fail
even if it were self-consistent.

Bit-identity of the aligned path independently confirmed by the five
golden-render ctests and the full 16-target suite passing unchanged.

## Files created / modified

- `core/src/dsp/CabConvolver.cpp` — FIFO'd `process()`, FDL offset moved,
  stack arrays removed, algorithm comment rewritten
- `core/include/clipper/dsp/CabConvolver.h` — FIFO members + latency/segmentation
  contract in the header comment
- `core/tests/test_amp_model.cpp` — `testConvolverChunking` rewritten to actually
  test segmentation (8 block sizes × 2 signals × in-place, plus the degenerate
  frame counts)
- `docs/DEVELOPMENT.md` — "Convolver design" gains a **Block-size independence**
  subsection (the FIFO, the no-underflow bound, the before/after table) and the
  stale `Y = Σ FDL[k+1]·H_k` description is corrected; §15.4 (the in-place bug)
  gains a follow-up note
- `docs/work/2026-07-25-cab-block-size.md` — this file
- `CLAUDE.md` — Current State

No ADR. The one decision with ADR shape — *the one-partition latency is stored as
the output FIFO's seed zeros rather than as an FDL offset* — is a relocation of an
existing, bit-identical behaviour rather than a new architectural commitment, and
the "don't put it back" warning a future session needs is recorded in
`docs/DEVELOPMENT.md` beside the algorithm it constrains. `docs/decisions/` does
not exist yet and this did not seem like the right entry to open it with.

## Deferred to next session

- **The committed WASM artifact must be rebuilt** (`bash scripts/build-wasm.sh`)
  because `core/` changed. Not done here by instruction — the orchestrator owns
  the artifact rebuild.
- Audit finding 2: cab swap allocates 11–46 ms inside `process()`.
- `native/src/ClipperEngine.cpp:372` only chunks when `numFrames > maxBlock_`.
  That is now *safe* for the convolver, but the rest of the native chain still
  sees raw host block sizes; whether the whole chain should be quantised to 128
  is an open question (the per-block parameter-sampling finding at audit line 274
  is the same shape of problem).
- The Hermitian/real-FFT ~4× convolver win (audit performance item 6).

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
