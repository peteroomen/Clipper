# ADR 002: Non-finite parameters are rejected, not clamped — and every unit can be reset

Date: 2026-07-25
Status: Accepted

## Context

The 2026-07-24 audit's finding 1: one NaN parameter permanently destroyed all audio,
and the in-app assistant could send one. Measured, before this decision: after a
single NaN write, every unit emitted ~100 % non-finite samples forever (48000/48000
in a 1 s window for all ten units), and writing a good value back never cleared it —
the NaN was latched in smoother / biquad / cap-voltage / Newton-warm-start state.
There was no `reset` anywhere in the valve-amp tree or the C ABI, so the only
recovery was to destroy and recreate the engine.

Two things had to be decided, and neither answer is obvious.

**1. What should a non-finite parameter write DO?** The tree's ~14 private clamps all
had the shape `v < 0 ? 0 : (v > 1 ? 1 : v)`, which is transparent to NaN (both
comparisons are false) but *does* clamp `Inf` to the rail. So `Inf` already had
established behaviour — "jump the knob to its extreme" — and NaN had none. Options:

- (a) Clamp everything, including NaN, to a chosen in-range value.
- (b) Reject the write entirely and keep the previous value.
- (c) Reject at the boundary; clamp deeper in as a backstop.

**2. What should recovery cost?** The natural implementation of "reset" is to call
`prepare()` again. But `TriodeStage::prepare()` solves a DC operating point *and then
settles ~12 grid-leak RCs of silent samples* (≈50 k samples per stage at 4×/48 k);
`Jcm800Amp::prepare()` measures 87.6 ms. If recovery means `prepare()`, then the
recovery path itself blows through 33 render deadlines.

## Decision

**Reject at the boundary, clamp at the source — option (c), both layers.**

- Every `*_set_param` export in the C ABI begins
  `if (!std::isfinite(value)) return;`. The write is **dropped**, not clamped, and the
  knob keeps its previous value. This applies to `Inf` too, deliberately changing its
  old behaviour: a caller sending `Inf` is malfunctioning, and silently slamming a
  knob to its extreme is a worse outcome than ignoring the message.
- Every parameter clamp in `core/` routes through one shared helper,
  `core/include/clipper/dsp/ParamGuard.h`, written with the comparison order
  **inverted** (`v > hi ? hi : (v > lo ? v : lo)`) so the unordered NaN case falls
  into the safe arm. For every finite input this is bit-identical to the clamp it
  replaces, so the change is fidelity-neutral by construction rather than by
  measurement.

Both layers, not one, because they cover different callers: the ABI gate is the
chokepoint guaranteed to be on the web path, and the in-model clamps are the only
protection the **JUCE plugin** has — it calls `setParameter()` directly and never
touches the C ABI.

**`reset()` re-parks at the cached, already-solved DC operating point and never
re-solves.** `TriodeStage` and `BjtStage` snapshot their settled fixed point
(`cachePark()` at the end of `settleDC()`); the three power amps factor their
idle-parking block into a `parkState()` shared by `setOversampling()` and `reset()`.
Measured result: `Jcm800Amp::reset()` = 0.0003 ms, ~302 000× cheaper than
`prepare()`. `reset()` clears recursive state only — knob positions, oversampling
factor, loaded IR, active amp voice and sample rate all survive.

## Consequences

**Easier.** A non-finite value cannot reach recursive state from any caller. The
assistant — the best-defended surface in the codebase, and still the one that could
brick audio — can no longer do so even with a malformed tool emission. There is now
one place to be right about parameter clamping instead of fourteen. And a poisoned
engine has a cheap, allocation-free recovery path, which also gives the front-end
something to call from a future watchdog.

**Harder / traded away.**

- **`Inf` no longer clamps.** Any caller relying on "send +Inf to max a knob" breaks.
  Nothing in the repo did, and the ABI never documented it, but it is a behaviour
  change and the new `clipper_nan_guard_tests` pins it.
- **A dropped write is silent.** The engine has no channel to tell the UI "I ignored
  that", so a buggy caller gets no feedback beyond the knob not moving. Accepted:
  the alternative (clamping) is what made the bug invisible in the first place.
- **`reset()` restarts modulator LFOs.** The opto tremolo's phase is state, and a
  recovery path has no better phase to resume from, so resetting a Twin with the
  tremolo engaged shifts its level by a measured 0.039 dB over a 0.3 s window. That is
  a fraction of the tremolo's own depth and is the right trade for a recovery path.
- **`cachePark()` is a new invariant to maintain.** Anyone adding state to
  `TriodeStage` or `BjtStage` must add it to the park snapshot too, or `reset()` will
  silently leave that state poisoned. The `parkState()` refactor in the power amps
  exists specifically so `setOversampling()` and `reset()` cannot drift apart; the new
  test's bit-exactness assertion (block C) is what catches a forgotten field.
- **The signal path is still NaN-transparent.** This ADR covers *parameters*.
  `OutputLimiter::clamp1` has the same broken shape, and a NaN can still arrive as an
  audio sample from a broken input device — which is precisely why the reset export
  had to exist rather than the guards being deemed sufficient.
