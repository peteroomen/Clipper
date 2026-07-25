# Halfband resampler: doubled ring buffer (no integer division per tap)

**Date:** 2026-07-25
**Branch:** perf/halfband-no-modulo
**Roadmap item:** 2026-07-24 audit → "Fidelity-neutral performance wins", item 2
(`docs/audits/2026-07-24-project-audit.md:309`)

## Goal

Remove the per-tap hardware integer division from the halfband interpolator and
decimator inner loops — the hottest loop in the project — with **bit-identical**
output, and quote the measured speedup per unit.

## Approach

`HalfbandFilter.h` indexes its ring buffer as `ring_[(w_ - p + M) % M]` with `M_`
a runtime `int` member, so the compiler cannot strength-reduce the `%` to a mask
(64 is a power of two, but the compiler does not know that at compile time) and
emits a real `div` per tap: 64 per output sample on the interpolator's tight
stage, 65 on the decimator's, both at the *oversampled* rate, for every
oversampled pedal (RAT, SD-1, TS, Muff, Gold) and every valve amp (per triode
stage).

**Fix — a doubled ring buffer.** Allocate `2*M` (resp. `2*L`) floats and maintain
the invariant `ring_[i] == ring_[i + M]` by writing every incoming sample twice.
The wrapped read `ring_[(w_ - p + M) % M]` then becomes the unwrapped
`ring_[w_ + M - p]`, which is in range for all `p ∈ [0, M)` and needs no
division. `w_` advances with a compare instead of `%`.

The decimator additionally swaps `std::vector<Tap>` (a struct of `{int, float}`,
so the coefficient stream is strided by 8 bytes) for parallel `int` / `float`
arrays.

**This is a data-structure change, not a DSP change.** Bit-identity is a hard
requirement, and it holds only if the *accumulation order* is preserved exactly:
float addition is not associative, so the tap loop must keep iterating `p` from
`0` upward (reading memory backwards) rather than being reversed into a
forward-streaming loop. No SIMD, no `-ffast-math`-style reassociation, no
reordering of coefficients. Fidelity-neutral by construction, and proven
sample-for-sample by a test that carries a copy of the current implementation.

## Steps

- [ ] Add an isolated `os4` unit to `clipper-bench` (Oversampler 4× up → trivial
      nonlinearity → down) so the resampler's own cost is visible instead of
      diluted through a whole pedal
- [ ] Baseline: build the unmodified tree, run `clipper-bench`, save the table
- [ ] Rewrite `HalfbandInterpolator2x` with a doubled ring (`2*M` floats, dual
      write, `w_ + M_ - p` reads, compare-advance)
- [ ] Rewrite `HalfbandDecimator2x` the same way, plus parallel tap arrays
- [ ] New test `core/tests/test_halfband.cpp` / target `clipper_halfband_tests`
      (registered with `clipper_add_test_flags()`):
      - block A: **bit-identity** vs a reference copy of the pre-change
        implementation — the two filter classes directly, and a reference
        `Oversampler` cascade at 2× / 4× / 8×, over a long harmonically rich
        program signal. Tolerance is exactly `0.0`, compared bit-for-bit.
      - block B: **stopband** — worst-case image rejection (interpolator) and
        worst-case alias rejection (decimator), measured *through the live
        objects* by sweeping tones, tight and relaxed designs
      - block C: **latency** — empirical round-trip group delay (impulse peak)
        must equal `Oversampler::latencySamples()` at 1× / 2× / 4× / 8×
- [ ] Rebuild, re-run `clipper-bench`, run the whole core suite
- [ ] Docs: new §32, update the §25.3 per-gear table with the new numbers

## How this will be measured

1. **Bit-identity:** `clipper_halfband_tests` block A asserts
   `max |new − old| == 0.0` *and* bit-for-bit equality of the float
   representations, over ≥ 200 k samples per factor.
2. **Speed:** `build/clipper-bench` before → after, quoting the rows for
   `os4`, rat / sd1 / ts / muff / gold / jcm800 / twin / ac30. Same machine,
   same deterministic riff, warm-up pass included, best of three runs.
3. **Stopband:** block B prints worst-case image/alias rejection in dB for the
   tight (M=64) and relaxed (M=16) designs; must be unchanged and ≤ −78 dB.
4. **Latency:** block C prints the measured group delay per factor; must be
   0 / 64 / 72 / 76 base-rate samples, i.e. unchanged.
5. **Goldens:** untouched. If `clipper_player_expectations_tests` block C moves,
   the change is not what it claims to be — stop and report, do not re-bless.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — every target green, the six
      `_xfail_ledger` entries still Skipped, no new XPASS
