# ADR 005: The halfband resampler uses a doubled ring buffer, and resampler perf work must be bit-identical

Date: 2026-07-25
Status: Accepted

## Context

`HalfbandFilter.h` indexed its delay line as `ring_[(w_ - p + M) % M]` with `M_` a
runtime `int` member. The compiler cannot strength-reduce that `%` to a mask — it does
not know the value is a power of two — so it emitted a hardware integer division **per
tap**: 64 per output sample on the interpolator's tight stage, 65 on the decimator's,
at the *oversampled* rate, for every oversampled pedal and (once per triode stage) every
valve amp. The 2026-07-24 audit measured a prototype fix at 3.3× on the interpolator and
called it the best perf-per-effort item in the project.

Two decisions were needed. First, the data structure. Second — and more durably — what
"fidelity-neutral" has to mean for a change inside the resampler, because the halfband
sits under *every* nonlinear stage in the rig: a change here moves every dirt pedal and
every amp voice at once, and it does so in a way no golden `.wav` diff makes legible to
a reviewer.

## Decision

**1. A doubled ring buffer.** Both classes hold `2*N` floats (N = `M_` or `L_`) and
write every incoming sample twice, maintaining `ring_[i] == ring_[i + N]`. Wrapped reads
become unwrapped `ring_[w_ + N - p]`; `w_` advances with a compare. The decimator also
stores its sparse taps as parallel `int` / `float` arrays instead of a
`std::vector<{int, float}>`. Allocation stays in `setup()`; `process()` gains one store
per sample and loses N divisions.

**2. Bit-identity is the acceptance bar for resampler performance work — not a
tolerance.** Concretely:

- The accumulation order of the tap sum is **fixed**. Float addition is not associative,
  so the loop keeps ascending `p` (reading memory backwards) even though a
  forward-streaming loop would be prettier and friendlier to a prefetcher. Reordering
  the coefficients to match is the same violation wearing a hat.
- No SIMD, no `-ffast-math`, no reassociation, no reduction splitting. Any of those may
  well be worth doing, but they are **tone changes** and must be argued as such with a
  fidelity measurement, not merged as clean-ups.
- The proof is a `memcmp` of the float representations against a copy of the previous
  implementation kept in the test TU (`core/tests/test_halfband.cpp`), not `==` and not
  a tolerance. `==` treats `+0.0` and `-0.0` as equal and every NaN as unequal to
  itself; a tolerance hides exactly the failure mode that matters here (reversing the
  loop changes 38 % of samples by ~1 ULP, which prints as a max abs diff of 0.0).
- Because an identity check against the old code cannot catch a wrong *filter*, the same
  target also measures absolute, player-observable properties through the live objects:
  worst-case image / alias rejection across the ≤ 20 kHz audio band, and the round-trip
  group delay from the composite impulse response.

## Consequences

**Easier.** The resampler is ~2.7× faster with a proof that nothing changed audibly —
the JCM800 drops from 60.6 % to 53.3 % of one realtime stream. Future work on this file
has a ready-made, sensitive regression harness: any indexing, buffering or tap-storage
change is a one-command check. And the two invariants that make the doubled ring correct
are stated in the header rather than living in someone's head.

**Harder.** The ring costs one extra store per sample and 2× the delay-line memory
(a few hundred floats per stage — irrelevant, but it is a real trade). Every future write
path into the ring must remember to mirror, including `reset()`; a half-cleared ring is a
nasty bug (correct output, then a ghost one filter length later), which is why there is
an explicit reset-after-NaN-poisoning case. And the ban on reassociation forecloses the
easiest remaining speedup — SIMD — until somebody makes the fidelity argument for it.

**Also.** The exercise surfaced a pre-existing inaccuracy it deliberately did not fix:
`Oversampler::latencySamples()` over-reports the true round-trip delay by up to 0.875
base samples, because the decimator emits on the second sample of each pair and the
`(2M) >> (s+1)` formula cannot express the half. It is now measured, pinned by a test,
and written up in docs §32 as an open item rather than silently corrected inside a perf
slice.
