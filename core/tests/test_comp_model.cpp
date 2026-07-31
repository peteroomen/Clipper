// Plain-assert tests for clipper::dsp::CompModel (M13.1 — the "Squash"
// compressor: a Dyna Comp / Ross-style CA3080 OTA compressor, and the project's
// first DYNAMICS processor). No framework: int main + <cassert>.
//
// EVERY bar here is something a player can hear or a meter can read. There is no
// test in this file that compares the model against an analytic expression
// derived from the same netlist — that would validate the discretization and
// could never catch a wrong topology (CLAUDE.md, "Measure, don't assert").
//
// What is asserted, and why each one is the property that matters:
//
//   1. STATIC COMPRESSION CURVE — dB in vs dB out across a 52 dB input sweep.
//      Below the knee the pedal is a flat linear boost (the knob's whole job);
//      above it the OUTPUT IS PINNED, because a feed-back compressor's threshold
//      is a fixed junction drop and cannot move. Ratio measured, knee located.
//   2. SUSTAIN IS NOT A THRESHOLD. The knee lands at essentially the SAME OUTPUT
//      level at every SUSTAIN setting while the small-signal GAIN moves 25 dB.
//      That single pair of facts is the whole "what does sensitivity do" answer,
//      and it is the one thing a `gain = f(env)` fake would get wrong.
//   3. ATTACK and RELEASE from a step, in ms and s, against the published ~5 ms /
//      ~1 s and against the derived R18*C9 = 1.500 s. No knob touches either.
//   4. SUSTAIN EXTENSION, level-matched — the reason the pedal exists.
//   5. FEED-BACK vs feed-forward, by measurement: flipping the tap collapses the
//      ratio. This is the claim online writeups most often get wrong, so it is
//      tested rather than commented.
//   6. ATTACK-TRANSIENT LOSS and NOISE (idle gain) — REPORTED, and bounded only
//      in the direction that says "this is still a Dyna Comp". A real one
//      squashes the pick and hisses at full sensitivity. Neither is a defect.
//   7. The tone print: three high-passes thin the low end, and the OTA's drive
//      network very nearly cancels the load pole through the core band.
//   8. Aliasing vs oversampling factor, DC offset ON SIGNAL, reset(), block-size
//      invariance, rate independence, no zipper.
//
// PERTURBATION PROOFS for the load-bearing bars are recorded in docs §59.9.

#include "clipper/dsp/CompModel.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::CompModel;

double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

double peakOf(const std::vector<float>& v, size_t a, size_t b) {
    double p = 0.0;
    for (size_t i = a; i < b && i < v.size(); ++i)
        p = std::max(p, std::fabs(static_cast<double>(v[i])));
    return p;
}

