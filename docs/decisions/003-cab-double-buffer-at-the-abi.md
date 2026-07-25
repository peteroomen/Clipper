# ADR 003: Double-buffer the cab convolvers at the C ABI, not inside CabConvolver

Date: 2026-07-25
Status: Accepted

## Context

The 2026-07-24 audit's finding 2: a cab change ran 11–46 ms of IR synthesis, heap
copying, peak normalization and FFT partitioning **inside `process()`** — against a
2.667 ms render deadline at 48 kHz / 128. Instrumented end to end, the worst single
render block spanning a 4096-tap IR load took **104.9 ms and missed 39 consecutive
render quanta**: an audible dropout on every cab change and every IR upload.

The obvious place to fix this is `CabConvolver` itself: give it a "load an IR into a
back buffer, then atomically switch" API. That was rejected for two reasons.

1. **A parallel branch owns that file.** `fix/cab-block-size` is rewriting
   `core/src/dsp/CabConvolver.{h,cpp}` to fix finding 3 (the convolver corrupts its
   stream on any host block size that is not a multiple of 128). Two open PRs
   restructuring the same buffers is a three-way merge conflict in the most
   fidelity-sensitive file in the tree — worse than the bug being fixed.
2. **The chain layer already had the right pattern.** `amp_set_model` keeps all four
   amp voices created and prepared up front so switching voices is a lock-free `int`
   flip. The cab was the one topology change in the chain that had *not* been given
   that treatment, which is why it was the one that blew the deadline.

The alternative considered and rejected outright was keeping the work where it is and
arguing it inaudible. The pre-existing comment did exactly that — "it runs at the
output-zero of the declick" — which conflates a step discontinuity with a missed
deadline. CLAUDE.md now names that as a non-argument.

## Decision

Double-buffer at the **C ABI layer**. `AmpChain` in `core/src/clipper_c_api.cpp` holds
two per-side convolver pairs (`CabPair cabs[2]`) plus an active index, and the single
cab-change call splits into a heavy half and a trivial half:

- `amp_prepare_cab_builtin(h, which)` / `amp_prepare_cab_custom(h, ir, len)` — synthesise
  or copy the IR, peak-normalize it, and run `CabConvolver::prepare` on both sides of
  the **inactive** pair. Allocates; must never be called from a render callback.
  Returns 1 on success, **0 on a rejected argument** so the caller knows not to commit.
- `amp_commit_cab(h)` — `activeCab ^= 1`. One integer write, no allocation, safe to
  call from `process()`.

`amp_set_cab_builtin` / `amp_load_custom_ir` are retained as prepare+commit wrappers,
so the ABI change is purely additive. `CabConvolver` is not modified at all.

`amp_create` prepares **both** pairs, so a commit can never activate an unprepared
convolver. `amp_reset` clears both pairs.

**Invariant:** every activation is preceded by a `prepare()` of the pair being
activated. The prepare must *not* be skipped when the requested IR already matches
what sits in the inactive pair — that pair's FDL still holds history from when it was
last live, and activating it without a prepare would splice a stale convolution tail
into the output.

The worklet's `cab` message handler now does the prepare and stages only the commit.

## Consequences

**Easier.**

- The audio-thread step of a cab change is 1 ns and allocation-free (asserted directly
  with a replaced global `operator new`), versus 10.97 / 42.66 ms before — a ratio of
  ~9.3 × 10⁶. Worst render block spanning an IR load: 104.92 ms → 0.42 ms.
- Provably fidelity-neutral: the same `prepare()` over the same IR with the same
  partition, so the swapped-in cab is **bit-identical** to the pre-fix path
  (`max|new − old| == 0.0` over 128 000 samples per side, including a swap-back).
- `fix/cab-block-size` can rewrite `CabConvolver` freely; the two branches touch
  disjoint files.
- The same split is the natural seam for the *real* fix later (compute the partitioned
  spectra on the main thread and copy them in) — only `amp_prepare_cab_*` changes.

**Harder / costs.**

- **Memory doubles for the cab.** Two extra convolvers: ~266 KB each at the app's
  4096-sample IR cap (`web/src/cab.ts` `MAX_IR_SAMPLES`), so ~530 KB worst case in
  WASM. Acceptable, but it is now a hard cap that must be respected — an uncapped user
  IR would cost twice what it used to.
- `amp_create` pays one extra IR synthesis (~11 ms) at engine start. Irrelevant next
  to the four tube-amp DC solves already there, and nowhere near the audio thread.
- **Two calls where there was one**, and callers must honour the prepare/commit
  ordering and the return code. The wrappers keep the old shape available, but they
  carry the full cost and are not render-safe — the doc comment says so.
- **The honest limit.** `AudioWorkletProcessor.port.onmessage` runs on the audio
  rendering thread, so this does *not* move the work off that thread. It moves it
  *between* render quanta instead of mid-block, turning a guaranteed multi-quantum
  stall into at worst one late quantum at the moment of a deliberate user action. Do
  not describe the result as "real-time safe".
