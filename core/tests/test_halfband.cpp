// Clipper — halfband resampler tests (docs §32).
//
// WHY THIS FILE EXISTS. `perf/halfband-no-modulo` replaced the halfband ring-buffer
// indexing `ring_[(w - p + N) % N]` with a doubled ring read unwrapped as
// `ring_[w + N - p]`, to delete one hardware integer division PER TAP from the hottest
// loop in the project (2026-07-24 audit, fidelity-neutral perf item 2). The claim that
// makes such a change acceptable at all under "audio quality is the product" is that it
// is *bit-identical* — not "within tolerance", not "sounds the same". So this suite
// carries a verbatim copy of the PRE-CHANGE implementation and compares the shipped
// classes against it sample-for-sample, bit-for-bit.
//
// Note on "don't test the implementation against a reference derived from the same
// code" (CLAUDE.md): that rule targets tests which validate a discretisation against an
// analytic model derived from the same netlist, and therefore cannot catch a wrong
// topology or a wrong component value. It does not apply here, and its inverse applies:
// the ONLY meaningful reference for "this refactor changed nothing" is the code it
// replaced. Blocks B and C do carry absolute, player-observable references — the image /
// alias rejection the filters must deliver across the 20 kHz audio band, and the
// round-trip group delay a player is charged in latency — measured through the live
// objects, so a wrong filter or a wrong delay cannot hide behind block A.
//
// Blocks:
//   A. bit-identity vs the pre-change implementation (both classes; the full
//      Oversampler cascade at 1x/2x/4x/8x over ragged blocks; and across reset()).
//   B. stopband, measured as worst-case image rejection (interpolator) and worst-case
//      alias rejection (decimator) over the audio band, for both shipped designs.
//   C. round-trip group delay == Oversampler::latencySamples(), measured from an
//      impulse rather than restated from the constants.

#include "support/AssertsLive.h"

#include "clipper/dsp/HalfbandFilter.h"
#include "clipper/dsp/Oversampler.h"
#include "measure/AliasMetric.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using clipper::dsp::HalfbandDecimator2x;
using clipper::dsp::HalfbandInterpolator2x;
using clipper::dsp::makeHalfband;
using clipper::dsp::Oversampler;

constexpr double kPi = 3.14159265358979323846;

// --------------------------------------------------------------------------------------
// The PRE-CHANGE implementation, copied verbatim from HalfbandFilter.h as of
// accd3c0 (the commit this slice branched from) except for the class names. Modulo
// indexing, single-length ring, vector<Tap>. Do not "fix" or optimise these — their
// whole job is to be the old code.
// --------------------------------------------------------------------------------------

class RefInterpolator2x {
public:
    void setup(const std::vector<double>& h) {
        M_ = (static_cast<int>(h.size()) - 1) / 2;
        halfM_ = M_ / 2;
        e1_.resize(static_cast<size_t>(M_));
        for (int p = 0; p < M_; ++p)
            e1_[static_cast<size_t>(p)] =
                static_cast<float>(2.0 * h[static_cast<size_t>(2 * p + 1)]);
        ring_.assign(static_cast<size_t>(M_), 0.0f);
        w_ = 0;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        w_ = 0;
    }

