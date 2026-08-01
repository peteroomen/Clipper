// Clipper — M13.6a: the "Curfew" noise gate.
//
// PLAYER-OBSERVABLE BARS ONLY. A gate is a control decision wrapped around a
// multiply, so almost everything here is measured on a RENDERED signal at a
// realistic level and asked the question a player would ask: does it open when I
// play, does it shut up when I stop, does it chatter on a decaying note, does it
// eat my pick attack, and does the THRESHOLD knob actually move a threshold.
//
// Two bars are load-bearing above all others and both are perturbation-proven
// (docs §61.9):
//
//   * THE CHATTER TEST. A decaying note crossing the threshold must produce ONE
//     close, not many. With the hysteresis removed the identical stimulus
//     measures 5-7 opens and 5-7 closes.
//   * THE CONTROL-LAW CONTRAST. THRESHOLD moves the level at which the gate
//     opens by 40.00 dB while moving the gain the pedal applies when it IS open
//     by 0.0000 dB. Docs §59.4 measured the Dyna Comp's SUSTAIN doing the exact
//     opposite: 25.33 dB of gain travel against 0.28 dB of output travel. That
//     pair of numbers is the difference between a threshold and a sensitivity
//     control, and it is asserted, not asserted-about.
//
// No XFAIL ledger: this suite has no known-bad property. Do not add one without
// a measured number.

#include "clipper/dsp/GateModel.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::GateModel;

double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-300)); }

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

void renderBlocks(GateModel& m, const std::vector<float>& in,
                  std::vector<float>& out, int block = 128) {
    out.assign(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += static_cast<size_t>(block)) {
        const int n =
            static_cast<int>(std::min(static_cast<size_t>(block), in.size() - i));
        m.process(in.data() + i, out.data() + i, n);
    }
}

std::vector<float> sineBuf(double sr, double freq, double peak, double seconds) {
    const int n = static_cast<int>(sr * seconds);
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(peak * std::sin(kTwoPi * freq * i / sr));
    return v;
}

// A plucked note: a fundamental plus an octave, decaying. The house shape.
std::vector<float> pluckBuf(double sr, double freq, double peak, double tau,
                            double seconds) {
    const int n = static_cast<int>(sr * seconds);
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        v[static_cast<size_t>(i)] = static_cast<float>(
            peak * std::exp(-t / tau) *
            (std::sin(kTwoPi * freq * t) + 0.35 * std::sin(2.0 * kTwoPi * freq * t)));
    }
    return v;
}

GateModel makeGate(double sr, float threshold, float decay) {
    GateModel g;
    g.prepare(sr, 128);
    g.setParameter(GateModel::PARAM_THRESHOLD, threshold);
    g.setParameter(GateModel::PARAM_DECAY, decay);
    return g;
}

// The input PEAK (volts) at which a fresh gate ends up OPEN on a steady tone.
// Bisected on the comparator's own state, so it measures the decision rather
// than inferring it from the audio.
double openLevelVolts(double sr, double knob, double freq = 220.0) {
    double lo = 1e-6, hi = 3.0;
    for (int it = 0; it < 36; ++it) {
        const double mid = std::sqrt(lo * hi);
        GateModel g = makeGate(sr, static_cast<float>(knob), 0.5f);
        const std::vector<float> in = sineBuf(sr, freq, mid, 0.4);
        std::vector<float> out;
        renderBlocks(g, in, out);
        if (g.isOpen()) hi = mid; else lo = mid;
    }
    return std::sqrt(lo * hi);
}

