// Clipper — portable DSP core (M13.10 follow-up, docs §70's named fix).
//
// PhaseVocoderShifter: a FREQUENCY-DOMAIN pitch shifter, built as the candidate
// replacement for `PitchShifter`'s time-domain SOLA splice — and REFUSED on
// measurement (docs §74, ADR 027).
//
// IT LIVES UNDER tools/measure/ AND NOT UNDER core/include/ DELIBERATELY. It is
// not shipped, it is not on any audio path, and it is not in the WASM artifact's
// hash closure. It is kept, rather than deleted, because §70 named "a
// frequency-domain shifter" as the fix for its one XFAIL and a later slice would
// otherwise rebuild it from scratch to reach the same answer. The head-to-head
// that refused it is `shifter_head_to_head.cpp`, next to this file, and it runs.
//
// WHAT IT DOES: it fixes the triad accuracy COMPLETELY — an E major triad reads
// 0.000/0.000/0.002/0.001 cents at -1/-2/-5/-12 where the shipped SOLA reads
// 0.615/1.163/4.124/12.516. So §70's diagnosis was right about the mechanism.
//
// WHAT IT COSTS, AND WHY THAT ENDS IT: that accuracy needs N = 8192 at 48 kHz,
// which is a MEASURED 171.6 ms of flat latency against SOLA's 8.8 ms envelope /
// 35.8 ms mean, plus a 112.5 ms transient rise at the octave against SOLA's 27.9.
// At any window this pedal could afford (N = 4096, 83.7 ms) it is WORSE than SOLA
// on the very bar it exists to fix: a rich triad at -2 reads 14.6 cents against
// SOLA's 1.5. The reason is structural and is the first thing the trade section
// below names — a low-register triad's partials are ~20 Hz apart, and a DOWNWARD
// shift halves that spacing again on relocation.
//
// ===========================================================================
// WHY THIS SHAPE, AND WHAT IT TRADES
// ===========================================================================
// SOLA re-seats a delay-line read once per grain and cross-correlates to find a
// splice that lands in phase. That works because a waveform REPEATS — and a
// 4:5:6 major triad only repeats at its composite period. ONE lag cannot align
// three partials at once, which is the whole content of §70's XFAIL
// `drop-triad-spread-at-minus-2` and the two refuted fixes recorded in
// PitchShifter.h (a wider span; sub-sample lag refinement).
//
// A phase vocoder has no splice at all. It resolves the signal into bins, reads
// each bin's INSTANTANEOUS frequency off the phase advance between frames,
// relocates that bin to `k * r` with its frequency scaled by `r`, and re-grows
// the phase from an accumulator. Every partial is moved independently, so
// "one lag for three partials" is not a question it can be asked.
//
// THE TRADE IS NAMED UP FRONT AND IT IS NOT SMALL:
//   (a) RESOLUTION. Two partials closer together than the analysis window's main
//       lobe share a bin, and a shared bin has ONE frequency estimate — so they
//       are moved together to a place that is neither. A low-register triad's
//       partials are ~20 Hz apart, which at 48 kHz needs a window far longer than
//       this pedal can spend. This is the property to measure first.
//   (b) TRANSIENTS. Phase is re-grown from an accumulator, so a pick attack is
//       spread over the window. SOLA passes a transient through essentially
//       intact for `1 - kCrossfade` of the time.
//   (c) LATENCY. An STFT costs its window; SOLA costs its sawtooth read.
//
// Identity phase locking (Laroche & Dolson) is implemented, because without it a
// vocoder's bins drift independently around a partial and the result is the
// classic "phasiness" — a defect that is structural, not a tuning miss.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17.

#ifndef CLIPPER_DSP_PHASE_VOCODER_SHIFTER_H
#define CLIPPER_DSP_PHASE_VOCODER_SHIFTER_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/FFT.h"  // the core primitive; this candidate lives OUTSIDE core/ (see banner)

namespace clipper::dsp {

class PhaseVocoderShifter {
public:
    // Overlap factor. 4 is the smallest that keeps a Hann analysis-AND-synthesis
    // pair COLA (the squared window sums to a constant 1.5 at N/4), and the hop
    // is what the instantaneous-frequency estimate is conditioned on — a longer
    // hop aliases the phase advance for high bins.
    static constexpr int kOverlap = 4;

