// Shared measurement battery for the drop pedal's two shifter implementations.
// Every number here is measured the same way for both, so the comparison is not
// against docs prose but against this machine, this harness.
#pragma once
#include "../../core/tests/support/PartialFreq.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace bench {
inline constexpr double kSr = 48000.0;
inline constexpr double kPi = 3.14159265358979323846;

// A shifter under test: render `n` samples of `in` at ratio r.
using RenderFn = std::function<std::vector<float>(const std::vector<float>&, double)>;

struct Shape { const char* name; std::vector<double> partials; };
inline const Shape kShapes[] = {
    {"single E2",      {82.41}},
    {"E5 power chord", {82.41, 123.47}},
    {"E major triad",  {82.41, 103.83, 123.47}},
    // A REAL triad is not three sines. Each note carries harmonics, which
    // multiply the number of partials a frequency-domain shifter has to resolve
    // and put many of them within a bin of each other (E's 3rd at 247.2 vs B's
    // 2nd at 246.9). Measured for BOTH shifters, because it is the stimulus most
    // likely to flatter the time-domain one and embarrass the other.
    {"rich triad",     {82.41, 103.83, 123.47, 164.82, 207.66, 246.94,
                        247.23, 311.49, 370.41, 329.64, 415.32, 493.88}},
};

inline std::vector<float> sustained(const std::vector<double>& partials, double sec,
                                    double amp = 0.5) {
    const size_t N = (size_t)(kSr * sec);
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        double v = 0;
        for (double f : partials) v += std::sin(2.0 * kPi * f * (double)i / kSr);
        x[i] = (float)(amp * v / (double)partials.size());
    }
    return x;
}

// A plucked low E: fast attack, decaying body, six harmonics. The transient probe.
inline std::vector<float> pluck(double f0, double sec, double amp = 0.6) {
    const size_t N = (size_t)(kSr * sec);
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) {
        const double t = (double)i / kSr;
        const double env = (1.0 - std::exp(-t * 400.0)) * std::exp(-t * 3.0);
        double v = 0;
        for (int h = 1; h <= 6; ++h) v += std::sin(2.0 * kPi * f0 * h * t) / (h * h);
        x[i] = (float)(amp * env * v);
    }
    return x;
}

// --- bar 1/2/3: pitch accuracy -------------------------------------------------
struct PitchResult { double worstAbs, spread; };
inline PitchResult pitchOf(const RenderFn& render, const Shape& s, int semis) {
    const double r = std::pow(2.0, -(double)semis / 12.0);
    const std::vector<float> y = render(sustained(s.partials, 4.0), r);
    double lo = 1e9, hi = -1e9, worst = 0;
    for (double f0 : s.partials) {
        const double target = f0 * r;
        const double c = est::cents(est::freqNear(y, target, kSr, 1.5), target);
        lo = std::min(lo, c); hi = std::max(hi, c);
        worst = std::max(worst, std::fabs(c));
    }
    return {worst, hi - lo};
}

// --- bar 4: non-harmonic artifact floor ---------------------------------------
// Rectangular Goertzel on an integer number of periods — §70's own correction:
// a Hann window read for AMPLITUDE without correcting its 0.5 coherent gain
// under-counts harmonic power 4x and fakes the floor.
inline double goertzelPow(const std::vector<float>& x, size_t from, size_t n, double f) {
    const double w = 2.0 * kPi * f / kSr, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) { const double s = x[from + i] + c * s1 - s2; s2 = s1; s1 = s; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
inline double artifactFloorDb(const RenderFn& render, int semis) {
    const double f0 = 82.41, r = std::pow(2.0, -(double)semis / 12.0);
    const std::vector<float> y = render(sustained({f0}, 4.0), r);
    const size_t from = y.size() / 2, n = y.size() - from;
    const double shifted = f0 * r;
    double harm = 0, worstNon = 0;
    for (int h = 1; h * shifted < kSr * 0.45; ++h) harm += goertzelPow(y, from, n, h * shifted);
    for (double f = 30.0; f < kSr * 0.45; f += 11.0) {
        bool isHarm = false;
        for (int h = 1; h * shifted < kSr * 0.45; ++h)
            if (std::fabs(f - h * shifted) < 8.0) { isHarm = true; break; }
        if (isHarm) continue;
        worstNon = std::max(worstNon, goertzelPow(y, from, n, f));
    }
    return 10.0 * std::log10((worstNon + 1e-30) / (harm + 1e-30));
}

// --- bar 5: TRANSIENT RESPONSE (new) ------------------------------------------
// Attack shape of a plucked low E. rise = ms from 10% to 90% of the peak;
// peak = the peak itself, normalized by the input's, so a shifter that smears the
// attack shows a LONGER rise and a LOWER peak.
struct Transient { double riseMs, peakRatio; };
inline Transient transientOf(const RenderFn& render, int semis) {
    const double r = std::pow(2.0, -(double)semis / 12.0);
    const std::vector<float> x = pluck(82.41, 2.0);
    const std::vector<float> y = render(x, r);
    auto envelope = [](const std::vector<float>& v) {
        std::vector<double> e(v.size());
        double s = 0;
        for (size_t i = 0; i < v.size(); ++i) {  // 3 ms one-pole on |v|
            const double a = 1.0 - std::exp(-1.0 / (0.003 * kSr));
            s += a * (std::fabs((double)v[i]) - s);
            e[i] = s;
        }
        return e;
    };
    const std::vector<double> ey = envelope(y), ex = envelope(x);
    const double pk = *std::max_element(ey.begin(), ey.end());
    const double pkIn = *std::max_element(ex.begin(), ex.end());
    size_t i10 = 0, i90 = 0;
    for (size_t i = 0; i < ey.size(); ++i) { if (ey[i] >= 0.10 * pk) { i10 = i; break; } }
    for (size_t i = i10; i < ey.size(); ++i) { if (ey[i] >= 0.90 * pk) { i90 = i; break; } }
    return {(double)(i90 - i10) / kSr * 1000.0, pk / (pkIn + 1e-12)};
}
}  // namespace bench
