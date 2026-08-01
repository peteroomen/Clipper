// Clipper — portable DSP core.
//
// OverdriveToneStack (docs §65): the Tube-Screamer-family TONE stage, as the
// netlist. This replaces §11.6/§21's documented approximation — "a first-order
// treble TILT about a ~1 kHz pivot, +/-12 dB, TRANSPARENT at noon" — which had
// no low-pass in it at all and therefore turned the family's mid HUMP into a
// high SHELF (measured: at TONE noon the old model rose monotonically to a
// plateau that ran flat from 4 kHz to 12 kHz).
//
// THE TOPOLOGY (identical on the SD-1 and the TS; only the values differ):
//
//        clipper out --[ Rin ]--+--------------------.
//                               |                    |
//                             [Cin]              [ pot: Rin-side leg ]
//                               |                    |
//                              GND      (+)------ wiper --[Cw]--[Rw]-- GND
//                               |        |           |
//        bias rail --[Rbias]----+     op-amp      [ pot: fb-side leg ]
//                                        |           |
//                                       (-)----------+-----.
//                                        |                 |
//                                        `--[ Rfb || Cfb ]--+--- out
//
// The op-amp is IDEAL here (the 4558's own band-limit / slew are modelled once,
// in the clipping stage, where the gain that makes them matter lives).
//
// WHY THIS IS THE UPPER-MID BODY. `Rin`/`Cin` is a LOW-PASS on the op-amp's
// non-inverting input — 1/(2*pi*1k*220n) = 723.4 Hz on the TS, 1/(2*pi*10k*18n)
// = 884.2 Hz on the SD-1. The clipping stage's own Zg leg puts a HIGH-pass rise
// at 720.5 Hz (SdModel.cpp / TsModel.cpp). The two together are the family's
// band-pass: a hump peaked in the upper mids, not a shelf that keeps rising.
// `Rbias` is the second half of the input divider (it returns to the 4.5 V rail,
// which is an AC ground), so the stage's DC gain is Rbias/(Rin+Rbias) EXACTLY —
// 0.9091 (-0.83 dB) on the TS and 0.5000 (-6.02 dB) on the SD-1. That insertion
// loss is real and is NOT compensated anywhere (docs §65.5, the §36/ADR 008
// precedent).
//
// THE TRANSFER FUNCTION, derived from the netlist. With the op-amp ideal both
// inputs sit at the same V, so the two pot legs are a parallel pair from V to the
// wiper. Writing p for the fraction of the pot between the NON-inverting node and
// the wiper (p == the TONE knob: p = 0 dark, p = 1 bright), gin = 1/Rin,
// gb = 1/Rbias, Zw = Rw + 1/(s*Cw), Rp = Rpot*p*(1-p) and Zfb = Rfb/(1+s*Rfb*Cfb):
//
//     H(s) = gin * [ 1 + p*Zfb/(Zw+Rp) ]  /  [ gin + gb + s*Cin + (1-p)/(Zw+Rp) ]
//
// Clearing the two first-order factors A = Rfb*Cfb and B = Cw*(Rw+Rp) gives
//
//     N(s) = gin * [ A*B s^2 + (A + B + p*Rfb*Cw) s + 1 ]
//     D(s) = (1 + s*A) * [ Cin*B s^2 + ((gin+gb)*B + Cin + (1-p)*Cw) s + (gin+gb) ]
//
// so the denominator ALWAYS carries the exact real pole 1/(2*pi*Rfb*Cfb) and the
// rest is one quadratic per side — no cubic solve is ever needed. With Cfb = 0
// (the TS has no cap across Rfb) A = 0 and both sides drop an order, which is
// exactly Yeh & Abel's published second-order TS tone-stage transfer function:
// this H(s) reproduces their wp, wz, wp*wz, alpha and alpha*W*wz TERM FOR TERM
// (docs §65.2 — and it found a transcription bug in the reference implementation
// this was checked against).
//
// EVERY ROOT IS REAL, across the whole knob and on both pedals (measured: the
// worst normalized discriminant is +0.0339, the TS denominator at the bright
// end), so the discrete form is a CASCADE OF FIRST-ORDER SECTIONS — the structure
// docs §56.4b names as the cure for direct forms of order >= 2 — in `double`.
//
// DISCRETIZATION. Matched-Z pole/zero mapping (each corner lands on exactly the
// right frequency, which is what "where the hump is" depends on), the cascade
// gain normalized so H(0) is exact, plus ONE extra real zero chosen so |H| at
// Nyquist is exact too. That last term is what makes the top octave usable at
// 44.1 kHz: without it the mapping over-shoots by 3-5 dB at 20 kHz. Measured
// worst |H_discrete - H_analytic| over 20 Hz..20 kHz x the whole knob:
// TS 0.96 dB / SD-1 1.21 dB at 44.1 kHz, 0.45 / 0.49 dB at 96 kHz, and under
// 0.5 dB everywhere below 8 kHz (a plain bilinear of the same network measures
// 13.6 / 19.4 dB). Residual named in docs §65.9.
//
// Platform-free (C++17, only <cmath>). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_OVERDRIVE_TONE_STACK_H
#define CLIPPER_DSP_OVERDRIVE_TONE_STACK_H

