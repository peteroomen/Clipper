# ADR 006: The scope of the anti-denormal policy, and reaching the WDF capacitor's state

Date: 2026-07-25
Status: Accepted

## Context

`core/include/clipper/dsp/Denormal.h` (docs §25) states the policy: **every recursive
accumulator gets `flushDenormal`**, because WASM has no flush-to-zero at all and a state that
asymptotes toward zero sticks in the subnormal range forever, becoming a permanent
audio-thread denormal generator.

Audit finding 11 found the policy had only ever been applied to the two shared primitives it
shipped with. Fixing that (docs §33) forced three decisions that the policy as written did not
answer.

**1. The `chowdsp` WDF capacitor's state is unreachable by the obvious means.** The RAT's and
GOLD's diode clippers are `chowdsp_wdf` networks, and the network's recursive memory is
`chowdsp::wdft::CapacitorT<double>`'s private member `z`. The class is `final`, so it cannot be
subclassed, and the library offers no anti-denormal guard. It was worth solving rather than
skipping: this one state was the RAT's entire measured 1.97× silence cost, and it is invisible
in the audio (the `double`→`float` cast of a subnormal is `0.0f`), so only the CPU can see it.

**2. "Every recursive accumulator" over-reaches.** Many of the states the audit listed —
`TriodeStage::vCk_`/`vCo_`, the power amps' `vRail_`/`vScreen_`, the AC30's `iSagEnv_`, the
Newton warm starts — rest at a **nonzero DC operating point** of hundreds of volts or
milliamps. A flush at 1e-30 there is unreachable code, and `TriodeStage::processSampleOS` is
the most-executed loop in the core.

**3. A `double`-state guard is not testable through the audio.** `float(1e-310)` is exactly
`0.0f`, so a test that only reads output samples cannot distinguish a flushed model from one
grinding through subnormals forever. This is precisely how the defect survived both a green
suite and a benchmark reporting a clean 1.00× cliff.

## Decision

**1. Reach the WDF capacitor's state through the library's own public API; do not fork it.**
`Denormal.h` gains `flushDenormalWdfCapacitor(cap)`, which relies only on documented public
behaviour: `incident(x)` performs `wdf.a = x; z = wdf.a;` — so the public `wdf.a` mirrors `z` —
and `reflected()` writes only `wdf.b`. Reading `cap.wdf.a` reads the state; `cap.incident(0)`
zeroes both. The helper reads, flushes, and writes back **only when the guard actually fires**.

It must be called **after** the sample's output voltage has been read, so a flush can only
affect the *next* sample and the sample that tripped the guard is bit-identical to the
unguarded network.

Rejected: vendoring or forking `CapacitorT` (drifts from the SHA-pinned dependency, and the pin
is deliberate); subclassing (`final`); patching the dependency in `FetchContent` (a private
fork of a third-party library to add two comparisons).

**2. Guard a state only if its value AT REST can be zero.** A state that rests at a nonzero DC
operating point cannot be subnormal and is left unguarded **on purpose**, with a comment at the
site naming the resting value and how it was measured. Corollary: `TriodeStage.cpp` does not
include `Denormal.h` at all, and says why.

Where a state rests at zero only *in principle* — the JCM800/Twin OT and feedback states, which
idle at ~1e-13 in a running amp because a push-pull pair does not cancel exactly, but which
`parkState()` sets to exactly zero — the flush is kept and **labelled a guard-rail rather than
a fix**, so nobody later cites it as evidence of a cliff it did not cure.

**3. Classes whose zero-resting state is `double` expose `double maxAbsRestingState() const`.**
The largest |value| among the states whose rest value is zero; the tests assert it is **exactly
0.0** after a silent tail. Not used by the audio path — the same role as `lastOutputPeak()` and
`lastMaxIters()`. A class with no zero-resting state does **not** get one, because the
assertion would have no teeth.

## Consequences

**Easier.** The RAT/GOLD WDF cliff is fixed without touching the pinned dependency, so a SHA
bump stays a one-line change. Every guard site now records *why* it is there or *why* it is
not, so the next reader does not have to re-derive it — and the guards that cannot fire are
labelled, so the next person does not mistake a guard-rail for a measured fix. Double-state
denormals are testable deterministically, without timing.

**Harder / the costs.**

- `flushDenormalWdfCapacitor` depends on `wdf.a` tracking `z`. That is public, stable across
  the `chowdsp_wdf` v1.0.0 API, and the invariant is spelled out in the helper's comment — but
  it is an invariant of *someone else's* code. Mitigation: `clipper_denormal_tests` builds a
  real unguarded `chowdsp` network and asserts it **does** go subnormal, so if a library bump
  changes the internals the suite reports "this no longer reproduces finding 11" rather than
  passing vacuously.
- `maxAbsRestingState()` is public API that exists for tests. It is deliberately narrow
  (const, no state, one number) and precedented in this codebase, but it is API, and its
  correctness is a claim about which states rest at zero — a claim that must be **re-measured**
  if a bias point, a tail resistor or a solver tolerance changes. The AC30 is the cautionary
  example: its TOP CUT states look like the obvious candidates and are not the culprit, while
  its OT pair is, and only bisection showed that.
- Judging "rests at zero" requires a measurement per site rather than a rule that can be
  applied by reading. That is the intended trade: this defect existed because a blanket policy
  was assumed to have been applied rather than checked.
