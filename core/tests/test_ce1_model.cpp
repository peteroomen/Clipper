// Plain-assert tests for clipper::dsp::Ce1Model (M13.7 — the Boss CE-1 Chorus
// Ensemble). No framework: int main + <cassert>.
//
// THE FRAME FOR THIS WHOLE FILE. The CE-1 is the Roland JC-120's chorus circuit
// in a floor box, and this project already models that circuit (`ChorusModel`,
// docs §11.2). `Ce1Model` OWNS one. So the interesting question is not "does a
// swept delay line work" — §11.2 answered that — it is "is this measurably a
// CE-1 rather than the amp's chorus with a new name?", and every bar here is
// pointed at that question or at a player-observable property.
//
// What is asserted, and why each is the property that matters:
//
//   1. THE RATE LAW, in Hz, against MEASURED figures from a teardown of a real
//      unit — 1.0..3.0 Hz in chorus, 3.2..11.6 Hz in vibrato. The TAPER is
//      DERIVED, not chosen: the same teardown publishes the chorus midpoint as
//      "about 1.75 Hz", which log gives as 1.7321 (1.0 % off) and linear gives
//      as 2.0 (14.3 % off). The test asserts the derivation, so nobody can
//      quietly swap in a linear pot.
//   2. THE NON-OVERLAP CROSS-CHECK. Fastest chorus (3.0) < slowest vibrato
//      (3.2). The two ranges were sourced independently AND the property is
//      stated separately in prose, so three facts agree. It is the strongest
//      check available on this voice.
//   3. MODULATION DEPTH AS PITCH DEVIATION IN CENTS. A delay-line chorus
//      DETUNES; that is the effect, so it is measured in cents off a rendered
//      zero-crossing pitch track, not inferred from a delay figure.
//   4. CHORUS vs VIBRATO AS TWO MEASURABLY DIFFERENT STATES. In vibrato the dry
//      path must be ABSENT — asserted as an EXACT 0.0 mix weight and, on audio,
//      as a steady tone whose envelope does not move (pure pitch modulation).
//      In chorus the dry path is present and the output COMBS.
//   5. THE COMB IS REAL AND IN THE RIGHT PLACE. A ~4 ms mono dry+wet mix nulls
//      first at 1/(2·4ms) = 125 Hz and peaks at 250 Hz. Both are measured, and
//      the null is shown to MOVE when DEPTH moves the base delay.
//   6. THE NUMERIC COMPARISON AGAINST THE AMP'S CHORUS. Including the one that
//      justifies the pedal existing: a MONO host taking the amp's chorus gets
//      the bit-exact DRY signal and therefore NO chorus at all.
//   7. Housekeeping a player still hears: DC on signal, reset(), no zipper on a
//      RATE sweep, block-size invariance, rate independence, the ADR 006
//      resting-state bar, and the NaN guard.
//
// PERTURBATION PROOFS for the load-bearing bars are recorded in docs §62.9.

#include "clipper/dsp/Ce1Model.h"

#include "clipper/dsp/ChorusModel.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCentsPerFrac = 1731.2340490667559;  // 1200 / ln2

// --- small helpers -----------------------------------------------------------

std::vector<float> tone(int n, double f, double fs, double amp = 1.0) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * f * i / fs));
    return v;
}

std::vector<float> render(double fs, float rate, float depth, float mode, double f0,
                          double secs, double amp = 1.0, int block = 128) {
    clipper::dsp::Ce1Model m;
    m.prepare(fs, block);
    m.setParameter(clipper::dsp::Ce1Model::PARAM_MODE, mode);
    m.setParameter(clipper::dsp::Ce1Model::PARAM_RATE, rate);
    m.setParameter(clipper::dsp::Ce1Model::PARAM_DEPTH, depth);
    const int n = static_cast<int>(fs * secs);
    std::vector<float> in = tone(n, f0, fs, amp), out(static_cast<size_t>(n));
    int done = 0;
    while (done < n) {
        const int k = std::min(block, n - done);
        m.process(in.data() + done, out.data() + done, k);
        done += k;
    }
    return out;
}

// Peak pitch deviation (cents) and LFO rate (Hz) from positive-going zero
// crossings of a steady tone. Zero-crossing spacing averages over a signal
// period, so it reads slightly UNDER the instantaneous peak at large deviations
// — the same caveat §11.2 records for the amp's chorus test.
void pitchTrack(const std::vector<float>& x, double fs, double f0, double& peakCents,
                double& lfoHz) {
    std::vector<double> cross;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
            cross.push_back(((i - 1) + (0.0 - x[i - 1]) / (x[i] - x[i - 1])) / fs);
    std::vector<double> cents;
    for (size_t i = 1; i < cross.size(); ++i)
        cents.push_back(1200.0 * std::log2((1.0 / (cross[i] - cross[i - 1])) / f0));
    const size_t s = cents.size() / 4;  // drop the settling quarter
    peakCents = 0.0;
    for (size_t i = s; i < cents.size(); ++i)
        peakCents = std::max(peakCents, std::fabs(cents[i]));
    int n = 0;
    for (size_t i = s + 1; i < cents.size(); ++i)
        if (cents[i - 1] <= 0.0 && cents[i] > 0.0) ++n;
    const double span = (cross.size() > s) ? (cross.back() - cross[s]) : 0.0;
    lfoHz = span > 0.0 ? n / span : 0.0;
}

