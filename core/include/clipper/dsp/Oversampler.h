// Clipper — portable DSP core.
//
// Oversampler (M2): a cascade of 2x polyphase halfband stages composed to reach
// a whole-power oversampling factor of 1x / 2x / 4x / 8x. Only the nonlinear
// (WDF diode) stage of the RAT model runs oversampled; the linear stages stay at
// the base rate. Usage per block:
//
//     os.upsample(in, n);           // fills buffer(), length n*factor()
//     float* w = os.buffer();
//     for (i in 0..n*factor()) w[i] = nonlinear(w[i]);   // in place
//     os.downsample(out, n);        // buffer() -> out (length n)
//
// Filter design (see docs/DEVELOPMENT.md, M2): the FIRST 2x stage (lowest rate)
// has the tightest transition — at a 44.1 kHz base the 20 kHz audio edge sits at
// 0.2268 of the 88.2 kHz stage rate, just below the pi/2 (0.25) halfband cutoff —
// so it uses a long halfband (L = kTightM*2+1). Every later stage runs at >=
// 176.4 kHz where 20 kHz is a small fraction of the rate, so a short halfband
// (L = kRelaxedM*2+1) already clears the stopband target with margin.
//
// factor() == 1 is an exact pass-through (no filtering, no delay): it reproduces
// the pre-M2 (M1) signal path bit-for-bit, which the tests use as a regression
// guard.
//
// All allocation happens in prepare()/setFactor(); upsample()/downsample() never
// allocate (they assert n <= maxBlockSize).

#ifndef CLIPPER_DSP_OVERSAMPLER_H
#define CLIPPER_DSP_OVERSAMPLER_H

#include <cassert>
#include <cstddef>
#include <vector>

#include "clipper/dsp/HalfbandFilter.h"

namespace clipper::dsp {

class Oversampler {
public:
    static constexpr int kMaxFactor = 8;   // 3 stages
    static constexpr int kMaxStages = 3;   // log2(kMaxFactor)

    // Halfband lengths (M even -> L = 2M+1). Values chosen from measured stopband
    // / passband ripple (docs/DEVELOPMENT.md, M2). Kaiser beta ~ 7.86 => ~80+ dB.
    static constexpr int kTightM = 64;     // L = 129, first (lowest-rate) stage
    static constexpr int kRelaxedM = 16;   // L =  33, all later stages
    static constexpr double kBeta = 7.857;

    // Allocate scratch + all stage filters for the worst case (kMaxFactor). Must
    // be called before setFactor()/upsample()/downsample(). Resets state.
    void prepare(int maxBlockSize) {
        maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 1;
        const std::vector<double> tight = makeHalfband(kTightM, kBeta);
        const std::vector<double> relaxed = makeHalfband(kRelaxedM, kBeta);
        for (int s = 0; s < kMaxStages; ++s) {
            const std::vector<double>& h = (s == 0) ? tight : relaxed;
            ups_[s].setup(h);
            downs_[s].setup(h);
        }
        const size_t cap = static_cast<size_t>(maxBlockSize_) * kMaxFactor;
        bufA_.assign(cap, 0.0f);
        bufB_.assign(cap, 0.0f);
        osData_ = nullptr;
        osLen_ = 0;
        setFactor(factor_);
    }

    // Select oversampling factor (1, 2, 4, or 8). Resets all filter state (so a
    // change takes effect immediately). No allocation.
    void setFactor(int factor) {
        int f = 1;
        while (f * 2 <= factor && f * 2 <= kMaxFactor) f *= 2;
        // Snap to a valid power of two in [1, kMaxFactor].
        factor_ = (factor <= 1) ? 1 : f;
        stages_ = 0;
        for (int t = factor_; t > 1; t >>= 1) ++stages_;
        for (int s = 0; s < kMaxStages; ++s) {
            ups_[s].reset();
            downs_[s].reset();
        }
    }

    int factor() const { return factor_; }

