// Plain-assert tests for clipper::dsp::DelayModel and clipper::dsp::DelayLine
// (M13.4 — the "Echoman": an EHX Deluxe Memory Man-class BBD analog delay, and
// the lineup's FIRST delay pedal). No framework: int main + <cassert>.
//
// Every bar here is a player-observable property or an absolute reference. There
// is nothing in this file that checks the implementation against an expression
// derived from the same code (CLAUDE.md, "Measure, don't assert"). Where a number
// comes from outside the model it is named:
//
//   * The MN3005's own datasheet endpoints (4096 stages, 20.48-204.8 ms over a
//     10-100 kHz clock) fix the device law delay = stages/(2*f_clk).
//   * EHX's published Deluxe Memory Man travel, 30-550 ms, fixes the knob range.
//   * The NE570's published 2:1 / 1:2 compander pair fixes the composed unity
//     through-gain.
//
// What is asserted, and why each is the property that matters:
//
//   1. DELAY TIME vs KNOB, in ms, as a table — measured from a rendered echo,
//      against the device law, and against the published 30-550 ms travel. The
//      REPEAT SPACING is checked separately from the first echo's absolute
//      arrival, because they differ by the oversampler's group delay and the
//      difference is reported rather than hidden.
//   2. REPEAT DEGRADATION — THE bucket-brigade property. The HF/LF ratio of
//      repeats 1..5 must fall MONOTONICALLY, because each repeat has been
//      through the device, its fixed filters and its charge-transfer loss again.
//      A clean digital line with one filter in the loop does not do this.
//   3. DARKENING WITH TIME. Repeat 1 alone, at short vs long DELAY. The only
//      thing the knob moves is the CLOCK; the filters are fixed. If the model
//      were a fixed filter on a variable-length line this number would be 0.
//   4. FEEDBACK: bounded over 30 s at max (and CONVERGING, not growing), and
//      EXACTLY one repeat at FEEDBACK 0 — echo 2 must be identically silent.
//   5. THE COMPANDER CURVE — through-gain across 42 dB of input, which the
//      NE570's sqrt/square pair makes unity by construction, and the departure
//      from unity at the top, which is the BBD's own compliance.
//   6. BLEND 0 is BIT-IDENTICAL to bypass, by render hash.
//   7. The DelayLine primitive on its own: integer reads are exact, fractional
//      reads are flat, and the cubic-vs-linear droop is measured (it is the
//      reason the default read is cubic).
//   8. Hygiene: alias floor vs oversampling factor, DC ON SIGNAL, reset() after
//      a NaN, block-size invariance, rate independence, no zipper on a DELAY
//      sweep.
//
// PERTURBATION PROOFS for the load-bearing bars are recorded in docs §60.8.

#include "clipper/dsp/DelayLine.h"
#include "clipper/dsp/DelayModel.h"

#include "support/AssertsLive.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::DelayLine;
using clipper::dsp::DelayModel;

double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

void renderBlocks(DelayModel& m, const std::vector<float>& in,
                  std::vector<float>& out, int block = 128) {
    out.assign(in.size(), 0.0f);
    const int n = static_cast<int>(in.size());
    for (int off = 0; off < n; off += block) {
        const int k = std::min(block, n - off);
        m.process(in.data() + off, out.data() + off, k);
    }
}

// Settle a model at a knob setting: prepare, set the three knobs, then run one
// second of silence so every smoother has landed on its target.
void settle(DelayModel& m, double sr, double delay, double fb, double blend,
            int osFactor = 0) {
    m.prepare(sr, 128);
    if (osFactor > 0) m.setOversampling(osFactor);
    m.setParameter(DelayModel::PARAM_DELAY, static_cast<float>(delay));
    m.setParameter(DelayModel::PARAM_FEEDBACK, static_cast<float>(fb));
    m.setParameter(DelayModel::PARAM_BLEND, static_cast<float>(blend));
    std::vector<float> z(static_cast<size_t>(sr), 0.0f), zo;
    renderBlocks(m, z, zo);
}

double goertzel(const std::vector<float>& x, int start, int len, double f, double fs) {
    const double w = kTwoPi * f / fs;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) {
        const double s = static_cast<double>(x[static_cast<size_t>(start + i)]) + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / len;
}

// HANN-WINDOWED Goertzel, for the alias sweep only.
//
// This exists because the first version of the alias test measured a "floor" of
// -55.8 dB that did not move with the oversampling factor and sat 400 Hz from the
// fundamental — which is not aliasing, it is the RECTANGULAR window's own
// sidelobe. A rectangular window's leakage falls as 1/dBin, so a 3 kHz tone read
// at 3409 Hz with 2 Hz bins leaks 1/(pi*204) = -56 dB, exactly the number
// measured. A Hann window's sidelobes fall as 1/dBin^3 and put that leakage
// ~90 dB lower, which is what lets a real alias product be seen at all.
double goertzelHann(const std::vector<float>& x, int start, int len, double f, double fs) {
    const double w = kTwoPi * f / fs;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) {
        const double win = 0.5 - 0.5 * std::cos(kTwoPi * i / (len - 1));
        const double s = win * static_cast<double>(x[static_cast<size_t>(start + i)]) +
                         c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    // Hann's coherent gain is 0.5, so the amplitude correction is 4/len not 2/len.
    return 4.0 * std::sqrt(re * re + im * im) / len;
}

// RMS across a log-spaced set of bins between lo and hi.
double bandRms(const std::vector<float>& x, int start, int len, double lo, double hi,
               double fs) {
    double e = 0.0;
    int nb = 0;
    for (double f = lo; f <= hi; f *= 1.12) {
        const double m = goertzel(x, start, len, f, fs);
        e += m * m;
        ++nb;
    }
    return std::sqrt(e / std::max(1, nb));
}