// Goertzel magnitude at f over the settled tail.
double binMag(const std::vector<float>& x, double f, double fs, double skip = 0.34) {
    const size_t from = static_cast<size_t>(x.size() * skip);
    const double w = 2.0 * kPi * f / fs, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < x.size(); ++i) {
        const double s0 = static_cast<double>(x[i]) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::fabs(s1 * s1 + s2 * s2 - c * s1 * s2)) /
           static_cast<double>(x.size() - from);
}

// Peak-envelope modulation depth (dB) of the settled tail, in 20 ms windows.
double envelopeDepthDb(const std::vector<float>& y, double fs) {
    const int w = static_cast<int>(fs * 0.02);
    double lo = 1e30, hi = 0.0;
    for (size_t i = y.size() / 3; i + static_cast<size_t>(w) < y.size();
         i += static_cast<size_t>(w)) {
        double pk = 0.0;
        for (int j = 0; j < w; ++j)
            pk = std::max(pk, std::fabs(static_cast<double>(y[i + static_cast<size_t>(j)])));
        lo = std::min(lo, pk);
        hi = std::max(hi, pk);
    }
    if (lo <= 0.0) return 1e30;
    return 20.0 * std::log10(hi / lo);
}

// ============================================================================
// 1 + 2. THE RATE LAW, and the non-overlap cross-check
// ============================================================================
void testRateLaw(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[1] LFO RATE vs the RATE knob, per mode (Hz)\n");
    std::printf("    knob |  CHORUS   |  VIBRATO\n");

    double chorus[5], vib[5];
    for (int i = 0; i < 5; ++i) {
        const float k = static_cast<float>(i) * 0.25f;
        M a, b;
        a.prepare(fs, 128);
        b.prepare(fs, 128);
        a.setParameter(M::PARAM_MODE, 0.0f);
        a.setParameter(M::PARAM_RATE, k);
        b.setParameter(M::PARAM_MODE, 1.0f);
        b.setParameter(M::PARAM_RATE, k);
        chorus[i] = a.rateHz();
        vib[i] = b.rateHz();
        std::printf("    %.2f | %8.4f  | %8.4f\n", k, chorus[i], vib[i]);
    }

    // Endpoints are the teardown's MEASURED figures. These are absolute
    // references, not this model's own numbers.
    assert(std::fabs(chorus[0] - 1.0) < 1e-9);
    assert(std::fabs(chorus[4] - 3.0) < 1e-9);
    assert(std::fabs(vib[0] - 3.2) < 1e-9);
    assert(std::fabs(vib[4] - 11.6) < 1e-9);

    // Monotone in both modes — a rate knob that folds back is a defect a player
    // finds immediately.
    for (int i = 1; i < 5; ++i) {
        assert(chorus[i] > chorus[i - 1]);
        assert(vib[i] > vib[i - 1]);
    }

    // THE TAPER IS DERIVED. The teardown reports the chorus midpoint as ~1.75 Hz.
    // A log pot predicts sqrt(1*3) = 1.7321; a linear pot predicts 2.0. Assert
    // that we are within 3 % of the PUBLISHED midpoint, which the linear law
    // misses by 14.3 %. This is the bar that stops a "tidy" linear map.
    const double published = 1.75;
    const double errLog = std::fabs(chorus[2] - published) / published;
    const double errLin = std::fabs(2.0 - published) / published;
    std::printf("    midpoint: model %.4f Hz vs published %.2f Hz -> %.2f %% "
                "(a LINEAR pot would be %.2f %% off)\n",
                chorus[2], published, 100.0 * errLog, 100.0 * errLin);
    assert(errLog < 0.03);
    assert(errLin > 0.10);  // the refuted alternative really is refuted

    // [2] THE CROSS-CHECK: the ranges do not overlap, and the gap is the right
    // way round. Sourced independently AND stated separately in prose.
    std::printf("    non-overlap: fastest chorus %.2f Hz < slowest vibrato %.2f Hz\n",
                chorus[4], vib[0]);
    assert(chorus[4] < vib[0]);
    // And they are genuinely different effects, not two speeds of one: the
    // vibrato range is nearly 4x faster at the top.
    assert(vib[4] / chorus[4] > 3.5);
}

