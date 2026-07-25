// Clipper — test support: the STEP / STEADY-SLEW metric for a parameter change.
//
// WHY THIS EXISTS
// The 2026-07-24 audit's finding 6 ("no parameter smoothing anywhere in the valve amps")
// is stated as a table of "step / steady slew" ratios, and finding 12's native declick
// gap is stated in the same units. The definition itself lived only inside
// native/tests/chain_edit_test.cpp, which the core suites cannot link against — so the
// core had no way to assert the property the audit measured. This header is that
// definition, once, so the valve amps, the pedals and the native chain cannot drift on
// what "a click" means.
//
// THE METRIC
// Drive the unit with a STEADY sine. `slew` is the largest sample-to-sample step the
// output makes on its own, measured over a long window BEFORE the change. `seam` is the
// largest sample-to-sample step in a window AROUND the change. The ratio seam/slew is
// what gets reported: 1.0x means the change is invisible inside the signal's own motion,
// and a click shows up as a large multiple.
//
// TWO THINGS THIS DEFINITION GETS RIGHT, AND WHY BOTH MATTER
//
//  1. THE CHANGE LANDS MID-RENDER, between blocks, exactly as a UI or host-automation
//     write does. A change applied before rendering starts cannot produce a
//     discontinuity at all, so measuring one that way passes on an implementation with
//     no smoothing whatsoever (the same trap CLAUDE.md names for "no click" tests).
//
//  2. IT IS THE WORST OF SEVERAL STEP POSITIONS. A multiplicative knob step scales the
//     signal, so its discontinuity is proportional to |x| at the instant it lands: a
//     step delivered at an output zero crossing produces NO step at all. With one fixed
//     step position the measurement is a lottery on the group delay and the tone-stack
//     phase — measured 1.35x vs 8.28x for the same AC30 knob at two frequencies, purely
//     from where in the cycle the change happened to land. Sweeping the position over
//     more than a full cycle and keeping the worst is the honest number, because the
//     player can turn the knob at any point in the waveform.
//
//  3. THE DENOMINATOR IS THE LARGER OF THE TWO STEADY SLEWS — before AND after the
//     change. A knob move is SUPPOSED to change the level, and a big move on an audio
//     taper changes it a lot: VOLUME 0.5 -> 0.9 on the valve amps' (e^{4x}-1)/(e^4-1)
//     taper is +14.5 dB, so once a perfectly-smoothed ramp has landed, the signal's own
//     slew is 5.6x what it was. Dividing by the slew BEFORE would score that legitimate
//     new level as a 5.6x "click" and would fail an implementation doing exactly the
//     right thing (measured: Twin VOLUME 5.32x, all of it new level). This is
//     chain_edit_test's case 3 in a different guise — "the fade must reopen at the NEW
//     chain's level, and comparing against the OLD level would be wrong".
//
// A SMOOTH WAVEFORM IS LOAD-BEARING (the same note chain_edit_test carries). Run this
// through a cranked amp and the output is nearly a square wave whose own edges step by
// 0.8 between samples, so a click of 0.1 hides completely inside `slew` and ANY
// implementation passes. Keep the level low enough that the unit is not slamming its
// clipping mechanism, and a discontinuity has nowhere to hide.
//
// Portable C++17, header-only, no platform includes — same rules as core/.

#ifndef CLIPPER_TESTS_SUPPORT_STEP_SLEW_H
#define CLIPPER_TESTS_SUPPORT_STEP_SLEW_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace clipper::test {

// Measurement geometry. 220 Hz and the ±window shape are chain_edit_test's, so the two
// suites' numbers are directly comparable.
struct StepSlewConfig {
    double sampleRate = 48000.0;
    double toneHz = 220.0;      // steady probe tone
    double amplitude = 0.015;   // volts at the amp input: a real pluck, well short of
                                // slamming the power section (see the note above)
    int blockSize = 64;         // the change arrives between blocks
    int preFrames = 24000;      // steady state before the earliest change position
    int postFrames = 14000;     // room for the latest change position + its window
    int positions = 16;         // step positions tried; the worst is reported
    int seamBefore = 200;       // seam window, samples before the change
    int seamAfter = 1200;       // ... and after (covers an 8 ms smoother 3x over)
    int slewWindow = 8000;      // length of each steady-slew measurement window
    int settleAfter = 2400;     // samples to skip after the change before calling the
                                // NEW steady state settled (50 ms = 6 tau at 8 ms)
};

struct StepSlewResult {
    double slewBefore = 0.0;  // the signal's own steady max |y[n] - y[n-1]|, pre-change
    double slewAfter = 0.0;   // ... and once the change has settled (a knob move is
                              // ALLOWED to change the level; see note 3 in the header)
    double slew = 0.0;        // the denominator: max(slewBefore, slewAfter)
    double seam = 0.0;        // the worst max |Δ| at a change position
    double ratio = 0.0;       // seam / slew
    int worstPosition = 0;    // which step position produced it (index, not samples)
};

// Largest |y[n] - y[n-1]| over [from, to).
inline double maxStep(const std::vector<float>& x, int from, int to) {
    double m = 0.0;
    const int hi = std::min(to, static_cast<int>(x.size()));
    for (int i = std::max(1, from); i < hi; ++i)
        m = std::max(m, std::fabs(static_cast<double>(x[static_cast<size_t>(i)]) -
                                  static_cast<double>(x[static_cast<size_t>(i - 1)])));
    return m;
}

