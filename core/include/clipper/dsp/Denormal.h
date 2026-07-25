// Clipper — portable anti-denormal helper.
//
// WHY THIS EXISTS
// A recursive float filter state (a biquad's z1/z2, a one-pole smoother's value)
// fed a signal that DECAYS TO SILENCE rings down through the SUBNORMAL float
// range (magnitudes below ~1.18e-38). On hardware, arithmetic on subnormals takes
// a microcoded slow path — 10-100x slower per op. Native x86/ARM does NOT enable
// flush-to-zero (FTZ/DAZ) by default, and WASM has NO FTZ AT ALL (the runtime
// cannot be asked to flush), so the penalty is unavoidable in code: every decaying
// note tail becomes a burst of thousands of subnormal ops on the audio thread —
// periodic crackle/lag that looks "random" and is independent of which amp/pedal
// is loaded (every stage's tone filters share these primitives). See docs §25 and
// scripts/denormal_bench.cpp for the measured cliff.
//
// THE GUARD
// After each recursive update, flush any state whose magnitude has fallen below a
// floor SAFELY ABOVE the subnormal boundary (kDenormalFloor = 1e-30, i.e. −600 dB,
// far below the -144 dB 24-bit noise floor — bit-irrelevant to audio) to exactly
// zero. The state is then always either 0 or normal: no subnormal ever reaches an
// arithmetic unit. This mirrors the ReverbModel's long-standing 1e-20 loop-input
// offset (the other house anti-denormal pattern), applied as a flush to the shared
// primitives so the guard is uniform and testable.
//
// The floor sits far ABOVE the largest subnormal yet far BELOW the reverb loop's
// 1e-20 anti-denormal signal, so a biquad carrying that 1e-20 floor is untouched
// (zero interaction with ReverbModel's existing guard) while true denormals are
// still eliminated with >8 orders of margin.
//
// Written as a branchless select (ternary compiles to cmp+select / fcsel), so it
// is cheap in the per-sample hot loop and correct regardless of -ffast-math.
//
// WHERE THE GUARD APPLIES (audit finding 11, fixed 2026-07-25 — docs §33)
// "Every recursive accumulator" means every one, not just the two shared primitives
// this header shipped with. The 2026-07-24 audit found the policy had been written
// but only half-applied, and — worse — that scripts/denormal_bench.cpp exercised
// ONLY Biquad and OnePoleSmoother, so it reported a reassuring ~1.00x cliff while a
// RAT ran 2.01x slower on silence than on a riff. Measured before the fix, per 10 s:
//   RAT 328 -> 658 ms (2.01x)   GOLD 326 -> 639 ms (1.96x, 393607 subnormal samples
//   emitted downstream)   SD-1 1.15x   Screamer 1.13x   Processor(gain 0) 12x
//   FenderToneStack 18.6x   MarshallToneStack 68.2x   Ac30PowerAmp 1.29x
// A state that parks at a NONZERO DC operating point (a cathode cap at its bias
// voltage, a B+ rail, a Newton warm start) can never be subnormal, so it is left
// unguarded ON PURPOSE and each site says so; that claim is measured, not assumed.
//
// Header-only, zero-dependency (<cmath>), platform-free: compiles native and WASM.

#ifndef CLIPPER_DSP_DENORMAL_H
#define CLIPPER_DSP_DENORMAL_H

#include <cmath>