// ---------------------------------------------------------------------------
// 1. THE THRESHOLD IS A THRESHOLD — the table (docs §61.4)
// ---------------------------------------------------------------------------
//
// Measured, shipped code, 220 Hz, 48 kHz:
//
//   knob   opens at        the pedal's stated law
//   0.00   -59.07 dBV      -59.07
//   0.20   -51.07          -51.07
//   0.35   -45.07          -45.07
//   0.50   -39.07          -39.07
//   0.65   -33.07          -33.07
//   0.80   -27.07          -27.07
//   1.00   -19.07          -19.07
//
// The SHAPE is the property: the pot is dB-linear and the detector between it
// and the comparator is EXPONENTIAL, so the threshold tracking the pot's law
// across 40 dB is a real prediction rather than an identity. (The one calibration
// constant, the sidechain drive at which the detector trips, sets the offset
// only — see `GateModel::statedThresholdDbV`.)
void testThresholdLaw(double sr) {
    std::printf("[gate] THRESHOLD law (220 Hz, 48 kHz)\n");
    std::printf("       knob   measured dBV   stated dBV   delta\n");
    const double knobs[] = {0.0, 0.2, 0.35, 0.5, 0.65, 0.8, 1.0};
    double prev = -1e9;
    double worst = 0.0;
    double first = 0.0, last = 0.0;
    GateModel label;  // for the stated law only — never rendered
    for (int i = 0; i < 7; ++i) {
        const double m = dbOf(openLevelVolts(sr, knobs[i]));
        const double s = label.statedThresholdDbV(knobs[i]);
        std::printf("       %.2f   %10.2f   %10.2f   %+.2f\n", knobs[i], m, s,
                    m - s);
        // Monotone: turning the knob up can only make the gate harder to open.
        assert(m > prev + 1.0);
        prev = m;
        worst = std::max(worst, std::fabs(m - s));
        if (i == 0) first = m;
        last = m;
    }
    const double span = last - first;
    std::printf("       span %.2f dB over the travel; worst |measured-stated| %.2f dB\n",
                span, worst);
    // The pot's designed range, surviving an exponential detector.
    assert(span > 30.0);
    assert(std::fabs(span - 40.0) < 3.0);
    // The printed scale is honest to better than a dB and a half anywhere.
    assert(worst < 1.5);

    // ABSOLUTE, player-observable windows at the shipped default (0.35). A gate
    // that cannot do both of these is not a gate, whatever its knob says.
    {
        // A realistic single-coil hiss floor stays SHUT.
        GateModel g = makeGate(sr, 0.35f, 0.5f);
        std::vector<float> in(static_cast<size_t>(sr * 1.5)), out;
        unsigned s = 987654321u;
        for (size_t i = 0; i < in.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            const double r = (static_cast<int>(s >> 9) / 4194304.0) - 1.0;
            in[i] = static_cast<float>(1.0e-3 * r);  // ~-60 dBV
        }
        renderBlocks(g, in, out);
        assert(!g.isOpen());
        // ...and a normally played note OPENS it.
        GateModel g2 = makeGate(sr, 0.35f, 0.5f);
        const std::vector<float> note = sineBuf(sr, 220.0, 0.15, 0.5);
        std::vector<float> o2;
        renderBlocks(g2, note, o2);
        assert(g2.isOpen());
        std::printf("       default 0.35: a -60 dBV hiss stays SHUT, a 0.15 V note OPENS\n");
    }
}

// ---------------------------------------------------------------------------
// 2. HYSTERESIS — measured as a GAP IN dB, and it must be > 0 (docs §61.5)
// ---------------------------------------------------------------------------
//
// Ramp the level up until the gate opens, then back down until it closes. With
// the shipped 6 dB of positive feedback in the sidechain the gap measures
// 6.09 dB. Set the hysteresis to zero and the identical measurement gives
// 0.21 dB — which is the detector's own ripple, not a design.
void testHysteresisGap(double sr) {
    std::printf("[gate] hysteresis gap (level ramp, 220 Hz, knob 0.35)\n");
    GateModel g = makeGate(sr, 0.35f, 0.5f);
    const int n = static_cast<int>(sr * 16.0);
    double openDb = 0.0, closeDb = 0.0;
    bool prev = false, sawOpen = false, sawClose = false;
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        const double frac = t < 8.0 ? t / 8.0 : (16.0 - t) / 8.0;
        const double dbv = -70.0 + 50.0 * frac;
        const float x =
            static_cast<float>(std::pow(10.0, dbv / 20.0) * std::sin(kTwoPi * 220.0 * t));
        float y = 0.0f;
        g.process(&x, &y, 1);
        if (g.isOpen() && !prev && t < 8.0) { openDb = dbv; sawOpen = true; }
        if (!g.isOpen() && prev && t > 8.0) { closeDb = dbv; sawClose = true; }
        prev = g.isOpen();
    }
    assert(sawOpen && sawClose);
    const double gap = openDb - closeDb;
    std::printf("       opens at %.2f dBV, closes at %.2f dBV -> GAP %.2f dB\n",
                openDb, closeDb, gap);
    // The bar the plan names: strictly greater than zero.
    assert(gap > 0.0);
    // ...and inside a window around the literature's own -6 dB starting point.
    // Wide, because the measured gap is the DESIGN plus the detector's ripple and
    // the ramp's own rate, not the design alone.
    assert(gap > 3.0 && gap < 10.0);
}