double rmsOf(const std::vector<float>& v, size_t a, size_t b) {
    double s = 0.0;
    size_t n = 0;
    for (size_t i = a; i < b && i < v.size(); ++i) {
        s += static_cast<double>(v[i]) * v[i];
        ++n;
    }
    return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

double goertzelAmp(const std::vector<float>& x, size_t start, size_t n, double f,
                   double fs) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * f * static_cast<double>(start + i) / fs;
        re += x[start + i] * std::cos(ph);
        im += x[start + i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

void renderBlocks(CompModel& m, const std::vector<float>& in,
                  std::vector<float>& out, int block = 128) {
    out.assign(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += static_cast<size_t>(block)) {
        const int n =
            static_cast<int>(std::min(static_cast<size_t>(block), in.size() - i));
        m.process(in.data() + i, out.data() + i, n);
    }
}

// A settled render of a steady tone: `seconds` long, measured over the last 0.5 s.
std::vector<float> renderTone(double sr, double freq, double amp, float sustain,
                              float level, double seconds = 3.0,
                              bool feedback = true) {
    CompModel m;
    m.prepare(sr, 128);
    m.setDetectorTap(feedback);
    m.setParameter(CompModel::PARAM_SUSTAIN, sustain);
    m.setParameter(CompModel::PARAM_LEVEL, level);
    const int n = static_cast<int>(sr * seconds);
    std::vector<float> in(static_cast<size_t>(n)), out;
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(kTwoPi * freq * i / sr));
    renderBlocks(m, in, out);
    return out;
}

double settledPeak(const std::vector<float>& out, double sr) {
    const size_t a = out.size() - static_cast<size_t>(sr * 0.5);
    return peakOf(out, a, out.size());
}

// 20 ms peak envelope, the way a player hears level.
std::vector<double> envelopeOf(const std::vector<float>& v, double sr) {
    const size_t w = static_cast<size_t>(sr * 0.02);
    std::vector<double> e;
    for (size_t i = 0; i + w < v.size(); i += w) e.push_back(peakOf(v, i, i + w));
    return e;
}

// ---------------------------------------------------------------------------
// 1 + 2. The static curve, and what SUSTAIN actually is.
// ---------------------------------------------------------------------------
//
// Measured, shipped code, 1 kHz, LEVEL 1.0, settled (docs §59.4):
//
//   SUSTAIN   small-signal gain   settled output peak above the knee
//     0.00        +11.25 dB                0.383 V
//     0.30        +14.14 dB                0.386 V
//     0.50        +16.79 dB                0.390 V
//     0.80        +23.49 dB                0.396 V
//     1.00        +36.58 dB                0.400 V
//
// The gain column moves 25.3 dB. The output column moves 0.4 dB. That is a
// feed-back compressor with a fixed junction threshold, and it is why SUSTAIN
// cannot be described as a threshold control.
void testStaticCurveAndSustain(double sr) {
    std::printf("[comp] static curve / SUSTAIN (1 kHz, LEVEL 1.0, settled)\n");
    const float sustains[] = {0.0f, 0.3f, 0.5f, 0.8f, 1.0f};
    double smallSignalDb[5] = {0, 0, 0, 0, 0};
    double pinnedPeak[5] = {0, 0, 0, 0, 0};

    for (int s = 0; s < 5; ++s) {
        // Small-signal: 2 mV in is far below the knee at every setting.
        const double lo = settledPeak(renderTone(sr, 1000.0, 0.002, sustains[s], 1.0f), sr);
        smallSignalDb[s] = dbOf(lo) - dbOf(0.002);
        // Well above the knee.
        const double hi = settledPeak(renderTone(sr, 1000.0, 0.4, sustains[s], 1.0f), sr);
        pinnedPeak[s] = hi;
        std::printf("   SUSTAIN %.2f : small-signal %+6.2f dB   settled peak above knee %.4f V\n",
                    static_cast<double>(sustains[s]), smallSignalDb[s], hi);
    }

    // (a) The knob has real authority and is monotone.
    for (int s = 1; s < 5; ++s) assert(smallSignalDb[s] > smallSignalDb[s - 1] + 1.0);
    const double range = smallSignalDb[4] - smallSignalDb[0];
    std::printf("   SUSTAIN travel = %.2f dB of small-signal gain\n", range);
    // The circuit's own span: Iabc goes from 7.1 V/(500 k + 27 k) to 7.1 V/27 k,
    // i.e. 19.5x = 25.8 dB, and gm is proportional to Iabc. Bar deliberately wide
    // on both sides — the point is that it is ~26 dB and not, say, 6 or 60.
    assert(range > 20.0 && range < 32.0);

    // (b) THE HEADLINE: the threshold does NOT move with the knob. Every setting
    // pins the output within 1 dB of the same level, because the detector's
    // turn-on is a base-emitter drop and no knob is in front of it.
    double pinMin = pinnedPeak[0], pinMax = pinnedPeak[0];
    for (int s = 1; s < 5; ++s) {
        pinMin = std::min(pinMin, pinnedPeak[s]);
        pinMax = std::max(pinMax, pinnedPeak[s]);
    }
    const double pinSpread = dbOf(pinMax) - dbOf(pinMin);
    std::printf("   pinned-output spread across the WHOLE knob = %.2f dB "
                "(gain travel was %.2f dB)\n", pinSpread, range);
    assert(pinSpread < 1.5);
    // And it must be dramatically smaller than the gain travel, or the statement
    // "the knob is not a threshold" is not being measured at all.
    assert(range > 10.0 * pinSpread);

    // (c) The curve itself: flat below the knee, pinned above it.
    std::printf("   curve at SUSTAIN 0.50:\n");
    const double amps[] = {0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8};
    double gains[9];
    double outDb[9];
    for (int i = 0; i < 9; ++i) {
        const double pk = settledPeak(renderTone(sr, 1000.0, amps[i], 0.5f, 1.0f), sr);
        gains[i] = dbOf(pk) - dbOf(amps[i]);
        outDb[i] = dbOf(pk);
        std::printf("     in %7.2f dB -> out %7.2f dB (gain %+6.2f)\n",
                    dbOf(amps[i]), outDb[i], gains[i]);
    }
    // Below the knee the pedal is LINEAR: 20 dB of input gives 20 dB of output.
    const double linSlope = (outDb[3] - outDb[0]) / (dbOf(amps[3]) - dbOf(amps[0]));
    std::printf("     slope below the knee = %.4f (1.0 = linear)\n", linSlope);
    assert(linSlope > 0.99 && linSlope < 1.01);
    // Above it, the ratio is extreme. Over the top 24 dB of input the output
    // moves less than 1 dB at SUSTAIN 0.5 (measured 0.24 dB -> ratio ~100:1);
    // the bar says "at least 12:1", which is still far more than any studio
    // compressor's usual setting and comfortably fails a soft-knee fake.
    const double topRatio = (dbOf(amps[8]) - dbOf(amps[5])) /
                            std::max(1e-9, outDb[8] - outDb[5]);
    std::printf("     ratio over the top %.0f dB of input = %.1f:1\n",
                dbOf(amps[8]) - dbOf(amps[5]), topRatio);
    assert(topRatio > 12.0);
    // The knee: the first input at which the pedal has given away 3 dB of gain.
    double kneeInDb = 0.0;
    for (int i = 0; i < 9; ++i)
        if (gains[i] < gains[0] - 3.0) { kneeInDb = dbOf(amps[i]); break; }
    std::printf("     3 dB-of-gain-reduction knee at %.2f dBV in\n", kneeInDb);
    assert(kneeInDb < -15.0);  // it engages well inside normal playing level
}

// ---------------------------------------------------------------------------
// 3. Attack and release, from a step. The pedal has no knob for either.
// ---------------------------------------------------------------------------
void testAttackAndRelease(double sr) {
    std::printf("[comp] attack / release from a step\n");
    const float sustains[] = {0.3f, 0.5f, 0.8f, 1.0f};
    for (float sus : sustains) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, sus);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        const int N = static_cast<int>(sr * 6.0);
        const int on = static_cast<int>(sr * 1.0), off = static_cast<int>(sr * 3.0);
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        for (int i = on; i < off; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.2 * std::sin(kTwoPi * 1000.0 * (i - on) / sr));
        renderBlocks(m, in, out);

        const size_t W = static_cast<size_t>(sr * 0.001);  // 1 ms resolution
        auto env = [&](size_t i) { return peakOf(out, i, i + W); };
        double pk = 0.0;
        size_t ipk = static_cast<size_t>(on);
        for (size_t i = static_cast<size_t>(on);
             i < static_cast<size_t>(on) + static_cast<size_t>(sr * 0.05); i += W) {
            const double e = env(i);
            if (e > pk) { pk = e; ipk = i; }
        }
        const double settled = env(static_cast<size_t>(off) - 4 * W);
        const double target = settled + (pk - settled) * std::exp(-1.0);
        double attackMs = -1.0;
        for (size_t i = ipk; i + W < static_cast<size_t>(off); i += W)
            if (env(i) <= target) { attackMs = 1000.0 * (i - ipk) / sr; break; }

        std::printf("   SUSTAIN %.2f : transient peak %.4f -> settled %.4f  "
                    "(loss %5.2f dB)   attack(1/e) %5.2f ms\n",
                    static_cast<double>(sus), pk, settled, dbOf(pk) - dbOf(settled),
                    attackMs);
        // FAST, and with no knob. The model measures 14 / 10 / 5 / 3 ms across
        // the SUSTAIN travel (it speeds up because more loop gain reaches the
        // detector sooner). The travel bar would fail an "instant" gain cell or
        // a lazy one.
        assert(attackMs > 0.5 && attackMs < 30.0);
        // ...and ONE bar against an ABSOLUTE reference: published figures for
        // the M102 put its (fixed) attack at ~5 ms, and this model measures
        // 5.00 ms at SUSTAIN 0.80. Source caveat recorded in docs §59.2 — it is
        // a review measurement, not a manufacturer spec, so the window is 3..9 ms
        // rather than tight. This is the bar that makes the detector
        // transistors' LOW-CURRENT beta load-bearing: deleting the Gummel-Poon
        // ISE term takes hFE from the datasheet's ~106 at 0.2 mA to the ideal
        // 416, and the attack here to 2.00 ms (docs §59.9, perturbation P4).
        if (sus == 0.8f) assert(attackMs > 3.0 && attackMs < 9.0);
    }

    // Release, measured on the control current itself so it is not confused with
    // the note's own decay. The circuit's number is R18*C9 = 150 k * 10 uF =
    // 1.500 s; measured 1.496 s / 1.448 s. Bar +/-15 %.
    for (float sus : {0.5f, 1.0f}) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, sus);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        std::vector<float> z(128, 0.0f), o(128), b(128);
        for (int i = 0; i < 400; ++i) m.process(z.data(), o.data(), 128);
        const double idle = m.controlCurrent();
        long t = 0;
        for (int k = 0; k < static_cast<int>(sr * 2.0) / 128; ++k) {
            for (int i = 0; i < 128; ++i, ++t)
                b[static_cast<size_t>(i)] =
                    static_cast<float>(0.2 * std::sin(kTwoPi * 1000.0 * t / sr));
            m.process(b.data(), o.data(), 128);
        }
        const double squashed = m.controlCurrent();
        const double tgt = squashed + (idle - squashed) * (1.0 - std::exp(-1.0));
        double rel = -1.0;
        for (int k = 0; k < static_cast<int>(sr * 10.0) / 128; ++k) {
            m.process(z.data(), o.data(), 128);
            if (rel < 0.0 && m.controlCurrent() >= tgt) rel = (k + 1) * 128.0 / sr;
        }
        std::printf("   SUSTAIN %.2f : Iabc %.4e -> %.4e, 63%% recovery in %.3f s "
                    "(R18*C9 = 1.500 s)\n",
                    static_cast<double>(sus), idle, squashed, rel);
        assert(squashed < idle * 0.6);   // the loop really did give gain away
        assert(rel > 1.20 && rel < 1.80);
    }
}

