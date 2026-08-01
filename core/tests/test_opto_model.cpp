// Clipper — M13.3: the "Lumen" OPTICAL compressor.
//
// PLAYER-OBSERVABLE BARS ONLY, and the load-bearing one is not a knob:
//
//   * THE ACCEPTANCE BAR — PROGRAM DEPENDENCE. The same depth of gain reduction
//     must take measurably longer to release after a long passage than after a
//     short stab, because the photocell's traps are still full. `testProgram
//     Dependence` measures both on this model AND on M13.1's OTA compressor with
//     the identical stimulus: an RC detector cannot have the property at any
//     coefficient, and the OTA voice measures 1.000x. That contrast is what makes
//     this a second VOICE rather than the first one with different constants.
//
//   * THE RATIO IS A PREDICTION, NOT A FIT. Deep in reduction, a feed-back loop
//     around a divider whose bottom leg follows a power law settles at a ratio of
//     1 + n*gamma — the EL panel's brightness exponent times the CdS cell's
//     gamma, both of them PUBLISHED device figures. `testRatioCurve` renders the
//     static curve and compares its measured asymptote against that prediction
//     (computed by different code from the device constants) and against the
//     widely-published "about 3:1" for the reference unit.
//
//   * THE ABSOLUTE REFERENCES. Attack 10 ms; ~0.06 s to 50 % release; 0.5 to 5 s
//     for complete release "depending upon the amount of previous reduction";
//     over 40 dB of available gain reduction. Every one of those is a published
//     figure for the reference unit, so the bars below are not identities.
//
// No XFAIL ledger: this suite has no known-bad property. Do not add one without
// a measured number.

#include "clipper/dsp/OptoModel.h"

#include "clipper/dsp/CompModel.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::CompModel;
using clipper::dsp::OptoModel;

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

template <typename M>
void renderBlocks(M& m, std::vector<float>& buf, int block = 128) {
    for (size_t i = 0; i < buf.size(); i += static_cast<size_t>(block)) {
        const int n =
            static_cast<int>(std::min(static_cast<size_t>(block), buf.size() - i));
        m.process(buf.data() + i, buf.data() + i, n);
    }
}

std::vector<float> tone(double fs, double seconds, double amp, double f0) {
    std::vector<float> v(static_cast<size_t>(fs * seconds));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<float>(amp * std::sin(kTwoPi * f0 * static_cast<double>(i) / fs));
    return v;
}

OptoModel* makeOpto(double fs, double peak, double gain, bool limit = false) {
    auto* m = new OptoModel();
    m->prepare(fs, 128);
    m->setParameter(OptoModel::PARAM_PEAK_REDUCTION, static_cast<float>(peak));
    m->setParameter(OptoModel::PARAM_MODE, limit ? 1.0f : 0.0f);
    m->setParameter(OptoModel::PARAM_GAIN, static_cast<float>(gain));
    return m;
}

// The settled gain reduction, in dB, for a steady tone.
double settledGrDb(double fs, double peak, double ampV, double f0 = 220.0,
                   double seconds = 2.0, bool limit = false) {
    OptoModel* m = makeOpto(fs, peak, 1.0, limit);
    std::vector<float> b = tone(fs, seconds, ampV, f0);
    renderBlocks(*m, b);
    const double gr = m->gainReductionDb();
    delete m;
    return gr;
}

// The settled OUTPUT peak, in dBV, for a steady tone.
double settledOutDbV(double fs, double peak, double gain, double ampV,
                     double f0 = 220.0, double seconds = 2.0, bool limit = false) {
    OptoModel* m = makeOpto(fs, peak, gain, limit);
    std::vector<float> b = tone(fs, seconds, ampV, f0);
    renderBlocks(*m, b);
    const double o = peakOf(b, b.size() - static_cast<size_t>(fs * 0.2), b.size());
    delete m;
    return dbOf(o);
}

// Gain-reduction trace on a burst-then-silence stimulus, sampled every `hop`
// samples. Works for any model exposing a gain-reduction reader through `grOf`.
template <typename M, typename F>
std::vector<double> burstTrace(M& m, double fs, double amp, double f0, double burstS,
                               double tailS, int hop, F grOf) {
    const size_t nB = static_cast<size_t>(fs * burstS);
    const size_t nT = static_cast<size_t>(fs * tailS);
    std::vector<double> tr;
    std::vector<float> buf(static_cast<size_t>(hop));
    size_t idx = 0;
    for (size_t blk = 0; blk < (nB + nT) / static_cast<size_t>(hop); ++blk) {
        for (int i = 0; i < hop; ++i, ++idx)
            buf[static_cast<size_t>(i)] =
                idx < nB ? static_cast<float>(
                               amp * std::sin(kTwoPi * f0 * static_cast<double>(idx) / fs))
                         : 0.0f;
        m.process(buf.data(), buf.data(), hop);
        tr.push_back(grOf(m));
    }
    return tr;
}