// ============================================================================
// 3. MODULATION DEPTH AS PITCH DEVIATION IN CENTS
// ============================================================================
void testDepthInCents(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[3] DEPTH -> delay window and RENDERED pitch deviation (cents)\n");
    std::printf("    depth | base_ms sweep_ms | window_ms       | analytic_c  rendered_c\n");

    double sweep[3], base[3], rendered[3];
    const double dk[3] = {0.0, 0.5, 1.0};
    for (int i = 0; i < 3; ++i) {
        M m;
        m.prepare(fs, 128);
        m.setParameter(M::PARAM_MODE, 1.0f);  // vibrato: a clean pitch track
        m.setParameter(M::PARAM_RATE, 0.0f);  // 3.2 Hz — slow enough to resolve
        m.setParameter(M::PARAM_DEPTH, static_cast<float>(dk[i]));
        base[i] = m.baseDelayMs();
        sweep[i] = m.sweepMs();
        const double an = kCentsPerFrac * 2.0 * kPi * sweep[i] * 1e-3 * m.rateHz();
        const auto y = render(fs, 0.0f, static_cast<float>(dk[i]), 1.0f, 440.0, 14.0);
        double pc = 0.0, lf = 0.0;
        pitchTrack(y, fs, 440.0, pc, lf);
        rendered[i] = pc;
        std::printf("    %.2f  |  %.3f   %.3f  | %.3f .. %.3f  | %9.3f  %10.3f\n", dk[i],
                    base[i], sweep[i], base[i] - sweep[i], base[i] + sweep[i], an, pc);
        // The render must agree with the physics of a swept delay to 15 %.
        assert(std::fabs(pc - an) / an < 0.15);
    }

    // The published window, read literally: the delay FLOOR is pinned at 3 ms and
    // the CEILING opens 5 -> 7 ms. Assert both ends against the source.
    assert(std::fabs((base[0] - sweep[0]) - 3.0) < 1e-9);
    assert(std::fabs((base[2] - sweep[2]) - 3.0) < 1e-9);
    assert(std::fabs((base[0] + sweep[0]) - 5.0) < 1e-9);
    assert(std::fabs((base[2] + sweep[2]) - 7.0) < 1e-9);

    // THE DEPTH KNOB AT ZERO STILL MODULATES. That is the circuit, and it is the
    // single most likely thing for a later slice to "fix" by mistake.
    assert(sweep[0] > 0.9);
    assert(rendered[0] > 20.0);
    std::printf("    DEPTH 0 still detunes by %.1f cents — the CE-1's floor, not a bug\n",
                rendered[0]);

    // And the knob has real authority: max is at least 1.8x min.
    assert(rendered[2] / rendered[0] > 1.8);
}

// ============================================================================
// 4. CHORUS vs VIBRATO — two measurably different states, and the dry-path null
// ============================================================================
void testChorusVsVibrato(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[4] CHORUS vs VIBRATO — the dry-path null\n");

    // (a) The mix weights, settled. VIBRATO's dry weight must be EXACTLY zero:
    // "absent" and "very quiet" are different claims and only one is the circuit.
    // The smoother needs ~0.5 s to snap (its residual guard fires at 1e-30).
    M c, v;
    c.prepare(fs, 128);
    v.prepare(fs, 128);
    c.setParameter(M::PARAM_MODE, 0.0f);
    v.setParameter(M::PARAM_MODE, 1.0f);
    const int settle = static_cast<int>(fs * 1.5);
    std::vector<float> z(static_cast<size_t>(settle), 0.0f), o(static_cast<size_t>(settle));
    for (int off = 0; off < settle; off += 128) {
        const int k = std::min(128, settle - off);
        c.process(z.data() + off, o.data() + off, k);
        v.process(z.data() + off, o.data() + off, k);
    }
    std::printf("    settled mix: CHORUS dry %.6f / wet %.6f | VIBRATO dry %.6f / wet %.6f\n",
                c.dryGain(), c.wetGain(), v.dryGain(), v.wetGain());
    // The dry weight must be EXACTLY 0.0 — "absent" and "very quiet" are
    // different claims and only one of them is the circuit. It reaches exact zero
    // because OnePoleSmoother's residual guard fires: a value decaying toward a
    // ZERO target keeps halving until (target - value) drops under 1e-30, and the
    // guard then snaps it.
    assert(v.dryGain() == 0.0);
    // The WET weight is deliberately NOT asserted exact, and the reason is worth
    // knowing before you write the next smoother test: approaching a NONZERO
    // target the same guard CANNOT fire, so the ramp stalls short and stays
    // there. `value += coeff*(target-value)` stops moving as soon as the
    // increment falls below half a float ULP at the target, i.e. at a residual of
    // about ULP/coeff = 1.19e-7 / 0.0026 = 4.6e-5 — twenty-five decades above the
    // 1e-30 floor the guard watches. Measured here: wet settles at 0.99998856,
    // i.e. 1.14e-5 short (-98.8 dB). Exact convergence is a property of a ZERO
    // target only, which is precisely the one this test needs to be exact.
    // Inaudible, but an `==` here would be a flake rather than a bar.
    assert(std::fabs(v.wetGain() - 1.0) < 1e-4);
    assert(c.dryGain() > 0.4 && c.wetGain() > 0.4);

    // (b) On audio. A steady tone through VIBRATO is pure pitch modulation, so
    // its ENVELOPE does not move. Through CHORUS the dry and wet sum and comb, so
    // the envelope swings hard. This is the two-states property as a number.
    const double envC = envelopeDepthDb(render(fs, 0.5f, 1.0f, 0.0f, 440.0, 8.0), fs);
    const double envV = envelopeDepthDb(render(fs, 0.5f, 1.0f, 1.0f, 440.0, 8.0), fs);
    std::printf("    440 Hz envelope depth: CHORUS %.2f dB  VIBRATO %.2f dB\n", envC, envV);
    assert(envC > 8.0);   // the comb is unmistakably there
    assert(envV < 0.5);   // and unmistakably absent
    assert(envC > 15.0 * envV + 5.0);

    // (c) The converse: VIBRATO must still MODULATE PITCH (it is not a bypass).
    double pc = 0.0, lf = 0.0;
    pitchTrack(render(fs, 0.5f, 1.0f, 1.0f, 440.0, 12.0), fs, 440.0, pc, lf);
    std::printf("    VIBRATO pitch deviation %.1f cents at %.2f Hz — flat envelope, moving pitch\n",
                pc, lf);
    assert(pc > 60.0);
}