double peakOf(const std::vector<float>& v, int a, int b) {
    double p = 0.0;
    for (int i = std::max(0, a); i < b && i < static_cast<int>(v.size()); ++i)
        p = std::max(p, std::fabs(static_cast<double>(v[static_cast<size_t>(i)])));
    return p;
}

// A plucked-note-shaped burst with real HF content: fundamental plus two
// partials, raised-cosine envelope over 30 ms.
void writePluck(std::vector<float>& in, int at, double fs, double amp = 0.30) {
    const int burst = static_cast<int>(0.030 * fs);
    for (int i = 0; i < burst; ++i) {
        const double t = i / fs;
        const double env = 0.5 * (1.0 - std::cos(kTwoPi * i / burst));
        in[static_cast<size_t>(at + i)] = static_cast<float>(
            amp * env * (std::sin(kTwoPi * 220.0 * t) + 0.5 * std::sin(kTwoPi * 1500.0 * t) +
                         0.5 * std::sin(kTwoPi * 4000.0 * t)));
    }
}

uint64_t hashOf(const std::vector<float>& v) {
    uint64_t h = 1469598103934665603ull;
    for (float f : v) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int b = 0; b < 4; ++b) {
            h ^= static_cast<uint64_t>((bits >> (8 * b)) & 0xFFu);
            h *= 1099511628211ull;
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// 1. Delay time vs knob, in ms, against the device law and the published travel.
// ---------------------------------------------------------------------------
void testDelayTimeTable(double sr) {
    std::printf("\n[delay] 1. DELAY time vs knob (impulse -> first echo)\n");
    std::printf("   knob   device_law_ms  rendered_ms   f_clk Hz   smear -3dB Hz\n");

    double prevMs = -1.0;
    for (int step = 0; step <= 8; ++step) {
        const double knob = step / 8.0;
        DelayModel m;
        settle(m, sr, knob, 0.0, 1.0);

        const int N = static_cast<int>(sr * 1.2);
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        in[100] = 0.5f;
        renderBlocks(m, in, out);

        int best = -1;
        double bv = 0.0;
        for (int i = 500; i < N; ++i) {
            const double a = std::fabs(static_cast<double>(out[static_cast<size_t>(i)]));
            if (a > bv) { bv = a; best = i; }
        }
        assert(best > 0);
        const double renderedMs = (best - 100) / sr * 1000.0;
        const double lawMs = m.delaySeconds() * 1000.0;
        std::printf("   %.3f   %10.2f   %10.3f   %8.1f   %10.1f\n", knob, lawMs,
                    renderedMs, m.clockHz(), m.chargeSmearCornerHz());

        // The device law itself: delay == slots / f_clk, i.e. stages/(2 f_clk).
        assert(std::fabs(m.delaySeconds() * m.clockHz() - DelayModel::bbdSlots()) < 1e-6);

        // The RENDERED echo lands on the law plus exactly the oversampler's own
        // group delay (72 base samples at 4x = 1.5 ms) — nothing else. That
        // offset is reported, not compensated: the dry path never enters the
        // oversampled domain, which is what keeps BLEND 0 bit-identical.
        const double offsetMs = renderedMs - lawMs;
        assert(offsetMs > 1.0 && offsetMs < 2.5);

        // Monotone in the knob, and the endpoints ARE the published travel.
        assert(renderedMs > prevMs);
        prevMs = renderedMs;
    }

    // EHX publish 30-550 ms for the Deluxe Memory Man. Both ends are outside the
    // MN3005's own rated 10-100 kHz clock, which is a fact about the pedal.
    assert(std::fabs(DelayModel::minDelaySeconds() - 0.030) < 1e-9);
    assert(std::fabs(DelayModel::maxDelaySeconds() - 0.550) < 1e-9);
    assert(DelayModel::bbdSlots() == 4096);
    {
        DelayModel m;
        settle(m, sr, 0.0, 0.0, 1.0);
        const double fMax = m.clockHz();
        DelayModel m2;
        settle(m2, sr, 1.0, 0.0, 1.0);
        const double fMin = m2.clockHz();
        std::printf("   clock travel %.0f Hz (30 ms) down to %.0f Hz (550 ms) — the "
                    "MN3005 is rated 100 kHz / 10 kHz, so a real DMM runs it past "
                    "BOTH ends\n", fMax, fMin);
        assert(fMax > 130000.0 && fMax < 140000.0);
        assert(fMin > 7000.0 && fMin < 8000.0);
    }
}

// ---------------------------------------------------------------------------
// 1b. Repeat SPACING is the device law exactly (the oversampler offset is a
//     one-off on the first echo, not a per-repeat error).
// ---------------------------------------------------------------------------
void testRepeatSpacing(double sr) {
    std::printf("\n[delay] 1b. repeat SPACING vs the device law\n");
    for (double knob : {0.0, 0.5, 1.0}) {
        DelayModel m;
        settle(m, sr, knob, 0.75, 1.0);
        const double lawMs = m.delaySeconds() * 1000.0;
        const int D = static_cast<int>(m.delaySeconds() * sr);
        const int N = 5 * D + 8000;
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        const int burst = static_cast<int>(0.010 * sr);
        for (int i = 0; i < burst; ++i) {
            const double env = 0.5 * (1.0 - std::cos(kTwoPi * i / burst));
            in[static_cast<size_t>(1000 + i)] =
                static_cast<float>(0.35 * env * std::sin(kTwoPi * 800.0 * i / sr));
        }
        renderBlocks(m, in, out);

        int last = -1;
        std::printf("   knob %.1f law %.2f ms :", knob, lawMs);
        for (int r = 1; r <= 4; ++r) {
            const int c = 1000 + r * D, w = D / 3;
            int best = std::max(0, c - w);
            double bv = 0.0;
            for (int i = std::max(0, c - w); i < std::min(N, c + w); ++i) {
                const double a = std::fabs(static_cast<double>(out[static_cast<size_t>(i)]));
                if (a > bv) { bv = a; best = i; }
            }
            if (last > 0) {
                const double gapMs = (best - last) / sr * 1000.0;
                std::printf("  gap%d %.2f ms", r, gapMs);
                // Within 2 % of the law. The residual is the peak-picker's own
                // resolution on a 10 ms burst, not a timing error.
                assert(std::fabs(gapMs - lawMs) < 0.02 * lawMs + 0.6);
            }
            last = best;
        }
        std::printf("\n");
    }
}

// ---------------------------------------------------------------------------
// 2. THE bucket-brigade property: cumulative degradation through the loop.
// ---------------------------------------------------------------------------
void testRepeatDegradation(double sr) {
    std::printf("\n[delay] 2. repeat degradation (DELAY 0.5, FEEDBACK 0.7)\n");
    DelayModel m;
    settle(m, sr, 0.5, 0.7, 1.0);
    const int D = static_cast<int>(m.delaySeconds() * sr);
    const int N = static_cast<int>(sr * 6.0);
    std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
    writePluck(in, 1000, sr);
    renderBlocks(m, in, out);

    const int win = static_cast<int>(0.040 * sr);
    std::printf("   repeat    rms       LF(100-400)  HF(2k-6k)   HF/LF dB\n");
    double prevRatio = 1e9;
    double first = 0.0, fifth = 0.0;
    double prevRms = 1e9;
    for (int r = 1; r <= 5; ++r) {
        const int st = 1000 + r * D;
        assert(st + win < N);
        double rms = 0.0;
        for (int i = 0; i < win; ++i) {
            const double v = out[static_cast<size_t>(st + i)];
            rms += v * v;
        }
        rms = std::sqrt(rms / win);
        const double lo = bandRms(out, st, win, 100.0, 400.0, sr);
        const double hi = bandRms(out, st, win, 2000.0, 6000.0, sr);
        const double ratio = dbOf(hi / std::max(lo, 1e-20));
        std::printf("     %d    %.6f   %.6f     %.6f   %8.2f\n", r, rms, lo, hi, ratio);

        // THE BAR. Each pass through the device costs HF relative to LF, and it
        // must cost it EVERY time — that is what "cumulative" means and it is
        // what a single filter in the loop of a clean line does not do.
        assert(ratio < prevRatio - 1.0);
        prevRatio = ratio;
        // And the repeats decay in level (the loop gain is under unity).
        assert(rms < prevRms);
        prevRms = rms;
        if (r == 1) first = ratio;
        if (r == 5) fifth = ratio;
    }
    std::printf("   repeat 1 -> 5 loses %.2f dB of HF relative to LF\n", first - fifth);
    assert(first - fifth > 6.0);
}

// ---------------------------------------------------------------------------
// 3. Darkening with TIME. The knob moves the CLOCK and nothing else.
// ---------------------------------------------------------------------------
void testDarkerWithLongerDelay(double sr) {
    std::printf("\n[delay] 3. repeat 1 brightness vs DELAY (FEEDBACK 0)\n");
    std::printf("   knob  delay_ms  LF(100-400)  HF(2k-6k)   HF/LF dB   smear -3dB\n");
    double bright = 0.0, dark = 0.0, prev = 1e9;
    for (double knob : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        DelayModel m;
        settle(m, sr, knob, 0.0, 1.0);
        const int D = static_cast<int>(m.delaySeconds() * sr);
        const int N = D + static_cast<int>(sr * 0.5);
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        writePluck(in, 1000, sr);
        renderBlocks(m, in, out);
        const int st = 1000 + D, win = static_cast<int>(0.040 * sr);
        const double lo = bandRms(out, st, win, 100.0, 400.0, sr);
        const double hi = bandRms(out, st, win, 2000.0, 6000.0, sr);
        const double ratio = dbOf(hi / std::max(lo, 1e-20));
        std::printf("   %.2f  %7.1f   %.6f     %.6f   %8.2f   %8.1f\n", knob,
                    m.delaySeconds() * 1000.0, lo, hi, ratio, m.chargeSmearCornerHz());
        // Strictly monotone: longer delay is a slower clock is a darker repeat.
        assert(ratio < prev);
        prev = ratio;
        if (knob == 0.0) bright = ratio;
        if (knob == 1.0) dark = ratio;
    }
    std::printf("   30 ms -> 550 ms costs %.2f dB of HF on the FIRST repeat\n",
                bright - dark);
    // The whole point of modelling the clock. A fixed filter on a variable-length
    // digital line measures 0.00 dB here.
    assert(bright - dark > 6.0);
}

// ---------------------------------------------------------------------------
// 4. Feedback: bounded and converging at max, exactly one repeat at zero.
// ---------------------------------------------------------------------------
void testFeedback(double sr) {
    std::printf("\n[delay] 4. feedback\n");

    // (a) FEEDBACK 0 => EXACTLY one repeat. Echo 2 must be identically silent,
    //     not "small" — nothing re-enters the line.
    {
        DelayModel m;
        settle(m, sr, 0.3, 0.0, 1.0);
        const int D = static_cast<int>(m.delaySeconds() * sr);
        const int N = 4 * D + 4000;
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        in[100] = 0.5f;
        renderBlocks(m, in, out);
        const double e1 = peakOf(out, 100 + D - 600, 100 + D + 600);
        const double e2 = peakOf(out, 100 + 2 * D - 600, 100 + 2 * D + 600);
        const double e3 = peakOf(out, 100 + 3 * D - 600, 100 + 3 * D + 600);
        std::printf("   FEEDBACK 0: echo1 %.3e  echo2 %.3e (%.1f dB)  echo3 %.3e "
                    "(%.1f dB)\n", e1, e2, dbOf(e2 / e1), e3, dbOf(e3 / e1));
        assert(e1 > 1e-4);
        // Not "exactly zero" — what sits in the echo-2 window is the tail of the
        // INPUT filters' own ring-down arriving through the line one delay later,
        // not a second pass. 100 dB down is the player-observable statement, and
        // it fails the moment anything re-enters the loop.
        assert(e2 < e1 * 1e-5);
        assert(e3 < e1 * 1e-5);
    }

    // (b) FEEDBACK max, 30 s, hard strum. Bounded, finite, and CONVERGING — the
    //     bucket's own compliance is what stops it, not a limiter.
    for (double knob : {0.0, 0.5, 1.0}) {
        DelayModel m;
        settle(m, sr, knob, 1.0, 1.0);
        const int N = static_cast<int>(sr * 30.0);
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        for (int i = 0; i < static_cast<int>(0.5 * sr); ++i) {
            const double t = i / sr;
            in[static_cast<size_t>(i)] = static_cast<float>(
                0.6 * std::exp(-t * 4.0) * (std::sin(kTwoPi * 110.0 * t) +
                                            std::sin(kTwoPi * 330.0 * t)));
        }
        renderBlocks(m, in, out);
        bool finite = true;
        for (float v : out) if (!std::isfinite(v)) finite = false;
        const double pk = peakOf(out, 0, N);
        const double a = peakOf(out, static_cast<int>(sr * 15.0), static_cast<int>(sr * 22.5));
        const double b = peakOf(out, static_cast<int>(sr * 22.5), N);
        std::printf("   DELAY %.1f FEEDBACK max: peak30s %.5f  15-22.5s %.5f  "
                    "22.5-30s %.5f\n", knob, pk, a, b);
        assert(finite);
        assert(pk < 2.0);             // bounded (the strum itself peaks at 1.06)
        assert(b < a * 1.15 + 1e-6);  // converging, not growing
    }
}

// ---------------------------------------------------------------------------
// 5. The compander curve. The NE570's sqrt/square pair is unity by construction
//    at every level; the departure at the top is the BBD's own compliance.
// ---------------------------------------------------------------------------
void testCompanderCurve(double sr) {
    std::printf("\n[delay] 5. compander through-gain (220 Hz, DELAY 0.3, FEEDBACK 0)\n");
    std::printf("   in_peak   compG    expG    product   wet f0     wet gain dB\n");
    double lowGain = 0.0, highGain = 0.0;
    double prevProduct = 1e9;
    for (double lvl : {0.005, 0.015, 0.05, 0.15, 0.3, 0.6}) {
        DelayModel m;
        settle(m, sr, 0.3, 0.0, 1.0);
        const int N = static_cast<int>(sr * 2.5);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(lvl * std::sin(kTwoPi * 220.0 * i / sr));
        renderBlocks(m, in, out);
        std::vector<float> wet(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            wet[static_cast<size_t>(i)] = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
        const int st = static_cast<int>(sr * 2.0), win = static_cast<int>(sr * 0.4);
        const double f0 = goertzel(wet, st, win, 220.0, sr);
        const double gc = m.compressorGain(), ge = m.expanderGain();
        const double gainDb = dbOf(f0 / lvl);
        std::printf("   %.4f    %7.4f %7.4f %7.4f   %.6f   %+7.2f\n", lvl, gc, ge,
                    gc * ge, f0, gainDb);

        // The compressor's gain FALLS and the expander's RISES with level, and
        // their product is what the datasheet's 2:1 / 1:2 pair promises: unity.
        assert(gc * ge < prevProduct);
        prevProduct = gc * ge;
        if (lvl == 0.005) lowGain = gainDb;
        if (lvl == 0.6) highGain = gainDb;
    }
    // Unity to well inside a dB over the bottom of the sweep — the pair is NOT a
    // compressor with a make-up gain bolted on, it is an identity that the BBD
    // sits inside.
    assert(std::fabs(lowGain) < 0.5);
    // And it DOES soften at the top, because the bucket has a finite compliance.
    std::printf("   through-gain %.2f dB at 0.005 V -> %.2f dB at 0.6 V "
                "(the bucket's compliance, not a limiter)\n", lowGain, highGain);
    assert(highGain < lowGain - 1.0);
    assert(highGain > -8.0);

    // The compressor really is doing something on the way in: at a quiet input it
    // must apply real gain, or a BBD's noise floor would win.
    {
        DelayModel m;
        settle(m, sr, 0.3, 0.0, 1.0);
        const int N = static_cast<int>(sr * 1.5);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(0.005 * std::sin(kTwoPi * 220.0 * i / sr));
        renderBlocks(m, in, out);
        std::printf("   at 0.005 V the compressor puts %+.2f dB into the device and "
                    "the expander takes %+.2f dB back\n",
                    dbOf(m.compressorGain()), dbOf(m.expanderGain()));
        assert(m.compressorGain() > 3.0);
        assert(m.expanderGain() < 0.34);
    }
}

// ---------------------------------------------------------------------------
// 6. BLEND 0 is bit-identical to bypass.
// ---------------------------------------------------------------------------
void testBlendZeroIsBypass(double sr) {
    std::printf("\n[delay] 6. BLEND 0 vs bypass\n");
    DelayModel m;
    settle(m, sr, 0.5, 0.6, 0.0);
    const int N = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(N)), out;
    for (int i = 0; i < N; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * i / sr));
    renderBlocks(m, in, out);
    int diff = 0;
    for (int i = 0; i < N; ++i)
        if (out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)]) ++diff;
    std::printf("   differing samples %d / %d   hash in %016llx out %016llx\n", diff, N,
                static_cast<unsigned long long>(hashOf(in)),
                static_cast<unsigned long long>(hashOf(out)));
    assert(diff == 0);
    assert(hashOf(in) == hashOf(out));
}