#include <cmath>

#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

// The netlist values of one pedal's tone stage. Designators follow the schematic
// role, not one pedal's part numbers (the SD-1 and TS number them differently).
struct OverdriveToneConfig {
    double rIn;    // SD-1 R5 10k   / TS R5 1k     — clipper out -> op-amp (+)
    double cIn;    // SD-1 C4 18n   / TS C4 220n   — (+) to ground: the LOW-PASS
    double rBias;  // SD-1 R11 10k  / TS R11 10k   — (+) to the 4.5 V rail (AC gnd)
    double rPot;   // the TONE pot
    double rW;     // SD-1 R7 470   / TS R7 220    — wiper leg to ground
    double cW;     // SD-1 C5 27n   / TS C5 220n
    double rFb;    // SD-1 R8 10k   / TS R8 1k     — feedback around the op-amp
    double cFb;    // SD-1 C3 10n   / TS: ABSENT (0)
};

class OverdriveToneStack {
public:
    void prepare(const OverdriveToneConfig& cfg, double sampleRate) {
        cfg_ = cfg;
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
        setPot(pot_);
    }

    // Clear every recursive state. Keeps the coefficients (i.e. the prepared rate
    // and knob position): a state flush, not a re-prepare. Allocation-free.
    void reset() {
        for (int i = 0; i < kSections; ++i) {
            x1_[i] = 0.0;
            y1_[i] = 0.0;
        }
        fx1_ = 0.0;
    }

    // p in [0,1]: the fraction of the pot between the op-amp's NON-inverting node
    // and the wiper. p == the TONE knob (0 dark, 1 bright). Rebuilds the cascade;
    // allocation-free, ~5 std::exp calls, and only called while the knob smoother
    // is still moving.
    void setPot(double p) {
        pot_ = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
        rebuild();
    }
    double pot() const { return pot_; }

    // Anti-denormal (ADR 006): EVERY state here rests at exactly zero on silence
    // (a linear filter with no bias anywhere), so every one is in scope and every
    // one is flushed. This is a CASCADE OF FIRST-ORDER SECTIONS, which is why the
    // per-tap guard is sufficient and correct here: within a section y1 decays by
    // |a1| < 1 each sample and x1 is a copy of an already-flushed value, so the
    // recursion cannot pump itself back over the floor. That is exactly the
    // condition docs §56.4b says a DIRECT FORM of order >= 2 violates — and is the
    // second reason (after the corner-frequency accuracy) this network is realized
    // as a cascade rather than a 3rd-order direct form.
    inline float processSample(float in) {
        double v = static_cast<double>(in);
        for (int i = 0; i < kSections; ++i) {
            const double y = b0_[i] * v + b1_[i] * x1_[i] + a1_[i] * y1_[i];
            x1_[i] = flushDenormal(v);
            y1_[i] = flushDenormal(y);
            v = y;
        }
        // The Nyquist-matching FIR zero, normalized to unity at DC.
        const double y = fb0_ * v + fb1_ * fx1_;
        fx1_ = flushDenormal(v);
        return static_cast<float>(y);
    }