    inline void processSample(float in, float& o0, float& o1) {
        const int M = M_;
        ring_[static_cast<size_t>(w_)] = in;
        o0 = ring_[static_cast<size_t>((w_ - halfM_ + M) % M)];
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

class RefDecimator2x {
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

    inline bool processSample(float in, float& out) {
        const int L = L_;
        const int wCur = w_;
        ring_[static_cast<size_t>(w_)] = in;
        w_ = (w_ + 1) % L;
        phase_ ^= 1;
        if (phase_ != 0) return false;
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

// The Oversampler cascade, rebuilt on the reference filters. Mirrors Oversampler's
// ping-pong plumbing exactly (which this slice does not touch) so that any difference
// observed at this level is attributable to the halfband classes alone.
class RefOversampler {
public:
    void prepare(int maxBlockSize) {
        maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 1;
        const std::vector<double> tight = makeHalfband(Oversampler::kTightM, Oversampler::kBeta);
        const std::vector<double> relaxed =
            makeHalfband(Oversampler::kRelaxedM, Oversampler::kBeta);
        for (int s = 0; s < Oversampler::kMaxStages; ++s) {
            const std::vector<double>& h = (s == 0) ? tight : relaxed;
            ups_[s].setup(h);
            downs_[s].setup(h);
        }
        const size_t cap = static_cast<size_t>(maxBlockSize_) * Oversampler::kMaxFactor;
        bufA_.assign(cap, 0.0f);
        bufB_.assign(cap, 0.0f);
        osData_ = nullptr;
        osLen_ = 0;
        setFactor(factor_);
    }

    void setFactor(int factor) {
        int f = 1;
        while (f * 2 <= factor && f * 2 <= Oversampler::kMaxFactor) f *= 2;
        factor_ = (factor <= 1) ? 1 : f;
        stages_ = 0;
        for (int t = factor_; t > 1; t >>= 1) ++stages_;
        for (int s = 0; s < Oversampler::kMaxStages; ++s) {
            ups_[s].reset();
            downs_[s].reset();
        }
    }

    void reset() {
        for (int s = 0; s < Oversampler::kMaxStages; ++s) {
            ups_[s].reset();
            downs_[s].reset();
        }
    }

    void upsample(const float* in, int n) {
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) bufA_[static_cast<size_t>(i)] = in[i];
            osData_ = bufA_.data();
            osLen_ = n;
            return;
        }
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
        osData_ = src;
        osLen_ = cur;
    }

    float* buffer() { return osData_; }
    int bufferLength() const { return osLen_; }

    void downsample(float* out, int n) {
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) out[i] = osData_[static_cast<size_t>(i)];
            return;
        }
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
    RefInterpolator2x ups_[Oversampler::kMaxStages];
    RefDecimator2x downs_[Oversampler::kMaxStages];
    std::vector<float> bufA_, bufB_;
    float* osData_ = nullptr;
    int osLen_ = 0;
};

// --------------------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------------------

// Bit-for-bit float comparison. `==` is not enough: it calls +0.0 equal to -0.0 and
// every NaN unequal to itself, so a sign-of-zero drift or an NaN-vs-NaN difference in
// the tap sum would slip past a `==` check (and a `max |a-b|` check).
bool bitEqual(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

int bitDiffCount(const std::vector<float>& a, const std::vector<float>& b) {
    int n = 0;
    const size_t m = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < m; ++i)
        if (!bitEqual(a[i], b[i])) ++n;
    return n + static_cast<int>(a.size() > b.size() ? a.size() - b.size()
                                                    : b.size() - a.size());
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

// A deliberately hostile program signal: a plucked-note riff (fundamental + 12
// harmonics, so there is real energy right up to Nyquist), plus a full-scale square
// wave burst, plus deterministic wideband noise, plus lone impulses and long silences
// (which is where a stale ring copy or a mis-wrapped index shows up as a ghost).
std::vector<float> makeProgram(int n, double sr) {
    std::vector<float> s(static_cast<size_t>(n), 0.0f);
    const double notes[4] = {82.41, 110.0, 146.83, 196.0};
    unsigned int rng = 0x1234567u;
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        const int pluck = static_cast<int>(t / 0.37);
        const double tp = t - pluck * 0.37;
        const double f0 = notes[pluck % 4];
        const double env = std::exp(-tp / 0.25) * (1.0 - std::exp(-tp / 0.0015));
        double v = 0.0;
        for (int h = 1; h <= 12; ++h) v += std::sin(2.0 * kPi * f0 * h * t + 0.3 * h) / h;
        double x = 0.22 * env * v;
        // Square burst (edges = broadband energy) for 30 ms every 0.37 s.
        if (tp < 0.03) x += 0.9 * ((std::fmod(t * 1234.0, 1.0) < 0.5) ? 1.0 : -1.0);
        // Deterministic LCG noise at -40 dBFS.
        rng = rng * 1664525u + 1013904223u;
        x += 0.01 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0);
        // Lone full-scale impulses, and stretches of exact silence.
        if (i % 9973 == 0) x = 1.0;
        if ((i / 4096) % 11 == 7) x = 0.0;
        s[static_cast<size_t>(i)] = static_cast<float>(x);
    }
    return s;
}

// The in-domain nonlinearity used when comparing full cascades. Any deterministic
// function does; a saturator is representative of what actually sits in the
// oversampled domain, and it makes the downsampler see real stopband content.
inline float inDomain(float x) { return std::tanh(2.5f * x) * 0.8f; }