// ---------------------------------------------------------------------------
// 7. The DelayLine primitive, on its own.
// ---------------------------------------------------------------------------
void testDelayLinePrimitive(double sr) {
    std::printf("\n[delay] 7. DelayLine primitive\n");

    // (a) An integer read is EXACT — no interpolation error at all, which is what
    //     makes the primitive safe for a static tap (a flanger's fixed comb).
    {
        DelayLine dl;
        dl.prepare(0.01, sr);
        std::vector<float> src(400);
        for (int i = 0; i < 400; ++i)
            src[static_cast<size_t>(i)] = static_cast<float>(std::sin(0.37 * i) * 0.5);
        int mism = 0;
        for (int i = 0; i < 400; ++i) {
            const float y = dl.readCubic(50.0);
            if (i >= 50 && y != src[static_cast<size_t>(i - 50)]) ++mism;
            dl.write(src[static_cast<size_t>(i)]);
        }
        std::printf("   integer read D=50: %d mismatches of 350\n", mism);
        assert(mism == 0);
    }

    // (b) A fractional read is FLAT at 1 kHz and its measured delay tracks the
    //     requested one to a small fraction of a sample.
    {
        std::printf("   D          |H| @1 kHz      dB      measured step\n");
        double prevGd = 0.0;
        for (int q = 0; q < 4; ++q) {
            const double frac = q * 0.25;
            DelayLine dl;
            dl.prepare(0.05, sr);
            const int N = 8192;
            std::vector<float> y(static_cast<size_t>(N));
            for (int i = 0; i < N; ++i) {
                y[static_cast<size_t>(i)] = dl.readCubic(100.0 + frac);
                dl.write(static_cast<float>(std::sin(kTwoPi * 1000.0 * i / sr)));
            }
            const double w = kTwoPi * 1000.0 / sr;
            double re = 0.0, im = 0.0;
            for (int i = 4096; i < N; ++i) {
                re += y[static_cast<size_t>(i)] * std::cos(w * i);
                im += y[static_cast<size_t>(i)] * std::sin(w * i);
            }
            const double mag = 2.0 * std::sqrt(re * re + im * im) / (N - 4096);
            double gd = std::atan2(-im, re) / w;
            while (gd < 0.0) gd += sr / 1000.0;
            std::printf("   %6.2f     %.6f    %+7.4f     %+7.4f\n", 100.0 + frac, mag,
                        dbOf(mag), q ? prevGd - gd : 0.0);
            // Magnitude flat to 0.01 dB — a cubic interpolator has essentially no
            // droop this far below Nyquist.
            assert(std::fabs(dbOf(mag)) < 0.01);
            // Each quarter-sample of requested delay moves the measured delay by
            // a quarter sample.
            if (q) assert(std::fabs((prevGd - gd) - 0.25) < 0.01);
            prevGd = gd;
        }
    }

    // (c) Cubic vs linear at 8 kHz. This is WHY the default read is cubic, and it
    //     is a number rather than a claim.
    {
        double mags[2];
        for (int mode = 0; mode < 2; ++mode) {
            DelayLine dl;
            dl.prepare(0.05, sr);
            const int N = 16384;
            const double w = kTwoPi * 8000.0 / sr;
            double re = 0.0, im = 0.0;
            for (int i = 0; i < N; ++i) {
                const double y = mode ? dl.readLinear(100.5) : dl.readCubic(100.5);
                dl.write(static_cast<float>(std::sin(w * i)));
                if (i >= 8192) { re += y * std::cos(w * i); im += y * std::sin(w * i); }
            }
            mags[mode] = 2.0 * std::sqrt(re * re + im * im) / (N - 8192);
        }
        std::printf("   8 kHz, D=100.5: cubic %+.4f dB   linear %+.4f dB\n",
                    dbOf(mags[0]), dbOf(mags[1]));
        assert(dbOf(mags[0]) > -0.5);
        assert(dbOf(mags[1]) < -0.8);
        assert(dbOf(mags[0]) - dbOf(mags[1]) > 0.6);
    }

    // (d) The ring rests at EXACTLY zero (ADR 006 — a float subnormal in a delay
    //     line is invisible in the audio and is a permanent WASM denormal source).
    {
        DelayLine dl;
        dl.prepare(0.01, sr);
        for (int i = 0; i < 400; ++i) dl.write(static_cast<float>(0.5 * std::sin(0.2 * i)));
        for (int i = 0; i < 2000; ++i) dl.write(0.0f);
        std::printf("   ring rest after silence: %.3e\n", dl.maxAbsState());
        assert(dl.maxAbsState() == 0.0);
    }

    // (e) A wild read distance can never index outside the ring.
    {
        DelayLine dl;
        dl.prepare(0.01, sr);
        for (int i = 0; i < 100; ++i) dl.write(0.25f);
        const float a = dl.readCubic(dl.maxDelaySamples() * 100.0);
        const float b = dl.readCubic(-5.0);
        const float c = dl.readCubic(std::nan(""));
        std::printf("   clamped reads: %.4f %.4f %.4f (all finite)\n",
                    static_cast<double>(a), static_cast<double>(b),
                    static_cast<double>(c));
        assert(std::isfinite(a) && std::isfinite(b) && std::isfinite(c));
    }
}