    // Largest |state| — the tests assert this is EXACTLY 0.0 after a silent tail
    // (Denormal.h's scope rule: a zero-resting state must be guarded).
    double maxAbsRestingState() const {
        double m = std::fabs(fx1_);
        for (int i = 0; i < kSections; ++i) {
            m = std::fmax(m, std::fabs(x1_[i]));
            m = std::fmax(m, std::fabs(y1_[i]));
        }
        return m;
    }

    // The analytic H(s) of the network at angular frequency w, from the netlist —
    // the reference the discretization is measured against (and the thing the
    // tests compare Yeh & Abel's published formula to). Returns |H| and the
    // real/imaginary parts through the out params.
    static void analytic(const OverdriveToneConfig& c, double p, double w, double* re,
                         double* im) {
        double n[3], d[4];
        polynomials(c, p, n, d);
        // N(jw) = n2*(jw)^2 + n1*(jw) + n0 ; same shape for D with a cubic term.
        const double w2 = w * w, w3 = w2 * w;
        const double nr = n[0] - n[2] * w2, ni = n[1] * w;
        const double dr = d[0] - d[2] * w2, di = d[1] * w - d[3] * w3;
        const double den = dr * dr + di * di;
        *re = (nr * dr + ni * di) / den;
        *im = (ni * dr - nr * di) / den;
    }
    static double analyticMag(const OverdriveToneConfig& c, double p, double w) {
        double re, im;
        analytic(c, p, w, &re, &im);
        return std::sqrt(re * re + im * im);
    }

    // N(s) = n0 + n1 s + n2 s^2 ; D(s) = d0 + d1 s + d2 s^2 + d3 s^3, straight
    // from the netlist (see the header comment for the derivation).
    static void polynomials(const OverdriveToneConfig& c, double p, double n[3],
                            double d[4]) {
        const double gin = 1.0 / c.rIn, gb = 1.0 / c.rBias;
        const double A = c.rFb * c.cFb;                        // the Cfb pole, 0 on the TS
        const double B = c.cW * (c.rW + c.rPot * p * (1.0 - p));
        n[0] = gin;
        n[1] = gin * (A + B + p * c.rFb * c.cW);
        n[2] = gin * A * B;
        d[0] = gin + gb;
        d[1] = (gin + gb) * (A + B) + c.cIn + (1.0 - p) * c.cW;
        d[2] = (gin + gb) * A * B + c.cIn * (A + B) + (1.0 - p) * c.cW * A;
        d[3] = c.cIn * A * B;
    }

private:
    // 2 zeros + 3 poles is the worst case (the SD-1); the TS uses 1 + 2 and the
    // spare section is an exact identity.
    static constexpr int kSections = 3;

    // Real roots of q0 + q1 s + q2 s^2 as POSITIVE corner frequencies in rad/s
    // (both roots of every quadratic in this network are real and negative —
    // asserted by the tests, worst normalized discriminant +0.0339). Returns the
    // count; a degenerate quadratic (q2 == 0) yields the single linear root.
    static int corners(double q0, double q1, double q2, double out[2]) {
        if (q2 <= 0.0) {
            if (q1 == 0.0) return 0;
            out[0] = q0 / q1;  // root at s = -q0/q1 -> corner q0/q1
            return 1;
        }
        double disc = q1 * q1 - 4.0 * q2 * q0;
        if (disc < 0.0) disc = 0.0;  // defensive: never taken with the shipped values
        const double sq = std::sqrt(disc);
        out[0] = (q1 - sq) / (2.0 * q2);
        out[1] = (q1 + sq) / (2.0 * q2);
        return 2;
    }