// ---------------------------------------------------------------------------
// 4. Sustain extension — the reason the pedal exists.
// ---------------------------------------------------------------------------
//
// LEVEL-MATCHED, because "it lasts longer" is only meaningful at equal loudness:
// the bypassed reference is scaled so that both signals have the SAME envelope
// 0.3 s into the note, and then we ask which one is still within 20 dB later.
void testSustainExtension(double sr) {
    std::printf("[comp] sustain extension (level-matched, 20 dB window)\n");
    const int N = static_cast<int>(sr * 12.0);
    std::vector<float> in(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const double t = i / sr;
        in[static_cast<size_t>(i)] = static_cast<float>(
            0.30 * std::exp(-t / 0.7) * std::sin(kTwoPi * 220.0 * t));
    }
    const std::vector<double> eIn = envelopeOf(in, sr);
    const size_t i03 = 15;  // 0.3 s at 20 ms per envelope frame

    double prev = 0.0;
    for (float sus : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f}) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, sus);
        m.setParameter(CompModel::PARAM_LEVEL, 0.4f);
        std::vector<float> out;
        renderBlocks(m, in, out);
        const std::vector<double> eOut = envelopeOf(out, sr);
        const double k = eOut[i03] / eIn[i03];             // the level match
        const double thr = eOut[i03] * std::pow(10.0, -20.0 / 20.0);
        auto lastAbove = [&](const std::vector<double>& e, double sc) {
            double last = 0.0;
            for (size_t i = i03; i < e.size(); ++i)
                if (e[i] * sc > thr) last = static_cast<double>(i) * 0.02;
            return last;
        };
        const double tComp = lastAbove(eOut, 1.0);
        const double tRef = lastAbove(eIn, k);
        std::printf("   SUSTAIN %.2f : holds %.2f s vs level-matched bypass %.2f s "
                    " -> +%.2f s (%.2fx)\n",
                    static_cast<double>(sus), tComp, tRef, tComp - tRef, tComp / tRef);
        // It must ADD sustain at every setting — a compressor that does not is
        // not doing the one thing this pedal is named for.
        assert(tComp > tRef + 0.15);
        // And more SUSTAIN must buy more of it (monotone in the knob).
        assert(tComp > prev - 1e-9);
        prev = tComp;
    }
    // The headline number, at the top of the knob: measured +2.26 s / 2.19x.
    CompModel m;
    m.prepare(sr, 128);
    m.setParameter(CompModel::PARAM_SUSTAIN, 1.0f);
    m.setParameter(CompModel::PARAM_LEVEL, 0.4f);
    std::vector<float> out;
    renderBlocks(m, in, out);
    const std::vector<double> eOut = envelopeOf(out, sr);
    const double thr = eOut[i03] * 0.1;
    double tComp = 0.0;
    for (size_t i = i03; i < eOut.size(); ++i)
        if (eOut[i] > thr) tComp = static_cast<double>(i) * 0.02;
    assert(tComp > 3.0);  // the note is still within 20 dB three seconds in
}

