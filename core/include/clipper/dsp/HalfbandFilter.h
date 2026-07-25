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
//
// DOUBLED RING BUFFER (docs §32, 2026-07-24 audit perf item 2). Both classes hold
// their delay line in a buffer of TWICE the logical length and write every incoming
// sample to both halves, maintaining the invariant
//
//     ring_[i] == ring_[i + N]   for all i in [0, N)     (N = M_ or L_)
//
// so a wrapped read `ring_[(w - p + N) % N]` can be written as the unwrapped
// `ring_[w + N - p]`, which is in range for every tap. This exists purely to delete
// an integer DIVISION per tap: the lengths are runtime members, so `% M_` compiles
// to a hardware `div` — 64 of them per output sample on the interpolator's tight
// stage and 65 on the decimator's, at the OVERSAMPLED rate, for every oversampled
// pedal and (once per triode stage) every valve amp. This is the hottest loop in the
// project; the audit measured 3.3x on the interpolator alone.
//
// Two properties are load-bearing and must survive any future edit here:
//
//   * The doubling is only correct while EVERY write updates both copies, including
//     in reset() (a half-cleared ring would keep re-injecting stale history for a
//     whole filter length). It costs one extra store per sample — a rounding error
//     next to a 64-tap FIR — and no extra arithmetic in the tap loop.
//   * The tap loop still walks p = 0, 1, 2 ... upward (i.e. reads memory BACKWARDS
//     from ring_[w + N]). Float addition is not associative, so reversing the loop
//     to stream forwards — or reordering the coefficients to match — changes the
//     accumulation order and is NOT bit-identical. This change is bit-identical by
//     construction and proven so, sample-for-sample, by clipper_halfband_tests.
//     Do not "tidy" the direction, and do not vectorise the reduction here.

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
        // Doubled ring: 2*M floats, every write mirrored M apart (see header note).
        ring_.assign(static_cast<size_t>(2 * M_), 0.0f);
        w_ = 0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);  // both halves
        w_ = 0;
    }

    // Push one input, emit two outputs (o0 then o1) at the doubled rate.
    inline void processSample(float in, float& o0, float& o1) {
        const int M = M_;
        // Mirror the write M apart so every tap read below is in range unwrapped.
        ring_[static_cast<size_t>(w_)] = in;
        ring_[static_cast<size_t>(w_ + M)] = in;
        // Even output phase = delayed input via the centre tap (unity: 2 * 0.5).
        o0 = ring_[static_cast<size_t>(w_ + M - halfM_)];
        // Odd output phase = odd polyphase FIR on the input history. Reads run
        // backwards from ring_[w_ + M]; p ascends so the summation order (and hence
        // the exact float result) is identical to the modulo-indexed version.
        const float* r = &ring_[static_cast<size_t>(w_ + M)];
        float acc = 0.0f;
        for (int p = 0; p < M; ++p) acc += e1_[static_cast<size_t>(p)] * r[-p];
        o1 = acc;
        if (++w_ == M) w_ = 0;
    }

private:
    int M_ = 0, halfM_ = 0, w_ = 0;
    std::vector<float> e1_;
    std::vector<float> ring_;  // length 2*M_; ring_[i] == ring_[i + M_]
};

// 2x decimator (sparse FIR at the high rate, emit one output per two inputs).
// DC gain 1. Feed both high-rate samples of a pair; processSample returns true on
// the second, delivering the decimated output.
class HalfbandDecimator2x {
public:
    void setup(const std::vector<double>& h) {
        L_ = static_cast<int>(h.size());
        // Parallel arrays rather than a vector<{int,float}>: the sparse tap list is
        // walked in full for every output, and a struct of a 4-byte int + a 4-byte
        // float strides the coefficient stream by 8 bytes. Order is preserved (k
        // ascending) because it fixes the summation order — see the header note.
        tapDelay_.clear();
        tapCoeff_.clear();
        for (int k = 0; k < L_; ++k) {
            if (std::fabs(h[static_cast<size_t>(k)]) > 1e-12) {
                tapDelay_.push_back(k);
                tapCoeff_.push_back(static_cast<float>(h[static_cast<size_t>(k)]));
            }
        }
        // Doubled ring: 2*L floats, every write mirrored L apart (see header note).
        ring_.assign(static_cast<size_t>(2 * L_), 0.0f);
        w_ = 0;
        phase_ = 0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);  // both halves
        w_ = 0;
        phase_ = 0;
    }

    // Push one high-rate sample. Returns true (and writes `out`) once per pair.
    inline bool processSample(float in, float& out) {
        const int L = L_;
        const int wCur = w_;
        // Mirror the write L apart so every tap read below is in range unwrapped.
        ring_[static_cast<size_t>(w_)] = in;
        ring_[static_cast<size_t>(w_ + L)] = in;
        if (++w_ == L) w_ = 0;
        phase_ ^= 1;
        if (phase_ != 0) return false;  // wait for the pair to complete
        // Reads run backwards from ring_[wCur + L] by each tap's delay; the tap
        // order is unchanged, so acc accumulates in the same order as before.
        const float* r = &ring_[static_cast<size_t>(wCur + L)];
        const int n = static_cast<int>(tapDelay_.size());
        float acc = 0.0f;
        for (int j = 0; j < n; ++j)
            acc += tapCoeff_[static_cast<size_t>(j)] * r[-tapDelay_[static_cast<size_t>(j)]];
        out = acc;
        return true;
    }

private:
    int L_ = 0, w_ = 0, phase_ = 0;
    std::vector<int> tapDelay_;    // non-zero tap positions, ascending
    std::vector<float> tapCoeff_;  // matching coefficients
    std::vector<float> ring_;      // length 2*L_; ring_[i] == ring_[i + L_]
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_HALFBAND_FILTER_H
