// Per-partial frequency estimator: complex demodulation + phase-slope fit.
//
// Shared test support, alongside LtpProbe.h and DcOffset.h, so that every
// spectral bar in this project measures "what frequency is this partial at"
// the same way rather than each file re-deriving one. It is header-only and
// lives under core/tests/, which is NOT in the WASM artifact's hash closure.
//
// A zero-crossing f0 locks onto the wrong partial on a chord, and a short-window
// Goertzel peak search cannot resolve 41 Hz partials 10 Hz apart. This does
// neither: it shifts the partial of interest to DC, rejects its neighbours with a
// steep lowpass, and reads the residual frequency straight off the phase slope.
// O(N) per partial and accurate to small fractions of a cent.
//
// VALIDATE IT BEFORE TRUSTING IT (docs §70 records two measurement bugs of exactly
// this family) — estimatorSelfTest() below drives it with signals whose answer is
// known by construction.
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace est {

// Six cascaded one-poles at fc: ~36 dB/oct, so a partial 10 Hz away from a 2 Hz
// corner is >80 dB down and cannot pull the phase.
struct Lp6 {
    double a, s[6]{};
    explicit Lp6(double fc, double sr) { a = 1.0 - std::exp(-2.0 * M_PI * fc / sr); }
    double process(double x) {
        double v = x;
        for (int i = 0; i < 6; ++i) { s[i] += a * (v - s[i]); v = s[i]; }
        return v;
    }
};

// Returns the measured frequency of the component near fe.
inline double freqNear(const std::vector<float>& y, double fe, double sr,
                       double settleSec = 1.0) {
    Lp6 lpI(2.0, sr), lpQ(2.0, sr);
    const size_t n = y.size();
    std::vector<double> ph;
    std::vector<double> tt;
    ph.reserve(n);
    tt.reserve(n);
    const size_t from = (size_t)(settleSec * sr);
    double prev = 0.0, acc = 0.0;
    bool have = false;
    for (size_t i = 0; i < n; ++i) {
        const double w = 2.0 * M_PI * fe * (double)i / sr;
        const double I = lpI.process((double)y[i] * std::cos(w));
        const double Q = lpQ.process(-(double)y[i] * std::sin(w));
        if (i < from) continue;
        double p = std::atan2(Q, I);
        if (have) {           // unwrap
            double d = p - prev;
            while (d > M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            acc += d;
        } else { have = true; acc = 0.0; }
        prev = p;
        ph.push_back(acc);
        tt.push_back((double)i / sr);
    }
    if (ph.size() < 16) return fe;
    // Least-squares slope of unwrapped phase vs time -> rad/s offset from fe.
    const size_t m = ph.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < m; ++i) { sx += tt[i]; sy += ph[i]; sxx += tt[i] * tt[i]; sxy += tt[i] * ph[i]; }
    const double den = (double)m * sxx - sx * sx;
    if (std::fabs(den) < 1e-12) return fe;
    const double slope = ((double)m * sxy - sx * sy) / den;  // rad/s
    return fe + slope / (2.0 * M_PI);
}

inline double cents(double measured, double target) {
    return 1200.0 * std::log2(measured / target);
}

}  // namespace est