// ---------------------------------------------------------------------------
// 5. FEED-BACK vs feed-forward, by measurement.
// ---------------------------------------------------------------------------
//
// The detector taps the gain cell's OUTPUT. Flipping it to the cell's INPUT
// leaves every component value untouched and every other block identical, so
// this test isolates the topology and nothing else. Measured at SUSTAIN 1.0
// over 26 dB of input: feed-back 216:1, feed-forward 3.3:1.
void testDetectorTopology(double sr) {
    std::printf("[comp] detector topology (feed-back vs feed-forward)\n");
    double ratio[2] = {0.0, 0.0};
    for (int fb = 1; fb >= 0; --fb) {
        const double lo = dbOf(settledPeak(
            renderTone(sr, 1000.0, 0.02, 1.0f, 1.0f, 3.0, fb != 0), sr));
        const double hi = dbOf(settledPeak(
            renderTone(sr, 1000.0, 0.4, 1.0f, 1.0f, 3.0, fb != 0), sr));
        ratio[fb] = 26.02 / std::max(1e-9, hi - lo);
        std::printf("   %s : 26.02 dB in -> %6.2f dB out  (ratio %.2f:1)\n",
                    fb ? "FEED-BACK" : "feed-fwd ", hi - lo, ratio[fb]);
    }
    // Feed-back must be a near-limiter.
    assert(ratio[1] > 20.0);
    // And the topology must be LOAD-BEARING: at least a factor of 5 between the
    // two. If someone "simplifies" the detector to tap the input, this fails.
    assert(ratio[1] > 5.0 * ratio[0]);
}