// ---------------------------------------------------------------------------
// 3. THE CHATTER TEST — the one that matters (docs §61.5)
// ---------------------------------------------------------------------------
//
// A decaying low E crosses the threshold once on its way down. A gate with no
// hysteresis oscillates across that crossing because the detector's envelope
// ripples at 2*f0. This must show exactly ONE close.
void testChatter(double sr) {
    std::printf("[gate] chatter on a decaying low E (82.4 Hz, 0.30 V, tau 0.9 s)\n");
    const double knobs[] = {0.35, 0.5, 0.65};
    for (double knob : knobs) {
        GateModel g = makeGate(sr, static_cast<float>(knob), 0.35f);
        const std::vector<float> in = pluckBuf(sr, 82.41, 0.30, 0.9, 6.0);
        int opens = 0, closes = 0;
        bool prev = false;
        for (size_t i = 0; i < in.size(); ++i) {
            float y = 0.0f;
            g.process(&in[i], &y, 1);
            if (g.isOpen() && !prev) ++opens;
            if (!g.isOpen() && prev) ++closes;
            prev = g.isOpen();
        }
        std::printf("       knob %.2f: %d open(s), %d close(s)\n", knob, opens,
                    closes);
        // It opened for the note, and it closed ONCE when the note died.
        assert(opens == 1);
        assert(closes == 1);
        assert(!g.isOpen());
    }
}

// ---------------------------------------------------------------------------
// 4. THE PICK ATTACK SURVIVES (docs §61.6)
// ---------------------------------------------------------------------------
//
// A gate's characteristic failure is chopping the front of the note. Measured
// against the SAME signal through the SAME oversampler with the gate held open
// (THRESHOLD at minimum, pre-opened by a preceding tone), so the comparison
// isolates the gate's decision from the interpolator's own group delay.
void testAttackTransient(double sr) {
    std::printf("[gate] pick attack through an opening gate\n");
    const double lead = 0.5;  // silence before the pick
    const int nLead = static_cast<int>(sr * lead);
    std::vector<float> in(static_cast<size_t>(sr * 2.0), 0.0f);
    const std::vector<float> note = pluckBuf(sr, 220.0, 0.30, 0.9, 1.5);
    for (size_t i = 0; i < note.size() && nLead + i < in.size(); ++i)
        in[static_cast<size_t>(nLead) + i] = note[i];

    GateModel gated = makeGate(sr, 0.35f, 0.5f);
    std::vector<float> outG;
    renderBlocks(gated, in, outG);

    // The reference: the same model with the gate held OPEN throughout, by
    // pre-charging it with a loud steady tone and then rendering the identical
    // buffer. Same code path, same latency, same coupling caps.
    GateModel ref = makeGate(sr, 0.0f, 0.5f);
    {
        const std::vector<float> hot = sineBuf(sr, 220.0, 0.5, 0.5);
        std::vector<float> tmp;
        renderBlocks(ref, hot, tmp);
        assert(ref.isOpen());
    }
    std::vector<float> outR;
    renderBlocks(ref, in, outR);

    // First 20 ms of the note, in both.
    const size_t a = static_cast<size_t>(nLead);
    const size_t b = a + static_cast<size_t>(sr * 0.020);
    const double pkG = peakOf(outG, a, b), pkR = peakOf(outR, a, b);
    const double rmsG = rmsOf(outG, a, b), rmsR = rmsOf(outR, a, b);
    // Time for each to first cross 90 % of the OPEN reference's 20 ms peak.
    auto crossMs = [&](const std::vector<float>& v) {
        for (size_t i = a; i < a + static_cast<size_t>(sr * 0.5); ++i)
            if (std::fabs(static_cast<double>(v[i])) > 0.9 * pkR)
                return 1000.0 * static_cast<double>(i - a) / sr;
        return 1e9;
    };
    const double tG = crossMs(outG), tR = crossMs(outR);
    std::printf("       20 ms peak: gated %.5f vs open %.5f -> %.2f dB lost\n", pkG,
                pkR, dbOf(pkR / pkG));
    std::printf("       20 ms rms : gated %.5f vs open %.5f -> %.2f dB lost\n", rmsG,
                rmsR, dbOf(rmsR / rmsG));
    std::printf("       time to 90%% of the open peak: gated %.2f ms, open %.2f ms"
                " -> %.2f ms lost\n", tG, tR, tG - tR);
    // The pick edge must not be eaten. A compressor at the same job loses
    // 7.7-20.0 dB of transient (docs §59.6); a gate has no business losing any.
    assert(dbOf(pkR / pkG) < 1.0);
    assert(dbOf(rmsR / rmsG) < 3.0);
    // ...and it must not be LATE by more than a few ms, or the note starts soft.
    assert(tG - tR < 5.0);
}