// ======================================================================================
// Block A — bit-identity against the pre-change implementation
// ======================================================================================

void testInterpolatorBitIdentical(int M, const char* label) {
    const std::vector<double> h = makeHalfband(M, Oversampler::kBeta);
    HalfbandInterpolator2x nw;
    RefInterpolator2x ref;
    nw.setup(h);
    ref.setup(h);

    const std::vector<float> in = makeProgram(240000, 88200.0);
    std::vector<float> outNew, outRef;
    outNew.reserve(in.size() * 2);
    outRef.reserve(in.size() * 2);
    for (float x : in) {
        float a0, a1, b0, b1;
        nw.processSample(x, a0, a1);
        ref.processSample(x, b0, b1);
        outNew.push_back(a0);
        outNew.push_back(a1);
        outRef.push_back(b0);
        outRef.push_back(b1);
    }
    const int diff = bitDiffCount(outNew, outRef);
    std::printf("  interp M=%-3d %-8s  %zu samples, differing bits: %d, max|d| = %.1f\n", M,
                label, outNew.size(), diff, maxAbsDiff(outNew, outRef));
    assert(diff == 0 && "interpolator is not bit-identical to the pre-change version");
    assert(maxAbsDiff(outNew, outRef) == 0.0);
}

void testDecimatorBitIdentical(int M, const char* label) {
    const std::vector<double> h = makeHalfband(M, Oversampler::kBeta);
    HalfbandDecimator2x nw;
    RefDecimator2x ref;
    nw.setup(h);
    ref.setup(h);

    const std::vector<float> in = makeProgram(240000, 176400.0);
    std::vector<float> outNew, outRef;
    for (float x : in) {
        float a = 0.0f, b = 0.0f;
        const bool ga = nw.processSample(x, a);
        const bool gb = ref.processSample(x, b);
        assert(ga == gb && "decimator changed WHICH sample completes a pair");
        if (ga) {
            outNew.push_back(a);
            outRef.push_back(b);
        }
    }
    assert(outNew.size() == in.size() / 2);
    const int diff = bitDiffCount(outNew, outRef);
    std::printf("  decim  L=%-3d %-8s  %zu samples, differing bits: %d, max|d| = %.1f\n",
                2 * M + 1, label, outNew.size(), diff, maxAbsDiff(outNew, outRef));
    assert(diff == 0 && "decimator is not bit-identical to the pre-change version");
    assert(maxAbsDiff(outNew, outRef) == 0.0);
}

// Drive both cascades exactly as a pedal does — upsample, edit the buffer in place,
// downsample — over RAGGED block sizes, so the ring wrap lands mid-block and at every
// possible offset rather than always on a 128 boundary.
void testCascadeBitIdentical(int factor) {
    constexpr int kMaxBlock = 256;
    Oversampler nw;
    RefOversampler ref;
    nw.prepare(kMaxBlock);
    ref.prepare(kMaxBlock);
    nw.setFactor(factor);
    ref.setFactor(factor);
    assert(nw.factor() == factor);

    const std::vector<float> in = makeProgram(200000, 48000.0);
    std::vector<float> outNew(in.size(), 0.0f), outRef(in.size(), 0.0f);
    // Ragged: cycle through block sizes that share no common factor with the ring
    // lengths (129, 33) or with each other.
    const int blocks[] = {1, 7, 128, 31, 200, 3, 65, 256, 17, 100};
    int off = 0, bi = 0, upDiff = 0;
    while (off < static_cast<int>(in.size())) {
        int n = blocks[bi++ % 10];
        if (off + n > static_cast<int>(in.size())) n = static_cast<int>(in.size()) - off;

        nw.upsample(in.data() + off, n);
        ref.upsample(in.data() + off, n);
        assert(nw.bufferLength() == ref.bufferLength());
        assert(nw.bufferLength() == n * factor);
        float* wn = nw.buffer();
        float* wr = ref.buffer();
        for (int i = 0; i < nw.bufferLength(); ++i) {
            if (!bitEqual(wn[i], wr[i])) ++upDiff;
            wn[i] = inDomain(wn[i]);
            wr[i] = inDomain(wr[i]);
        }
        nw.downsample(outNew.data() + off, n);
        ref.downsample(outRef.data() + off, n);
        off += n;
    }
    const int diff = bitDiffCount(outNew, outRef);
    std::printf("  cascade %dx  ragged blocks, %zu samples: upsampled bits differing %d, "
                "downsampled %d, max|d| = %.1f\n",
                factor, outNew.size(), upDiff, diff, maxAbsDiff(outNew, outRef));
    assert(upDiff == 0 && "upsampled buffer is not bit-identical");
    assert(diff == 0 && "round-tripped signal is not bit-identical");
    assert(maxAbsDiff(outNew, outRef) == 0.0);

    // Every sample finite (a doubled ring read out of range would most likely show up
    // here first, as garbage rather than as a mismatch).
    for (float v : outNew) assert(std::isfinite(v));
}