// ---------------------------------------------------------------------------
// 6. The honest ones: transient loss and noise. REPORTED, bounded only in the
//    direction that says the pedal is still a Dyna Comp.
// ---------------------------------------------------------------------------
void testHonestExpectations(double sr) {
    std::printf("[comp] attack-transient loss and idle gain (both REPORTED, "
                "neither designed out)\n");
    // Transient loss: how much of the pick attack the compressor eats.
    for (float sus : {0.3f, 0.5f, 0.8f, 1.0f}) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, sus);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        const int N = static_cast<int>(sr * 4.0);
        const int on = static_cast<int>(sr * 1.0);
        std::vector<float> in(static_cast<size_t>(N), 0.0f), out;
        for (int i = on; i < N; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.2 * std::sin(kTwoPi * 1000.0 * (i - on) / sr));
        renderBlocks(m, in, out);
        const size_t W = static_cast<size_t>(sr * 0.001);
        double pk = 0.0;
        for (size_t i = static_cast<size_t>(on);
             i < static_cast<size_t>(on) + static_cast<size_t>(sr * 0.05); i += W)
            pk = std::max(pk, peakOf(out, i, i + W));
        const double settled = peakOf(out, out.size() - W * 8, out.size() - W * 4);
        std::printf("   SUSTAIN %.2f : transient LOSS %5.2f dB\n",
                    static_cast<double>(sus), dbOf(pk) - dbOf(settled));
        // A real Dyna Comp squashes the attack. The bar says it MUST — a model
        // that preserved the transient would be a different, better-behaved
        // pedal and would fail here on purpose. Measured 7.7 / 10.0 / 15.2 /
        // 20.0 dB.
        assert(dbOf(pk) - dbOf(settled) > 4.0);
    }

    // Idle gain: the mechanism behind a real one's hiss. This project models no
    // noise anywhere, so what is measured is the GAIN applied to whatever noise
    // floor arrives — which is the honest, source-independent statement.
    std::printf("   idle gain on a -84.8 dBFS input noise floor (LEVEL 1.0):\n");
    double g0 = 0.0, g1 = 0.0;
    for (float sus : {0.0f, 0.5f, 1.0f}) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, sus);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        const int N = static_cast<int>(sr * 4.0);
        std::vector<float> in(static_cast<size_t>(N)), out;
        unsigned s = 12345u;
        for (int i = 0; i < N; ++i) {
            s = s * 1664525u + 1013904223u;
            in[static_cast<size_t>(i)] = static_cast<float>(
                1e-4 * ((static_cast<int>(s >> 9) & 0xFFFF) / 32768.0 - 1.0));
        }
        renderBlocks(m, in, out);
        const double g = dbOf(rmsOf(out, out.size() / 2, out.size())) -
                         dbOf(rmsOf(in, 0, in.size()));
        std::printf("     SUSTAIN %.2f : %+6.2f dB\n", static_cast<double>(sus), g);
        if (sus == 0.0f) g0 = g;
        if (sus == 1.0f) g1 = g;
    }
    // The knob really does trade noise for compression, by ~20 dB or more. If a
    // future slice "fixes" the noise by capping the idle gain, this fails and
    // the fix has to be argued as a departure from the circuit.
    std::printf("     SUSTAIN 0 -> 1 costs %.2f dB of noise gain\n", g1 - g0);
    assert(g1 - g0 > 15.0);
}

