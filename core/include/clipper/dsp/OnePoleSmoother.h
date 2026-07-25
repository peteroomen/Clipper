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

#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

// Exponential one-pole approach toward a target:  y += coeff * (target - y).
// Same formulation as M0's Processor gain smoother (coeff = 1 - exp(-1/(tau*fs))).
//
// Templated on the value type (2026-07-25, fix/valve-amp-smoothing — docs §35).
// `OnePoleSmoother` is the float instantiation and is byte-for-byte the class it has
// always been; every existing user is unaffected. `OnePoleSmootherD` exists because
// the three valve amps carry their pot fractions and post-taper scale factors as
// DOUBLES all the way to the multiply, and smoothing them through a float would
// perturb the last mantissa bits of a STATIC render — the goldens and
// native/tests/identical_core_test both demand that a steady chain is unchanged, so
// the smoother has to be able to return the caller's own double back exactly.
template <class T>
class OnePoleSmootherT {
public:
    // Configure the per-sample coefficient for the given time constant (seconds)
    // and sample rate. Snaps the current value to the target so we don't ramp
    // from stale state after a (re)prepare.
    void prepare(double timeConstantSeconds, double sampleRate) {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double tau = timeConstantSeconds > 0.0 ? timeConstantSeconds : 1e-6;
        coeff_ = static_cast<T>(1.0 - std::exp(-1.0 / (tau * fs)));
        value_ = target_;
    }

    // Jump the target instantly to v and snap the current value to it (used at
    // prepare-time to avoid an initial ramp from a default).
    void setImmediate(T v) {
        target_ = v;
        value_ = v;
    }

    // Set the target the smoother ramps toward.
    void setTarget(T v) { target_ = v; }

    // True once the ramp has landed exactly on the target. next() is then a no-op
    // that returns target_ bit-exactly, so a caller may legitimately skip it — which
    // is how the valve amps' tone stacks pay nothing per sample while idle.
    bool settled() const { return value_ == target_; }

    // Recovery seam (audit finding 1). Snap the current value onto the target,
    // discarding whatever is in the recursive state — and sanitize the target
    // itself if a non-finite one ever got past the guards, because
    // `value_ += coeff*(target_ - value_)` can never climb back out of NaN. Keeps
    // the coefficient (i.e. the prepared rate) so this is NOT a re-prepare.
    // Allocation-free and O(1): safe to call from a control message.
    void reset() {
        if (!std::isfinite(target_)) target_ = static_cast<T>(0);
        value_ = target_;
    }

    // Advance one sample and return the new smoothed value. Anti-denormal: once the
    // residual (target - value) has decayed below the denormal floor, snap exactly
    // to the target. This kills BOTH denormal traps in an exponential approach — a
    // value_ ringing down toward a 0 target (it would otherwise asymptote and STICK
    // in the subnormal range forever, a permanent audio-thread denormal generator),
    // and the (target - value) residual drifting subnormal near a nonzero target.
    // Snapping within 1e-30 of the target is imperceptible (well past any param
    // resolution) and makes the smoother settle exactly. See Denormal.h / docs §25.
    inline T next() {
        value_ += coeff_ * (target_ - value_);
        const T residual = target_ - value_;
        if (residual < static_cast<T>(kDenormalFloor) && residual > -static_cast<T>(kDenormalFloor))
            value_ = target_;
        return value_;
    }

    T value() const { return value_; }
    T target() const { return target_; }

private:
    T coeff_ = static_cast<T>(0);
    T value_ = static_cast<T>(0);
    T target_ = static_cast<T>(0);
};

// The historical name: unchanged float smoother, used by the pedals, the clean amp,
// chorus, phaser and reverb.
using OnePoleSmoother = OnePoleSmootherT<float>;
// Double-precision sibling for the valve amps (see the note on the template).
using OnePoleSmootherD = OnePoleSmootherT<double>;

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ONE_POLE_SMOOTHER_H