// Time from the end of the burst until the gain reduction has fallen below
// `floorDb` — "the pedal has let go", which is what a player hears.
double completeReleaseS(const std::vector<double>& tr, size_t endIdx, double dt,
                        double floorDb = 0.5) {
    for (size_t i = endIdx; i < tr.size(); ++i)
        if (tr[i] <= floorDb) return static_cast<double>(i - endIdx) * dt;
    return static_cast<double>(tr.size() - endIdx) * dt;
}
double fractionalReleaseS(const std::vector<double>& tr, size_t endIdx, double dt,
                          double frac) {
    const double peak = tr[endIdx];
    for (size_t i = endIdx; i < tr.size(); ++i)
        if (tr[i] <= frac * peak) return static_cast<double>(i - endIdx) * dt;
    return static_cast<double>(tr.size() - endIdx) * dt;
}

// ---------------------------------------------------------------------------
// 1. THE ACCEPTANCE BAR — program dependence, against the OTA voice.
// ---------------------------------------------------------------------------
void testProgramDependence(double fs) {
    std::printf("[opto] THE ACCEPTANCE BAR: program-dependent release\n");
    const int hop = 48;
    const double dt = hop / fs;
    const double bursts[2] = {0.150, 6.0};
    double optoT[2] = {0.0, 0.0};
    double optoPeak[2] = {0.0, 0.0};
    for (int k = 0; k < 2; ++k) {
        OptoModel* m = makeOpto(fs, 0.70, 1.0);
        auto tr = burstTrace(*m, fs, 0.30, 220.0, bursts[k], 16.0, hop,
                             [](OptoModel& x) { return x.gainReductionDb(); });
        const size_t endIdx = static_cast<size_t>(bursts[k] / dt) - 1;
        optoPeak[k] = tr[endIdx];
        optoT[k] = completeReleaseS(tr, endIdx, dt);
        std::printf("       opto  %5.2f s burst: peak GR %5.2f dB, releases in %.2f s\n",
                    bursts[k], optoPeak[k], optoT[k]);
        delete m;
    }
    // The two stimuli must reach the SAME depth, or the comparison is not about
    // memory at all.
    assert(std::fabs(optoPeak[0] - optoPeak[1]) < 0.5);
    const double optoRatio = optoT[1] / optoT[0];

    // The identical measurement on M13.1's OTA compressor. Its gain reduction is
    // read from the gain cell rather than the audio, so the two models are being
    // asked exactly the same question about their control paths.
    double compT[2] = {0.0, 0.0};
    for (int k = 0; k < 2; ++k) {
        CompModel idle;
        idle.prepare(fs, 128);
        idle.setParameter(CompModel::PARAM_SUSTAIN, 0.8f);
        idle.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        std::vector<float> z(static_cast<size_t>(fs * 0.1), 0.0f);
        renderBlocks(idle, z);
        const double g0 = idle.cellGainAtDc();

        CompModel m;
        m.prepare(fs, 128);
        m.setParameter(CompModel::PARAM_SUSTAIN, 0.8f);
        m.setParameter(CompModel::PARAM_LEVEL, 1.0f);
        auto tr = burstTrace(m, fs, 0.30, 220.0, bursts[k], 16.0, hop,
                             [g0](CompModel& x) { return -dbOf(x.cellGainAtDc() / g0); });
        const size_t endIdx = static_cast<size_t>(bursts[k] / dt) - 1;
        compT[k] = completeReleaseS(tr, endIdx, dt);
        std::printf("       OTA   %5.2f s burst: peak GR %5.2f dB, releases in %.2f s\n",
                    bursts[k], tr[endIdx], compT[k]);
    }
    const double compRatio = compT[1] / compT[0];
    std::printf("       program dependence: opto %.3fx   OTA %.3fx\n", optoRatio, compRatio);

    // The bar is 2.0: unambiguously more than measurement noise, and less than
    // the shipped 2.89 so the margin is RECORDED rather than snugged around the
    // model. The OTA bound is the contrast, and it is the half that cannot be
    // satisfied by any coefficient choice in an RC detector.
    assert(optoRatio > 2.0);
    assert(compRatio < 1.25);

    // The mechanism, not just the symptom: the trap occupancy is what differs.
    OptoModel* shortRun = makeOpto(fs, 0.70, 1.0);
    OptoModel* longRun = makeOpto(fs, 0.70, 1.0);
    std::vector<float> sb = tone(fs, 0.150, 0.30, 220.0);
    std::vector<float> lb = tone(fs, 6.0, 0.30, 220.0);
    renderBlocks(*shortRun, sb);
    renderBlocks(*longRun, lb);
    std::printf("       trap occupancy at the end of the burst: %.4f (short) vs %.4f (long)\n",
                shortRun->cellMemory(), longRun->cellMemory());
    assert(longRun->cellMemory() > 4.0 * shortRun->cellMemory());
    delete shortRun;
    delete longRun;
}