// ---------------------------------------------------------------------------
// 5. NOISE REDUCTION, in dB, on a realistic floor (docs §61.6)
// ---------------------------------------------------------------------------
void testNoiseReduction(double sr) {
    std::printf("[gate] noise reduction with the gate closed\n");
    for (double dbv : {-60.0, -70.0}) {
        GateModel g = makeGate(sr, 0.35f, 0.2f);
        std::vector<float> in(static_cast<size_t>(sr * 2.0)), out;
        unsigned s = 24680u;
        const double amp = std::pow(10.0, dbv / 20.0);
        for (size_t i = 0; i < in.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            const double r = (static_cast<int>(s >> 9) / 4194304.0) - 1.0;
            in[i] = static_cast<float>(amp * r);
        }
        renderBlocks(g, in, out);
        const size_t h = in.size() / 2;
        const double red = dbOf(rmsOf(in, h, in.size()) / rmsOf(out, h, out.size()));
        std::printf("       %.0f dBV floor: %.2f dB of reduction (open=%d)\n", dbv,
                    red, static_cast<int>(g.isOpen()));
        assert(!g.isOpen());
        // The floor is the VCA's own off-isolation (-80 dB); assert well inside
        // it so the bar measures the GATE rather than that constant.
        assert(red > 60.0);
        assert(red < -dbOf(g.control().closedGain) + 1.0);
    }
}