// ============================================================================
// 5. THE COMB — in the right place, and it moves
// ============================================================================
void testComb(double fs) {
    std::printf("\n[5] THE COMB in CHORUS mode (mono dry+wet sum)\n");

    // At DEPTH 0 the delay centres on 4.0 ms, so a dry+wet mix nulls first at
    // 1/(2*4ms) = 125 Hz and peaks at 1/4ms = 250 Hz. Those frequencies come from
    // the SOURCED delay window, not from this model's internals.
    const double null0 = binMag(render(fs, 0.0f, 0.0f, 0.0f, 125.0, 6.0), 125.0, fs);
    const double peak0 = binMag(render(fs, 0.0f, 0.0f, 0.0f, 250.0, 6.0), 250.0, fs);
    const double contrast = 20.0 * std::log10(peak0 / null0);
    std::printf("    DEPTH 0 (delay centres 4.00 ms): |H| at 125 Hz %.5f, at 250 Hz %.5f"
                " -> %.2f dB of comb\n", null0, peak0, contrast);
    assert(contrast > 12.0);

    // AND THE COMB MOVES. DEPTH 1 centres the delay on 5.0 ms, whose first null
    // is at 100 Hz — so the 125 Hz probe must come UP out of the null while the
    // 100 Hz probe goes DOWN into it.
    const double at125_d1 = binMag(render(fs, 0.0f, 1.0f, 0.0f, 125.0, 6.0), 125.0, fs);
    const double at100_d0 = binMag(render(fs, 0.0f, 0.0f, 0.0f, 100.0, 6.0), 100.0, fs);
    const double at100_d1 = binMag(render(fs, 0.0f, 1.0f, 0.0f, 100.0, 6.0), 100.0, fs);
    std::printf("    125 Hz: DEPTH 0 %.5f -> DEPTH 1 %.5f (%+.2f dB, rises out of the null)\n",
                null0, at125_d1, 20.0 * std::log10(at125_d1 / null0));
    std::printf("    100 Hz: DEPTH 0 %.5f -> DEPTH 1 %.5f (%+.2f dB, sinks toward it)\n",
                at100_d0, at100_d1, 20.0 * std::log10(at100_d1 / at100_d0));
    assert(at125_d1 > 2.0 * null0);
    assert(at100_d1 < at100_d0);

    // The comb is a CHORUS-mode property: vibrato has no dry path to interfere
    // with, so its 125 Hz bin is not nulled at all.
    const double vibAt125 = binMag(render(fs, 0.0f, 0.0f, 1.0f, 125.0, 6.0), 125.0, fs);
    std::printf("    125 Hz in VIBRATO %.5f — no dry path, so no null (%.2f dB above chorus)\n",
                vibAt125, 20.0 * std::log10(vibAt125 / null0));
    assert(vibAt125 > 4.0 * null0);
}