// ---------------------------------------------------------------------------
// 2. The published release windows, as absolute references.
// ---------------------------------------------------------------------------
void testPublishedReleaseWindows(double fs) {
    std::printf("[opto] the published release spec (0.06 s to 50 %%, 0.5-5 s complete)\n");
    const int hop = 48;
    const double dt = hop / fs;
    // 50 % release, measured on a SHORT burst at a modest depth — the case the
    // published single figure describes.
    OptoModel* m = makeOpto(fs, 0.50, 1.0);
    auto tr = burstTrace(*m, fs, 0.30, 220.0, 0.150, 8.0, hop,
                         [](OptoModel& x) { return x.gainReductionDb(); });
    const size_t endIdx = static_cast<size_t>(0.150 / dt) - 1;
    const double t50 = fractionalReleaseS(tr, endIdx, dt, 0.5);
    std::printf("       50 %% release %.0f ms (published 60 ms; a 40-80 ms window)\n", t50 * 1e3);
    assert(t50 > 0.040 && t50 < 0.080);
    delete m;

    // "Complete release 0.5 to 5 seconds depending upon the amount of previous
    // reduction" — every point of the operating space must land inside it, and
    // the span across the space must be a real span rather than a single value.
    double lo = 1e9, hi = -1e9;
    for (double peak : {0.30, 0.50, 0.70, 0.90, 1.00}) {
        OptoModel* q = makeOpto(fs, peak, 1.0);
        auto t = burstTrace(*q, fs, 0.30, 220.0, 4.0, 16.0, hop,
                            [](OptoModel& x) { return x.gainReductionDb(); });
        const size_t e = static_cast<size_t>(4.0 / dt) - 1;
        const double c = completeReleaseS(t, e, dt);
        std::printf("       PEAK %.2f: peak GR %5.2f dB, complete release %.2f s\n", peak, t[e], c);
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        assert(c > 0.5 && c < 5.0);
        delete q;
    }
    std::printf("       depth-driven span %.2f s .. %.2f s = %.2fx\n", lo, hi, hi / lo);
    assert(hi / lo > 2.0);
}

// ---------------------------------------------------------------------------
// 3. Attack — the published 10 ms, and (the real bar) knob-INDEPENDENT.
// ---------------------------------------------------------------------------
void testAttack(double fs) {
    std::printf("[opto] attack, and its independence from the knob\n");
    double lo = 1e9, hi = -1e9;
    for (double peak : {0.30, 0.50, 0.70, 1.00}) {
        OptoModel* m = makeOpto(fs, peak, 1.0);
        const double f0 = 1000.0;
        std::vector<float> b = tone(fs, 1.0, 0.30, f0);
        renderBlocks(*m, b);
        const double settled = peakOf(b, b.size() - static_cast<size_t>(fs * 0.1), b.size());
        const size_t per = static_cast<size_t>(fs / f0);
        const size_t lat = static_cast<size_t>(m->latencySamples());
        std::vector<double> env;
        for (size_t c = lat; c + per < b.size(); c += per) env.push_back(peakOf(b, c, c + per));
        size_t onset = 0;
        while (onset < env.size() && env[onset] < 0.1 * settled) ++onset;
        const double bar = settled * std::pow(10.0, 1.0 / 20.0);
        size_t over = onset;
        while (over < env.size() && env[over] <= bar) ++over;
        size_t back = over;
        while (back < env.size() && env[back] > bar) ++back;
        const double attackMs =
            static_cast<double>(back - onset) * static_cast<double>(per) / fs * 1e3;
        std::printf("       PEAK %.2f: overshoot %+.2f dB, settles within 1 dB in %.1f ms\n", peak,
                    dbOf(env[std::min(over, env.size() - 1)] / settled), attackMs);
        lo = std::min(lo, attackMs);
        hi = std::max(hi, attackMs);
        delete m;
    }
    std::printf("       attack %.1f..%.1f ms (published 10 ms), spread %.1f ms\n", lo, hi, hi - lo);
    // Absolute reference: the published 10 ms, with a window that reflects how a
    // "settles within 1 dB" reading differs from whatever the factory measured.
    assert(lo > 5.0 && hi < 20.0);
    // The REAL bar, and the one that is a prediction rather than a constant read
    // back: an optical attack does not move with the sensitivity control, because
    // the cell's own lag sets it. Docs §59.2 measured the OTA voice's attack
    // moving 14 -> 3 ms across SUSTAIN, because ITS attack is a current-starved
    // discharge. A 3 ms window here cannot contain that behaviour.
    assert(hi - lo < 3.0);
}