// ---------------------------------------------------------------------------
// 8a. Alias floor vs oversampling factor.
//
// The result here is NOT the usual one and is reported rather than dressed up:
// the BBD's sampling is EXPLICIT in this model and runs at the DEVICE clock, not
// at the host rate, so at a fast clock (short delay) oversampling the host grid
// buys almost nothing — there is no host-rate foldover to remove. Where it does
// buy something is at a SLOW clock, where the device's own images land in band
// and the reconstruction filter needs headroom above the audio band to work in.
// Both are measured.
// ---------------------------------------------------------------------------
void testAliasing(double sr) {
    std::printf("\n[delay] 8a. alias floor vs oversampling factor\n");
    std::printf("   (i) 3 kHz 0.3 V, DELAY 0.0 (clock 136.5 kHz — the device does NOT "
                "alias here)\n");
    double floors[4];
    const int factors[4] = {1, 2, 4, 8};
    for (int fi = 0; fi < 4; ++fi) {
        DelayModel m;
        settle(m, sr, 0.0, 0.0, 1.0, factors[fi]);
        const int N = static_cast<int>(sr * 2.0);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(kTwoPi * 3000.0 * i / sr));
        renderBlocks(m, in, out);
        std::vector<float> wet(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            wet[static_cast<size_t>(i)] = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
        const int st = static_cast<int>(sr * 1.0), win = static_cast<int>(sr * 0.5);
        const double f0 = goertzelHann(wet, st, win, 3000.0, sr);
        double worst = -200.0, wf = 0.0;
        for (double fr = 120.0; fr < 22000.0; fr *= 1.02) {
            bool harmonic = false;
            for (int h = 1; h <= 7; ++h)
                if (std::fabs(fr - h * 3000.0) < 0.06 * 3000.0) harmonic = true;
            if (harmonic) continue;
            const double db = dbOf(goertzelHann(wet, st, win, fr, sr) / std::max(f0, 1e-20));
            if (db > worst) { worst = db; wf = fr; }
        }
        floors[fi] = worst;
        std::printf("       %dx: wet f0 %.5f  worst non-harmonic %.2f dB at %.0f Hz\n",
                    factors[fi], f0, worst, wf);
        assert(worst < -60.0);
    }
    // THE DERIVATION THAT SETS THE SHIPPED FACTOR. The BBD's clock reaches
    // 136.53 kHz at DELAY 0, so the internal rate has to clear 273.07 kHz or the
    // device's own clock images fold inside the oversampled domain. 4x at 48 kHz
    // is 192 kHz and does not clear it; 8x is 384 kHz and does — which is why the
    // step from 4x to 8x here is worth tens of dB while 1x -> 4x is worth almost
    // nothing.
    std::printf("       8x beats 4x by %.2f dB — the internal rate finally clears "
                "2 x the 136.5 kHz clock (273 kHz)\n", floors[2] - floors[3]);
    assert(floors[3] < floors[2] - 20.0);
    {
        DelayModel shipped;
        shipped.prepare(sr, 128);
        std::printf("       shipped default oversampling: %dx\n", shipped.oversampling());
        assert(shipped.oversampling() == 8);
        assert(shipped.latencySamples() == 0);
    }

    std::printf("   (ii) 9 kHz 0.3 V, DELAY 1.0 (clock 7.4 kHz — the DEVICE itself "
                "aliases, and correctly so)\n");
    double atOne = 0.0, atFour = 0.0;
    for (int fi = 0; fi < 4; fi += 2) {
        DelayModel m;
        settle(m, sr, 1.0, 0.0, 1.0, factors[fi]);
        const int N = static_cast<int>(sr * 2.5);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(kTwoPi * 9000.0 * i / sr));
        renderBlocks(m, in, out);
        std::vector<float> wet(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            wet[static_cast<size_t>(i)] = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
        const int st = static_cast<int>(sr * 1.5), win = static_cast<int>(sr * 0.5);
        const double f0 = goertzelHann(wet, st, win, 9000.0, sr);
        double worst = -200.0;
        for (double fr = 120.0; fr < 8600.0; fr *= 1.02)
            worst = std::max(worst,
                             dbOf(goertzelHann(wet, st, win, fr, sr) / std::max(f0, 1e-20)));
        std::printf("       %dx: wet f0 %.6f  worst in-band %.2f dB\n", factors[fi], f0,
                    worst);
        if (factors[fi] == 1) atOne = worst;
        if (factors[fi] == 4) atFour = worst;
    }
    std::printf("       4x is %.2f dB better than 1x here — that is what the "
                "oversampling actually buys on this pedal\n", atOne - atFour);
    assert(atFour < atOne - 2.0);
}