// ---------------------------------------------------------------------------
// 6. THE CONTROL-LAW CONTRAST — THRESHOLD vs the Dyna Comp's SUSTAIN
// ---------------------------------------------------------------------------
//
// Docs §59.4, measured on the compressor: SUSTAIN moves the small-signal gain
// 25.33 dB (+11.25 -> +36.58) while moving the settled output 0.28 dB. This gate
// is the mirror image and the numbers are asserted in both directions.
void testControlLawContrast(double sr) {
    std::printf("[gate] control law: what THRESHOLD moves, and what it does not\n");
    const double knobs[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    double gMin = 1e9, gMax = -1e9;
    for (double knob : knobs) {
        GateModel g = makeGate(sr, static_cast<float>(knob), 0.5f);
        const std::vector<float> in = sineBuf(sr, 220.0, 0.30, 0.5);
        std::vector<float> out;
        renderBlocks(g, in, out);
        assert(g.isOpen());  // 0.30 V is above the threshold at every position
        const double pk = peakOf(out, out.size() / 2, out.size());
        const double gdb = dbOf(pk / 0.30);
        std::printf("       knob %.2f: gain when OPEN %+.4f dB\n", knob, gdb);
        gMin = std::min(gMin, gdb);
        gMax = std::max(gMax, gdb);
    }
    const double gainTravel = gMax - gMin;
    const double threshTravel =
        dbOf(openLevelVolts(sr, 1.0)) - dbOf(openLevelVolts(sr, 0.0));
    std::printf("       THRESHOLD travel: %.2f dB of THRESHOLD, %.4f dB of GAIN\n",
                threshTravel, gainTravel);
    std::printf("       (docs §59.4, the Dyna Comp's SUSTAIN: 25.33 dB of GAIN,"
                " 0.28 dB of OUTPUT — the mirror image)\n");
    // A threshold control moves a threshold and nothing else.
    assert(threshTravel > 30.0);
    assert(gainTravel < 0.05);
    // ...and an OPEN gate is unity, which is what makes it usable always-on.
    assert(std::fabs(gMax) < 0.2 && std::fabs(gMin) < 0.2);
}

// ---------------------------------------------------------------------------
// 7. THE DECAY KNOB IS THE ONLY TIME CONSTANT THE PLAYER HAS (docs §61.6)
// ---------------------------------------------------------------------------
void testDecayKnob(double sr) {
    std::printf("[gate] DECAY: gain fall time after the note stops\n");
    double prev = -1.0;
    for (double knob : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        GateModel g = makeGate(sr, 0.35f, static_cast<float>(knob));
        const std::vector<float> in = sineBuf(sr, 220.0, 0.30, 0.5);
        std::vector<float> out;
        renderBlocks(g, in, out);
        assert(g.isOpen());
        double t3 = -1.0, t20 = -1.0;
        const int n = static_cast<int>(sr * 12.0);
        for (int i = 0; i < n; ++i) {
            const float z = 0.0f;
            float y = 0.0f;
            g.process(&z, &y, 1);
            const double gdb = dbOf(g.gainNow());
            if (t3 < 0.0 && gdb < -3.0) t3 = i / sr;
            if (t20 < 0.0 && gdb < -20.0) { t20 = i / sr; break; }
        }
        assert(t3 > 0.0 && t20 > 0.0);
        std::printf("       knob %.2f: -3 dB at %6.1f ms, -20 dB at %7.1f ms,"
                    " span %7.1f ms\n", knob, 1000 * t3, 1000 * t20,
                    1000 * (t20 - t3));
        // Monotone in the knob, and the fast end really is fast.
        assert(t20 > prev);
        prev = t20;
    }
    // The HOLD is fixed, has no knob, and is what stops a gate chasing a low
    // note's own period. Measured as the player hears it, NOT read back off the
    // constant: at the FASTEST decay setting, how long the gain stays within
    // 0.5 dB of unity after the comparator says closed. Reading
    // `control().holdSeconds` and asserting it would be an identity — moving the
    // constant would move the bar with it.
    GateModel g = makeGate(sr, 0.35f, 0.0f);
    {
        const std::vector<float> in = sineBuf(sr, 220.0, 0.30, 0.5);
        std::vector<float> out;
        renderBlocks(g, in, out);
        assert(g.isOpen());
        double heldMs = -1.0;
        const int n = static_cast<int>(sr * 1.0);
        for (int i = 0; i < n; ++i) {
            const float z = 0.0f;
            float y = 0.0f;
            g.process(&z, &y, 1);
            if (!g.isOpen() && heldMs < 0.0) {
                // The comparator has let go; count from here.
                int held = 0;
                while (held < n && dbOf(g.gainNow()) > -0.5) {
                    g.process(&z, &y, 1);
                    ++held;
                }
                heldMs = 1000.0 * held / sr;
            }
            if (heldMs >= 0.0) break;
        }
        std::printf("       gain stays within 0.5 dB of unity for %.1f ms after the"
                    " comparator closes (fixed hold, no knob)\n", heldMs);
        // The published designer practice is 20-30 ms of built-in hold. Measured
        // from the audio, at the knob setting that would otherwise decay fastest.
        assert(heldMs > 18.0 && heldMs < 45.0);
    }
    std::printf("       fixed attack %.2f ms (no knob)\n",
                1000 * g.control().attackSeconds);
    assert(g.control().attackSeconds <= 0.001);
}

// ---------------------------------------------------------------------------
// 8. THE DETECTOR IS FEED-FORWARD, AND FEED-BACK IS BROKEN (docs §61.7)
// ---------------------------------------------------------------------------
//
// The compressor's detector tastes its own OUTPUT and docs §59 proves that is
// load-bearing there. For a gate the same tap is not a variant, it is a latch:
// the moment the gate closes the detector sees the VCA's off-isolation and can
// never see enough to re-open. Measured rather than argued.
void testDetectorTapIsFeedForward(double sr) {
    std::printf("[gate] detector tap (a gate cannot be feed-back)\n");
    double pk[2] = {0.0, 0.0};
    int open[2] = {0, 0};
    for (int fb = 0; fb < 2; ++fb) {
        GateModel g;
        g.prepare(sr, 128);
        g.setDetectorFromOutput(fb != 0);
        g.setParameter(GateModel::PARAM_THRESHOLD, 0.35f);
        const std::vector<float> in = sineBuf(sr, 220.0, 0.30, 1.0);
        std::vector<float> out;
        renderBlocks(g, in, out);
        pk[fb] = peakOf(out, out.size() / 2, out.size());
        open[fb] = static_cast<int>(g.isOpen());
        std::printf("       %s: settled peak %.6e (%+.2f dB re input), open=%d\n",
                    fb ? "feed-BACK  " : "feed-forward", pk[fb],
                    dbOf(pk[fb] / 0.30), open[fb]);
    }
    assert(open[0] == 1);
    assert(open[1] == 0);
    // The latch is total: a loud note gets the VCA's off-isolation and nothing else.
    assert(dbOf(pk[0] / pk[1]) > 40.0);
}

// ---------------------------------------------------------------------------
// 9. NOT OVERSAMPLED - and the measurement that decided it (docs §61.7)
// ---------------------------------------------------------------------------
//
// The gate's signal path is a MULTIPLY. The only nonlinearity in the pedal is in
// the sidechain, and the sidechain reaches the output only through a control ramp
// whose own corner is 400 Hz, so nothing it makes can fold back into the band.
// Two bars, both of which go red if a future slice puts a nonlinearity in the
// signal path or re-introduces an oversampler:
//
//   * on the WORST stimulus available - a 9 kHz tone forced open and shut at
//     4 Hz, so the gate is switching while the tone sits near a fifth of the
//     sample rate - the non-harmonic floor is 176.9 dB below the fundamental;
//   * `setOversampling()` is a real no-op: the render is BIT-IDENTICAL at every
//     factor and the latency is exactly 0.
void testNotOversampled(double sr) {
    std::printf("[gate] the gate is NOT oversampled, and this is why\n");
    const int n = static_cast<int>(sr * 2.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        const double env = 0.02 + 0.28 * (0.5 + 0.5 * std::sin(kTwoPi * 4.0 * t));
        in[static_cast<size_t>(i)] =
            static_cast<float>(env * std::sin(kTwoPi * 9000.0 * t));
    }
    std::vector<float> ref;
    for (int factor : {1, 2, 4, 8}) {
        GateModel g;
        g.prepare(sr, 128);
        g.setOversampling(factor);
        g.setParameter(GateModel::PARAM_THRESHOLD, 0.5f);
        g.setParameter(GateModel::PARAM_DECAY, 0.0f);
        std::vector<float> out;
        renderBlocks(g, in, out);
        const size_t A = out.size() / 2, L = out.size() - A;
        const double f1 = goertzelAmp(out, A, L, 9000.0, sr);
        double w = 0.0;
        for (double fr = 200.0; fr < 8000.0; fr += 100.0)
            w = std::max(w, goertzelAmp(out, A, L, fr, sr));
        std::printf("       setOversampling(%d): floor %.2f dB re fundamental,"
                    " reported factor %d, latency %d\n",
                    factor, dbOf(w / f1), g.oversampling(), g.latencySamples());
        assert(dbOf(w / f1) < -120.0);
        assert(g.oversampling() == 1);
        assert(g.latencySamples() == 0);
        if (factor == 1) {
            ref = out;
        } else {
            double worst = 0.0;
            for (size_t j = 0; j < out.size(); ++j)
                worst = std::max(worst,
                                 std::fabs(static_cast<double>(out[j] - ref[j])));
            assert(worst == 0.0);
        }
    }
    std::printf("       every factor bit-identical to 1x; a utility pedal adds 0"
                " samples to the board\n");
}

// ---------------------------------------------------------------------------
// 10. DC on SIGNAL, both stimuli (support/DcOffset.h).
// ---------------------------------------------------------------------------
void testDcOnSignal(double sr) {
    std::printf("[gate] DC offset ON SIGNAL\n");
    for (int withOffset = 0; withOffset < 2; ++withOffset) {
        GateModel g = makeGate(sr, 0.35f, 0.5f);
        std::vector<float> in = sineBuf(sr, 220.0, 0.30, 2.0), out;
        if (withOffset)
            for (auto& v : in) v += 0.1f;
        renderBlocks(g, in, out);
        const auto d = clipper::test::measureDcOnSignal(out, 0.5);
        std::printf("       %s: mean %.6f V on a %.4f V peak = %.4f %%\n",
                    withOffset ? "with +0.1 V input offset" : "clean tone          ",
                    d.mean, d.peak, 100.0 * d.fraction);
        assert(d.fraction < 0.01);
    }
}

// ---------------------------------------------------------------------------
// 11. reset(), NaN recovery, ragged blocking, denormal rest.
// ---------------------------------------------------------------------------
void testHygiene(double sr) {
    std::printf("[gate] hygiene\n");

    // Block-size invariance: 128 vs a ragged sequence, bit-for-bit.
    {
        const std::vector<float> in = pluckBuf(sr, 110.0, 0.30, 1.2, 3.0);
        GateModel a = makeGate(sr, 0.35f, 0.5f);
        std::vector<float> outA;
        renderBlocks(a, in, outA);

        GateModel b = makeGate(sr, 0.35f, 0.5f);
        std::vector<float> outB(in.size(), 0.0f);
        const int sizes[] = {100, 37, 256, 1, 411};
        size_t i = 0;
        int k = 0;
        while (i < in.size()) {
            const int n = static_cast<int>(
                std::min(static_cast<size_t>(sizes[k % 5]), in.size() - i));
            b.process(in.data() + i, outB.data() + i, n);
            i += static_cast<size_t>(n);
            ++k;
        }
        double worst = 0.0;
        for (size_t j = 0; j < in.size(); ++j)
            worst = std::max(worst, std::fabs(static_cast<double>(outA[j] - outB[j])));
        std::printf("       block-size invariance (128 vs ragged): %.3e\n", worst);
        assert(worst == 0.0);
    }

    // reset() re-parks exactly on a fresh model, and clears a NaN.
    {
        GateModel a = makeGate(sr, 0.35f, 0.5f);
        const std::vector<float> hot = sineBuf(sr, 220.0, 0.5, 0.5);
        std::vector<float> tmp;
        renderBlocks(a, hot, tmp);
        std::vector<float> poison(256, 0.0f);
        poison[10] = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> pOut;
        renderBlocks(a, poison, pOut);
        int bad = 0;
        for (float v : pOut) if (!std::isfinite(v)) ++bad;
        a.reset();
        std::vector<float> in2 = sineBuf(sr, 220.0, 0.30, 1.0), out2;
        renderBlocks(a, in2, out2);
        int bad2 = 0;
        for (float v : out2) if (!std::isfinite(v)) ++bad2;
        std::printf("       one NaN poisons %d/256 samples; after reset() %d/%zu\n",
                    bad, bad2, out2.size());
        assert(bad2 == 0);

        GateModel fresh = makeGate(sr, 0.35f, 0.5f);
        std::vector<float> outFresh;
        renderBlocks(fresh, in2, outFresh);
        double worst = 0.0;
        for (size_t j = 0; j < in2.size(); ++j)
            worst = std::max(worst,
                             std::fabs(static_cast<double>(out2[j] - outFresh[j])));
        std::printf("       reset() vs a fresh model: %.3e\n", worst);
        assert(worst == 0.0);
    }

    // Denormals (ADR 006): the states that rest at ZERO must be EXACTLY zero.
    {
        GateModel g = makeGate(sr, 0.35f, 0.5f);
        const std::vector<float> hot = sineBuf(sr, 220.0, 0.5, 0.5);
        std::vector<float> tmp;
        renderBlocks(g, hot, tmp);
        std::vector<float> quiet(static_cast<size_t>(sr * 8.0), 0.0f), qOut;
        renderBlocks(g, quiet, qOut);
        std::printf("       after 8 s of silence: maxAbsRestingState %.3e,"
                    " envelope %.4f V, VCA gain %.4e\n",
                    g.maxAbsRestingState(), g.envelopeVolts(), g.gainNow());
        assert(g.maxAbsRestingState() == 0.0);
        // The two states deliberately NOT guarded, each at a real operating point.
        assert(g.envelopeVolts() > 8.9);
        assert(g.gainNow() > 0.9e-4 && g.gainNow() < 1.1e-4);
        // ...and the output is EXACT digital silence.
        double pk = 0.0;
        for (size_t j = qOut.size() / 2; j < qOut.size(); ++j)
            pk = std::max(pk, std::fabs(static_cast<double>(qOut[j])));
        assert(pk == 0.0);
    }
}

// ---------------------------------------------------------------------------
// 12. No zipper: a THRESHOLD move must not step the audio.
// ---------------------------------------------------------------------------
void testNoZipper(double sr) {
    std::printf("[gate] knob-move seam vs the signal's own slew\n");
    const std::vector<float> in = sineBuf(sr, 220.0, 0.30, 2.0);
    GateModel g = makeGate(sr, 0.10f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);
    const size_t half = in.size() / 2;
    g.process(in.data(), out.data(), static_cast<int>(half));
    g.setParameter(GateModel::PARAM_THRESHOLD, 0.50f);
    g.process(in.data() + half, out.data() + half,
              static_cast<int>(in.size() - half));
    double ownSlew = 0.0;
    for (size_t i = half + 2000; i + 1 < out.size(); ++i)
        ownSlew = std::max(ownSlew, std::fabs(static_cast<double>(out[i + 1] - out[i])));
    const double seam =
        std::fabs(static_cast<double>(out[half] - out[half - 1]));
    std::printf("       seam step %.3e vs settled slew %.3e -> %.2fx\n", seam,
                ownSlew, seam / ownSlew);
    assert(seam < 2.0 * ownSlew);
}

// ---------------------------------------------------------------------------
// 13. Rate independence.
// ---------------------------------------------------------------------------
void testRateIndependence() {
    std::printf("[gate] threshold across sample rates\n");
    double lo = 1e9, hi = -1e9;
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
        const double db = dbOf(openLevelVolts(sr, 0.35));
        std::printf("       %.0f Hz: opens at %.2f dBV\n", sr, db);
        lo = std::min(lo, db);
        hi = std::max(hi, db);
    }
    std::printf("       spread %.2f dB\n", hi - lo);
    assert(hi - lo < 1.0);
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    const double sr = 48000.0;
    std::printf("=== M13.6a: the \"Curfew\" noise gate (docs §61) ===\n");
    testThresholdLaw(sr);
    testHysteresisGap(sr);
    testChatter(sr);
    testAttackTransient(sr);
    testNoiseReduction(sr);
    testControlLawContrast(sr);
    testDecayKnob(sr);
    testDetectorTapIsFeedForward(sr);
    testNotOversampled(sr);
    testDcOnSignal(sr);
    testHygiene(sr);
    testNoZipper(sr);
    testRateIndependence();
    std::printf("=== gate tests OK ===\n");
    return 0;
}
