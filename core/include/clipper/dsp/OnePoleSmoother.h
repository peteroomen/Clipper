// Clipper — portable DSP core.
//
// A tiny one-pole parameter smoother, extracted from the M0 gain-smoothing
// philosophy in Processor.cpp so the RAT model's three knobs can share exactly
// the same click-free ramp behaviour without duplicating the math.
//
// This header is zero-dependency (only <cmath>) and platform-free: it must
// compile natively and under Emscripten.

#ifndef CLIPPER_DSP_ONE_POLE_SMOOTHER_H
#define CLIPPER_DSP_ONE_POLE_SMOOTHER_H

#include <cmath>

namespace clipper::dsp {

// Exponential one-pole approach toward a target:  y += coeff * (target - y).
// Same formulation as M0's Processor gain smoother (coeff = 1 - exp(-1/(tau*fs))).
class OnePoleSmoother {
public:
    // Configure the per-sample coefficient for the given time constant (seconds)
    // and sample rate. Snaps the current value to the target so we don't ramp
    // from stale state after a (re)prepare.
    void prepare(double timeConstantSeconds, double sampleRate) {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double tau = timeConstantSeconds > 0.0 ? timeConstantSeconds : 1e-6;
        coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (tau * fs)));
        value_ = target_;
    }

    // Jump the target instantly to v and snap the current value to it (used at
    // prepare-time to avoid an initial ramp from a default).
    void setImmediate(float v) {
        target_ = v;
        value_ = v;
    }

    // Set the target the smoother ramps toward.
    void setTarget(float v) { target_ = v; }

    // Advance one sample and return the new smoothed value.
    inline float next() {
        value_ += coeff_ * (target_ - value_);
        return value_;
    }

    float value() const { return value_; }
    float target() const { return target_; }

private:
    float coeff_ = 0.0f;
    float value_ = 0.0f;
    float target_ = 0.0f;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ONE_POLE_SMOOTHER_H