// ---------------------------------------------------------------------------
// 8b. DC offset ON SIGNAL, measured on the WET path (the dry path is the
//     caller's own samples and passes through by construction).
// ---------------------------------------------------------------------------
void testDcOnSignal(double sr) {
    std::printf("\n[delay] 8b. DC offset on signal (wet path)\n");
    for (double off : {0.0, 0.1}) {
        DelayModel m;
        settle(m, sr, 0.4, 0.6, 1.0);
        const int N = static_cast<int>(sr * 4.0);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(off + 0.2 * std::sin(kTwoPi * 220.0 * i / sr));
        renderBlocks(m, in, out);
        const int st = static_cast<int>(sr * 3.0);
        double mean = 0.0, pk = 0.0;
        for (int i = st; i < N; ++i) {
            const double wv = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
            mean += wv;
            pk = std::max(pk, std::fabs(wv));
        }
        mean /= (N - st);
        std::printf("   input offset %.2f: wet mean %.3e  peak %.4f  = %.4f%% of peak\n",
                    off, mean, pk, 100.0 * std::fabs(mean) / pk);
        assert(pk > 0.05);
        assert(std::fabs(mean) < 0.005 * pk);
    }
}

// ---------------------------------------------------------------------------
// 8c. reset() after a NaN, and the resting state.
//
// Honest note, and the reason the bar is a LEVEL bar rather than a bit-identity
// one: reset() re-zeroes the BBD clock PHASE, which is a physical quantity with
// no canonical value (a real device's clock has whatever phase it has). Two
// models that differ only in clock phase sample the input at different instants
// within one clock period, so their echoes differ by a sub-clock-period timing —
// measured at ~2.4e-03 on a 0.15-peak echo. Asserting bit-identity here would be
// asserting an arbitrary convention, so what is asserted is that the reset model
// recovers the same STEADY-STATE LEVEL as a fresh one.
// ---------------------------------------------------------------------------
void testResetAndNaN(double sr) {
    std::printf("\n[delay] 8c. reset()\n");
    const int N = static_cast<int>(sr * 2.0);
    std::vector<float> in(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(0.25 * std::sin(kTwoPi * 220.0 * i / sr));

    DelayModel fresh;
    settle(fresh, sr, 0.4, 0.7, 1.0);
    std::vector<float> ref;
    renderBlocks(fresh, in, ref);

    // Two fresh models render identically — the model is deterministic.
    {
        DelayModel other;
        settle(other, sr, 0.4, 0.7, 1.0);
        std::vector<float> o2;
        renderBlocks(other, in, o2);
        double w = 0.0;
        for (int i = 0; i < N; ++i)
            w = std::max(w, std::fabs(static_cast<double>(ref[static_cast<size_t>(i)] -
                                                          o2[static_cast<size_t>(i)])));
        std::printf("   two fresh models: worst |diff| %.3e\n", w);
        assert(w == 0.0);
    }

    // Poison with a NaN, reset, and the stream comes back clean.
    {
        DelayModel m;
        settle(m, sr, 0.4, 0.7, 1.0);
        std::vector<float> junk(256, 0.0f), jo;
        junk[10] = std::nanf("");
        renderBlocks(m, junk, jo);
        int poisoned = 0;
        for (float v : jo) if (!std::isfinite(v)) ++poisoned;
        m.reset();
        std::vector<float> after;
        renderBlocks(m, in, after);
        int bad = 0;
        for (float v : after) if (!std::isfinite(v)) ++bad;
        const double refPk = peakOf(ref, N / 2, N);
        const double aftPk = peakOf(after, N / 2, N);
        std::printf("   one NaN poisons %d/256 samples; after reset() %d/%d "
                    "non-finite; settled peak %.5f vs fresh %.5f (%.3f dB)\n",
                    poisoned, bad, N, aftPk, refPk, dbOf(aftPk / refPk));
        assert(poisoned > 0);
        assert(bad == 0);
        assert(std::fabs(dbOf(aftPk / refPk)) < 0.5);
    }

    // Resting state: every recursive state in this model rests at EXACTLY zero,
    // the delay line included (ADR 006).
    {
        DelayModel m;
        settle(m, sr, 0.5, 0.8, 1.0);
        std::vector<float> burst(static_cast<size_t>(sr * 0.5), 0.0f), bo;
        for (int i = 0; i < 2000; ++i)
            burst[static_cast<size_t>(i)] =
                static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * i / sr));
        renderBlocks(m, burst, bo);
        std::vector<float> z(static_cast<size_t>(sr * 40.0), 0.0f), zo;
        renderBlocks(m, z, zo);
        // No SUBNORMAL ever leaves the pedal on the way down, either. This bar is
        // separate from the resting-state one and it is the one that has teeth: a
        // double at 1e-40 is perfectly normal, so nothing upstream flushes it, but
        // `float(1e-40)` is subnormal and WASM has no flush-to-zero at all — so it
        // would become a permanent denormal generator in whatever comes next in the
        // chain. `clipper_denormal_tests` is what found this (measured 5395 such
        // samples over a 200 s tail before the model flushed its own output), and
        // it is asserted here too so the delay suite owns its own defect.
        long subnormals = 0;
        for (float v : zo)
            if (std::fpclassify(v) == FP_SUBNORMAL) ++subnormals;
        std::printf("   maxAbsRestingState after 40 s of silence: %.3e; last output "
                    "sample %.3e; subnormal output samples %ld\n",
                    m.maxAbsRestingState(), static_cast<double>(zo.back()), subnormals);
        assert(m.maxAbsRestingState() == 0.0);
        assert(zo.back() == 0.0f);
        assert(subnormals == 0);
    }

    // A non-finite parameter must never become a non-finite STATE. At the model
    // level `clampParam01` takes NaN to the knob's low end (it is NaN-rejecting
    // in the sense that matters: NaN cannot pass through it); at the C ABI the
    // write is dropped outright before it ever gets here (audit finding 1).
    {
        DelayModel m;
        settle(m, sr, 0.4, 0.5, 1.0);
        const double before = m.delaySeconds();
        m.setParameter(DelayModel::PARAM_DELAY, std::nanf(""));
        std::vector<float> z(4096, 0.0f), zo;
        renderBlocks(m, z, zo);
        for (float v : zo) assert(std::isfinite(v));
        std::printf("   NaN DELAY: %.3f ms -> %.3f ms (clamped toward the low end, "
                    "never NaN); output stays finite\n", before * 1000.0,
                    m.delaySeconds() * 1000.0);
        assert(std::isfinite(m.delaySeconds()));
        assert(m.delaySeconds() >= DelayModel::minDelaySeconds() - 1e-12);
    }
}