inline std::vector<float> steadyTone(const StepSlewConfig& c) {
    const int n = c.preFrames + c.postFrames;
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t>(i)] = static_cast<float>(
            c.amplitude * std::sin(2.0 * M_PI * c.toneHz * (i / c.sampleRate)));
    return x;
}

// Render the steady tone through a freshly-prepared unit, applying `applyChange` at the
// first block boundary at or after `changeAt`.
//
//   prepareUnit : construct + prepare + set the STARTING parameter values
//   applyChange : write the new parameter value (called between blocks, once)
//   processBlock : render one block, in -> out
//
// The three callbacks close over the caller's unit; this header deliberately knows
// nothing about any model type.
inline std::vector<float> renderWithChange(const StepSlewConfig& c,
                                           const std::vector<float>& in,
                                           const std::function<void()>& prepareUnit,
                                           const std::function<void()>& applyChange,
                                           const std::function<void(const float*, float*, int)>&
                                               processBlock,
                                           int changeAt) {
    prepareUnit();
    const int n = static_cast<int>(in.size());
    std::vector<float> out(in.size(), 0.0f);
    bool applied = false;
    for (int off = 0; off < n; off += c.blockSize) {
        const int k = std::min(c.blockSize, n - off);
        if (!applied && off >= changeAt) {
            applyChange();
            applied = true;
        }
        processBlock(in.data() + off, out.data() + off, k);
    }
    return out;
}

// The metric: worst step/slew ratio over `c.positions` successive block-boundary change
// positions. `applyChange` may be called many times across the sweep (once per render).
inline StepSlewResult measureStepSlew(const StepSlewConfig& c,
                                      const std::function<void()>& prepareUnit,
                                      const std::function<void()>& applyChange,
                                      const std::function<void(const float*, float*, int)>&
                                          processBlock) {
    const std::vector<float> in = steadyTone(c);
    StepSlewResult worst;
    for (int p = 0; p < c.positions; ++p) {
        const int at = c.preFrames + p * c.blockSize;
        const std::vector<float> out =
            renderWithChange(c, in, prepareUnit, applyChange, processBlock, at);
        // The signal's own slew before the change, from a long window that ends well
        // before the earliest change position (the same reference for every position)...
        const double before = maxStep(out, c.preFrames - c.slewWindow, c.preFrames - 100);
        // ... and after it has settled, which is the level the knob ASKED for.
        const double after =
            maxStep(out, at + c.settleAfter, at + c.settleAfter + c.slewWindow);
        const double slew = std::max(before, after);
        const double seam = maxStep(out, at - c.seamBefore, at + c.seamAfter);
        const double ratio = slew > 0.0 ? seam / slew : 0.0;
        if (ratio > worst.ratio) {
            worst.slewBefore = before;
            worst.slewAfter = after;
            worst.slew = slew;
            worst.seam = seam;
            worst.ratio = ratio;
            worst.worstPosition = p;
        }
    }
    return worst;
}

// SENSITIVITY REFERENCE: what the same change costs with no smoothing of any kind —
// the two settings rendered separately and spliced at the change point, which is
// literally what "assign the knob straight to state" produces at its worst. Any bar
// worth asserting has to be one this blows past (chain_edit_test's case 2 discipline).
inline StepSlewResult measureHardSpliceStepSlew(
    const StepSlewConfig& c, const std::function<void()>& prepareAtOld,
    const std::function<void()>& prepareAtNew,
    const std::function<void(const float*, float*, int)>& processBlock) {
    const std::vector<float> in = steadyTone(c);
    // Two static renders. The change position sweep is the same as above: the splice
    // point moves, because a splice at a zero crossing is invisible too.
    const std::vector<float> a =
        renderWithChange(c, in, prepareAtOld, [] {}, processBlock, c.preFrames + c.postFrames);
    const std::vector<float> b =
        renderWithChange(c, in, prepareAtNew, [] {}, processBlock, c.preFrames + c.postFrames);
    // Same denominator as measureStepSlew: the larger of the two steady slews, so the
    // sensitivity case is scored on exactly the scale block A is scored on.
    StepSlewResult worst;
    worst.slewBefore = maxStep(a, c.preFrames - c.slewWindow, c.preFrames - 100);
    worst.slewAfter = maxStep(b, c.preFrames - c.slewWindow, c.preFrames - 100);
    const double slew = std::max(worst.slewBefore, worst.slewAfter);
    worst.slew = slew;
    for (int p = 0; p < c.positions; ++p) {
        const size_t at = static_cast<size_t>(c.preFrames + p * c.blockSize);
        const double step = std::fabs(static_cast<double>(b[at]) - static_cast<double>(a[at - 1]));
        const double ratio = slew > 0.0 ? step / slew : 0.0;
        if (ratio > worst.ratio) {
            worst.seam = step;
            worst.ratio = ratio;
            worst.worstPosition = p;
        }
    }
    return worst;
}

}  // namespace clipper::test

#endif  // CLIPPER_TESTS_SUPPORT_STEP_SLEW_H
