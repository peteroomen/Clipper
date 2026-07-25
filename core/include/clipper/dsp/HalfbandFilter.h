// Clipper — portable DSP core.
//
// Polyphase halfband FIR building blocks for 2x oversampling (M2). A halfband
// lowpass has its cutoff at exactly Fs/4 (pi/2), a transition band symmetric
// about Fs/4, and — crucially — every even-indexed tap is zero except the
// centre tap (= 0.5). That structure makes 2x up/down-sampling cheap:
//
//   * Interpolator (zero-stuff + LP): the even output phase is just a delayed
//     copy of the input (the centre tap), the odd phase is a short FIR on the
//     input stream (the odd polyphase branch). No zeros are ever multiplied.
//   * Decimator (LP + decimate): a sparse FIR at the high rate, emitting one
//     output per two inputs, skipping the (zero) even taps.
//
// Coefficients are Kaiser-windowed sinc, generated at prepare-time from a length
// parameter and a beta chosen for the desired stopband. See docs/DEVELOPMENT.md
// (M2) for the chosen filter lengths, measured stopband/ripple, and latency.
//
// Zero-dependency (only <cmath>, <vector>) and platform-free: compiles natively
// and under Emscripten. All heap use is in prepare()/setup — never in the
// per-sample process methods.

#ifndef CLIPPER_DSP_HALFBAND_FILTER_H
#define CLIPPER_DSP_HALFBAND_FILTER_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace clipper::dsp {

namespace halfband_detail {

// Zeroth-order modified Bessel function of the first kind (for the Kaiser window).
inline double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 80; ++k) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

}  // namespace halfband_detail

// Build a length L = 2M+1 (M even) Kaiser-windowed halfband lowpass, cutoff pi/2,
// normalised to unity DC gain. Even-indexed taps are analytically zero except the
// centre (0.5); we rely on that in the interpolator/decimator.
inline std::vector<double> makeHalfband(int M, double beta) {
    const double kPi = 3.14159265358979323846;
    const int L = 2 * M + 1;
    std::vector<double> h(static_cast<size_t>(L));
    const double i0b = halfband_detail::besselI0(beta);
    for (int k = -M; k <= M; ++k) {
        const double ideal = (k == 0) ? 0.5 : std::sin(kPi * k / 2.0) / (kPi * k);
        const double r = static_cast<double>(k) / M;
        const double w =
            halfband_detail::besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[static_cast<size_t>(k + M)] = ideal * w;
    }
    double s = 0.0;
    for (double v : h) s += v;
    for (double& v : h) v /= s;  // normalise DC gain to 1
    return h;
}

// 2x interpolator (polyphase two-branch). One input sample -> two output samples.
// Output DC gain is 1 (the +6 dB that compensates zero-stuffing is baked in).
class HalfbandInterpolator2x {
public:
    // Configure from halfband taps (length 2M+1, M even, DC gain 1). Allocates.
    void setup(const std::vector<double>& h) {
        M_ = (static_cast<int>(h.size()) - 1) / 2;
        halfM_ = M_ / 2;
        e1_.resize(static_cast<size_t>(M_));
        for (int p = 0; p < M_; ++p)
            e1_[static_cast<size_t>(p)] =
                static_cast<float>(2.0 * h[static_cast<size_t>(2 * p + 1)]);  // x2 interp gain
        ring_.assign(static_cast<size_t>(M_), 0.0f);
        w_ = 0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        w_ = 0;
    }

    // Push one input, emit two outputs (o0 then o1) at the doubled rate.
    inline void processSample(float in, float& o0, float& o1) {
        const int M = M_;
        ring_[static_cast<size_t>(w_)] = in;
        // Even output phase = delayed input via the centre tap (unity: 2 * 0.5).
        o0 = ring_[static_cast<size_t>((w_ - halfM_ + M) % M)];
        // Odd output phase = odd polyphase FIR on the input history.
        float acc = 0.0f;
        for (int p = 0; p < M; ++p)
            acc += e1_[static_cast<size_t>(p)] * ring_[static_cast<size_t>((w_ - p + M) % M)];
        o1 = acc;
        w_ = (w_ + 1) % M;
    }

private:
    int M_ = 0, halfM_ = 0, w_ = 0;
    std::vector<float> e1_;
    std::vector<float> ring_;
};

// 2x decimator (sparse FIR at the high rate, emit one output per two inputs).
// DC gain 1. Feed both high-rate samples of a pair; processSample returns true on
// the second, delivering the decimated output.
class HalfbandDecimator2x {
public:
    void setup(const std::vector<double>& h) {
        L_ = static_cast<int>(h.size());
        taps_.clear();
        for (int k = 0; k < L_; ++k)
            if (std::fabs(h[static_cast<size_t>(k)]) > 1e-12)
                taps_.push_back({k, static_cast<float>(h[static_cast<size_t>(k)])});
        ring_.assign(static_cast<size_t>(L_), 0.0f);
        w_ = 0;
        phase_ = 0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        w_ = 0;
        phase_ = 0;
    }

    // Push one high-rate sample. Returns true (and writes `out`) once per pair.
    inline bool processSample(float in, float& out) {
        const int L = L_;
        const int wCur = w_;
        ring_[static_cast<size_t>(w_)] = in;
        w_ = (w_ + 1) % L;
        phase_ ^= 1;
        if (phase_ != 0) return false;  // wait for the pair to complete
        float acc = 0.0f;
        for (const auto& t : taps_)
            acc += t.coeff * ring_[static_cast<size_t>((wCur - t.index + L) % L)];
        out = acc;
        return true;
    }

private:
    struct Tap {
        int index;
        float coeff;
    };
    int L_ = 0, w_ = 0, phase_ = 0;
    std::vector<Tap> taps_;
    std::vector<float> ring_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_HALFBAND_FILTER_H