// ============================================================================
// 6. THE NUMERIC COMPARISON AGAINST THE AMP'S CHORUS
// ============================================================================
void testVersusAmpChorus(double fs) {
    using M = clipper::dsp::Ce1Model;
    using C = clipper::dsp::ChorusModel;
    std::printf("\n[6] CE-1 vs the amp's JC-120 chorus — where they differ, in numbers\n");

    M lo, hi;
    lo.prepare(fs, 128);
    hi.prepare(fs, 128);
    lo.setParameter(M::PARAM_RATE, 0.0f);
    hi.setParameter(M::PARAM_RATE, 1.0f);
    const double cLo = lo.rateHz(), cHi = hi.rateHz();
    lo.setParameter(M::PARAM_MODE, 1.0f);
    hi.setParameter(M::PARAM_MODE, 1.0f);
    const double vLo = lo.rateHz(), vHi = hi.rateHz();
    M d0, d1;
    d0.prepare(fs, 128);
    d1.prepare(fs, 128);
    d0.setParameter(M::PARAM_DEPTH, 0.0f);
    d1.setParameter(M::PARAM_DEPTH, 1.0f);

    // The amp's own published law (docs §11.2): 0.15..8 Hz for BOTH modes,
    // sweep 0..3.5 ms, base fixed at 5 ms.
    std::printf("    rate, chorus (Hz)   CE-1 %5.2f .. %5.2f   |  amp %5.2f .. %5.2f (one range,\n",
                cLo, cHi, 0.15, 8.0);
    std::printf("    rate, vibrato (Hz)  CE-1 %5.2f .. %5.2f   |      both modes)\n", vLo, vHi);
    std::printf("    sweep (ms)          CE-1 %5.2f .. %5.2f   |  amp %5.2f .. %5.2f\n",
                d0.sweepMs(), d1.sweepMs(), 0.0, 3.5);
    std::printf("    base delay (ms)     CE-1 %5.2f .. %5.2f   |  amp  5.00 (fixed)\n",
                d0.baseDelayMs(), d1.baseDelayMs());

    // The CE-1's chorus rate range sits INSIDE the amp's, and its sweep is far
    // more restrained — but never reaches zero. Both are real differences.
    assert(cLo > 0.15 && cHi < 8.0);
    assert(d1.sweepMs() < 3.5);
    assert(d0.sweepMs() > 0.0);
    // The CE-1's vibrato goes FASTER than the amp can (11.6 > 8.0).
    assert(vHi > 8.0);

    // THE ONE THAT JUSTIFIES THE PEDAL. The amp's CHORUS mode is a stereo SPLIT:
    // its LEFT channel is the BIT-EXACT dry input, so a MONO host taking one
    // channel hears no chorus whatsoever. The CE-1 ships the factory MONO jack,
    // which mixes — so it combs.
    C amp;
    amp.prepare(fs);
    amp.setSpeed(0.5f);
    amp.setDepth(0.7f);
    amp.setMode(C::MODE_CHORUS);
    const int n = static_cast<int>(fs * 6.0);
    std::vector<float> in = tone(n, 440.0, fs), L(static_cast<size_t>(n)),
                       R(static_cast<size_t>(n));
    amp.processStereo(in.data(), L.data(), R.data(), n);
    double maxDiff = 0.0;
    for (int i = 0; i < n; ++i)
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(L[static_cast<size_t>(i)] -
                                                                 in[static_cast<size_t>(i)])));
    const double envAmpL = envelopeDepthDb(L, fs);
    const double envCe1 = envelopeDepthDb(render(fs, 0.5f, 1.0f, 0.0f, 440.0, 6.0), fs);
    std::printf("    MONO behaviour: amp chorus L-channel is bit-exact dry (max|L-in| = %.3e),\n",
                maxDiff);
    std::printf("                    envelope depth amp-L %.2f dB vs CE-1 %.2f dB\n", envAmpL,
                envCe1);
    assert(maxDiff == 0.0);      // the amp really does pass dry on L
    assert(envAmpL < 0.01);      // ...so a mono host gets NO chorus from it
    assert(envCe1 > 8.0);        // ...where the CE-1 gives a full comb
}

// ============================================================================
// 7. Housekeeping a player still hears
// ============================================================================
void testDcOnSignal(double fs) {
    std::printf("\n[7a] DC offset ON SIGNAL\n");
    for (int mode = 0; mode <= 1; ++mode) {
        // Clean tone, and the same tone riding +0.1 V of input offset (the case a
        // clean-input test structurally cannot fail — see support/DcOffset.h).
        for (int off = 0; off <= 1; ++off) {
            clipper::dsp::Ce1Model m;
            m.prepare(fs, 128);
            m.setParameter(clipper::dsp::Ce1Model::PARAM_MODE, mode ? 1.0f : 0.0f);
            m.setParameter(clipper::dsp::Ce1Model::PARAM_RATE, 0.5f);
            m.setParameter(clipper::dsp::Ce1Model::PARAM_DEPTH, 0.7f);
            const int n = static_cast<int>(fs * 3.0);
            std::vector<float> in = tone(n, 220.0, fs, 0.5), out(static_cast<size_t>(n));
            if (off)
                for (auto& s : in) s += 0.1f;
            m.process(in.data(), out.data(), n);
            const auto d = clipper::test::measureDcOnSignal(out);
            std::printf("     mode=%-7s offset=%s  DC %.6f of peak %.4f = %.4f %%\n",
                        mode ? "VIBRATO" : "CHORUS", off ? "+0.1V" : "none ", d.mean, d.peak,
                        100.0 * d.fraction);
            // A delay line is LINEAR: it cannot rectify, so with a clean input the
            // DC must be ~zero. With an input offset it PASSES the offset through
            // (there is no coupling cap in a CE-1's wet path either) — bounded at
            // the input offset scaled by the mix, not at zero. Stated, not hidden.
            if (!off)
                assert(d.fraction < 0.01);
            else
                assert(d.fraction < 0.35);
        }
    }
}