// ---------------------------------------------------------------------------
// 4. The static ratio curve, against the device-exponent prediction.
// ---------------------------------------------------------------------------
void testRatioCurve(double fs) {
    std::printf("[opto] static curve and the predicted ratio (PEAK 1.00, GAIN 1.00)\n");
    double prevIn = 0.0, prevOut = 0.0;
    bool first = true;
    double deepRatio = 0.0, kneeRatio = 0.0;
    double maxGr = 0.0;
    for (double indb = -54.0; indb <= 6.001; indb += 6.0) {
        const double amp = std::pow(10.0, indb / 20.0);
        OptoModel* m = makeOpto(fs, 1.0, 1.0);
        std::vector<float> b = tone(fs, 2.0, amp, 220.0);
        renderBlocks(*m, b);
        const double outdb = dbOf(peakOf(b, b.size() - static_cast<size_t>(fs * 0.2), b.size()));
        const double gr = m->gainReductionDb();
        maxGr = std::max(maxGr, gr);
        double ratio = 0.0;
        if (!first) ratio = (indb - prevIn) / (outdb - prevOut);
        std::printf("       in %+6.1f dBV -> out %+7.2f dBV   GR %6.2f dB   ratio %5.2f:1\n", indb,
                    outdb, gr, ratio);
        if (!first && gr > 2.0 && gr < 5.0) kneeRatio = ratio;
        if (!first && indb >= 0.0) deepRatio = ratio;
        prevIn = indb;
        prevOut = outdb;
        first = false;
    }
    OptoModel probe;
    const double predicted = probe.predictedDeepRatio();
    std::printf("       ratio near the knee %.2f:1, deep %.2f:1; device prediction 1+n*gamma = %.3f:1\n",
                kneeRatio, deepRatio, predicted);
    // Soft knee then a stiffening ratio — the divider's own shape, and the thing
    // players describe as "gentle until it isn't".
    assert(kneeRatio > 1.0 && kneeRatio < 2.0);
    assert(deepRatio > kneeRatio);
    // The measured asymptote must agree with the prediction computed by different
    // code from the two published device exponents. 12 % is the room the
    // divider's own curvature needs at a finite level.
    assert(std::fabs(deepRatio - predicted) / predicted < 0.12);
    // And it must sit in the bracket the PUBLISHED exponent ranges allow
    // (n = 2..3, gamma = 0.58..0.92 => 2.16:1 .. 3.76:1), which contains the
    // widely-published "about 3:1" for the reference unit.
    assert(deepRatio > 2.16 && deepRatio < 3.76);
    std::printf("       greatest gain reduction reached %.2f dB (the divider's ceiling is 40.09)\n",
                maxGr);
    assert(maxGr > 20.0);
}

// ---------------------------------------------------------------------------
// 5. The divider's ceiling — derived from two published figures.
// ---------------------------------------------------------------------------
void testGainReductionCeiling(double fs) {
    std::printf("[opto] the gain-reduction ceiling is DERIVED from published figures\n");
    OptoModel probe;
    const double rs = probe.voice().seriesOhms;
    const double rLight = probe.cell().spec().rLightOhms;
    const double ceilingDb = 20.0 * std::log10(1.0 + rs / rLight);
    std::printf("       Rs %.0f ohm over a lit cell of %.0f ohm = %.2f dB analytic\n", rs, rLight,
                ceilingDb);
    // Published: "0 to 40 dB gain limiting". The series resistor exists to make
    // that true at the published lit-cell resistance.
    assert(ceilingDb > 40.0 && ceilingDb < 41.0);
    // ...and the analytic line above is an IDENTITY on two constants, so it is
    // not the bar. RENDER it: slam the pedal until the cell is at its light limit
    // and check the reduction it really reaches. (Docs §57.9 / §61.9's lesson —
    // an Ohm's-law identity cannot fail, so it must be joined by a measurement.)
    OptoModel* m = makeOpto(fs, 1.0, 1.0);
    std::vector<float> b = tone(fs, 3.0, 30.0, 220.0);
    renderBlocks(*m, b);
    const double measured = m->gainReductionDb();
    std::printf("       slammed: measured %.2f dB of gain reduction, cell at %.0f ohm\n", measured,
                m->cellResistanceOhms());
    assert(measured > ceilingDb - 2.0 && measured <= ceilingDb + 0.01);
    delete m;
}

