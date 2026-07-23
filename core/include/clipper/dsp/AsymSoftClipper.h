// Clipper — portable DSP core.
//
// AsymSoftClipper (M8): the soft, ASYMMETRIC clipping nonlinearity of the Boss
// SD-1 Super Overdrive. Where the RAT clamps hard to ground (a symmetric shunt
// diode pair, odd harmonics only), the SD-1 clips inside the op-amp FEEDBACK
// loop with 2 silicon diodes one way and 1 the other. Two consequences this
// class models:
//
//   1. SOFT knee — the feedback diodes limit gradually (a resistor Rf always in
//      parallel), so THD rises smoothly with input rather than snapping to a
//      square. Modelled with the tanh family (unity origin slope, gentle
//      saturation) — the same closed-form the M2 DiodeClipperADAA uses, so the
//      ADAA machinery drops straight in.
//
//   2. ASYMMETRY — 2 diodes clip the positive feedback swing at ~2*Vf, 1 diode
//      clips the negative at ~Vf. Different clip levels per polarity => an
//      even-harmonic component (the SD-1's warmth). We take the positive knee
//      Vp and the negative knee Vn as independent asymptotes.
//
//         f(u) =  Vp * tanh(u / Vp),  u >= 0
//                 Vn * tanh(u / Vn),  u <  0
//
//   f is C1 across 0 (both pieces -> u with unit slope near the origin), and its
//   antiderivative F is continuous with F(0)=0, so first-order ADAA (Parker,
//   Esqueda, Bilbao, Valimaki 2016) applies unchanged across the sign boundary:
//
//         F(u) =  Vp^2 * ln(cosh(u/Vp)),  u >= 0
//                 Vn^2 * ln(cosh(u/Vn)),  u <  0
//         y[n] = (F(u[n]) - F(u[n-1])) / (u[n] - u[n-1])          (divided diff)
//              = f((u[n]+u[n-1])/2)   when |u[n]-u[n-1]| is tiny  (fallback)
//
// WHY ADAA (not WDF) for the SD-1: chowdsp_wdf ships a SYMMETRIC DiodePairT only;
// an asymmetric (2-vs-1) diode root would need a custom nonlinearity solve. The
// SD-1's feedback clipper is a genuinely SOFT limiter that the tanh-family closed
// form captures well, and ADAA gives clean antialiasing of that soft curve with
// zero extra state — so the SD-1 uses ADAA as its PRODUCTION nonlinearity (run
// oversampled per M2), where the RAT uses WDF. A naive (aliasing) path is kept
// for the measurement A/B, mirroring DiodeClipperADAA.
//
// Zero-dependency (only <cmath>), platform-free: native and Emscripten.
// Convention: 1.0f == 1.0 V (the model-wide diode reference).

#ifndef CLIPPER_DSP_ASYM_SOFT_CLIPPER_H
#define CLIPPER_DSP_ASYM_SOFT_CLIPPER_H

#include <cmath>

namespace clipper::dsp {

class AsymSoftClipper {
public:
    // SD-1 knees: 2 silicon diodes (~2 * 0.5 V) positive, 1 diode (~0.5 V)
    // negative. The tanh asymptote sits at the knee voltage; the ~1.9:1 ratio is
    // the audible asymmetry (documented in SdModel.cpp).
    static constexpr double kDefaultVp = 0.95;  // positive swing (2 diodes)
    static constexpr double kDefaultVn = 0.50;  // negative swing (1 diode)

    // Set the per-polarity knee asymptotes (volts). Vp==Vn makes the curve odd
    // (symmetric) — used by the tests as the even-harmonic reference.
    void setKnees(double vp, double vn) {
        vp_ = vp > 1e-6 ? vp : 1e-6;
        vn_ = vn > 1e-6 ? vn : 1e-6;
        reset();
    }
    double vp() const { return vp_; }
    double vn() const { return vn_; }

    void reset() {
        u1_ = 0.0;
        f1u1_ = antiderivative(0.0);
    }

    // The memoryless transfer curve f(u). Static form lets a test build a naive
    // (aliasing) reference from the identical curve.
    static double f(double u, double vp, double vn) {
        const double vc = (u >= 0.0) ? vp : vn;
        return vc * std::tanh(u / vc);
    }
    double f(double u) const { return f(u, vp_, vn_); }

    // Naive (no antialiasing) memoryless evaluation. Aliases at high drive.
    inline float processSampleNaive(float u) {
        return static_cast<float>(f(static_cast<double>(u)));
    }

    // First-order ADAA evaluation. Reduces harmonics that would fold back.
    inline float processSampleADAA(float uin) {
        const double u = static_cast<double>(uin);
        const double f1u = antiderivative(u);
        const double du = u - u1_;
        double y;
        if (std::fabs(du) > kEps) {
            y = (f1u - f1u1_) / du;
        } else {
            y = f(0.5 * (u + u1_));
        }
        u1_ = u;
        f1u1_ = f1u;
        return static_cast<float>(y);
    }

private:
    static constexpr double kEps = 1e-6;
    static constexpr double kLn2 = 0.6931471805599453;

    // F(u) = Vc^2 * ln(cosh(u/Vc)), Vc chosen by sign; stable for large |u|:
    //   ln(cosh(z)) = |z| - ln2 + log1p(exp(-2|z|)).
    double antiderivative(double u) const {
        const double vc = (u >= 0.0) ? vp_ : vn_;
        const double z = u / vc;
        const double az = std::fabs(z);
        const double lncosh = az - kLn2 + std::log1p(std::exp(-2.0 * az));
        return vc * vc * lncosh;
    }

    double vp_ = kDefaultVp;
    double vn_ = kDefaultVn;
    double u1_ = 0.0;
    double f1u1_ = 0.0;  // = antiderivative(u1_)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ASYM_SOFT_CLIPPER_H