// The doubled ring is only correct while BOTH halves are maintained. reset() is the
// one place that could plausibly clear just one of them, and the symptom would be
// subtle: correct output for a while, then a stale ghost for one filter length. So:
// poison the filters with NaN, reset, and require the subsequent render to be
// bit-identical to a virgin instance's render of the same input.
void testResetClearsBothHalves(int factor) {
    constexpr int kBlock = 64;
    const std::vector<float> in = makeProgram(20000, 48000.0);

    auto render = [&](bool poisonThenReset) {
        Oversampler os;
        os.prepare(kBlock);
        os.setFactor(factor);
        if (poisonThenReset) {
            std::vector<float> nan(kBlock, std::nanf(""));
            for (int rep = 0; rep < 4; ++rep) {
                os.upsample(nan.data(), kBlock);
                std::vector<float> junk(kBlock, 0.0f);
                os.downsample(junk.data(), kBlock);
            }
            os.reset();
        }
        std::vector<float> out(in.size(), 0.0f);
        for (int off = 0; off + kBlock <= static_cast<int>(in.size()); off += kBlock) {
            os.upsample(in.data() + off, kBlock);
            float* w = os.buffer();
            for (int i = 0; i < os.bufferLength(); ++i) w[i] = inDomain(w[i]);
            os.downsample(out.data() + off, kBlock);
        }
        return out;
    };

    const std::vector<float> virgin = render(false);
    const std::vector<float> recovered = render(true);
    const int diff = bitDiffCount(virgin, recovered);
    std::printf("  reset  %dx  after NaN poisoning: differing bits vs a virgin instance: "
                "%d\n",
                factor, diff);
    for (float v : recovered) assert(std::isfinite(v) && "reset() left NaN in the ring");
    assert(diff == 0 && "reset() did not fully clear the doubled ring");
}

// ======================================================================================
// Block B — stopband, measured through the live objects
// ======================================================================================

// Worst-case IMAGE rejection of the interpolator over the audio band. Zero-stuffing a
// base-rate tone f puts an image at (baseRate - f) in the doubled-rate stream; the
// halfband's job is to kill it. Absolute reference: every audio frequency a guitar rig
// can carry (up to 20 kHz) must have its image suppressed, and the worst case sits at
// the top of the band where the image is closest to the transition.
double worstImageRejectionDb(int M, double baseRate, double audioEdgeHz) {
    const std::vector<double> h = makeHalfband(M, Oversampler::kBeta);
    const int n = 65536;             // doubled-rate samples analysed
    const int skip = 4 * (2 * M + 1);  // let the FIR fill
    double worst = -400.0;
    for (double f = 1000.0; f <= audioEdgeHz + 1.0; f += 500.0) {
        HalfbandInterpolator2x in;
        in.setup(h);
        std::vector<float> y(static_cast<size_t>(n + 2 * skip), 0.0f);
        for (int i = 0; i < (n + 2 * skip) / 2; ++i) {
            float o0, o1;
            in.processSample(static_cast<float>(0.5 * std::sin(2.0 * kPi * f * i / baseRate)),
                             o0, o1);
            y[static_cast<size_t>(2 * i)] = o0;
            y[static_cast<size_t>(2 * i + 1)] = o1;
        }
        const double fund = clipper::measure::goertzelAmp(y, static_cast<size_t>(2 * skip),
                                                          static_cast<size_t>(n), f,
                                                          2.0 * baseRate);
        const double image = clipper::measure::goertzelAmp(y, static_cast<size_t>(2 * skip),
                                                           static_cast<size_t>(n),
                                                           baseRate - f, 2.0 * baseRate);
        const double db = 20.0 * std::log10((image + 1e-30) / (fund + 1e-30));
        if (db > worst) worst = db;
    }
    return worst;
}