namespace clipper::dsp {

// Magnitudes below this flush to zero. Above every subnormal (max ~1.18e-38),
// below the reverb loop's 1e-20 offset, and ~460 dB below any audible signal.
constexpr float kDenormalFloor = 1e-30f;

// Return v, or exactly 0 if |v| has decayed below the denormal floor. Guarantees
// the result is never subnormal (it is 0 or a normal float).
inline float flushDenormal(float v) {
    return (v < kDenormalFloor && v > -kDenormalFloor) ? 0.0f : v;
}

// Double-precision overload, same floor. Double recursive state (the phaser's
// allpass memory, the tremolo's control smoothers) decays into DOUBLE subnormals
// (< ~2.2e-308) during silence — the identical CPU cliff, just further down. The
// shared 1e-30 floor is a NORMAL double, so flushing there keeps double state out
// of its subnormal range with ~278 orders of magnitude to spare, while remaining
// −600 dB below audio (bit-transparent for any signal that matters).
inline double flushDenormal(double v) {
    return (v < static_cast<double>(kDenormalFloor) && v > -static_cast<double>(kDenormalFloor))
               ? 0.0
               : v;
}

// True iff v is a nonzero subnormal float (the range this guard exists to keep
// out of recursive state). Used by the denormal tests.
inline bool isSubnormal(float v) {
    return v != 0.0f && std::fabs(v) < 1.1754943508222875e-38f;  // FLT_MIN
}

// True iff v is a nonzero subnormal double. The double boundary is ~2.2e-308, so a
// double state takes far longer to get there than a float one — but it gets there,
// and once there it is just as stuck and just as expensive. Used by the tests.
inline bool isSubnormal(double v) {
    return v != 0.0 && std::fabs(v) < 2.2250738585072014e-308;  // DBL_MIN
}

// ---------------------------------------------------------------------------
// THE `maxAbsRestingState()` DIAGNOSTIC (audit finding 11, docs §33)
//
// Several models carry their recursive state in DOUBLE. A double subnormal parked in
// that state is a permanent audio-thread cost, but it is INVISIBLE IN THE AUDIO: the
// model's output is a float cast of a double node voltage, and float(1e-310) is
// exactly 0.0f. So a test that only looks at output samples cannot tell a correctly
// flushed model from one grinding through subnormals forever — which is precisely how
// this defect survived a green suite and a benchmark reporting a 1.00x cliff.
//
// So the affected classes expose ONE const diagnostic, `double maxAbsRestingState()`,
// returning the largest |value| among the recursive states whose value AT REST (silent
// input) is ZERO — exactly the states this header must flush. The property the tests
// pin: after a silent tail it is EXACTLY 0.0. Any tiny nonzero residual is a state
// asymptoting through the subnormal range.
//
// It deliberately EXCLUDES state that rests at a nonzero DC operating point (a cathode
// cap at its bias voltage, a B+ rail, a sag envelope at idle draw, a Newton warm
// start). Those can never be subnormal and are left unguarded on purpose; each site
// says so. Not used by the audio path — same role as lastOutputPeak()/lastMaxIters().
// ---------------------------------------------------------------------------

// Largest |value| among the arguments, or 0.0 for none. The one-liner behind every
// maxAbsRestingState(), so the diagnostic cannot be spelled inconsistently.
template <typename... Ts>
inline double maxAbsState(Ts... vs) {
    double m = 0.0;
    ((m = (std::fabs(static_cast<double>(vs)) > m) ? std::fabs(static_cast<double>(vs)) : m),
     ...);
    return m;
}

// ---------------------------------------------------------------------------
// The WDF capacitor guard (audit finding 11, docs §33).
//
// chowdsp::wdft::CapacitorT<T> holds the recursive wave state in a PRIVATE member
// `z`, the class is `final`, and the library offers no anti-denormal guard of its
// own. Under a silent input the WDF network's waves ring down and `z` parks in the
// subnormal range permanently — the same cliff as any other unguarded recursive
// state, and the one that costs the RAT and GOLD ~2x on silence. The pedal's float
// OUTPUT is clean (the double->float cast of a subnormal yields 0.0f), so nothing
// in the audio can see it; only the CPU can. That is why it survived.
//
// The state is nonetheless reachable through the library's OWN PUBLIC API, with no
// fork of the pinned dependency and no reliance on anything undocumented:
//   incident(x) does `wdf.a = x; z = wdf.a;`   -> the public wdf.a mirrors z
//   reflected() does `wdf.b = z;`              -> never writes z or wdf.a
// so reading `cap.wdf.a` reads the state, and `cap.incident(0)` zeroes BOTH z and
// wdf.a. Hence: read, flush, and write back only when the guard actually fires.
//
// CALL THIS AFTER the sample's output voltage has been read, never before: then a
// flush can only ever affect the NEXT sample, so the sample that tripped the guard
// is bit-identical to the unguarded computation.
//
// (After CapacitorT::reset() the library leaves `z` at 0 while `wdf.a` keeps its old
// value. That asymmetry is harmless here: a stale-large wdf.a means the guard simply
// does not fire, and a stale-tiny one makes it write a 0 that z already holds.)
template <typename CapacitorT>
inline void flushDenormalWdfCapacitor(CapacitorT& cap) {
    const auto z = cap.wdf.a;
    const auto flushed = flushDenormal(z);
    if (flushed != z) cap.incident(flushed);
}

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DENORMAL_H