// ---------------------------------------------------------------------------
// 6. PEAK REDUCTION is a THRESHOLD; GAIN is a LEVEL. Neither is the other.
// ---------------------------------------------------------------------------
void testControlLawContrast(double fs) {
    std::printf("[opto] the control-law contrast\n");
    double lo = 1e9, hi = -1e9, prev = -1e9;
    for (double peak = 0.0; peak <= 1.001; peak += 0.2) {
        double a = -80.0, b = 20.0;
        for (int it = 0; it < 20; ++it) {
            const double mid = 0.5 * (a + b);
            if (settledGrDb(fs, peak, std::pow(10.0, mid / 20.0), 220.0, 1.5) >= 3.0)
                b = mid;
            else
                a = mid;
        }
        const double trip = 0.5 * (a + b);
        std::printf("       PEAK %.2f: 3 dB of gain reduction at %+7.2f dBV\n", peak, trip);
        assert(trip < prev - 1.0 || prev < -1e8);  // monotone, and by a usable step
        prev = trip;
        lo = std::min(lo, trip);
        hi = std::max(hi, trip);
    }
    const double thresholdSpan = hi - lo;

    // GAIN, across its whole travel, must move the OUTPUT and not the COMPRESSION.
    double grLo = 1e9, grHi = -1e9, outLo = 1e9, outHi = -1e9;
    for (double g : {0.2, 0.4, 0.6, 0.8, 1.0}) {
        OptoModel* m = makeOpto(fs, 0.70, g);
        std::vector<float> b = tone(fs, 2.0, 0.30, 220.0);
        renderBlocks(*m, b);
        const double o = dbOf(peakOf(b, b.size() - static_cast<size_t>(fs * 0.2), b.size()));
        grLo = std::min(grLo, m->gainReductionDb());
        grHi = std::max(grHi, m->gainReductionDb());
        outLo = std::min(outLo, o);
        outHi = std::max(outHi, o);
        delete m;
    }
    std::printf("       PEAK REDUCTION moves the threshold %.2f dB\n", thresholdSpan);
    std::printf("       GAIN moves the output %.2f dB and the gain reduction %.4f dB\n",
                outHi - outLo, grHi - grLo);
    // Docs §59.4 measured M13.1's SUSTAIN doing the opposite (25.33 dB of gain
    // travel against 0.28 dB of output travel) and §61.4 measured the gate's
    // THRESHOLD (40.00 dB of threshold against 0.00 dB of gain). This pedal has
    // BOTH kinds of control, one per knob, and each is asserted in its own
    // direction.
    assert(thresholdSpan > 40.0);
    assert(outHi - outLo > 25.0);
    assert(grHi - grLo < 0.01);
}

// ---------------------------------------------------------------------------
// 7. MODE is not dead UI — it is worth real dB, and in the right direction.
// ---------------------------------------------------------------------------
void testModeAuthority(double fs) {
    std::printf("[opto] MODE: compress vs limit\n");
    double worst = 0.0;
    for (double indb : {-24.0, -12.0, 0.0, 6.0}) {
        const double amp = std::pow(10.0, indb / 20.0);
        const double c = settledOutDbV(fs, 1.0, 1.0, amp, 220.0, 2.0, false);
        const double l = settledOutDbV(fs, 1.0, 1.0, amp, 220.0, 2.0, true);
        std::printf("       in %+6.1f dBV: COMPRESS %+7.2f dBV, LIMIT %+7.2f dBV (%+.2f dB)\n",
                    indb, c, l, l - c);
        // LIMIT feeds the sidechain part of the pedal's own INPUT, so it must
        // always clamp harder, never softer.
        assert(l <= c + 0.01);
        worst = std::max(worst, c - l);
    }
    std::printf("       greatest MODE authority %.2f dB\n", worst);
    // §61.3 refused to ship the noise gate's MODE switch because it changes what
    // the footswitch does rather than the audio. This one changes the audio, and
    // this is the number that says so.
    assert(worst > 3.0);
}