void testResetAndNaN(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[7b] reset() and the NaN guard\n");

    // reset() must leave NO trace of what was played before it. Both models are
    // prepared and knobbed identically and both are reset — `a` after a second of
    // real signal, `b` without ever having seen any — so the only thing that could
    // differ is history, which is exactly what reset() exists to discard.
    //
    // (Note the reset() on `b`: a freshly PREPARED model is not the right
    // reference, because prepare() snaps the smoothers to the JC-inherited
    // defaults and a later setParameter then RAMPS them over ~8 ms. reset()
    // snaps them onto their targets. Comparing "played + reset" against
    // "prepared but still ramping" would measure the ramp, not the history.)
    M a, b;
    a.prepare(fs, 128);
    b.prepare(fs, 128);
    for (auto* m : {&a, &b}) {
        m->setParameter(M::PARAM_MODE, 0.0f);
        m->setParameter(M::PARAM_RATE, 0.4f);
        m->setParameter(M::PARAM_DEPTH, 0.8f);
    }
    const int n = static_cast<int>(fs * 1.0);
    std::vector<float> in = tone(n, 330.0, fs), o(static_cast<size_t>(n));
    a.process(in.data(), o.data(), n);  // give `a` some history
    a.reset();
    b.reset();
    std::vector<float> oa(static_cast<size_t>(n)), ob(static_cast<size_t>(n));
    a.process(in.data(), oa.data(), n);
    b.process(in.data(), ob.data(), n);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(oa[static_cast<size_t>(i)] -
                                                             ob[static_cast<size_t>(i)])));
    std::printf("     played+reset vs never-played+reset: max |diff| = %.3e\n", worst);
    assert(worst == 0.0);

    // A NaN sample poisons the delay line; reset() must clear it completely.
    M p;
    p.prepare(fs, 128);
    p.setParameter(M::PARAM_DEPTH, 0.8f);
    std::vector<float> bad(256, 0.2f), bo(256);
    bad[100] = std::nanf("");
    p.process(bad.data(), bo.data(), 256);
    int nonFinite = 0;
    for (float s : bo)
        if (!std::isfinite(s)) ++nonFinite;
    p.reset();
    const int m2 = static_cast<int>(fs * 1.0);
    std::vector<float> gi = tone(m2, 440.0, fs), go(static_cast<size_t>(m2));
    p.process(gi.data(), go.data(), m2);
    int after = 0;
    for (float s : go)
        if (!std::isfinite(s)) ++after;
    std::printf("     one NaN in: %d/256 non-finite; after reset(): %d/%d\n", nonFinite,
                after, m2);
    assert(after == 0);

    // A non-finite PARAMETER must never reach the model's state (ParamGuard).
    M g;
    g.prepare(fs, 128);
    g.setParameter(M::PARAM_RATE, std::nanf(""));
    g.setParameter(M::PARAM_DEPTH, std::numeric_limits<float>::infinity());
    g.setParameter(M::PARAM_MODE, std::nanf(""));
    std::printf("     after NaN/Inf params: rate %.4f Hz, sweep %.4f ms, mode %d\n", g.rateHz(),
                g.sweepMs(), g.mode());
    assert(std::isfinite(g.rateHz()) && std::isfinite(g.sweepMs()));
    std::vector<float> ni = tone(4096, 440.0, fs), no(4096);
    g.process(ni.data(), no.data(), 4096);
    for (float s : no) assert(std::isfinite(s));
}