    // `fftSize` must be a power of two.
    void prepare(int fftSize, double sampleRate) {
        n_ = fftSize;
        hop_ = n_ / kOverlap;
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        fft_.prepare(n_);

        const size_t N = static_cast<size_t>(n_);
        window_.assign(N, 0.0);
        for (int i = 0; i < n_; ++i)
            window_[static_cast<size_t>(i)] =
                0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(n_));
        // Hann^2 at 75 % overlap sums to 3/2 exactly.
        olaNorm_ = 1.0 / (0.5 * kOverlap * 0.75);

        re_.assign(N, 0.0);
        im_.assign(N, 0.0);
        mag_.assign(N / 2 + 1, 0.0);
        ana_.assign(N / 2 + 1, 0.0);
        nu_.assign(N / 2 + 1, 0.0);
        prevAna_.assign(N / 2 + 1, 0.0);
        synMag_.assign(N / 2 + 1, 0.0);
        synNu_.assign(N / 2 + 1, 0.0);
        synSrc_.assign(N / 2 + 1, 0.0);
        synPhase_.assign(N / 2 + 1, 0.0);
        peakOf_.assign(N / 2 + 1, 0);
        inRing_.assign(N, 0.0f);
        outRing_.assign(N, 0.0);
        reset();
    }

    void reset() {
        std::fill(inRing_.begin(), inRing_.end(), 0.0f);
        std::fill(outRing_.begin(), outRing_.end(), 0.0);
        std::fill(prevAna_.begin(), prevAna_.end(), 0.0);
        std::fill(synPhase_.begin(), synPhase_.end(), 0.0);
        wr_ = 0;
        rd_ = 0;
        hopCount_ = 0;
    }

    void setRatio(double r) { ratio_ = r > 0.0 ? r : 1.0; }
    double ratio() const { return ratio_; }
    int fftSize() const { return n_; }
    int hopSize() const { return hop_; }

    // Constant, unlike SOLA's sawtooth — which is the one structural advantage
    // this shape has, because a constant delay CAN be compensated by a host.
    // MEASURED by cross-correlation at r == 1 rather than reasoned: an output
    // sample is complete only once every frame overlapping it has been added,
    // and the newest of those is placed starting at that sample while spanning
    // the previous `n_` inputs, so the delay is the whole window, NOT n - hop.
    double latencySamples() const { return static_cast<double>(n_); }

    inline float process(float x) {
        inRing_[static_cast<size_t>(wr_)] = x;
        wr_ = wr_ + 1 == n_ ? 0 : wr_ + 1;

        const double y = outRing_[static_cast<size_t>(rd_)];
        outRing_[static_cast<size_t>(rd_)] = 0.0;
        rd_ = rd_ + 1 == n_ ? 0 : rd_ + 1;

        if (++hopCount_ >= hop_) {
            hopCount_ = 0;
            runFrame();
        }
        return static_cast<float>(flushDenormal(y * olaNorm_));
    }