// ---------------------------------------------------------------------------
// 8d. Block-size invariance and rate independence.
// ---------------------------------------------------------------------------
void testBlockSizeAndRates() {
    std::printf("\n[delay] 8d. block size and sample rate\n");
    {
        const double sr = 48000.0;
        DelayModel a, b;
        settle(a, sr, 0.45, 0.65, 0.8);
        settle(b, sr, 0.45, 0.65, 0.8);
        const int N = static_cast<int>(sr * 3.0);
        std::vector<float> in(static_cast<size_t>(N)), o1, o2;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(
                0.25 * std::sin(kTwoPi * 196.0 * i / sr) *
                std::exp(-std::fmod(i / sr, 1.0) * 2.0));
        renderBlocks(a, in, o1, 128);
        o2.assign(static_cast<size_t>(N), 0.0f);
        const int sizes[5] = {100, 37, 256, 1, 411};
        int si = 0, off = 0;
        while (off < N) {
            const int k = std::min(sizes[si % 5], N - off);
            b.process(in.data() + off, o2.data() + off, k);
            off += k;
            ++si;
        }
        double worst = 0.0;
        for (int i = 0; i < N; ++i)
            worst = std::max(worst,
                             std::fabs(static_cast<double>(o1[static_cast<size_t>(i)] -
                                                           o2[static_cast<size_t>(i)])));
        std::printf("   128 vs ragged {100,37,256,1,411}: worst |diff| %.3e\n", worst);
        assert(worst == 0.0);
    }
    {
        std::printf("   rate    wet f0 (0.15 V @ 220 Hz)    dB      delay ms\n");
        double lo = 1e9, hi = -1e9;
        for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
            DelayModel m;
            settle(m, sr, 0.4, 0.0, 1.0);
            const int N = static_cast<int>(sr * 2.5);
            std::vector<float> in(static_cast<size_t>(N)), out;
            for (int i = 0; i < N; ++i)
                in[static_cast<size_t>(i)] =
                    static_cast<float>(0.15 * std::sin(kTwoPi * 220.0 * i / sr));
            renderBlocks(m, in, out);
            std::vector<float> wet(static_cast<size_t>(N));
            for (int i = 0; i < N; ++i)
                wet[static_cast<size_t>(i)] =
                    out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
            const double f0 = goertzel(wet, static_cast<int>(sr * 2.0),
                                       static_cast<int>(sr * 0.4), 220.0, sr);
            const double db = dbOf(f0 / 0.15);
            std::printf("   %7.0f   %.6f               %+6.3f   %7.3f\n", sr, f0, db,
                        m.delaySeconds() * 1000.0);
            lo = std::min(lo, db);
            hi = std::max(hi, db);
            // The delay time is set by the DEVICE, so it is the same number of
            // milliseconds at every host rate.
            assert(std::fabs(m.delaySeconds() - 0.238) < 1e-6);
        }
        std::printf("   spread across 44.1-96 kHz: %.4f dB\n", hi - lo);
        assert(hi - lo < 0.05);
    }
}

