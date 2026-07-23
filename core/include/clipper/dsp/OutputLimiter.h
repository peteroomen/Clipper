// Clipper — portable DSP core.
//
// OutputLimiter (M6.5): the rig's final safety limiter. It is a TRUE SAFETY
// catch, not a tone stage — transparent (bit-identity) below +/-kThreshold, then
// a narrow tanh knee asymptoting to +/-1.0 so the output can never emit raw
// overs. M6.1 shipped this with a 0.9 threshold; combined with the loud M6.1
// staging that made the CLEAN (pedal-bypassed) path ride into the knee on every
// cycle — soft-clipping a clean amp = "fizz". M6.5 raises the threshold to 0.97
// AND pulls the gain staging down (amp volume tops out at unity, see
// AmpModel.cpp) so realistic levels stay comfortably below the knee; the limiter
// then only catches genuine transient overs.
//
// This header is the SINGLE SOURCE OF TRUTH for the limiter math. The AudioWorklet
// (web/worklet/clipper-processor.js) is plain JS and cannot include it, so it
// mirrors kThreshold as LIM_THRESH and the same tanh formula — keep them in sync
// (like the mirrored param ids). The native tests exercise THIS implementation.
//
// Zero-dependency (<cmath>) and platform-free.

#ifndef CLIPPER_DSP_OUTPUT_LIMITER_H
#define CLIPPER_DSP_OUTPUT_LIMITER_H

#include <cmath>

namespace clipper::dsp {

struct OutputLimiter {
    // Transparent below +/-kThreshold; tanh knee to +/-1.0 above. Mirrored in
    // web/worklet/clipper-processor.js (LIM_THRESH). Raised from 0.9 (M6.1) so
    // the clean path has real headroom (M6.5).
    static constexpr float kThreshold = 0.97f;

    static inline float process(float x) {
        const float t = kThreshold;
        const float k = 1.0f - t;
        if (x > t) return t + k * std::tanh((x - t) / k);
        if (x < -t) return -t + k * std::tanh((x + t) / k);
        return x;
    }
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OUTPUT_LIMITER_H