// ---------------------------------------------------------------------------
// 7. The tone print: a thinned low end and a near-flat core band.
// ---------------------------------------------------------------------------
void testTonePrint(double sr) {
    std::printf("[comp] small-signal response (dB re 1 kHz, 5 mV, SUSTAIN 0.5)\n");
    const double freqs[] = {20.0, 41.0, 82.0, 110.0, 220.0, 440.0,
                            1000.0, 2000.0, 5000.0, 10000.0};
    double resp[10];
    for (int i = 0; i < 10; ++i) {
        const std::vector<float> out = renderTone(sr, freqs[i], 0.005, 0.5f, 1.0f, 2.0);
        resp[i] = dbOf(rmsOf(out, out.size() - static_cast<size_t>(sr * 0.5), out.size()));
    }
    for (int i = 0; i < 10; ++i)
        std::printf("   %8.1f Hz : %+6.2f dB\n", freqs[i], resp[i] - resp[6]);

    // (a) The low end IS thinned — three high-passes (input 30.5 Hz, output
    // 51.9 Hz, and the drive network's own 9.3 Hz pole) sit between in and out.
    // Low E (82 Hz) measured -2.42 dB, 41 Hz -6.59 dB, 20 Hz -15.20 dB. Asserted
    // as a REQUIREMENT, not tolerated: a model with a full low end would not be
    // this pedal.
    std::printf("   low E (82 Hz) %+.2f dB, 41 Hz %+.2f dB, 20 Hz %+.2f dB re 1 kHz\n",
                resp[2] - resp[6], resp[1] - resp[6], resp[0] - resp[6]);
    assert(resp[2] - resp[6] < -1.0);
    assert(resp[1] - resp[6] < -4.0);
    assert(resp[0] - resp[6] < -10.0);

    // (b) The core band is FLAT, and that is not an accident: the OTA's
    // differential-drive network rises with frequency by almost exactly as much
    // as the 150 k || 1 nF load pole falls. Measured 110 Hz..5 kHz spread 1.4 dB.
    double lo = resp[3], hi = resp[3];
    for (int i = 3; i <= 8; ++i) { lo = std::min(lo, resp[i]); hi = std::max(hi, resp[i]); }
    std::printf("   110 Hz..5 kHz spread = %.2f dB (the two networks cancel)\n", hi - lo);
    assert(hi - lo < 3.0);

    // (c) ...and it does roll off on top, which is why a Dyna Comp is described
    // as "not very bright". 10 kHz measured -2.88 dB.
    assert(resp[9] - resp[6] < -1.0);
}

// ---------------------------------------------------------------------------
// 8. Aliasing vs the oversampling factor.
// ---------------------------------------------------------------------------
//
// The stimulus is a SINGLE 9 kHz tone, not a two-tone pair: a compressor's gain
// is modulated by its own detector, so a two-tone stimulus fills the spectrum
// with legitimate intermodulation sidebands and measures nothing about aliasing.
// (That mistake was made first and reported -25 dB flat across 1x..8x, which was
// the tell — a real alias floor MOVES with the factor.) With one tone the
// envelope is constant, so everything below the fundamental is fold-back.
void testAliasing(double sr) {
    std::printf("[comp] aliasing (single 9 kHz tone at 0.3 V, SUSTAIN 1.0)\n");
    double worst[4] = {0, 0, 0, 0};
    const int factors[] = {1, 2, 4, 8};
    for (int fi = 0; fi < 4; ++fi) {
        CompModel m;
        m.prepare(sr, 128);
        m.setOversampling(factors[fi]);
        m.setParameter(CompModel::PARAM_SUSTAIN, 1.0f);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        const int N = static_cast<int>(sr * 2.0);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.3 * std::sin(kTwoPi * 9000.0 * i / sr));
        renderBlocks(m, in, out);
        const size_t A = out.size() - static_cast<size_t>(sr * 0.5);
        const size_t L = static_cast<size_t>(sr * 0.5);
        const double f1 = goertzelAmp(out, A, L, 9000.0, sr);
        double w = 1e-30;
        for (double fr : {21000.0, 15000.0, 12000.0, 7500.0, 6000.0, 3000.0})
            w = std::max(w, goertzelAmp(out, A, L, fr, sr));
        worst[fi] = dbOf(w) - dbOf(f1);
        std::printf("   %dx : worst fold %.2f dB rel\n", factors[fi], worst[fi]);
    }
    // The shipped 4x path. Measured -62.4 dB; the bar is the project's -55 dB
    // working floor for a deliberately brutal stimulus.
    assert(worst[2] < -55.0);
    // And it must still DEPEND on the factor — the §54 lesson: a floor that
    // stops improving with oversampling is measuring a base-rate clip, not
    // aliasing. Measured 1x -7.6 -> 4x -62.4 = 54.8 dB of improvement.
    assert(worst[0] - worst[2] > 35.0);
    assert(worst[3] < worst[2]);
}