    void rebuild() {
        double n[3], d[4];
        polynomials(cfg_, pot_, n, d);

        // Zeros: the numerator quadratic (linear when Cfb == 0, i.e. on the TS).
        double zc[2], pc[3];
        const int nz = corners(n[0], n[1], n[2], zc);

        // Poles: D factors EXACTLY as (1 + s*A) * quadratic, so the extra pole is
        // read off rather than found — no cubic solve is ever needed.
        //   D(s) = (1 + s*A) * [ Cin*B s^2 + ((gin+gb)*B + Cin + (1-p)*Cw) s + gin+gb ]
        const double A = cfg_.rFb * cfg_.cFb;
        const double B = cfg_.cW * (cfg_.rW + cfg_.rPot * pot_ * (1.0 - pot_));
        const double gin = 1.0 / cfg_.rIn, gb = 1.0 / cfg_.rBias;
        int np = corners(gin + gb, (gin + gb) * B + cfg_.cIn + (1.0 - pot_) * cfg_.cW,
                         cfg_.cIn * B, pc);
        if (A > 0.0 && np < kSections) pc[np++] = 1.0 / A;

        sortAsc(zc, nz);
        sortAsc(pc, np);

        const double fs = sampleRate_;
        const double dcGain = n[0] / d[0];  // = rBias/(rIn+rBias), exactly

        // Matched-Z: every corner lands on exactly its analog frequency, and each
        // section is normalized to unity at DC so the cascade's DC gain is exactly
        // the divider above.
        for (int i = 0; i < kSections; ++i) {
            b0_[i] = 1.0;
            b1_[i] = 0.0;
            a1_[i] = 0.0;
        }
        for (int i = 0; i < np && i < kSections; ++i) {
            const double rp = std::exp(-pc[i] / fs);
            if (i < nz) {
                const double rz = std::exp(-zc[i] / fs);
                const double g = (1.0 - rp) / (1.0 - rz);
                b0_[i] = g;
                b1_[i] = -g * rz;
            } else {
                b0_[i] = 1.0 - rp;
                b1_[i] = 0.0;
            }
            a1_[i] = rp;
        }

        // One real zero so |H| at Nyquist is exact as well as at DC. Solve
        // (1-alpha)/(1+alpha) = |H_analytic(Nyquist)| / |H_cascade(Nyquist)|; at
        // z^-1 = -1 every section's response is real, so this costs a handful of
        // flops. Without it the mapping over-shoots by 3.2 dB (TS) / 5.1 dB (SD-1)
        // at 20 kHz / 44.1 kHz; with it the worst error is 0.96 / 1.21 dB.
        const double wN = kPi * fs;
        double cascadeNyq = dcGain;
        for (int i = 0; i < kSections; ++i) {
            cascadeNyq *= (b0_[i] - b1_[i]) / (1.0 + a1_[i]);
        }
        double alpha = 0.0;
        const double magC = std::fabs(cascadeNyq);
        if (magC > 1e-300) {
            const double r = analyticMag(cfg_, pot_, wN) / magC;
            alpha = (1.0 - r) / (1.0 + r);
            if (alpha > 0.9) alpha = 0.9;  // defensive; measured |alpha| <= 0.345
            if (alpha < -0.9) alpha = -0.9;
        }
        fb0_ = dcGain / (1.0 + alpha);
        fb1_ = dcGain * alpha / (1.0 + alpha);
    }

    static void sortAsc(double* v, int n) {
        for (int i = 1; i < n; ++i) {
            const double k = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > k) {
                v[j + 1] = v[j];
                --j;
            }
            v[j + 1] = k;
        }
    }

    static constexpr double kPi = 3.141592653589793;

    OverdriveToneConfig cfg_{1e3, 220e-9, 10e3, 20e3, 220.0, 220e-9, 1e3, 0.0};
    double sampleRate_ = 44100.0;
    double pot_ = 0.5;

    double b0_[kSections] = {1.0, 1.0, 1.0};
    double b1_[kSections] = {0.0, 0.0, 0.0};
    double a1_[kSections] = {0.0, 0.0, 0.0};
    double x1_[kSections] = {0.0, 0.0, 0.0};
    double y1_[kSections] = {0.0, 0.0, 0.0};
    double fb0_ = 1.0, fb1_ = 0.0, fx1_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OVERDRIVE_TONE_STACK_H