// ---------------------------------------------------------------------------
// 8. The honest expectations — measured, and asserted in the direction that
//    KEEPS them. An opto that stopped doing these would be a different pedal.
// ---------------------------------------------------------------------------
void testHonestExpectations(double fs) {
    std::printf("[opto] honest expectations\n");

    // (a) THE PICK ATTACK SURVIVES. That is the opto's characteristic behaviour
    //     and the exact opposite of M13.1's, which docs §59.6 measured LOSING
    //     7.72-20.00 dB of the same transient. Measured the same way: the first
    //     50 ms against the settled level.
    for (double peak : {0.50, 0.70, 1.00}) {
        OptoModel* m = makeOpto(fs, peak, 1.0);
        std::vector<float> b = tone(fs, 2.0, 0.30, 1000.0);
        renderBlocks(*m, b);
        const size_t lat = static_cast<size_t>(m->latencySamples());
        const double first = rmsOf(b, lat, lat + static_cast<size_t>(fs * 0.05));
        const double settled = rmsOf(b, b.size() - static_cast<size_t>(fs * 0.2), b.size());
        std::printf("       PEAK %.2f: first 50 ms is %+.2f dB re settled\n", peak,
                    dbOf(first / settled));
        assert(first > settled);  // the transient gets THROUGH
        delete m;
    }

    // (b) IT IS NOT PHASE-INVERTING. M13.1 is (its signal drives the OTA's
    //     inverting input); this one is a divider into a non-inverting amplifier,
    //     so a player stacking the two should know they disagree.
    {
        OptoModel* m = makeOpto(fs, 0.0, 1.0);
        std::vector<float> b = tone(fs, 0.5, 0.05, 220.0);
        std::vector<float> ref = b;
        renderBlocks(*m, b);
        const size_t lat = static_cast<size_t>(m->latencySamples());
        double dot = 0.0;
        for (size_t i = lat + 4800; i + 1 < b.size(); ++i)
            dot += static_cast<double>(b[i]) * ref[i - lat];
        std::printf("       phase: correlation with the input is %s\n", dot > 0 ? "POSITIVE" : "NEGATIVE");
        assert(dot > 0.0);
        delete m;
    }

    // (c) IDLE NOISE GAIN. This project synthesises no noise, so what is measured
    //     is the mechanism: the gain the pedal applies to whatever floor arrives.
    //     An opto at rest is a near-unity divider, so unlike M13.1 (+8.22 to
    //     +33.56 dB, docs §59.6) it does NOT amplify the floor as the knob rises.
    double noiseLo = 1e9, noiseHi = -1e9;
    for (double peak : {0.0, 0.5, 1.0}) {
        OptoModel* m = makeOpto(fs, peak, 0.62);
        std::vector<float> b = tone(fs, 2.0, 5.8e-5, 220.0);  // about -84.75 dBFS
        const double in = rmsOf(b, 0, b.size());
        renderBlocks(*m, b);
        const double g = dbOf(rmsOf(b, b.size() / 2, b.size()) / in);
        std::printf("       PEAK %.2f: idle gain on a -84.75 dBFS floor %+.2f dB\n", peak, g);
        noiseLo = std::min(noiseLo, g);
        noiseHi = std::max(noiseHi, g);
        delete m;
    }
    std::printf("       noise gain moves %.2f dB across the whole PEAK travel\n", noiseHi - noiseLo);
    assert(noiseHi - noiseLo < 3.0);

    // (d) TONE COLOUR. Two coupling caps and nothing else in the audio path, so
    //     the pedal is nearly flat — and that is a claim worth pinning, because
    //     it is what a leveling amplifier is for.
    const double freqs[7] = {20.0, 41.0, 82.0, 220.0, 1000.0, 5000.0, 10000.0};
    double amps[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; ++i) {
        OptoModel* m = makeOpto(fs, 0.0, 1.0);
        std::vector<float> b = tone(fs, 1.0, 0.02, freqs[i]);
        renderBlocks(*m, b);
        amps[i] = goertzelAmp(b, b.size() / 2, b.size() / 2, freqs[i], fs);
        delete m;
    }
    double worstInBand = 0.0;
    for (int i = 0; i < 7; ++i) {
        const double d = dbOf(amps[i] / amps[4]);
        std::printf("       %6.0f Hz %+6.2f dB re 1 kHz\n", freqs[i], d);
        if (freqs[i] >= 82.0) worstInBand = std::max(worstInBand, std::fabs(d));
    }
    std::printf("       worst deviation from 82 Hz up: %.2f dB\n", worstInBand);
    assert(worstInBand < 1.0);
}

