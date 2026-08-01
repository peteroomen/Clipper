// Clipper — portable DSP core.
//
// M13.4: `DelayLine` — the project's FIRST delay primitive, and deliberately the
// most reusable thing that slice leaves behind. It is a plain interpolating ring
// buffer and it knows NOTHING about bucket brigades, tape, feedback, companders
// or knobs. `DelayModel` (the "Echoman" BBD analog delay) is the first consumer;
// the named next ones are M13.6's flanger and any future tape echo, and they are
// cheap only if this primitive is right.
//
// Contract, and the three rules that make it real-time safe:
//
//   1. `prepare(maxDelaySeconds, sampleRate)` is the ONLY thing that allocates.
//      It sizes the ring for the longest delay the caller will ever ask for, at
//      the rate it will run at. `write()` / `read*()` never allocate, never
//      branch on capacity, and never reallocate — changing the delay TIME just
//      moves a read offset.
//
//   2. Reads are relative to the WRITE cursor, in samples, and are taken BEFORE
//      the matching `write()` advances it (see `write()`'s doc). `readCubic(d)`
//      with an integer `d >= 1` returns exactly the sample written `d` calls ago.
//
//   3. The ring is indexed with conditional wraps, never `%`. docs §32 measured
//      a hardware integer division per tap in the halfband ring and it cost
//      2.7x; a 4-point interpolator is four taps per sample, at the OVERSAMPLED
//      rate, inside a feedback loop. Same rule applies here.
//
// Interpolation: 4-point Lagrange (cubic) is the default read, for the reason
// `ChorusModel` already documents — it is STATELESS, so a read position that
// sweeps continuously (and, in a delay, reverses direction when the knob is
// swept back) leaves no ringing, where an allpass interpolator would smear the
// reversal. `readLinear` is offered for callers that need the cheaper read and
// can accept its HF droop; it is NOT what `DelayModel` uses.
//
// Anti-denormal (ADR 006 / docs §33): the buffer's rest value IS zero, and a
// decaying feedback tail circulates subnormals through it forever on WASM (which
// has no flush-to-zero at all). `write()` therefore flushes. That is the whole
// guard — the ring has no recursion of its own, so the per-value contract of
// `flushDenormal` is sufficient here and docs §56.4b's whole-state form is not
// needed (there is no order >= 2 recursion inside this class).
//
// Platform-free C++17. Header-only, like OnePoleSmoother / Biquad / HalfbandFilter.

#ifndef CLIPPER_DSP_DELAY_LINE_H
#define CLIPPER_DSP_DELAY_LINE_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

class DelayLine {
public:
    // Sizes the ring for `maxDelaySeconds` at `sampleRate` and clears it.
    // ALLOCATES — call from prepare(), never from process().
    //
    // The ring is `ceil(maxDelaySeconds * sampleRate) + kGuard` long. The guard
    // covers the cubic interpolator's oldest tap (d + 2) plus one sample of
    // rounding, so `readCubic(maxDelaySamples())` is always in bounds.
    void prepare(double maxDelaySeconds, double sampleRate) {
        const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double maxSec = maxDelaySeconds > 0.0 ? maxDelaySeconds : 0.001;
        long n = static_cast<long>(std::ceil(maxSec * fs)) + kGuard;
        if (n < kMinLen) n = kMinLen;
        len_ = static_cast<int>(n);
        buf_.assign(static_cast<size_t>(len_), 0.0f);
        w_ = 0;
    }

    // Clears the ring without reallocating. Safe on the audio thread.
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        w_ = 0;
    }

    // Largest delay (in samples) this line can be read at with the cubic
    // interpolator still in bounds.
    double maxDelaySamples() const { return static_cast<double>(len_ - kGuard); }

    int capacity() const { return len_; }

    // Writes one sample at the cursor and advances it.
    //
    // ORDER MATTERS and is part of the contract: the intended per-sample shape is
    //   y = line.readCubic(D);   // reads the PAST
    //   line.write(x);           // then commits the present
    // With that order `readCubic(1.0)` is the previous input and `readCubic(D)`
    // is the input D samples ago. Writing first shifts every read by one sample.
    inline void write(float x) {
        buf_[static_cast<size_t>(w_)] = flushDenormal(x);
        if (++w_ >= len_) w_ = 0;
    }

    // Reads `d` samples into the past (d >= 1), 4-point Lagrange (cubic).
    // `d` is clamped into [1, maxDelaySamples()] so a bad control value can never
    // index out of the ring — the clamp is NaN-rejecting in the ParamGuard sense
    // (the `!(d > lo)` form takes the low branch for NaN).
    inline float readCubic(double d) const {
        const double dMax = static_cast<double>(len_ - kGuard);
        double dc = !(d > 1.0) ? 1.0 : (d > dMax ? dMax : d);
        const int id = static_cast<int>(dc);          // floor, dc >= 1
        const double a = dc - static_cast<double>(id);  // fraction in [0,1)

        // Taps at delays id-1 .. id+2, oldest last. id >= 1 so id-1 >= 0, and the
        // guard covers id+2.
        const double xm1 = tap(id - 1);
        const double x0 = tap(id);
        const double x1 = tap(id + 1);
        const double x2 = tap(id + 2);

        const double cm1 = -a * (a - 1.0) * (a - 2.0) / 6.0;
        const double c0 = (a + 1.0) * (a - 1.0) * (a - 2.0) / 2.0;
        const double c1 = -(a + 1.0) * a * (a - 2.0) / 2.0;
        const double c2 = (a + 1.0) * a * (a - 1.0) / 6.0;
        return static_cast<float>(cm1 * xm1 + c0 * x0 + c1 * x1 + c2 * x2);
    }

    // Reads `d` samples into the past with linear interpolation. Cheaper, and
    // measurably duller (docs §60.7 records the A/B) — offered for callers whose
    // read position is static, NOT for a swept one.
    inline float readLinear(double d) const {
        const double dMax = static_cast<double>(len_ - kGuard);
        double dc = !(d > 1.0) ? 1.0 : (d > dMax ? dMax : d);
        const int id = static_cast<int>(dc);
        const double a = dc - static_cast<double>(id);
        const double x0 = tap(id);
        const double x1 = tap(id + 1);
        return static_cast<float>(x0 + a * (x1 - x0));
    }

    // Largest absolute value held anywhere in the ring. The buffer's rest value is
    // exactly zero, so `clipper_delay_tests` asserts this is 0.0 after a silent
    // tail (ADR 006's teeth — a subnormal in a `float` ring is invisible in the
    // audio but is a permanent WASM denormal source).
    double maxAbsState() const {
        double m = 0.0;
        for (float v : buf_) m = std::max(m, std::fabs(static_cast<double>(v)));
        return m;
    }

private:
    // 2 taps past the requested delay for the cubic interpolator, +2 rounding.
    static constexpr int kGuard = 4;
    static constexpr int kMinLen = 8;

    // Sample written `k` calls ago (k >= 0 => the value at w_-k-... see below).
    // The cursor `w_` points at the slot the NEXT write will use, so the sample
    // written k calls ago lives at w_ - k. Conditional wrap, never `%` (docs §32).
    inline double tap(int k) const {
        int idx = w_ - k;
        if (idx < 0) idx += len_;
        if (idx < 0) idx = 0;  // unreachable for k <= len_; keeps the index total
        return static_cast<double>(buf_[static_cast<size_t>(idx)]);
    }

    std::vector<float> buf_;
    int len_ = 0;
    int w_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DELAY_LINE_H
