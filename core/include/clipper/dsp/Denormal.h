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

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DENORMAL_H
