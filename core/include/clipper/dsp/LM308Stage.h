// Clipper — portable DSP core.
//
// LM308Stage (M6.5): the two large-signal / limited-bandwidth behaviours of the
// ProCo RAT's LM308 op-amp gain stage that an *ideal* op-amp lacks — and whose
// absence makes a digital RAT sound "fizzy" (razor-sharp edges hitting the diode
// clamp). Placed at the op-amp OUTPUT node: after the frequency-dependent
// (feedback-network) gain, before the shunt-diode clamp, and INSIDE the
// oversampled section (a bandwidth limit + a nonlinearity both belong at the
// oversampled rate).
//
//   1. Gain-tracking closed-loop bandwidth. A single-pole (dominant-pole
//      compensated) op-amp has a roughly constant gain-bandwidth product GBW; the
//      closed-loop -3 dB corner is  f_c = GBW / A_noise , where A_noise is the
//      (frequency-independent, plateau) noise gain set by the DISTORTION pot. At
//      max gain (+66 dB, A ~ 2000) f_c collapses to the few-hundred-Hz region, so
//      a cranked hardware RAT is THICK, never fizzy; at min gain the corner sits
//      far above audio. Modelled as a one-pole low-pass whose corner is refreshed
//      (at control rate) from the smoothed pre-gain, so it glides click-free with
//      the knob.
//
//   2. Slew-rate limiting. The LM308 slews at ~0.3 V/us; the gain stage's steep
//      edges are rate-limited, softening the transitions that would otherwise
//      alias / fizz through the diode clamp. A hard per-sample dV clamp — a
//      genuine nonlinearity, which is why it lives inside oversampling (ADAA is
//      not trivially applicable to a slew clamp; oversampling + measurement
//      decide, see docs §M6.5).
//
// Zero-dependency (only <cmath>) and platform-free: compiles natively and under
// Emscripten. The GBW and slew-rate VALUES are supplied by the caller
// (RatModel.cpp documents the chosen 1.0 MHz / 0.3 V/us and why).
//
// Convention: 1.0f == 1.0 V (the model-wide diode reference), so slewVoltsPerSec
// is in the same "volts" per second.

#ifndef CLIPPER_DSP_LM308_STAGE_H
#define CLIPPER_DSP_LM308_STAGE_H

#include <cmath>

namespace clipper::dsp {

class LM308Stage {
public:
    // Configure for the (already oversampled) rate the stage runs at, the op-amp
    // gain-bandwidth product (Hz), and the slew rate (volts/second). Resets state
    // and parks the corner at unity noise gain (LP wide open).
    void prepare(double sampleRate, double gbwHz, double slewVoltsPerSec) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        gbwHz_ = gbwHz > 0.0 ? gbwHz : 1.0e6;
        maxDeltaPerSample_ =
            static_cast<float>((slewVoltsPerSec > 0.0 ? slewVoltsPerSec : 1.0e30) / sampleRate_);
        reset();
        setNoiseGain(1.0f);
    }

    void reset() {
        lpState_ = 0.0f;
        slewState_ = 0.0f;
    }

    // Update the closed-loop corner from the current noise gain (>= 1). Control
    // rate is fine: the corner is a filter coefficient, not the signal, and the
    // caller passes the smoothed pre-gain so it glides. f_c = GBW / A, clamped to
    // a safe fraction of Nyquist so the one-pole stays well-behaved (at low gain
    // the true corner is far above the oversampled Nyquist anyway — transparent).
    void setNoiseGain(float noiseGain) {
        const double A = noiseGain > 1.0f ? static_cast<double>(noiseGain) : 1.0;
        double fc = gbwHz_ / A;
        const double fcMax = 0.45 * sampleRate_;
        if (fc > fcMax) fc = fcMax;
        lpCoeff_ = static_cast<float>(1.0 - std::exp(-kTwoPi * fc / sampleRate_));
    }

    // One op-amp sample: closed-loop bandwidth limit, then slew-rate limit.
    inline float processSample(float x) {
        // 1. Closed-loop bandwidth: one-pole low-pass (corner = GBW / A_noise).
        lpState_ += lpCoeff_ * (x - lpState_);
        // 2. Slew-rate limit: clamp the per-sample change of the output node.
        float d = lpState_ - slewState_;
        if (d > maxDeltaPerSample_) d = maxDeltaPerSample_;
        else if (d < -maxDeltaPerSample_) d = -maxDeltaPerSample_;
        slewState_ += d;
        return slewState_;
    }

    // Introspection (used by the measurement tests).
    float lpCoeff() const { return lpCoeff_; }
    float maxDeltaPerSample() const { return maxDeltaPerSample_; }
    double sampleRate() const { return sampleRate_; }

private:
    static constexpr double kTwoPi = 6.283185307179586;
    double sampleRate_ = 44100.0;
    double gbwHz_ = 1.0e6;
    float lpCoeff_ = 1.0f;    // one-pole coefficient for the closed-loop LP
    float lpState_ = 0.0f;
    float maxDeltaPerSample_ = 1.0e30f;  // slew clamp (volts/sample)
    float slewState_ = 0.0f;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_LM308_STAGE_H