- [ ] `build/clipper-render --alias-report --sr 44100 --distortion 0.9` — the
      alias table must be identical to the documented one (§7): −18.4 / −26.7 /
      −86.6 / −90.6 worst-alias and 0 / 64 / 72 / 76 latency
- [ ] Edge case: `Oversampler::setFactor(1)` (pass-through, no filter) still
      bit-reproduces the input, and `reset()` after a NaN still clears both
      halves of the doubled ring — a half-cleared ring would keep re-injecting
      the stale copy for one full filter length. Covered by
      `clipper_nan_guard_tests` (which drives a NaN audio sample through every
      oversampled unit) plus an explicit reset case in block A.
- [ ] Edge case: an odd block length through the cascade (the ring wrap lands
      mid-block) — block A drives ragged block sizes, not just 128.

## Out of scope for this session

- SIMD / explicit vectorisation of the tap loop (arguable, not provably free —
  reassociating a float reduction is not bit-identical).
- Audit performance item 1 (one oversampler per preamp instead of one per triode
  stage). That halves latency and is a *fidelity* change; its own slice.
- Adding `flushDenormal` to the halfband ring (would not be bit-identical; the
  FIR is non-recursive so it cannot latch a denormal anyway).
- Rebuilding the committed WASM artifact — `core/` changed, so it is required,
  but the orchestrator owns that step for this slice.

---

<!-- Fill in below during/after the session -->

## What actually happened

Went as planned; the change itself is ~30 lines. Three things worth recording.

**1. The accumulation order really is the whole ballgame.** The obvious "nicer"
version of the fix reverses the tap loop so both the coefficients and the ring
stream forwards. It is mathematically the same sum and it is **not** bit-identical:
perturbing the shipped loop that way makes 183 049 of 480 000 samples differ, every
one of them by about 1 ULP. `max |new − old|` prints as `0.0` at one decimal, so a
tolerance-based test — even a tight one — would have waved it through. That is why
the comparison is a `memcmp` of the float representations rather than `==` or a
tolerance, and why the header now carries an explicit "do not tidy the direction,
do not vectorise this reduction" note.

**2. Block C found a real (pre-existing) off-by-a-fraction in the reported
latency.** The first version of the delay measurement used the argmax of the
composite impulse response and failed at 2×: measured 63, reported 64. That was not
my change (block A proves bit-identity) — the true round-trip delay is genuinely
fractional, because the decimator emits on the **second** sample of each pair, which
costs half a sample at that stage's rate. Stage `s` therefore contributes
`(2M − 1) / 2^(s+1)` base samples, not `(2M) >> (s+1)`. An energy centroid was also
wrong (biased at 4×, 71.1954 vs a true 71.25, because the sample grid is not
symmetric about a non-half-integer centre); a **phase-slope** estimator at
`w = 2π/512` matches the structural derivation exactly. `latencySamples()`
over-reports by 0.5 / 0.75 / 0.875 base samples at 2× / 4× / 8×. Left alone (the API
returns an `int`, and the value is surfaced in the UI and in the plugin's latency
reporting) but now pinned and written up in §32.

**3. The stopband measures better than the audit's figure**, and the reason is the
definition, not the filter. Measuring worst-case image / alias rejection across the
band a guitar rig actually carries (≤ 20 kHz) gives −86.1 / −87.3 dB tight and
−88.5 / −87.9 dB relaxed; the audit's −79.8 / −78.7 dB is the worst case over the
whole stopband including the 20–22.05 kHz sliver. Same untouched design. The test
asserts ≤ −78 dB, so it holds under either reading.

Also noticed while verifying, and **not** fixed here: the `--alias-report` table in
docs §7 no longer matches what the tool prints. The before and after builds print the
same table to the digit, so the drift is pre-existing (it dates from the §11.1 RAT
re-voice / input calibration). Attributing it belongs to whichever slice moved it.

## Measured results

**Bit-identity — 0 differing bits everywhere** (`clipper_halfband_tests`):

| comparison | samples | differing bits | max abs diff |
|---|---|---|---|
| interpolator tight (M=64) / relaxed (M=16) | 480 000 each | 0 / 0 | 0.0 |
| decimator tight (L=129) / relaxed (L=33) | 120 000 each | 0 / 0 | 0.0 |
| full cascade 1× / 2× / 4× / 8×, ragged blocks, up **and** down | 200 000 each | 0 | 0.0 |
| after NaN poisoning + `reset()` vs a virgin instance (2×/4×/8×) | 20 000 each | 0 | — |

End to end, `clipper-render --alias-report --sr 44100 --distortion 0.9` prints an
identical table before and after (`−17.6 / −16.2 / −89.7 / −91.9`, latency
0/64/72/76). Core ctest **25/25** (19 targets + the 6 `_xfail_ledger` entries
Skipped), goldens untouched, no XPASS.