    // Clear every halfband stage's delay line without touching the factor or
    // re-designing the filters (setFactor() also resets, but re-derives the stage
    // count). Recovery seam (audit finding 1): the polyphase FIRs hold up to 129
    // taps of history, so a single NaN sample keeps reappearing at the output for
    // the whole filter length even after the nonlinearity itself is clean.
    // Allocation-free.
    void reset() {
        for (int s = 0; s < kMaxStages; ++s) {
            ups_[s].reset();
            downs_[s].reset();
        }
    }

    // Group delay of the up+down round trip, in base-rate samples (0 at 1x).
    // Measured empirically at prepare-time filter lengths; see docs.
    int latencySamples() const {
        // Each stage's halfband has group delay M samples at its own (doubled)
        // rate. Referred to the base rate and summed over the up+down passes.
        // Stage s runs at 2^(s+1) * baseRate. Up and down each contribute M
        // samples at that rate => 2*M / 2^(s+1) base samples.
        int total = 0;
        for (int s = 0; s < stages_; ++s) {
            const int M = (s == 0) ? kTightM : kRelaxedM;
            total += (2 * M) >> (s + 1);
        }
        return total;
    }

    // in[0..n) -> internal buffer()[0..n*factor()). No allocation.
    void upsample(const float* in, int n) {
        assert(n <= maxBlockSize_ && "Oversampler: block exceeds maxBlockSize");
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) bufA_[static_cast<size_t>(i)] = in[i];
            osData_ = bufA_.data();
            osLen_ = n;
            return;
        }
        // Ping-pong between bufA_ and bufB_. Start with input in bufA_.
        float* src = bufA_.data();
        float* dst = bufB_.data();
        for (int i = 0; i < n; ++i) src[static_cast<size_t>(i)] = in[i];
        int cur = n;
        for (int s = 0; s < stages_; ++s) {
            int outIdx = 0;
            for (int i = 0; i < cur; ++i) {
                float o0, o1;
                ups_[s].processSample(src[static_cast<size_t>(i)], o0, o1);
                dst[static_cast<size_t>(outIdx++)] = o0;
                dst[static_cast<size_t>(outIdx++)] = o1;
            }
            cur = outIdx;
            float* tmp = src;
            src = dst;
            dst = tmp;
        }
        osData_ = src;  // final result landed in src after the last swap
        osLen_ = cur;
    }

    // Working buffer holding the (in-place editable) oversampled signal.
    float* buffer() { return osData_; }
    int bufferLength() const { return osLen_; }

    // internal buffer()[0..n*factor()) -> out[0..n). No allocation.
    void downsample(float* out, int n) {
        assert(n <= maxBlockSize_ && "Oversampler: block exceeds maxBlockSize");
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) out[i] = osData_[static_cast<size_t>(i)];
            return;
        }
        // Decimate in reverse stage order (highest rate first). osData_ points at
        // whichever of bufA_/bufB_ holds the oversampled signal; ping-pong down.
        float* src = osData_;
        float* dst = (osData_ == bufA_.data()) ? bufB_.data() : bufA_.data();
        int cur = osLen_;
        for (int s = stages_ - 1; s >= 0; --s) {
            int outIdx = 0;
            for (int i = 0; i < cur; ++i) {
                float o;
                if (downs_[s].processSample(src[static_cast<size_t>(i)], o))
                    dst[static_cast<size_t>(outIdx++)] = o;
            }
            cur = outIdx;
            float* tmp = src;
            src = dst;
            dst = tmp;
        }
        for (int i = 0; i < n && i < cur; ++i) out[i] = src[static_cast<size_t>(i)];
    }

private:
    int maxBlockSize_ = 0;
    int factor_ = 4;
    int stages_ = 2;
    HalfbandInterpolator2x ups_[kMaxStages];
    HalfbandDecimator2x downs_[kMaxStages];
    std::vector<float> bufA_;
    std::vector<float> bufB_;
    float* osData_ = nullptr;
    int osLen_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OVERSAMPLER_H