void testNoZipperOnRateSweep(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[7c] no zipper on a RATE sweep\n");
    // Slam RATE from one end of the travel to the other mid-render, and compare
    // the seam's worst sample-to-sample step against the signal's OWN steady slew.
    for (int mode = 0; mode <= 1; ++mode) {
        M m;
        m.prepare(fs, 128);
        m.setParameter(M::PARAM_MODE, mode ? 1.0f : 0.0f);
        m.setParameter(M::PARAM_RATE, 0.0f);
        m.setParameter(M::PARAM_DEPTH, 0.7f);
        const int n = static_cast<int>(fs * 2.0);
        std::vector<float> in = tone(n, 220.0, fs), out(static_cast<size_t>(n));
        const int flip = n / 2;
        int done = 0;
        while (done < n) {
            if (done >= flip && done < flip + 128) m.setParameter(M::PARAM_RATE, 1.0f);
            const int k = std::min(128, n - done);
            m.process(in.data() + done, out.data() + done, k);
            done += k;
        }
        auto slewIn = [&](int lo, int hi) {
            double s = 0.0;
            for (int i = lo; i < hi && i + 1 < n; ++i)
                s = std::max(s, std::fabs(static_cast<double>(out[static_cast<size_t>(i + 1)] -
                                                             out[static_cast<size_t>(i)])));
            return s;
        };
        // THE REFERENCE IS THE SLEW AT THE NEW RATE, not the old one. A swept
        // delay's output slew IS the pitch shift, and this slam multiplies the
        // rate by 3.6x, so the signal's own slew legitimately grows across the
        // seam. Comparing against the pre-flip slew would measure the intended
        // effect and call it a click. Take the worst of the settled windows on
        // either side and ask only that the seam adds no STEP on top.
        const double before = slewIn(n / 8, n / 8 + 4096);
        const double after = slewIn(n - 8192, n - 4096);
        const double base = std::max(before, after);
        const double seam = slewIn(flip - 128, flip + 4096);
        std::printf("     mode=%-7s RATE 0 -> 1: seam %.6f vs settled slew (pre %.6f / post "
                    "%.6f) -> %.2fx\n", mode ? "VIBRATO" : "CHORUS", seam, before, after,
                    seam / base);
        assert(seam <= 1.05 * base);
    }

    // The MODE switch removes the dry path AND changes the LFO waveform, so it
    // must not step either.
    //
    // IT IS SWEPT ACROSS A WHOLE LFO PERIOD, and that is not thoroughness — it is
    // the difference between a bar with teeth and one without. The waveform step
    // this slice found (docs §62.8) is `A·(sin(phi) − tri(phi))`, which is ZERO at
    // phi = 0, pi/2, pi, 3pi/2 and worst near pi/4. A single flip time therefore
    // lands wherever it lands: the first version of this test flipped at exactly
    // 1.0 s, drew phi ~ 0.73 of a cycle where the two shapes differ by only 0.073,
    // and stayed GREEN with the smoothing removed. Perturbation P7 caught that.
    // Sweeping the flip across one full period and taking the WORST seam makes the
    // bar independent of where the LFO happens to be.
    {
        const double rateHz = 1.7320508;  // chorus mode, RATE knob 0.5
        const int period = static_cast<int>(fs / rateHz);
        double worstRatio = 0.0;
        double worstSeam = 0.0, worstBase = 0.0;
        for (int step = 0; step < 8; ++step) {
            M mm;
            mm.prepare(fs, 128);
            mm.setParameter(M::PARAM_MODE, 0.0f);
            mm.setParameter(M::PARAM_RATE, 0.5f);
            mm.setParameter(M::PARAM_DEPTH, 0.7f);
            const int n2 = static_cast<int>(fs * 2.5);
            std::vector<float> in2 = tone(n2, 220.0, fs), out2(static_cast<size_t>(n2));
            // Land the flip at a different LFO phase each time.
            const int flip2 = static_cast<int>(fs * 1.0) + (step * period) / 8;
            int done2 = 0;
            while (done2 < n2) {
                if (done2 >= flip2 && done2 < flip2 + 128)
                    mm.setParameter(M::PARAM_MODE, 1.0f);
                const int k2 = std::min(128, n2 - done2);
                mm.process(in2.data() + done2, out2.data() + done2, k2);
                done2 += k2;
            }
            auto slew = [&](int lo, int hi) {
                double v = 0.0;
                for (int i = lo; i < hi && i + 1 < n2; ++i)
                    v = std::max(v, std::fabs(static_cast<double>(
                                      out2[static_cast<size_t>(i + 1)] -
                                      out2[static_cast<size_t>(i)])));
                return v;
            };
            // Reference: the settled slew on BOTH sides (vibrato detunes harder, so
            // its own slew is legitimately larger — see the RATE note above).
            const double b = std::max(slew(n2 / 8, n2 / 8 + 4096), slew(n2 - 8192, n2 - 4096));
            const double sm = slew(flip2 - 128, flip2 + 2048);
            if (sm / b > worstRatio) {
                worstRatio = sm / b;
                worstSeam = sm;
                worstBase = b;
            }
        }
        std::printf("     CHORUS -> VIBRATO, worst of 8 flip phases across one LFO period:\n");
        std::printf("       seam %.6f vs settled slew %.6f -> %.2fx\n", worstSeam, worstBase,
                    worstRatio);
        assert(worstRatio < 1.5);
    }
}

