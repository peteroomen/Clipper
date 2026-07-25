// Clipper — portable DSP core (M5).
//
// A minimal biquad (transposed direct form II) plus RBJ "Audio EQ Cookbook"
// coefficient designers for the shelves/peaks the amp tone stack and the cab IR
// generator need. TDF2 is chosen for its graceful behaviour under slow
// coefficient changes (the amp recomputes coefficients at a control rate as the
// tone knobs are smoothed), so there is no zipper noise on a knob sweep.
//
// Header-only, platform-free C++17 (only <cmath>).

#ifndef CLIPPER_DSP_BIQUAD_H
#define CLIPPER_DSP_BIQUAD_H

#include <cmath>

#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

struct BiquadCoeffs {
    // Normalized so a0 == 1 (b's are already divided through).
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    const BiquadCoeffs& coeffs() const { return c_; }

    void reset() { z1_ = z2_ = 0.0f; }

    // Transposed Direct Form II. The two state updates are flushed through the
    // anti-denormal guard (Denormal.h): a decaying tail rings z1_/z2_ down through
    // the subnormal range, which is a CPU cliff on WASM (no FTZ) — the guard snaps
    // the state to exactly 0 once it falls below the denormal floor (-600 dB), so
    // no subnormal ever feeds the next sample's multiplies. Audio-transparent.
    inline float process(float x) {
        const float y = c_.b0 * x + z1_;
        z1_ = flushDenormal(c_.b1 * x - c_.a1 * y + z2_);
        z2_ = flushDenormal(c_.b2 * x - c_.a2 * y);
        return y;
    }

private:
    BiquadCoeffs c_{};
    float z1_ = 0.0f, z2_ = 0.0f;
};

namespace rbj {

constexpr double kTwoPi = 6.283185307179586;

// Low-shelf: boosts/cuts below fc by dbGain, with slope shape S (1.0 = as steep
// as possible without overshoot). RBJ cookbook.
inline BiquadCoeffs lowShelf(double fc, double dbGain, double S, double fs) {
    const double A = std::pow(10.0, dbGain / 40.0);
    const double w0 = kTwoPi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1) - (A - 1) * cw + twoSqrtAalpha);
    const double b1 = 2 * A * ((A - 1) - (A + 1) * cw);
    const double b2 = A * ((A + 1) - (A - 1) * cw - twoSqrtAalpha);
    const double a0 = (A + 1) + (A - 1) * cw + twoSqrtAalpha;
    const double a1 = -2 * ((A - 1) + (A + 1) * cw);
    const double a2 = (A + 1) + (A - 1) * cw - twoSqrtAalpha;

    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0),
            static_cast<float>(b2 / a0), static_cast<float>(a1 / a0),
            static_cast<float>(a2 / a0)};
}

// High-shelf: boosts/cuts above fc by dbGain.
inline BiquadCoeffs highShelf(double fc, double dbGain, double S, double fs) {
    const double A = std::pow(10.0, dbGain / 40.0);
    const double w0 = kTwoPi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1) + (A - 1) * cw + twoSqrtAalpha);
    const double b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    const double b2 = A * ((A + 1) + (A - 1) * cw - twoSqrtAalpha);
    const double a0 = (A + 1) - (A - 1) * cw + twoSqrtAalpha;
    const double a1 = 2 * ((A - 1) - (A + 1) * cw);
    const double a2 = (A + 1) - (A - 1) * cw - twoSqrtAalpha;

    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0),
            static_cast<float>(b2 / a0), static_cast<float>(a1 / a0),
            static_cast<float>(a2 / a0)};
}

// Peaking EQ: bell of dbGain at fc with quality Q.
inline BiquadCoeffs peaking(double fc, double dbGain, double Q, double fs) {
    const double A = std::pow(10.0, dbGain / 40.0);
    const double w0 = kTwoPi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);

    const double b0 = 1 + alpha * A;
    const double b1 = -2 * cw;
    const double b2 = 1 - alpha * A;
    const double a0 = 1 + alpha / A;
    const double a1 = -2 * cw;
    const double a2 = 1 - alpha / A;

    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0),
            static_cast<float>(b2 / a0), static_cast<float>(a1 / a0),
            static_cast<float>(a2 / a0)};
}

// 2nd-order high-pass (Butterworth-ish, Q = 1/sqrt2 by default).
inline BiquadCoeffs highPass(double fc, double Q, double fs) {
    const double w0 = kTwoPi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);

    const double b0 = (1 + cw) / 2;
    const double b1 = -(1 + cw);
    const double b2 = (1 + cw) / 2;
    const double a0 = 1 + alpha;
    const double a1 = -2 * cw;
    const double a2 = 1 - alpha;

    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0),
            static_cast<float>(b2 / a0), static_cast<float>(a1 / a0),
            static_cast<float>(a2 / a0)};
}

// 2nd-order low-pass.
inline BiquadCoeffs lowPass(double fc, double Q, double fs) {
    const double w0 = kTwoPi * fc / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * Q);

    const double b0 = (1 - cw) / 2;
    const double b1 = 1 - cw;
    const double b2 = (1 - cw) / 2;
    const double a0 = 1 + alpha;
    const double a1 = -2 * cw;
    const double a2 = 1 - alpha;

    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0),
            static_cast<float>(b2 / a0), static_cast<float>(a1 / a0),
            static_cast<float>(a2 / a0)};
}

}  // namespace rbj
}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_BIQUAD_H