    // ADR 006: every state here is either a bounded phase accumulator or a buffer
    // fed only by the input, and the OLA ring is zeroed as it is read. Nothing
    // recurses into itself, so nothing can asymptote into the subnormal range.
    double maxAbsRestingState() const {
        double m = 0.0;
        for (double v : outRing_) m = std::max(m, std::fabs(v));
        return m;
    }

private:
    void runFrame() {
        const int half = n_ / 2;
        const double expected = 2.0 * kPi * static_cast<double>(hop_) / static_cast<double>(n_);

        // --- analysis -------------------------------------------------------
        for (int i = 0; i < n_; ++i) {
            const int idx = wr_ + i >= n_ ? wr_ + i - n_ : wr_ + i;  // oldest first
            re_[static_cast<size_t>(i)] =
                static_cast<double>(inRing_[static_cast<size_t>(idx)]) * window_[static_cast<size_t>(i)];
            im_[static_cast<size_t>(i)] = 0.0;
        }
        fft_.transform(re_.data(), im_.data(), false);

        for (int k = 0; k <= half; ++k) {
            const size_t s = static_cast<size_t>(k);
            const double a = re_[s], b = im_[s];
            mag_[s] = std::sqrt(a * a + b * b);
            ana_[s] = std::atan2(b, a);
            // Instantaneous frequency in BIN units: the bin centre plus the phase
            // advance that the bin centre does not account for.
            double dev = ana_[s] - prevAna_[s] - expected * k;
            dev -= 2.0 * kPi * std::round(dev / (2.0 * kPi));
            nu_[s] = static_cast<double>(k) + dev / expected;
            prevAna_[s] = ana_[s];
        }

        // --- relocation: bin k -> k*r, frequency scaled by r -----------------
        std::fill(synMag_.begin(), synMag_.end(), 0.0);
        std::fill(synNu_.begin(), synNu_.end(), 0.0);
        std::fill(synSrc_.begin(), synSrc_.end(), 0.0);
        std::vector<double> dom(static_cast<size_t>(half) + 1, 0.0);
        for (int k = 0; k <= half; ++k) {
            const int j = static_cast<int>(std::lround(static_cast<double>(k) * ratio_));
            if (j < 0 || j > half) continue;
            const size_t d = static_cast<size_t>(j), s = static_cast<size_t>(k);
            synMag_[d] += mag_[s];
            // Frequency and source phase come from the DOMINANT contributor: an
            // average of two partials sharing a bin is a frequency neither has.
            if (mag_[s] > dom[d]) {
                dom[d] = mag_[s];
                synNu_[d] = nu_[s] * ratio_;
                synSrc_[d] = ana_[s];
            }
        }

        // --- identity phase locking ----------------------------------------
        // Each bin's synthesis phase is grown from the accumulator only at the
        // spectral PEAKS; every other bin is rigidly attached to its peak by the
        // analysis phase difference, so a partial's bins stay vertically coherent
        // instead of drifting into phasiness.
        int lastPeak = -1;
        for (int j = 0; j <= half; ++j) peakOf_[static_cast<size_t>(j)] = -1;
        for (int j = 1; j <= half - 1; ++j) {
            const size_t s = static_cast<size_t>(j);
            if (synMag_[s] > synMag_[s - 1] && synMag_[s] > synMag_[s + 1]) {
                const int from = lastPeak < 0 ? 0 : (lastPeak + j) / 2 + 1;
                for (int b = from; b <= j; ++b) peakOf_[static_cast<size_t>(b)] = j;
                lastPeak = j;
            }
        }
        if (lastPeak >= 0)
            for (int b = lastPeak + 1; b <= half; ++b) peakOf_[static_cast<size_t>(b)] = lastPeak;

        for (int j = 0; j <= half; ++j) {
            const size_t s = static_cast<size_t>(j);
            if (peakOf_[s] == j || peakOf_[s] < 0)
                synPhase_[s] += expected * synNu_[s];
        }
        for (int j = 0; j <= half; ++j) {
            const size_t s = static_cast<size_t>(j);
            const int p = peakOf_[s];
            double phase = synPhase_[s];
            if (p >= 0 && p != j) {
                const size_t ps = static_cast<size_t>(p);
                phase = synPhase_[ps] + (synSrc_[s] - synSrc_[ps]);
            }
            re_[s] = synMag_[s] * std::cos(phase);
            im_[s] = synMag_[s] * std::sin(phase);
            if (j > 0 && j < half) {  // hermitian mirror
                re_[static_cast<size_t>(n_ - j)] = re_[s];
                im_[static_cast<size_t>(n_ - j)] = -im_[s];
            }
        }

        // --- synthesis + overlap-add ---------------------------------------
        fft_.transform(re_.data(), im_.data(), true);
        for (int i = 0; i < n_; ++i) {
            const int idx = rd_ + i >= n_ ? rd_ + i - n_ : rd_ + i;
            outRing_[static_cast<size_t>(idx)] +=
                re_[static_cast<size_t>(i)] / static_cast<double>(n_) * window_[static_cast<size_t>(i)];
        }
    }

    static constexpr double kPi = 3.14159265358979323846;

    FFT fft_;
    int n_ = 2048, hop_ = 512;
    double sampleRate_ = 44100.0, ratio_ = 1.0, olaNorm_ = 1.0;
    int wr_ = 0, rd_ = 0, hopCount_ = 0;
    std::vector<double> window_, re_, im_, mag_, ana_, nu_, prevAna_;
    std::vector<double> synMag_, synNu_, synSrc_, synPhase_, outRing_;
    std::vector<int> peakOf_;
    std::vector<float> inRing_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_PHASE_VOCODER_SHIFTER_H