void testBlockSizeAndRates() {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[7d] block-size invariance and rate independence\n");

    // Identical audio whether the host hands us 128 frames or a ragged stream.
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 2.0);
    std::vector<float> in = tone(n, 196.0, fs);
    auto run = [&](bool ragged) {
        M m;
        m.prepare(fs, 512);
        m.setParameter(M::PARAM_MODE, 0.0f);
        m.setParameter(M::PARAM_RATE, 0.6f);
        m.setParameter(M::PARAM_DEPTH, 0.7f);
        std::vector<float> out(static_cast<size_t>(n));
        int done = 0, blk = 128;
        while (done < n) {
            const int k = std::min(blk, n - done);
            m.process(in.data() + done, out.data() + done, k);
            done += k;
            if (ragged) blk = 1 + (blk * 7 + 13) % 401;
        }
        return out;
    };
    const auto a = run(false), b = run(true);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[static_cast<size_t>(i)] -
                                                             b[static_cast<size_t>(i)])));
    std::printf("     128-frame vs ragged blocks: max |diff| = %.3e\n", worst);
    assert(worst == 0.0);

    // The voicing is in MILLISECONDS and HERTZ, so it must not drift with the
    // host's sample rate. Measure the rendered LFO rate at four of them.
    std::printf("     rendered VIBRATO LFO rate vs sample rate (set 6.0926 Hz):\n");
    double lo = 1e30, hi = 0.0;
    for (double r : {44100.0, 48000.0, 88200.0, 96000.0}) {
        double pc = 0.0, lf = 0.0;
        pitchTrack(render(r, 0.5f, 1.0f, 1.0f, 440.0, 12.0), r, 440.0, pc, lf);
        std::printf("       %7.0f Hz -> %.4f Hz, %.1f cents\n", r, lf, pc);
        lo = std::min(lo, lf);
        hi = std::max(hi, lf);
    }
    std::printf("     spread across rates: %.4f Hz (%.2f %%)\n", hi - lo,
                100.0 * (hi - lo) / lo);
    assert((hi - lo) / lo < 0.02);
}

void testRestingState(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[7e] ADR 006 resting state\n");
    // The delay ring is a pure FIFO of the input with no recursion into it, so on
    // silence it must reach EXACTLY 0.0 — asserted rather than assumed, which is
    // the scope rule's own requirement for a zero-resting state.
    M m;
    m.prepare(fs, 128);
    m.setParameter(M::PARAM_DEPTH, 1.0f);
    const int n = static_cast<int>(fs * 1.0);
    std::vector<float> in = tone(n, 440.0, fs), o(static_cast<size_t>(n));
    m.process(in.data(), o.data(), n);
    std::vector<float> z(static_cast<size_t>(n), 0.0f);
    m.process(z.data(), o.data(), n);  // 1 s of silence
    std::printf("     max |delay-line state| after a silent second: %.3e\n",
                m.maxAbsRestingState());
    assert(m.maxAbsRestingState() == 0.0);
    // ...and the OUTPUT reaches exact digital silence too. Only the tail is
    // checked: the front of this buffer legitimately carries the ring-down of the
    // delay line that was still full of signal when the silence started, which is
    // the effect working, not a leak.
    for (size_t i = o.size() - o.size() / 10; i < o.size(); ++i) assert(o[i] == 0.0f);
}

void testModeMappingBothEnds(double fs) {
    using M = clipper::dsp::Ce1Model;
    std::printf("\n[7f] the MODE slot's mapping, at both ends and across the threshold\n");
    const float pts[] = {0.0f, 0.25f, 0.49f, 0.5f, 0.75f, 1.0f};
    for (float p : pts) {
        M m;
        m.prepare(fs, 128);
        m.setParameter(M::PARAM_MODE, p);
        std::printf("     MODE %.2f -> %s (rate range now %.2f Hz at knob 0)\n", p,
                    m.mode() == M::MODE_VIBRATO ? "VIBRATO" : "CHORUS ", m.rateHz());
        assert(m.mode() == (p >= 0.5f ? M::MODE_VIBRATO : M::MODE_CHORUS));
    }
    // Out-of-range values are clamped, not wrapped.
    M m;
    m.prepare(fs, 128);
    m.setParameter(M::PARAM_MODE, -3.0f);
    assert(m.mode() == M::MODE_CHORUS);
    m.setParameter(M::PARAM_MODE, 9.0f);
    assert(m.mode() == M::MODE_VIBRATO);

    // The pedal is LINEAR and adds no latency — asserted so the chain's PDC
    // accounting cannot silently acquire a number.
    assert(m.latencySamples() == 0);
    m.setOversampling(8);
    assert(m.latencySamples() == 0);
}

}  // namespace

int main() {
    // Line-buffered: an assert() aborts without flushing, and every printf in
    // this file is the measurement the assert below it is judging. Losing them on
    // failure is losing the diagnosis.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    clipper::test::requireAssertsLive();
    const double sr = 48000.0;
    testRateLaw(sr);
    testDepthInCents(sr);
    testChorusVsVibrato(sr);
    testComb(sr);
    testVersusAmpChorus(sr);
    testDcOnSignal(sr);
    testResetAndNaN(sr);
    testNoZipperOnRateSweep(sr);
    testBlockSizeAndRates();
    testRestingState(sr);
    testModeMappingBothEnds(sr);
    std::printf("\nAll CE-1 (M13.7 Chorus Ensemble) tests passed\n");
    return 0;
}