// Worst-case ALIAS rejection of the decimator over the audio band. A high-rate tone at
// f (above the post-decimation Nyquist) folds to |f - outRate| after decimation; the
// halfband must kill it before it does. Sweeps every f that would land inside the
// protected 0..audioEdge band.
double worstAliasRejectionDb(int M, double outRate, double audioEdgeHz) {
    const std::vector<double> h = makeHalfband(M, Oversampler::kBeta);
    const double inRate = 2.0 * outRate;
    const int n = 32768;  // output (decimated) samples analysed
    const int skip = 4 * (2 * M + 1);
    double worst = -400.0;
    for (double f = outRate - audioEdgeHz; f <= outRate - 1.0; f += 500.0) {
        HalfbandDecimator2x dec;
        dec.setup(h);
        std::vector<float> y;
        y.reserve(static_cast<size_t>(n + 2 * skip));
        for (int i = 0; i < 2 * (n + 2 * skip); ++i) {
            float o = 0.0f;
            if (dec.processSample(static_cast<float>(0.5 * std::sin(2.0 * kPi * f * i / inRate)),
                                  o))
                y.push_back(o);
        }
        const double alias = clipper::measure::goertzelAmp(
            y, static_cast<size_t>(skip), static_cast<size_t>(n), outRate - f, outRate);
        const double db = 20.0 * std::log10((alias + 1e-30) / 0.5);
        if (db > worst) worst = db;
    }
    return worst;
}

// ======================================================================================
// Block C — latency, measured from an impulse
// ======================================================================================

// Round-trip group delay, measured from the composite impulse response rather than
// restated from the constants.
//
// Estimator: the PHASE SLOPE at a low frequency. Neither the argmax nor the energy
// centroid works here. The true delay is fractional (the decimator emits on the second
// sample of each pair, which costs half a sample at that stage's rate), so the argmax is
// an arbitrary pick between two near-equal peaks, and the centroid is biased whenever
// the fractional part is not exactly 0.5 because the sample grid is then not symmetric
// about the true centre. The cascade is linear phase, so at a frequency well inside the
// flat passband the phase is exactly -w*D and D falls out with no bias. w is chosen so
// that w*D < pi (no unwrapping needed) and the whole impulse response fits the window.
struct DelayMeasurement {
    double delay;  // group delay in base-rate samples, from the phase slope
    int argmax;    // loudest single sample
};