**Speed** — `clipper-bench`, 6 s riff @ 48 kHz, 128-frame blocks, best of 3 on one
idle machine, the header being the only difference:

| unit | before | after | speedup | % of one 48 k stream |
|---|---|---|---|---|
| os2x (up+down only) | 71.5× | 207.3× | **2.90×** | 1.40 % → 0.48 % |
| os4x (up+down only) | 44.6× | 121.5× | **2.72×** | 2.24 % → 0.82 % |
| os8x (up+down only) | 25.4× | 67.3× | **2.65×** | 3.94 % → 1.49 % |
| sd1 | 36.9× | 80.6× | 2.18× | 2.71 % → 1.24 % |
| screamer (ts) | 37.3× | 80.7× | 2.16× | 2.68 % → 1.24 % |
| rat | 29.7× | 53.5× | 1.80× | 3.37 % → 1.87 % |
| gold | 31.1× | 56.1× | 1.80× | 3.22 % → 1.78 % |
| jcm800 | 1.65× | 1.88× | 1.14× | **60.6 % → 53.3 %** |
| twin | 2.50× | 2.86× | 1.14× | 40.0 % → 35.0 % |
| ac30 | 3.03× | 3.39× | 1.12× | 33.1 % → 29.5 % |
| muff | 3.91× | 4.15× | 1.06× | 25.6 % → 24.1 % |
| ninety (phaser), cab, reverb, limiter, clean amp — *controls* | — | — | 1.00× | unchanged |

**Stopband (unchanged)** — worst case over the ≤ 20 kHz audio band, measured through
the live objects: tight L=129 image **−86.1 dB** / alias **−87.3 dB**; relaxed L=33
image **−88.5 dB** / alias **−87.9 dB**.

**Latency (unchanged)** — `latencySamples()` 0 / 64 / 72 / 76 at 1× / 2× / 4× / 8×;
measured phase-slope delay 0 / 63.500 / 71.250 / 75.125, matching the structural
derivation to < 0.01 samples.

**Perturbations (the test has teeth)** — each patched in the tree, `touch`ed, rebuilt
and observed to fail: dropping the mirrored write (block A, 329 277 samples differ);
reversing the tap loop (block A, 183 049 differ by ~1 ULP); `kBeta` 7.857 → 5.0
(block B, −86.1 → −62.3 dB — and block A stays **green**, which is exactly why B
exists); `kRelaxedM` 16 → 20 (block C, latency 72 → 74, delay 71.25 → 73.25).

## Files created / modified

- `core/include/clipper/dsp/HalfbandFilter.h` — doubled ring in both classes,
  parallel tap arrays in the decimator, compare-advance instead of `%`, and the
  two invariants documented at the top of the file
- `core/tests/test_halfband.cpp` — **new**; carries the pre-change implementation as
  the reference. Blocks A (bit-identity), B (stopband), C (group delay)
- `core/CMakeLists.txt` — `clipper_halfband_tests` target via `clipper_add_test_flags()`
- `core/tools/bench/main.cpp` — new `os2x` / `os4x` / `os8x` resampler-only units
- `docs/DEVELOPMENT.md` — new **§32**; §25.3 table gains a "× faster after §32"
  column and a note that its absolute numbers predate this slice
- `docs/decisions/005-halfband-doubled-ring.md` — **new** ADR
- `docs/work/2026-07-25-halfband-no-modulo.md`, `CLAUDE.md` — this plan, Current State

## Deferred to next session

- **Rebuild the committed WASM artifact** (`bash scripts/build-wasm.sh`) — `core/`
  changed, so this is required before the change is complete. Owned by the
  orchestrator for this slice. The win has **not** been confirmed in-browser; the DSP
  load meter (§25.4) is the place to look.
- **`Oversampler::latencySamples()` over-reports by up to 0.875 base samples**
  (0.5 / 0.75 / 0.875 at 2× / 4× / 8×), now measured and pinned. Fixing it means
  deciding what an `int` API should return and touching the UI/plugin latency
  readouts — a plumbing slice.
- **Audit perf item 1** — one oversampler per preamp instead of one per triode stage.
  Now the biggest remaining resampling win (removes ~4× of the *calls*, and halves
  the JCM800's 7.5 ms latency). A fidelity change, so it needs its own argument.
- **SIMD in the tap loop** — deliberately not attempted: vectorising a float
  reduction reassociates it and cannot be bit-identical, so it needs a fidelity
  argument rather than a "provably free" one.
- **docs §7's `--alias-report` table is stale** (pre-existing, unrelated to this
  slice — see above).

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