// ---------------------------------------------------------------------------
// 9. Hygiene: DC on signal, reset, block-size invariance, rates, no zipper.
// ---------------------------------------------------------------------------
void testDcOnSignal(double sr) {
    std::printf("[comp] DC offset ON SIGNAL\n");
    for (float off : {0.0f, clipper::test::kInputDcOffset}) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, 0.8f);
        m.setParameter(CompModel::PARAM_LEVEL, 0.4f);
        const int N = static_cast<int>(sr * 3.0);
        std::vector<float> in(static_cast<size_t>(N)), out;
        for (int i = 0; i < N; ++i)
            in[static_cast<size_t>(i)] =
                off + static_cast<float>(0.2 * std::sin(kTwoPi * 220.0 * i / sr));
        renderBlocks(m, in, out);
        const clipper::test::DcOnSignal d = clipper::test::measureDcOnSignal(out, 0.5);
        std::printf("   input offset %.2f V : output mean %.3e on peak %.4f "
                    "(%.4f %% of peak)\n", static_cast<double>(off), d.mean, d.peak,
                    100.0 * d.fraction);
        assert(d.fraction < clipper::test::kDcFractionBar);
    }
}

void testResetAndNaN(double sr) {
    std::printf("[comp] reset() / NaN recovery / denormal rest\n");
    CompModel m;
    m.prepare(sr, 128);
    m.setParameter(CompModel::PARAM_SUSTAIN, 0.8f);
    m.setParameter(CompModel::PARAM_LEVEL, 0.5f);
    const int N = static_cast<int>(sr * 1.0);
    std::vector<float> in(static_cast<size_t>(N)), out;
    for (int i = 0; i < N; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * i / sr));
    renderBlocks(m, in, out);
    const double afterSignal = m.envelopeVolts();
    m.reset();
    const double afterReset = m.envelopeVolts();
    CompModel fresh;
    fresh.prepare(sr, 128);
    fresh.setParameter(CompModel::PARAM_SUSTAIN, 0.8f);
    fresh.setParameter(CompModel::PARAM_LEVEL, 0.5f);
    std::vector<float> z(128, 0.0f), oz(128);
    fresh.process(z.data(), oz.data(), 128);
    std::printf("   envelope: %.4f V under signal -> %.4f V after reset "
                "(fresh model parks at %.4f V)\n", afterSignal, afterReset,
                fresh.envelopeVolts());
    // reset() re-parks at the CACHED quiescent point — it must land exactly
    // where a freshly prepared model sits, without re-solving anything.
    assert(std::fabs(afterReset - fresh.envelopeVolts()) < 1e-9);
    assert(m.maxAbsRestingState() == 0.0);

    // A single NaN must not latch.
    std::vector<float> bad(256, 0.0f), ob(256);
    bad[10] = std::nanf("");
    m.process(bad.data(), ob.data(), 256);
    int poisoned = 0;
    for (float v : ob) if (!std::isfinite(v)) ++poisoned;
    m.reset();
    std::vector<float> clean;
    renderBlocks(m, in, clean);
    int after = 0;
    for (float v : clean) if (!std::isfinite(v)) ++after;
    std::printf("   after one NaN sample: %d/256 non-finite; after reset(): "
                "%d/%d non-finite\n", poisoned, after, static_cast<int>(clean.size()));
    assert(poisoned > 0);   // the poison really did propagate (the test has teeth)
    assert(after == 0);

    // A non-finite parameter must be rejected, not clamped into the state.
    m.setParameter(CompModel::PARAM_SUSTAIN, std::nanf(""));
    std::vector<float> after2;
    renderBlocks(m, in, after2);
    for (float v : after2) assert(std::isfinite(v));

    // Denormal policy: everything that rests at zero must reach EXACTLY zero.
    CompModel d;
    d.prepare(sr, 128);
    d.setParameter(CompModel::PARAM_SUSTAIN, 1.0f);
    d.setParameter(CompModel::PARAM_LEVEL, 1.0f);
    std::vector<float> o1;
    renderBlocks(d, in, o1);
    std::vector<float> tail(static_cast<size_t>(sr * 6.0), 0.0f), o2;
    renderBlocks(d, tail, o2);
    std::printf("   after 6 s of silence: maxAbsRestingState = %.6e ; "
                "envelope parks at %.6f V (deliberately unguarded)\n",
                d.maxAbsRestingState(), d.envelopeVolts());
    assert(d.maxAbsRestingState() == 0.0);
    assert(d.envelopeVolts() > 5.0);  // a real operating point, never subnormal
    for (size_t i = o2.size() / 2; i < o2.size(); ++i) assert(o2[i] == 0.0f);
}

