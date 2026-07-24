// Clipper — OptoTremolo (M10.1). See OptoTremolo.h for the AB763 optical-tremolo
// model, the "vibrato" misnomer, and the asymmetric-lag rationale. This file is
// the LFO + opto-cell numerics.

#include "clipper/dsp/OptoTremolo.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;
// One-pole time constant -> per-sample coefficient a = 1 - exp(-1/(tau*fs)).
inline double tcCoeff(double tauSec, double fs) {
    if (tauSec <= 0.0) return 1.0;
    return 1.0 - std::exp(-1.0 / (tauSec * fs));
}
}  // namespace

OptoTremolo::OptoTremolo() = default;

double OptoTremolo::rateHz() const { return rateSmoothed_; }

void OptoTremolo::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    // Control smoother ~10 ms so speed/intensity moves glide (no zipper).
    smoothA_ = tcCoeff(0.010, sampleRate_);
    attackA_ = tcCoeff(kAttackMs * 1e-3, sampleRate_);
    releaseA_ = tcCoeff(kReleaseMs * 1e-3, sampleRate_);
    // Snap smoothers to their current knob targets.
    rateSmoothed_ = kMinHz * std::pow(kMaxHz / kMinHz, std::clamp(speed_, 0.0, 1.0));
    depthSmoothed_ = std::clamp(intensity_, 0.0, 1.0);
    reset();
}

void OptoTremolo::reset() {
    phase_ = 0.0;
    cell_ = 0.0;
    lastGain_ = 1.0;
}

void OptoTremolo::setSpeed(float knob01) {
    speed_ = std::clamp(static_cast<double>(knob01), 0.0, 1.0);
}

void OptoTremolo::setIntensity(float knob01) {
    intensity_ = std::clamp(static_cast<double>(knob01), 0.0, 1.0);
}

double OptoTremolo::tick() {
    // Smooth the control targets toward the knobs.
    const double rateTarget = kMinHz * std::pow(kMaxHz / kMinHz, speed_);
    rateSmoothed_ += smoothA_ * (rateTarget - rateSmoothed_);
    depthSmoothed_ += smoothA_ * (intensity_ - depthSmoothed_);

    // LFO: raised-cosine lamp drive in [0,1] (0 = dark, 1 = bright). A raised
    // cosine (not a bare sine) keeps the lamp bounded to [0,1] like a real neon
    // lamp's light output; the opto lag then shapes the throb.
    const double lamp = 0.5 * (1.0 - std::cos(kTwoPi * phase_));
    phase_ += rateSmoothed_ / sampleRate_;
    if (phase_ >= 1.0) phase_ -= 1.0;

    // Opto cell follows the lamp with ASYMMETRIC lag: fast when brightening
    // (attack), slow when darkening (release). This is what makes the gain
    // envelope non-sinusoidal — sharper dips than recoveries.
    const double a = (lamp > cell_) ? attackA_ : releaseA_;
    cell_ += a * (lamp - cell_);

    // Bright lamp -> LDR conducts -> signal DIPS. Gain in [1-depth, 1].
    const double gain = 1.0 - depthSmoothed_ * cell_;
    lastGain_ = gain;
    return gain;
}

void OptoTremolo::process(const float* in, float* out, int numFrames) {
    for (int i = 0; i < numFrames; ++i) {
        const double g = tick();
        out[i] = static_cast<float>(static_cast<double>(in[i]) * g);
    }
}

}  // namespace clipper::dsp
