// Clipper — measurement infrastructure (M2). NOT part of the DSP core or the
// WASM build; shared by the native tests (core/tests) and the render CLI
// (core/tools/render) so both compute the aliasing metric identically.
//
// Aliasing metric: drive a memoryless/near-memoryless clipper with a single high
// sine f0. Symmetric clipping generates odd harmonics n*f0. Harmonics above
// Nyquist fold back to |n*f0 - k*fs| — inharmonic frequencies that, at a
// well-chosen f0 (e.g. 4186 Hz @ 44.1 kHz), do NOT coincide with any true
// (below-Nyquist) harmonic. We Goertzel-measure the fundamental and each folded
// alias bin over the signal tail (Hann-windowed) and report:
//   * worstAliasDb : loudest alias, in dB relative to the fundamental
//   * sumAliasDb    : total alias energy, in dB relative to the fundamental
// Lower (more negative) is better; oversampling must push these down.

#ifndef CLIPPER_MEASURE_ALIAS_METRIC_H
#define CLIPPER_MEASURE_ALIAS_METRIC_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace clipper::measure {

// Hann-windowed Goertzel amplitude estimate at frequency f over x[start, start+n).
inline double goertzelAmp(const std::vector<float>& x, std::size_t start,
                          std::size_t n, double f, double fs) {
    const double kTwoPi = 6.283185307179586;
    const double w = kTwoPi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0, winSum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 * (1.0 - std::cos(kTwoPi * i / (n - 1)));
        winSum += win;
        const double s0 = x[start + i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return winSum > 0.0 ? 2.0 * std::sqrt(re * re + im * im) / winSum : 0.0;
}

// Fold a frequency above Nyquist back into [0, fs/2].
inline double foldFreq(double f, double fs) {
    const double k = std::round(f / fs);
    return std::fabs(f - k * fs);
}

struct AliasReport {
    double fundAmp = 0.0;       // fundamental amplitude
    double worstAliasDb = -300; // loudest alias, dB rel. fundamental
    double sumAliasDb = -300;   // summed alias energy, dB rel. fundamental
    int numBins = 0;            // alias bins measured
};

// Measure aliasing of a rendered tone. Uses the last (1 - tailFrac) of the
// buffer to skip start-up transients. Considers odd harmonics 3..hMax; those
// above Nyquist are folded and measured as aliases.
inline AliasReport measureAliasing(const std::vector<float>& x, double fs,
                                   double f0, int hMax = 29,
                                   double tailFrac = 0.4) {
    AliasReport r;
    if (x.empty()) return r;
    const std::size_t total = x.size();
    const std::size_t start =
        static_cast<std::size_t>(static_cast<double>(total) * tailFrac);
    const std::size_t n = total - start;
    if (n < 16) return r;

    r.fundAmp = goertzelAmp(x, start, n, f0, fs);
    const double fundRef = r.fundAmp > 1e-12 ? r.fundAmp : 1e-12;
    const double nyq = fs * 0.5;

    double worst = 0.0, sumE = 0.0;
    for (int h = 3; h <= hMax; h += 2) {
        const double hf = h * f0;
        if (hf < nyq) continue;  // true harmonic, not an alias
        const double af = foldFreq(hf, fs);
        if (af < 50.0 || af > nyq - 50.0) continue;  // avoid DC/Nyquist edges
        const double a = goertzelAmp(x, start, n, af, fs);
        worst = std::max(worst, a);
        sumE += a * a;
        ++r.numBins;
    }
    r.worstAliasDb = 20.0 * std::log10(worst / fundRef + 1e-15);
    r.sumAliasDb = 10.0 * std::log10(sumE / (fundRef * fundRef) + 1e-30);
    return r;
}

}  // namespace clipper::measure

#endif  // CLIPPER_MEASURE_ALIAS_METRIC_H