DelayMeasurement measuredRoundTripDelay(int factor) {
    constexpr int kBlock = 128;
    Oversampler os;
    os.prepare(kBlock);
    os.setFactor(factor);
    const int n = 8 * kBlock;
    std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n), 0.0f);
    in[0] = 1.0f;
    for (int off = 0; off + kBlock <= n; off += kBlock) {
        os.upsample(in.data() + off, kBlock);
        os.downsample(out.data() + off, kBlock);  // identity in the domain
    }
    int peak = 0;
    double best = -1.0;
    for (int i = 0; i < n; ++i) {
        const double a = std::fabs(out[static_cast<size_t>(i)]);
        if (a > best) {
            best = a;
            peak = i;
        }
    }
    // D from the phase at w = 2*pi/512 rad/sample (base rate): w*D < 1 rad at the
    // largest delay here (76), so atan2 needs no unwrapping. The impulse response is
    // finite and fully inside the window, so a rectangular window is exact.
    const double w = 2.0 * kPi / 512.0;
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        re += out[static_cast<size_t>(i)] * std::cos(w * i);
        im -= out[static_cast<size_t>(i)] * std::sin(w * i);
    }
    const double delay = (re == 0.0 && im == 0.0) ? 0.0 : -std::atan2(im, re) / w;
    return {delay, peak};
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();

    std::printf("== Block A: bit-identity vs the pre-change (modulo-indexed) implementation\n");
    testInterpolatorBitIdentical(Oversampler::kTightM, "(tight)");
    testInterpolatorBitIdentical(Oversampler::kRelaxedM, "(relaxed)");
    testDecimatorBitIdentical(Oversampler::kTightM, "(tight)");
    testDecimatorBitIdentical(Oversampler::kRelaxedM, "(relaxed)");
    testCascadeBitIdentical(1);
    testCascadeBitIdentical(2);
    testCascadeBitIdentical(4);
    testCascadeBitIdentical(8);
    testResetClearsBothHalves(2);
    testResetClearsBothHalves(4);
    testResetClearsBothHalves(8);

    std::printf("== Block B: stopband through the live objects (audio band <= 20 kHz)\n");
    // Tight design: the first 2x stage, running at a 44.1 kHz base.
    const double tightImage = worstImageRejectionDb(Oversampler::kTightM, 44100.0, 20000.0);
    const double tightAlias = worstAliasRejectionDb(Oversampler::kTightM, 44100.0, 20000.0);
    // Relaxed design: every later stage, whose input rate is already >= 88.2 kHz.
    const double relaxedImage = worstImageRejectionDb(Oversampler::kRelaxedM, 88200.0, 20000.0);
    const double relaxedAlias = worstAliasRejectionDb(Oversampler::kRelaxedM, 88200.0, 20000.0);
    std::printf("  tight   (L=129) worst image %.1f dB, worst alias %.1f dB\n", tightImage,
                tightAlias);
    std::printf("  relaxed (L= 33) worst image %.1f dB, worst alias %.1f dB\n", relaxedImage,
                relaxedAlias);
    // The documented design target is ~80 dB (docs §7); the audit's measured worst cases
    // were -79.8 dB (tight) and -78.7 dB (relaxed). Assert the shipped filters still
    // clear -78 dB everywhere in the audio band, both directions.
    assert(tightImage < -78.0 && "tight halfband image rejection regressed");
    assert(tightAlias < -78.0 && "tight halfband alias rejection regressed");
    assert(relaxedImage < -78.0 && "relaxed halfband image rejection regressed");
    assert(relaxedAlias < -78.0 && "relaxed halfband alias rejection regressed");

    std::printf("== Block C: round-trip group delay (measured) vs latencySamples()\n");
    const int reported[9] = {0, 0, 64, 0, 72, 0, 0, 0, 76};  // latencySamples(), by factor
    // The TRUE round-trip delay, derived from the structure rather than copied from
    // latencySamples(): stage s contributes M taps of interpolator delay and M-1 of
    // decimator delay at its own (doubled) rate, i.e. (2M-1) / 2^(s+1) base samples.
    // The missing 1 per stage is the decimator emitting on the SECOND sample of each
    // pair. latencySamples() uses (2M) >> (s+1) and so over-reports by up to a sample.
    const double trueDelay[9] = {0.0,
                                 0.0,
                                 127.0 / 2.0,                              // 2x: 63.500
                                 0.0,
                                 127.0 / 2.0 + 31.0 / 4.0,                 // 4x: 71.250
                                 0.0,
                                 0.0,
                                 0.0,
                                 127.0 / 2.0 + 31.0 / 4.0 + 31.0 / 8.0};   // 8x: 75.125
    for (const int factor : {1, 2, 4, 8}) {
        Oversampler os;
        os.prepare(128);
        os.setFactor(factor);
        const DelayMeasurement d = measuredRoundTripDelay(factor);
        std::printf("  %dx  latencySamples() = %3d (documented %3d), measured delay = "
                    "%8.4f (structural %8.4f), peak sample = %3d\n",
                    factor, os.latencySamples(), reported[factor], d.delay,
                    trueDelay[factor], d.argmax);
        assert(os.latencySamples() == reported[factor] &&
               "reported oversampler latency changed");
        // The measured delay must match the structure to well under a hundredth of a
        // sample. This is what actually pins the filter lengths and the polyphase
        // alignment: change either and this moves.
        assert(std::fabs(d.delay - trueDelay[factor]) < 0.01 &&
               "the filters' actual group delay changed");
        // ...and the number the UI/plugin reports must stay within a sample of the truth.
        // It is 0.5 / 0.75 / 0.875 base samples OPTIMISTIC-in-reverse (it over-reports)
        // at 2x / 4x / 8x — 18 us at worst, inaudible, pre-existing, and unchanged by
        // this slice (block A proves bit-identity). Pinned so it cannot quietly drift.
        assert(os.latencySamples() >= trueDelay[factor] &&
               os.latencySamples() - trueDelay[factor] < 1.0 &&
               "reported latency drifted more than a sample from the measured delay");
    }

    std::printf("All halfband resampler tests passed.\n");
    return 0;
}