void testBlockSizeAndRates() {
    std::printf("[comp] block-size invariance and rate independence\n");
    const double sr = 48000.0;
    const int N = static_cast<int>(sr * 2.0);
    std::vector<float> in(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(0.2 * std::sin(kTwoPi * 220.0 * i / sr));
    std::vector<float> a, b;
    {
        CompModel m;
        m.prepare(sr, 512);
        m.setParameter(CompModel::PARAM_SUSTAIN, 0.7f);
        m.setParameter(CompModel::PARAM_LEVEL, 0.5f);
        renderBlocks(m, in, a, 128);
    }
    {
        CompModel m;
        m.prepare(sr, 512);
        m.setParameter(CompModel::PARAM_SUSTAIN, 0.7f);
        m.setParameter(CompModel::PARAM_LEVEL, 0.5f);
        b.assign(static_cast<size_t>(N), 0.0f);
        const int sz[5] = {100, 37, 256, 1, 411};
        int i = 0, k = 0;
        while (i < N) {
            const int n = std::min(sz[k % 5], N - i);
            m.process(in.data() + i, b.data() + i, n);
            i += n;
            ++k;
        }
    }
    double worst = 0.0;
    for (size_t i = static_cast<size_t>(sr * 1.0); i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i] - b[i])));
    std::printf("   ragged blocks vs 128: worst |diff| after 1 s = %.3e\n", worst);
    // BIT-IDENTICAL: nothing in this model is block-size dependent.
    assert(worst == 0.0);

    // Every derived time constant is in seconds, so the settled level must not
    // depend on the sample rate. Measured spread 0.16 % over 44.1..96 kHz.
    double lo = 1e9, hi = 0.0;
    for (double rate : {44100.0, 48000.0, 88200.0, 96000.0}) {
        const std::vector<float> out = renderTone(rate, 220.0, 0.15, 0.5f, 0.4f);
        const double pk = settledPeak(out, rate);
        std::printf("   %.0f Hz : settled peak %.5f V\n", rate, pk);
        lo = std::min(lo, pk);
        hi = std::max(hi, pk);
    }
    std::printf("   rate spread = %.3f dB\n", dbOf(hi) - dbOf(lo));
    assert(dbOf(hi) - dbOf(lo) < 0.15);
}

// A knob move must not step the waveform. Compared against the signal's OWN
// steady-state slew, the same metric docs §35 uses for the valve amps.
void testNoZipper(double sr) {
    std::printf("[comp] parameter smoothing (no zipper)\n");
    const int N = static_cast<int>(sr * 2.0);
    std::vector<float> in(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(0.15 * std::sin(kTwoPi * 220.0 * i / sr));
    for (int which = 0; which < 2; ++which) {
        CompModel m;
        m.prepare(sr, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, 0.3f);
        m.setParameter(CompModel::PARAM_LEVEL, 0.3f);
        std::vector<float> out(static_cast<size_t>(N), 0.0f);
        const int flip = N / 2;
        for (int i = 0; i < N; i += 128) {
            if (i <= flip && i + 128 > flip)
                m.setParameter(which == 0 ? CompModel::PARAM_SUSTAIN
                                          : CompModel::PARAM_LEVEL,
                               0.7f);
            m.process(in.data() + i, out.data() + i, std::min(128, N - i));
        }
        // Baseline slew of the settled signal, measured well away from the seam.
        double base = 0.0;
        for (int i = N - 4000; i < N - 1; ++i)
            base = std::max(base, std::fabs(static_cast<double>(out[i + 1] - out[i])));
        double seam = 0.0;
        for (int i = flip - 64; i < flip + 640; ++i)
            seam = std::max(seam, std::fabs(static_cast<double>(out[i + 1] - out[i])));
        std::printf("   %s 0.30 -> 0.70 : seam step %.6f vs steady slew %.6f "
                    "(%.2fx)\n", which == 0 ? "SUSTAIN" : "LEVEL  ", seam, base,
                    seam / base);
        // Under 2x the signal's own slew: the seam is not a step.
        assert(seam < 2.0 * base);
    }
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    const double sr = 48000.0;
    testStaticCurveAndSustain(sr);
    testAttackAndRelease(sr);
    testSustainExtension(sr);
    testDetectorTopology(sr);
    testHonestExpectations(sr);
    testTonePrint(sr);
    testAliasing(sr);
    testDcOnSignal(sr);
    testResetAndNaN(sr);
    testBlockSizeAndRates();
    testNoZipper(sr);
    std::printf("All comp (M13.1 OTA compressor) tests passed\n");
    return 0;
}