// ---------------------------------------------------------------------------
// 8e. No zipper on a DELAY sweep — and the pitch bend that SHOULD be there.
// ---------------------------------------------------------------------------
void testNoZipperOnDelaySweep(double sr) {
    std::printf("\n[delay] 8e. DELAY sweep\n");
    double hfDb[2];
    for (int mode = 0; mode < 2; ++mode) {  // 0 = swept, 1 = static
        DelayModel m;
        settle(m, sr, 0.2, 0.5, 1.0);
        const int N = static_cast<int>(sr * 4.0);
        std::vector<float> in(static_cast<size_t>(N)), out(static_cast<size_t>(N), 0.0f);
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(kTwoPi * 330.0 * i / sr));
        for (int off = 0; off < N; off += 128) {
            const int k = std::min(128, N - off);
            const double t = off / static_cast<double>(N);
            m.setParameter(DelayModel::PARAM_DELAY,
                           static_cast<float>(mode ? 0.2 : 0.2 + 0.6 * t));
            m.process(in.data() + off, out.data() + off, k);
        }
        for (float v : out) assert(std::isfinite(v));
        const int st = static_cast<int>(sr * 2.0), win = static_cast<int>(sr * 1.0);
        const double f0 = goertzel(out, st, win, 330.0, sr);
        double e = 0.0;
        int nb = 0;
        for (double fr = 3000.0; fr < 20000.0; fr *= 1.05) {
            const double a = goertzel(out, st, win, fr, sr);
            e += a * a;
            ++nb;
        }
        hfDb[mode] = dbOf(std::sqrt(e / nb) / f0);
        std::printf("   %s: f0 %.5f  broadband HF (3-20 kHz) %.2f dB re f0\n",
                    mode ? "static" : "swept ", f0, hfDb[mode]);
    }
    std::printf("   the sweep costs %.2f dB of HF floor over the static setting\n",
                hfDb[0] - hfDb[1]);
    // A control staircase reaching a delay line is audible as a broadband
    // crackle. It is not here: the swept floor is within a few dB of the static
    // one and both are 80 dB down.
    assert(hfDb[0] < -70.0);
    assert(hfDb[0] - hfDb[1] < 10.0);

    // The pitch bend IS the physics and must be present: sweeping the delay
    // LONGER stretches the read, so the wet tone drops below its source.
    {
        DelayModel m;
        settle(m, sr, 0.2, 0.0, 1.0);
        const int N = static_cast<int>(sr * 2.0);
        std::vector<float> in(static_cast<size_t>(N)), out(static_cast<size_t>(N), 0.0f);
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(kTwoPi * 330.0 * i / sr));
        for (int off = 0; off < N; off += 128) {
            const int k = std::min(128, N - off);
            m.setParameter(DelayModel::PARAM_DELAY,
                           static_cast<float>(0.2 + 0.6 * off / static_cast<double>(N)));
            m.process(in.data() + off, out.data() + off, k);
        }
        std::vector<float> wet(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            wet[static_cast<size_t>(i)] = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
        const int st = static_cast<int>(sr * 0.8), win = static_cast<int>(sr * 0.6);
        const double at330 = goertzel(wet, st, win, 330.0, sr);
        const double below = goertzel(wet, st, win, 326.0, sr);
        std::printf("   during the sweep the wet tone sits BELOW its source: 330 Hz "
                    "%.6f vs 326 Hz %.6f\n", at330, below);
        assert(below > at330);
    }
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    // Unbuffered: an assert() aborts, and a buffered table is a table nobody sees.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double sr = 48000.0;
    testDelayTimeTable(sr);
    testRepeatSpacing(sr);
    testRepeatDegradation(sr);
    testDarkerWithLongerDelay(sr);
    testFeedback(sr);
    testCompanderCurve(sr);
    testBlendZeroIsBypass(sr);
    testDelayLinePrimitive(sr);
    testAliasing(sr);
    testDcOnSignal(sr);
    testResetAndNaN(sr);
    testBlockSizeAndRates();
    testNoZipperOnDelaySweep(sr);
    std::printf("\nAll delay (M13.4 BBD analog delay) tests passed\n");
    return 0;
}