// ---------------------------------------------------------------------------
// 9. Oversampling — the decision, measured rather than assumed.
// ---------------------------------------------------------------------------
void testAliasing(double fs) {
    std::printf("[opto] alias floor vs oversampling factor (single 9 kHz tone, 0.30 V)\n");
    double floors[4] = {0, 0, 0, 0};
    const int factors[4] = {1, 2, 4, 8};
    for (int k = 0; k < 4; ++k) {
        OptoModel m;
        m.prepare(fs, 128);
        m.setOversampling(factors[k]);
        m.setParameter(OptoModel::PARAM_PEAK_REDUCTION, 1.0f);
        m.setParameter(OptoModel::PARAM_GAIN, 1.0f);
        std::vector<float> b = tone(fs, 1.0, 0.30, 9000.0);
        renderBlocks(m, b);
        const size_t a = b.size() / 2, n = b.size() / 2;
        const double f1 = goertzelAmp(b, a, n, 9000.0, fs);
        double worst = 0.0;
        for (double f = 200.0; f < 20000.0; f += 100.0) {
            const double d = std::fmod(f, 9000.0);
            if (d < 300.0 || d > 8700.0) continue;  // skip the harmonics themselves
            worst = std::max(worst, goertzelAmp(b, a, n, f, fs));
        }
        floors[k] = dbOf(worst / f1);
        std::printf("       %dx (latency %2d): %.2f dB\n", m.oversampling(), m.latencySamples(),
                    floors[k]);
    }
    // A SINGLE tone, deliberately: docs §59.7 records that a two-tone stimulus on
    // a compressor reports a flat floor across every factor, because the gain is
    // modulated and the spectrum fills with legitimate IMD. A real alias floor
    // MOVES with the factor (§54's tell), and this one moves 27 dB.
    assert(floors[0] < -56.0);          // even un-oversampled it clears the house bar
    assert(floors[2] < floors[0] - 20.0);  // ...and 4x is worth more than 20 dB of it
    assert(floors[2] < -100.0);
    // The shipped default is 4x. Pinned, because nothing else in the suite sets
    // it and a silent change to 1x would cost 27 dB (docs §58.7's could-not-fail
    // finding: a test that never asserts the shipped factor does not guard it).
    OptoModel shipped;
    shipped.prepare(fs, 128);
    std::printf("       shipped default: %dx, latency %d samples\n", shipped.oversampling(),
                shipped.latencySamples());
    assert(shipped.oversampling() == 4);
    assert(shipped.latencySamples() == 72);
}

// ---------------------------------------------------------------------------
// 10. DC on signal, block-size invariance, reset, rate independence, denormals.
// ---------------------------------------------------------------------------
void testHygiene(double fs) {
    std::printf("[opto] hygiene\n");

    for (double off : {0.0, 0.1}) {
        OptoModel* m = makeOpto(fs, 0.70, 0.62);
        std::vector<float> b = tone(fs, 3.0, 0.30, 220.0);
        for (auto& s : b) s = static_cast<float>(s + off);
        renderBlocks(*m, b);
        const size_t a = b.size() - static_cast<size_t>(fs * 1.0);
        double sum = 0.0;
        for (size_t i = a; i < b.size(); ++i) sum += b[i];
        const double dc = sum / static_cast<double>(b.size() - a);
        const double pct = 100.0 * std::fabs(dc) / peakOf(b, a, b.size());
        std::printf("       DC on signal (input offset %+.2f V): %.4f %% of peak\n", off, pct);
        assert(pct < 0.5);
        delete m;
    }

    {
        std::vector<float> ref = tone(fs, 1.0, 0.25, 196.0);
        std::vector<float> rag = ref;
        OptoModel* a = makeOpto(fs, 0.70, 0.62);
        renderBlocks(*a, ref, 128);
        OptoModel* b = makeOpto(fs, 0.70, 0.62);
        const int sizes[5] = {100, 37, 256, 1, 411};
        size_t p = 0;
        int k = 0;
        while (p < rag.size()) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(sizes[k % 5]), rag.size() - p));
            b->process(&rag[p], &rag[p], n);
            p += static_cast<size_t>(n);
            ++k;
        }
        double worst = 0.0;
        for (size_t i = 0; i < ref.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(ref[i] - rag[i])));
        std::printf("       block-size invariance: worst |diff| %.3e\n", worst);
        assert(worst == 0.0);
        delete a;
        delete b;
    }

    {
        OptoModel* a = makeOpto(fs, 0.70, 0.62);
        std::vector<float> junk = tone(fs, 0.5, 0.8, 300.0);
        renderBlocks(*a, junk);
        a->reset();
        OptoModel* f = makeOpto(fs, 0.70, 0.62);
        std::vector<float> x = tone(fs, 0.5, 0.2, 220.0);
        std::vector<float> y = x;
        renderBlocks(*a, x);
        renderBlocks(*f, y);
        double worst = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(x[i] - y[i])));
        std::printf("       reset() vs a fresh model: worst |diff| %.3e\n", worst);
        assert(worst == 0.0);
        delete a;
        delete f;
    }

    {
        double lo = 1e9, hi = -1e9;
        for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
            const double o = settledOutDbV(sr, 0.70, 0.62, 0.30);
            std::printf("       %.0f Hz: settled %.4f dBV\n", sr, o);
            lo = std::min(lo, o);
            hi = std::max(hi, o);
        }
        std::printf("       rate spread %.4f dB\n", hi - lo);
        assert(hi - lo < 0.10);
    }
}

