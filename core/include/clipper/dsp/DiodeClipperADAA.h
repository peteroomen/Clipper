// Clipper — portable DSP core.
//
// DiodeClipperADAA (M2): an EXPERIMENTAL, memoryless stand-in for the WDF diode
// stage, used to compare first-order antiderivative antialiasing (ADAA) against
// oversampling. It is NOT the production clipper (that stays WDF + oversampling);
// it exists so the tests/CLI can quantify how close ADAA at 1x/2x gets to WDF at
// 4x/8x on aliasing and CPU.
//
// Transfer curve (matching the WDF static response, see docs/DEVELOPMENT.md M2):
//   f(x) = Vk * tanh(x / Vk)
// The WDF diode node has small-signal slope ~1 (diodes off) and a soft knee whose
// output saturates around ~0.33-0.39 V under realistic overdrive. tanh(x/Vk)*Vk
// matches the unity origin slope exactly and limits at +/-Vk; Vk = 0.35 places
// the tanh ceiling in the WDF's measured mid/high-drive output band. (The WDF
// keeps rising ~logarithmically above the knee while tanh flattens — a documented
// approximation; the comparison is about aliasing behaviour, not an exact match.)
//
// First-order ADAA (Parker, Esqueda, Bilbao, Valimaki 2016): integrate the
// nonlinearity once and difference,
//   y[n] = (F1(x[n]) - F1(x[n-1])) / (x[n] - x[n-1])
// with a fall-back to f((x[n]+x[n-1])/2) when successive inputs are too close
// (the divided difference is ill-conditioned). F1 is the antiderivative of f:
//   F1(x) = Vk^2 * ln(cosh(x / Vk))
// evaluated in a numerically stable form.
//
// Zero-dependency, platform-free. State is scalar; nothing allocates.

#ifndef CLIPPER_DSP_DIODE_CLIPPER_ADAA_H
#define CLIPPER_DSP_DIODE_CLIPPER_ADAA_H

#include <cmath>

namespace clipper::dsp {

class DiodeClipperADAA {
public:
    // Default knee matched to the RAT WDF static curve (see header comment).
    static constexpr double kDefaultVk = 0.35;

    void setKnee(double vk) { vk_ = vk > 1e-6 ? vk : 1e-6; }
    double knee() const { return vk_; }

    void reset() {
        x1_ = 0.0;
        f1x1_ = antiderivative(0.0);
    }

    // The memoryless transfer curve, f(x). Static so tests can build a "naive"
    // (aliasing) reference from the identical curve.
    static double f(double x, double vk) { return vk * std::tanh(x / vk); }
    double f(double x) const { return f(x, vk_); }

    // Naive (no antialiasing) memoryless evaluation. Aliases at high drive.
    inline float processSampleNaive(float x) {
        return static_cast<float>(f(static_cast<double>(x)));
    }

    // First-order ADAA evaluation. Reduces harmonics that would fold back.
    inline float processSampleADAA(float xin) {
        const double x = static_cast<double>(xin);
        const double f1x = antiderivative(x);
        const double dx = x - x1_;
        double y;
        if (std::fabs(dx) > kEps) {
            y = (f1x - f1x1_) / dx;
        } else {
            y = f(0.5 * (x + x1_));
        }
        x1_ = x;
        f1x1_ = f1x;
        return static_cast<float>(y);
    }

private:
    static constexpr double kEps = 1e-6;

    // F1(x) = Vk^2 * ln(cosh(x/Vk)), stable for large |x|:
    //   ln(cosh(z)) = |z| - ln(2) + log1p(exp(-2|z|))
    double antiderivative(double x) const {
        const double z = x / vk_;
        const double az = std::fabs(z);
        const double lncosh = az - kLn2 + std::log1p(std::exp(-2.0 * az));
        return vk_ * vk_ * lncosh;
    }

    static constexpr double kLn2 = 0.6931471805599453;
    double vk_ = kDefaultVk;
    double x1_ = 0.0;
    double f1x1_ = 0.0;  // = antiderivative(x1_)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DIODE_CLIPPER_ADAA_H
