// Clipper — portable DSP core.
//
// OutputLimiter (M6.6): the rig's final safety limiter, now a LOOKAHEAD
// GAIN-RIDER instead of a waveshaper.
//
// History: M6.1 shipped a tanh knee at 0.9 which, with the loud staging, soft-
// clipped the CLEAN path on every cycle ("fizzy JC"). M6.5 raised the knee to
// 0.97 and pulled staging to unity — but a tanh knee still WAVESHAPES every
// sample above threshold, so any program that rides into it (e.g. the cab's
// presence region pushing peaks over) turns overs into harmonics: fizz. Field
// report confirmed: fizz present only with the cab engaged (its former +3 dB
// presence bump ate the margin — see CabIR.cpp, now peak-normalized).
//
// M6.6 replaces the shaper with gain-riding: the signal is delayed by a short
// lookahead window; a sliding maximum over that window computes a target gain
// ceiling/peak; the applied gain reaches the target BEFORE the peak arrives
// (attack smoothed over ~1/3 window), holds ~50 ms, and recovers with a ~40 ms
// release (hold prevents inter-peak gain ripple = AM sidebands). Gain
// scaling adds NO harmonics — a steady over is just turned down; transients
// duck briefly. While no over is in the window the gain is exactly 1.0 and the
// output is bit-identical to the (delayed) input. A final hard clamp at ±1.0
// remains as an absolute backstop; by construction it should never engage.
//
// This header is the SINGLE SOURCE OF TRUTH for the limiter math. The
// AudioWorklet (web/worklet/clipper-processor.js) is plain JS and cannot
// include it, so it mirrors the algorithm and constants (LIM_* there) — keep
// them in sync, like the mirrored param ids. The native tests exercise THIS
// implementation.
//
// Stereo: one shared gain for both channels (max of |L|,|R| feeds the window)
// so limiting never shifts the stereo image.
//
// Latency: kLookahead samples (64 ≈ 1.3–1.5 ms at 44.1/48 k).
//
// Zero-dependency (<cmath>, <vector>, <algorithm>) and platform-free.

#ifndef CLIPPER_DSP_OUTPUT_LIMITER_H
#define CLIPPER_DSP_OUTPUT_LIMITER_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

class OutputLimiter {
public:
    // Kept name kThreshold (M6.5) — now the gain-rider's ceiling.
    static constexpr float kThreshold = 0.97f;
    static constexpr int kLookahead = 64;      // samples of delay / max window
    static constexpr double kReleaseSeconds = 0.040;
    // Hold: after an attack, the gain is pinned for this long before releasing.
    // Without it the gain creeps up between the cycle peaks of a steady over and
    // gets yanked back down every peak — amplitude modulation = sidebands. With
    // hold >> one period of any bass note, a steady over gets ONE constant gain:
    // pure scaling, zero added harmonics (asserted in testLimiterGainRiding).
    static constexpr double kHoldSeconds = 0.050;

    explicit OutputLimiter(float ceiling = kThreshold) : ceiling_(ceiling) {}

    void prepare(double sampleRate) {
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        // Attack reaches the target well inside the lookahead window.
        attackCoeff_ = std::exp(-1.0 / (kLookahead / 3.0));
        releaseCoeff_ = std::exp(-1.0 / (kReleaseSeconds * fs));
        holdSamples_ = static_cast<int>(kHoldSeconds * fs);
        holdCount_ = 0;
        delayL_.assign(kLookahead, 0.0f);
        delayR_.assign(kLookahead, 0.0f);
        // Monotonic-deque state for the sliding-window maximum: ring of
        // {sampleIndex, value} pairs, values strictly decreasing head->tail.
        // +2: the window spans kLookahead+1 samples and a ring buffer needs one
        // spare slot so a full deque is distinguishable from an empty one.
        dqIdx_.assign(kLookahead + 2, 0);
        dqVal_.assign(kLookahead + 2, 0.0f);
        dqHead_ = dqTail_ = 0;
        writePos_ = 0;
        sampleCount_ = 0;
        gain_ = 1.0;
    }

    int latencySamples() const { return kLookahead; }
    float ceiling() const { return ceiling_; }

    // Stereo in-place. L and R must have numFrames samples. Mono: pass the same
    // pointer for both or use processMono.
    void processStereo(float* L, float* R, int numFrames) {
        for (int i = 0; i < numFrames; ++i) {
            const float inL = L[i];
            const float inR = R[i];
            // Push |peak| of this (future) sample into the window max.
            const float mag = std::max(std::fabs(inL), std::fabs(inR));
            dequePush(sampleCount_, mag);
            dequeExpire(sampleCount_ - kLookahead);
            ++sampleCount_;

            // Delayed output samples.
            const float outL = delayL_[writePos_];
            const float outR = delayR_[writePos_];
            delayL_[writePos_] = inL;
            delayR_[writePos_] = inR;
            writePos_ = (writePos_ + 1) % kLookahead;

            // Target gain from the window max (covers the delayed sample AND
            // everything about to arrive after it).
            const float peak = dqVal_[dqHead_];
            const double target = peak > ceiling_ ? static_cast<double>(ceiling_) / peak : 1.0;
            // gain_ is a DOUBLE accumulator: near unity the release increment
            // (~1e-8) is below float32 epsilon and a float gain stalls short of
            // the snap-to-unity threshold (observed at 96 kHz).
            if (target < gain_) {
                gain_ = attackCoeff_ * gain_ + (1.0 - attackCoeff_) * target;
                if (gain_ < target) gain_ = target;  // never undershoot
                holdCount_ = holdSamples_;           // arm the hold
            } else if (holdCount_ > 0) {
                --holdCount_;                        // pinned: no release yet
            } else {
                gain_ = releaseCoeff_ * gain_ + (1.0 - releaseCoeff_) * target;
            }

            // Bit-exact transparency when idle: snap to unity once the window
            // holds no over and release is within 1e-4 (< 0.001 dB) of unity —
            // from there the output is bit-identical to the delayed input.
            if (target >= 1.0f && gain_ > 0.9999) gain_ = 1.0;

            const float g = static_cast<float>(gain_);
            L[i] = clamp1(outL * g);
            R[i] = clamp1(outR * g);
        }
    }

    void processMono(float* buf, int numFrames) { processStereo(buf, buf, numFrames); }

private:
    static float clamp1(float x) { return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x); }

    void dequePush(long long idx, float v) {
        // Drop tail entries <= v (they can never be the max while v is alive).
        while (dqTail_ != dqHead_) {
            const int prev = (dqTail_ + static_cast<int>(dqIdx_.size()) - 1) %
                             static_cast<int>(dqIdx_.size());
            if (dqVal_[prev] > v) break;
            dqTail_ = prev;
        }
        dqIdx_[dqTail_] = idx;
        dqVal_[dqTail_] = v;
        dqTail_ = (dqTail_ + 1) % static_cast<int>(dqIdx_.size());
    }

    void dequeExpire(long long oldestAllowed) {
        while (dqHead_ != dqTail_ && dqIdx_[dqHead_] < oldestAllowed)
            dqHead_ = (dqHead_ + 1) % static_cast<int>(dqIdx_.size());
    }

    float ceiling_;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double gain_ = 1.0;
    std::vector<float> delayL_, delayR_;
    std::vector<long long> dqIdx_;
    std::vector<float> dqVal_;
    int dqHead_ = 0, dqTail_ = 0;
    int writePos_ = 0;
    long long sampleCount_ = 0;
    int holdSamples_ = 0;
    int holdCount_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OUTPUT_LIMITER_H