// ---------------------------------------------------------------------------
// 11. Denormals — ADR 006's scope rule, with the settle time MEASURED.
// ---------------------------------------------------------------------------
void testDenormals(double fs) {
    std::printf("[opto] denormal scope\n");
    OptoModel* m = makeOpto(fs, 1.0, 1.0);
    std::vector<float> b = tone(fs, 1.0, 0.5, 220.0);
    renderBlocks(*m, b);
    // The OUTPUT reaches exact digital silence almost at once; the CELL's states
    // take much longer, because the trap occupancy's own time constant is 2 s and
    // it has to fall ~30 decades. Measured at 137 s, so the tail here is 160 s.
    // Nothing is denormal in the meantime — the states are ~1e-5 at 10 s — so the
    // long tail is about proving convergence, not about a cost.
    std::vector<float> sil(static_cast<size_t>(fs), 0.0f);
    double outMax = 0.0;
    for (int sec = 0; sec < 160; ++sec) {
        std::fill(sil.begin(), sil.end(), 0.0f);
        renderBlocks(*m, sil);
        if (sec == 4) outMax = peakOf(sil, 0, sil.size());
    }
    std::printf("       output at 5 s of silence: %.3e   maxAbsRestingState at 160 s: %.6e\n",
                outMax, m->maxAbsRestingState());
    assert(outMax == 0.0);
    // Every state this class reports rests at EXACTLY zero (ADR 006), and the
    // cell's three are in it — the opposite side of the scope rule from
    // SidechainDetector's rail-resting envelope node.
    assert(m->maxAbsRestingState() == 0.0);
    // NOT in it, and measured rather than assumed: the loop's cell resistance
    // rests at the cell's DARK value, a real operating point.
    std::printf("       cell resistance at rest: %.0f ohm (a real operating point, not guarded)\n",
                m->cellResistanceOhms());
    assert(m->cellResistanceOhms() > 9.0e6);
    delete m;
}

// ---------------------------------------------------------------------------
// 12. Knob moves do not click.
// ---------------------------------------------------------------------------
void testNoZipper(double fs) {
    std::printf("[opto] knob-move seam vs the signal's own slew\n");
    const size_t n = static_cast<size_t>(fs);
    const size_t mid = n / 2;
    double worstRatio = 0.0;
    for (int which = 0; which < 2; ++which) {
        for (double from = 0.0; from <= 0.61; from += 0.2) {
            OptoModel* m = makeOpto(fs, which == 0 ? from : 0.5, which == 1 ? from : 0.6);
            std::vector<float> b = tone(fs, 1.0, 0.25, 220.0);
            for (size_t i = 0; i < n; i += 128) {
                if (i == mid)
                    m->setParameter(which == 0 ? OptoModel::PARAM_PEAK_REDUCTION
                                               : OptoModel::PARAM_GAIN,
                                    static_cast<float>(from + 0.4));
                m->process(&b[i], &b[i], static_cast<int>(std::min<size_t>(128, n - i)));
            }
            double slew = 0.0;
            for (size_t i = mid - 4000; i < mid - 100; ++i)
                slew = std::max(slew, std::fabs(static_cast<double>(b[i]) - b[i - 1]));
            double step = 0.0;
            for (size_t i = mid; i < mid + 2000; ++i)
                step = std::max(step, std::fabs(static_cast<double>(b[i]) - b[i - 1]));
            worstRatio = std::max(worstRatio, step / slew);
            delete m;
        }
    }
    std::printf("       worst seam / slew = %.2fx\n", worstRatio);
    assert(worstRatio < 1.5);
}

// ---------------------------------------------------------------------------
// 13. The opening voice is unity at a normal playing level.
// ---------------------------------------------------------------------------
void testDefaultsAreUnity(double fs) {
    std::printf("[opto] the shipped defaults (PEAK 0.50, MODE compress, GAIN 0.62)\n");
    const double out = settledOutDbV(fs, 0.50, 0.62, 0.15);
    std::printf("       a 0.15 V 220 Hz note comes out at %.2f dBV (input %.2f dBV)\n", out,
                dbOf(0.15));
    assert(std::fabs(out - dbOf(0.15)) < 1.0);
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    const double sr = 48000.0;
    std::printf("=== M13.3: the \"Lumen\" optical compressor (docs §64) ===\n");
    testProgramDependence(sr);
    testPublishedReleaseWindows(sr);
    testAttack(sr);
    testRatioCurve(sr);
    testGainReductionCeiling(sr);
    testControlLawContrast(sr);
    testModeAuthority(sr);
    testHonestExpectations(sr);
    testAliasing(sr);
    testHygiene(sr);
    testDenormals(sr);
    testNoZipper(sr);
    testDefaultsAreUnity(sr);
    std::printf("=== opto tests OK ===\n");
    return 0;
}
