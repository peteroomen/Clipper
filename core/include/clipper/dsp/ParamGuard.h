// Clipper — portable DSP core.
//
// ParamGuard: the ONE parameter clamp for the whole core. Every model used to
// carry a private copy of
//
//     float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
//
// and that shape is TRANSPARENT TO NaN: both comparisons are false for NaN, so
// the NaN falls straight through to the model. `std::clamp(v, 0.0, 1.0)` and
// JavaScript's `Math.min(1, Math.max(0, v))` have exactly the same hole. Once a
// NaN lands in a recursive state — a smoother value, a biquad delay, a WDF cap
// voltage, a Newton warm start — it is LATCHED: every subsequent sample is NaN,
// writing a good parameter afterwards never clears it, and the only recovery is
// to destroy and recreate the engine. That was measured (2026-07-24 audit,
// finding 1): 12672/12672 non-finite samples in a 1 s window, forever, for
// Jcm800Amp / AmpModel / RatModel alike, from ONE bad parameter write. Inf was
// handled correctly all along; NaN was not.
//
// The fix is the INVERTED comparison order:
//
//     v > hi ? hi : (v > lo ? v : lo)
//
// For every finite v this is BIT-IDENTICAL to the old clamp (same branches, same
// returned value), so replacing the old clamps with these is fidelity-neutral by
// construction — not a tone change. For NaN both comparisons are false and the
// expression lands on `lo`, a safe in-range fallback, so the NaN never reaches
// the state. There is no isnan() call and no branch added: it is the same two
// compares, written so the unordered case falls into the safe arm.
//
// This is DEFENCE IN DEPTH. The primary gate is at the C ABI boundary (every
// *_set_param export rejects non-finite outright, see clipper_c_api.cpp) and at
// the worklet/TS boundaries. These helpers guarantee that any OTHER caller —
// the JUCE plugin, which calls model setParameter() directly and never touches
// the C ABI, a test, a future FFI — cannot poison a model either.
//
// Recovery is the third leg: see the reset() methods added across the tree, and
// the *_reset C ABI exports.
//
// Header-only, <cmath> only, platform-free (C++17, no OS/browser/Emscripten).

#ifndef CLIPPER_DSP_PARAM_GUARD_H
#define CLIPPER_DSP_PARAM_GUARD_H

#include <cmath>

namespace clipper::dsp {

// True only for a value that is safe to let into the engine. Use this at a
// boundary where the right answer is "ignore the write entirely" rather than
// "substitute a fallback" (the C ABI exports, the worklet message handler).
inline bool paramIsFinite(float v) { return std::isfinite(v); }
inline bool paramIsFinite(double v) { return std::isfinite(v); }

// Clamp a normalized knob position to [0, 1], mapping NaN to 0. Bit-identical to
// the old `v < 0 ? 0 : (v > 1 ? 1 : v)` for every finite input.
inline float clampParam01(float v) { return v > 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f); }
inline double clampParam01(double v) { return v > 1.0 ? 1.0 : (v > 0.0 ? v : 0.0); }

// Clamp to an arbitrary [lo, hi], mapping NaN to lo. Same guarantee: identical
// to std::clamp for finite inputs, safe for NaN. Requires lo <= hi.
template <typename T>
inline T clampParam(T v, T lo, T hi) {
    return v > hi ? hi : (v > lo ? v : lo);
}

// Round a parameter carrying a small INTEGER (a mode selector, a switch) to the
// nearest int, mapping non-finite to `fallback`. std::lround(NaN) is
// unspecified — it does not have to give anything in range — so a mode selector
// must not be fed a raw parameter value.
inline int paramToInt(float v, int fallback = 0) {
    return std::isfinite(v) ? static_cast<int>(std::lround(v)) : fallback;
}

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_PARAM_GUARD_H
