// Plain-assert tests for clipper::dsp::WahModel — the lineup's first FILTER
// pedal (a GCB-95-class wah whose tank is also driven by an envelope). No
// framework: int main + <cassert>. Frequency content via a hand-rolled Goertzel.
//
// The convention this file is written to (CLAUDE.md, "Measure, don't assert"):
// EVERY headline bar is a property of a RENDER, compared against something that
// is NOT a copy of the implementation.
//
//   (a) SWEEP LAW vs the DERIVED law — the rendered peak frequency at nine
//       POSITION points against WahModel::positionToHz(). This one IS
//       implementation-vs-its-own-recipe, and is labelled as such: it validates
//       the DISCRETISATION, nothing more.
//   (b) SWEEP LAW vs an INDEPENDENT MEASUREMENT — the same rendered peaks against
//       the CCRMA / Julius Smith digitised CryBaby (Faust vaeffects.lib
//       `crybaby`), which was fitted to three MEASURED GCB-95 responses. This is
//       the bar with teeth: nothing in this repo produced that reference.
//   (c) The SHAPE of the law. Two properties a wrong law fails even when the
//       endpoints are right: the travel is 2.322 octaves, and the two halves of
//       the travel are within 0.05 octaves of each other (the audio taper and the
//       circuit's square root cancel). A LINEAR control law measures 0.472 /
//       1.851 — the "all the wah in the last inch" failure — and fails this bar
//       by a factor of 3.9.
//   (d) RESONANCE HEIGHT AND WIDTH across the sweep: the rendered peak boost is
//       the published +18 dB and does NOT move across the travel (the topology's
//       prediction), while the rendered Q tracks the derived 1/f0 law and the
//       independently measured CCRMA Q.
//   (e) VOICE moves WIDTH ONLY — Q spans 2.27..5.36 while the centre frequency
//       and the peak height stay put.
//   (f) AUTO is a PLAYER-OBSERVABLE property: a real plucked note sweeps the peak
//       UP and back DOWN, by a measured number of octaves that scales with
//       SENSITIVITY, peaking tens of ms after the pick and returning within a
//       second. Watching the follower's own output would be a tautology; this
//       watches the filter.
//   (g) SENS = 0 is BIT-IDENTICAL to a model that never had the feature.
//   (h) NO ZIPPER, measured against its own control: a static POSITION floors at
//       −322 dB, a pathological per-block 0<->1 slam at −104 dB, and a fast full
//       sweep leaves a skirt that DECAYS with frequency (a stepping artifact
//       would be flat).
//   (i) ALIAS FLOOR falls with the oversampling factor (the §54 lesson: a bar
//       that does not move with the factor is not measuring aliasing).
//   (j) DC OFFSET ON SIGNAL, including the +0.1 V input-offset case.
//   (k) reset() / NaN: a poisoned engine recovers, and every zero-resting state
//       reaches EXACTLY 0.0 after a silent tail.
//   (l) THE FOLLOWER'S LEVEL LAW: the sweep depth is PROPORTIONAL to pick
//       strength with NO threshold, and time-to-peak does not move with level.
//       That is the measured reason this pedal does NOT use the shared
//       `SidechainDetector` (docs §58.8, ADR 023) — that component is a
//       THRESHOLD detector, which is what its two consumers want and the
//       opposite of what a wah needs.
//
// Full derivations, sources and the measured tables: docs/DEVELOPMENT.md §58.

#include "clipper/dsp/WahModel.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::WahModel;

// Goertzel amplitude of a real tone at f over the whole of x (rectangular — the
// callers choose an integer number of carrier cycles where it matters).
double goertzel(const std::vector<float>& x, double f, double fs) {
    const double w = kTwoPi * f / fs, cw = std::cos(w), sw = std::sin(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) {
        const double s0 = static_cast<double>(v) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw, im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

// The RENDERED magnitude response of the whole pedal at fixed knobs, from ONE
// render. A tiny impulse (1 mV) keeps the transistor stage inside its linear
// region — verified below by rendering at two amplitudes a decade apart and
// checking the response is the same — so the impulse response IS the transfer
// function, and 200 Goertzel bins over one 16384-sample tail costs what a single
// steady-tone render used to.
struct Response {
    std::vector<double> hz;
    std::vector<double> mag;  // linear, referred to the impulse area
};

Response renderResponse(double pos, double voice, double fs, double amp = 1e-3) {
    WahModel m;
    m.prepare(fs, 128);
    m.setPosition(static_cast<float>(pos));
    m.setSensitivity(0.0f);
    m.setVoice(static_cast<float>(voice));
    // Settle the knob smoothers (they start at the model's own defaults) and the
    // transistor stage's coupling cap before the impulse lands.
    {
        std::vector<float> z(static_cast<size_t>(0.2 * fs), 0.0f), zo(z.size());
        m.process(z.data(), zo.data(), static_cast<int>(z.size()));
    }
    const int n = 16384;
    std::vector<float> in(static_cast<size_t>(n), 0.0f), out(in.size());
    in[0] = static_cast<float>(amp);
    m.process(in.data(), out.data(), n);

    Response r;
    for (double f = 100.0; f < 8000.0; f *= 1.0035) {
        r.hz.push_back(f);
        // goertzel() divides by the block length; the impulse's own spectrum is
        // flat at `amp`, so this ratio is the transfer magnitude.
        r.mag.push_back(goertzel(out, f, fs) * static_cast<double>(n) * 0.5 / amp);
    }
    return r;
}

struct PeakShape {
    double f0 = 0.0;      // rendered peak frequency (Hz)
    double gainDb = 0.0;  // rendered peak gain (dB)
    double q = 0.0;       // rendered f0 / (-3 dB bandwidth)
};

PeakShape measurePeak(double pos, double voice, double fs) {
    const Response r = renderResponse(pos, voice, fs);
    PeakShape p;
    size_t iPk = 0;
    double best = 0.0;
    for (size_t i = 0; i < r.hz.size(); ++i)
        if (r.mag[i] > best) { best = r.mag[i]; iPk = i; }
    p.f0 = r.hz[iPk];
    p.gainDb = 20.0 * std::log10(best);
    const double half = best / std::sqrt(2.0);
    double lo = r.hz.front(), hi = r.hz.back();
    for (size_t i = iPk; i-- > 0;)
        if (r.mag[i] < half) { lo = r.hz[i]; break; }
    for (size_t i = iPk; i < r.hz.size(); ++i)
        if (r.mag[i] < half) { hi = r.hz[i]; break; }
    p.q = p.f0 / (hi - lo);
    return p;
}

// A plucked note: sharp attack, decaying harmonic stack.
std::vector<float> pluck(double f0, double peak, double secs, double fs,
                         double decay = 3.0) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> x(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        const double env = std::exp(-decay * t) * (1.0 - std::exp(-t / 0.003));
        double s = 0.0;
        for (int h = 1; h <= 8; ++h) s += std::sin(kTwoPi * f0 * h * t) / h;
        x[static_cast<size_t>(i)] = static_cast<float>(peak * env * s / 1.6);
    }
    return x;
}

// Worst magnitude in [lo, hi) excluding +/-tol around every harmonic of `fund`.
double bandFloor(const std::vector<float>& x, double lo, double hi, double fund,
                 double tol, double fs) {
    double worst = 0.0;
    for (double f = lo; f < hi; f += 24.0) {
        const double r = std::fmod(f, fund);
        if (r < tol || r > fund - tol) continue;
        worst = std::max(worst, goertzel(x, f, fs));
    }
    return worst;
}

// -----------------------------------------------------------------------------
// (a)+(b)+(c) the sweep law
// -----------------------------------------------------------------------------
void testSweepLaw(double fs) {
    std::printf("  [sweep law @ %.0f Hz]\n", fs);
    std::printf("     p    derived     rendered    err%%     CCRMA      err%%\n");
    double worstOwn = 0.0, worstRef = 0.0, sumSq = 0.0;
    int nPts = 0;
    for (int i = 0; i <= 8; ++i) {
        const double p = i / 8.0;
        const double derived = WahModel::positionToHz(p);
        const double rendered = measurePeak(p, 0.5, fs).f0;
        const double ref = WahModel::referenceHz(p);
        const double eOwn = 100.0 * (rendered / derived - 1.0);
        const double eRef = 100.0 * (rendered / ref - 1.0);
        std::printf("   %.3f %10.2f %11.2f %+7.2f %10.2f %+8.2f\n", p, derived,
                    rendered, eOwn, ref, eRef);
        worstOwn = std::max(worstOwn, std::fabs(eOwn));
        worstRef = std::max(worstRef, std::fabs(eRef));
        sumSq += eRef * eRef;
        ++nPts;
    }
    const double rmsRef = std::sqrt(sumSq / nPts);
    std::printf("     worst vs derived %.2f %%   vs CCRMA worst %.2f %% rms %.2f %%\n",
                worstOwn, worstRef, rmsRef);

    // (a) DISCRETISATION ONLY — the rendered peak must land on the model's own
    // continuous-time law. This bar cannot catch a wrong law; it catches a wrong
    // discretisation, and it is labelled as such.
    assert(worstOwn < 1.5);

    // (b) THE BAR WITH TEETH: the rendered sweep against an INDEPENDENT
    // measurement of a real GCB-95 (CCRMA / Julius Smith, fitted to three
    // measured responses). Nothing in this repo produced that curve.
    assert(worstRef < 3.0);
    assert(rmsRef < 1.5);

    // The endpoints, against the published figures the research pinned.
    assert(std::fabs(WahModel::positionToHz(0.0) - 450.0) < 0.5);
    assert(std::fabs(WahModel::bareTankHz() - 2250.79) < 0.05);
    assert(std::fabs(WahModel::positionToHz(1.0) - WahModel::bareTankHz()) < 1e-9);

    // (c) SHAPE. A law with the right endpoints and the wrong middle is the
    // classic modelled-wah failure. Total travel, and the balance of the halves.
    const double lo = WahModel::positionToHz(0.0);
    const double mid = WahModel::positionToHz(0.5);
    const double hi = WahModel::positionToHz(1.0);
    const double octLo = std::log2(mid / lo), octHi = std::log2(hi / mid);
    std::printf("     travel %.3f oct   halves %.3f / %.3f oct\n",
                std::log2(hi / lo), octLo, octHi);
    assert(std::fabs(std::log2(hi / lo) - 2.322) < 0.02);
    // The halves must balance. The counterfactual is computed here rather than
    // asserted from memory: a LINEAR control law over the same circuit.
    {
        const double A = (WahModel::bareTankHz() / 450.0) *
                             (WahModel::bareTankHz() / 450.0) - 1.0;
        const double linMid = WahModel::bareTankHz() / std::sqrt(1.0 + A * 0.5);
        const double linLo = std::log2(linMid / lo), linHi = std::log2(hi / linMid);
        std::printf("     linear-taper counterfactual halves %.3f / %.3f oct\n",
                    linLo, linHi);
        assert(std::fabs(linHi - linLo) > 1.0);          // the failure mode is real
        assert(std::fabs(octHi - octLo) < 0.05);         // and we are not it
        assert(std::fabs(octHi - octLo) * 3.9 < std::fabs(linHi - linLo));
    }

    // The wiper law's endpoints (heel = full pot, toe = none) and its half-
    // rotation value, which is the honesty check on the one fitted number: it has
    // to look like an audio-taper pot (10-20 % at 50 % rotation).
    assert(std::fabs(WahModel::wiperLaw(0.0) - 1.0) < 1e-12);
    assert(std::fabs(WahModel::wiperLaw(1.0)) < 1e-12);
    const double halfRot = WahModel::wiperLaw(0.5);
    std::printf("     wiper at half rotation = %.2f %% (audio-taper spec 10-20 %%)\n",
                100.0 * halfRot);
    assert(halfRot > 0.10 && halfRot < 0.20);

    // Strictly monotone — a wah that reverses anywhere is broken.
    double prev = 0.0;
    for (int i = 0; i <= 200; ++i) {
        const double f = WahModel::positionToHz(i / 200.0);
        assert(f > prev);
        prev = f;
    }
}

// -----------------------------------------------------------------------------
// (d)+(e) resonance height and width
// -----------------------------------------------------------------------------
void testResonance(double fs) {
    std::printf("  [resonance @ %.0f Hz]\n", fs);
    // The impulse probe is only a transfer function if the stage is linear there.
    // Prove it rather than assume it: a decade of amplitude must not move the
    // response. (At 0.1 V it does — that is the pedal barking, and it is measured
    // separately in testStaging.)
    {
        const Response a = renderResponse(0.5, 0.5, fs, 1e-4);
        const Response b = renderResponse(0.5, 0.5, fs, 1e-3);
        double worst = 0.0;
        for (size_t i = 0; i < a.hz.size(); ++i)
            worst = std::max(worst, std::fabs(20.0 * std::log10(b.mag[i] / a.mag[i])));
        std::printf("     probe linearity over a decade of level: worst %.4f dB\n",
                    worst);
        assert(worst < 0.05);
    }
    std::printf("     p    peak(dB)   Qrendered  Qderived  QCCRMA\n");
    double minDb = 1e9, maxDb = -1e9, worstQ = 0.0, worstQRef = 0.0;
    for (int i = 0; i <= 4; ++i) {
        const double p = i / 4.0;
        const PeakShape s = measurePeak(p, 0.5, fs);
        const double qd = WahModel::resonanceQ(p, 0.5);
        const double qr = WahModel::referenceQ(p);
        std::printf("   %.2f %10.2f %11.3f %9.3f %8.3f\n", p, s.gainDb, s.q, qd, qr);
        minDb = std::min(minDb, s.gainDb);
        maxDb = std::max(maxDb, s.gainDb);
        worstQ = std::max(worstQ, std::fabs(s.q / qd - 1.0));
        worstQRef = std::max(worstQRef, std::fabs(s.q / qr - 1.0));
    }
    std::printf("     peak boost %.2f..%.2f dB (published 18.0), spread %.3f dB\n",
                minDb, maxDb, maxDb - minDb);
    std::printf("     Q vs derived worst %.1f %%   vs CCRMA worst %.1f %%\n",
                100.0 * worstQ, 100.0 * worstQRef);

    // The published resonant boost, and the topology's prediction that it does
    // NOT move across the travel (fixed resistors set it). Both are bars.
    //
    // The 18.0 is a LITERAL, deliberately, not WahModel::peakBoostDb(). Written
    // against the accessor this is an identity that cannot fail — the
    // perturbation run proved it: moving the constant to 24 dB left this test
    // green and only tripped an unrelated assertion three functions later.
    // 18 dB is the published figure (Catalinbread / ElectroSmash, quoted at BOTH
    // ends of the sweep) and that is what the render is measured against.
    constexpr double kPublishedBoostDb = 18.0;
    assert(std::fabs(WahModel::peakBoostDb() - kPublishedBoostDb) < 1e-12);
    assert(std::fabs(minDb - kPublishedBoostDb) < 0.5);
    assert(std::fabs(maxDb - kPublishedBoostDb) < 0.5);
    assert(maxDb - minDb < 0.25);

    // Width tracks the derived 1/f0 law, and stays near the independent
    // measurement. The CCRMA bar is looser BY DESIGN: the derived exponent is
    // -1.000 and the measured one is -0.870, which is +-11 % at the ends. That
    // difference is REPORTED, not fitted away (docs §58.3).
    assert(worstQ < 0.05);
    assert(worstQRef < 0.16);

    // Q must fall monotonically as the peak rises — the parallel-RLC signature,
    // and the opposite of what a series-LCR topology would give.
    double prev = 1e9;
    for (int i = 0; i <= 40; ++i) {
        const double q = WahModel::resonanceQ(i / 40.0, 0.5);
        assert(q < prev);
        prev = q;
    }

    // (e) VOICE: width only.
    std::printf("     VOICE   R7(kohm)   peak(dB)    f0(Hz)   Qrendered  Qderived\n");
    double f0Ref = 0.0, dbRef = 0.0, qLo = 0.0, qHi = 0.0;
    for (int i = 0; i <= 2; ++i) {
        const double v = i / 2.0;
        const PeakShape s = measurePeak(0.5, v, fs);
        std::printf("     %.2f %10.2f %10.2f %9.2f %11.3f %9.3f\n", v,
                    WahModel::voiceToR7Ohms(v) / 1000.0, s.gainDb, s.f0, s.q,
                    WahModel::resonanceQ(0.5, v));
        if (i == 0) { f0Ref = s.f0; dbRef = s.gainDb; qLo = s.q; }
        if (i == 2) {
            qHi = s.q;
            assert(std::fabs(s.f0 / f0Ref - 1.0) < 0.01);   // centre does not move
            assert(std::fabs(s.gainDb - dbRef) < 0.25);     // height does not move
        }
    }
    assert(qHi > 2.0 * qLo);  // but the width certainly does
    // VOICE 0.5 is the stock 33 kOhm exactly — the knob is centred on the real part.
    assert(std::fabs(WahModel::voiceToR7Ohms(0.5) - 33000.0) < 1.0);
}

// -----------------------------------------------------------------------------
// (f)+(g) AUTO
// -----------------------------------------------------------------------------
void testAutoTracking(double fs) {
    std::printf("  [auto tracking @ %.0f Hz]\n", fs);
    std::printf("     sens   rest(Hz)   peak(Hz)   octaves   t_peak(ms)  t_back(ms)\n");
    double prevOct = -1.0;
    for (int i = 0; i <= 4; ++i) {
        const double sens = i / 4.0;
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(0.10f);
        m.setSensitivity(static_cast<float>(sens));
        m.setVoice(0.5f);
        // Settle the knob smoothers before the note so the trace measures the
        // ENVELOPE and not the model's own start-up ramp. 0.5 s is not padding:
        // the cascaded 4 ms poles only snap EXACTLY onto their target once the
        // residual crosses the 1e-30 denormal floor, which takes ~69 tau = 0.28 s,
        // and the SENS = 0 bar below is an exact-zero bar.
        {
            std::vector<float> z(static_cast<size_t>(0.5 * fs), 0.0f), zo(z.size());
            m.process(z.data(), zo.data(), static_cast<int>(z.size()));
        }
        const std::vector<float> in = pluck(146.83, 0.30, 2.0, fs);
        std::vector<float> out(64);
        // The model's OWN settled centre, not positionToHz(0.10): the knob is a
        // float, so double(0.10f) != 0.10 and the SENS = 0 bar is exact-zero.
        const double fRest = m.currentCentreHz();
        assert(std::fabs(fRest / WahModel::positionToHz(0.10) - 1.0) < 1e-6);
        double fMax = 0.0;
        int iMax = 0, iBack = static_cast<int>(in.size());
        std::vector<double> trace;
        for (size_t off = 0; off < in.size(); off += 64) {
            const int n = static_cast<int>(std::min<size_t>(64, in.size() - off));
            m.process(in.data() + off, out.data(), n);
            trace.push_back(m.currentCentreHz());
            if (m.currentCentreHz() > fMax) {
                fMax = m.currentCentreHz();
                iMax = static_cast<int>(off) + n;
            }
        }
        const double octaves = std::log2(fMax / fRest);
        for (size_t k = static_cast<size_t>(iMax / 64); k < trace.size(); ++k) {
            if (std::log2(trace[k] / fRest) <= 0.10 * octaves) {
                iBack = static_cast<int>(k) * 64;
                break;
            }
        }
        std::printf("     %.2f %10.1f %10.1f %9.3f %11.1f %11.1f\n", sens, fRest, fMax,
                    octaves, 1000.0 * iMax / fs, 1000.0 * (iBack - iMax) / fs);

        if (i == 0) {
            // SENS = 0: the filter does not move AT ALL under a loud pluck.
            assert(octaves == 0.0);
        } else {
            // A real note opens the filter, and further with more SENS.
            assert(octaves > prevOct);
            // ...and it comes BACK. The whole excursion is transient.
            assert(iBack > iMax);
            assert(1000.0 * (iBack - iMax) / fs < 1500.0);
            // The peak arrives after the pick, not at it, and not a second later.
            assert(1000.0 * iMax / fs > 5.0 && 1000.0 * iMax / fs < 250.0);
        }
        prevOct = octaves;
    }
    // At full SENSITIVITY a single pluck must be worth more than an octave —
    // otherwise the control is decorative.
    assert(prevOct > 1.0);

    // (g) SENS = 0 is BIT-IDENTICAL to a model whose sensitivity was never set.
    {
        const std::vector<float> in = pluck(146.83, 0.5, 1.0, fs);
        std::vector<float> a(in.size()), b(in.size());
        WahModel m1, m2;
        m1.prepare(fs, 128);
        m1.setPosition(0.4f);
        m1.setSensitivity(0.0f);
        m1.setVoice(0.5f);
        m1.process(in.data(), a.data(), static_cast<int>(in.size()));
        m2.prepare(fs, 128);
        m2.setPosition(0.4f);
        m2.setVoice(0.5f);
        m2.process(in.data(), b.data(), static_cast<int>(in.size()));
        double worst = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(a[i] - b[i])));
        std::printf("     SENS=0 vs never-set: worst |diff| = %.3e\n", worst);
        assert(worst == 0.0);
    }
}

// -----------------------------------------------------------------------------
// (l) THE FOLLOWER'S LEVEL LAW — proportional, with NO threshold (docs §58.8)
//
// This is the bar that says, as a measurement, that the wah's envelope follower
// is NOT the shared `SidechainDetector` (docs §61.2) and must not be replaced by
// it. `SidechainDetector` is a THRESHOLD detector: a DC-restorer clamp into a
// base-emitter junction, whose conduction is exponential and whose collector
// current is orders of magnitude larger than the envelope resistor's pull-up.
// Open loop, its proportional range measures 1.95 dB with M13.1's own component
// values and 2.38 dB with M13.6a's — it is a switch, and both consumers want one
// (the compressor closes a feedback loop around it, the gate feeds it to a
// comparator). A wah has no gain to reduce, so it is feed-forward by necessity
// and needs the OTHER thing: an envelope that is proportional to pick strength,
// open loop, all the way down to a quiet note. Full derivation + the substitution
// experiment: docs §58.8 and ADR 023.
//
// Three player-observable bars, all of which the substitution fails:
//   1. a QUIET pick still opens the filter (a threshold makes it do nothing);
//   2. sweep depth is PROPORTIONAL to pick strength (a threshold detector's is
//      exponential — it goes from nothing to railed across ~2 dB of input);
//   3. time-to-peak does not depend on how hard you pick (the detector's attack
//      is a current-starved discharge, so it does — §59 measured 14/10/5/3 ms
//      across SUSTAIN on the compressor).
// -----------------------------------------------------------------------------
void testFollowerLevelLaw(double fs) {
    std::printf("  [follower level law @ %.0f Hz]\n", fs);

    // One pluck at a given peak level, at full SENSITIVITY: how far does the
    // filter open, and when does it get there? Watches the FILTER, not the
    // follower — reading the follower's own output would be a tautology.
    struct Shot { double octaves, tPeakMs; };
    auto shot = [&](double level) {
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(0.10f);
        m.setSensitivity(1.0f);
        m.setVoice(0.5f);
        {
            std::vector<float> z(static_cast<size_t>(0.5 * fs), 0.0f), zo(z.size());
            m.process(z.data(), zo.data(), static_cast<int>(z.size()));
        }
        const std::vector<float> in = pluck(146.83, level, 2.0, fs);
        std::vector<float> out(64);
        const double fRest = m.currentCentreHz();
        double fMax = 0.0;
        int iMax = 0;
        for (size_t off = 0; off < in.size(); off += 64) {
            const int n = static_cast<int>(std::min<size_t>(64, in.size() - off));
            m.process(in.data() + off, out.data(), n);
            if (m.currentCentreHz() > fMax) {
                fMax = m.currentCentreHz();
                iMax = static_cast<int>(off) + n;
            }
        }
        return Shot{std::log2(fMax / fRest), 1000.0 * iMax / fs};
    };

    // 0.05 V is a QUIET pick — half the house clean probe (0.10 V, docs §11.1)
    // and a fifth of the follower's own reference level. 0.20 V is a firm one.
    const Shot q = shot(0.05), h = shot(0.10), f = shot(0.20);
    std::printf("     level   octaves   t_peak(ms)\n");
    std::printf("     0.05 %9.3f %11.1f\n", q.octaves, q.tPeakMs);
    std::printf("     0.10 %9.3f %11.1f\n", h.octaves, h.tPeakMs);
    std::printf("     0.20 %9.3f %11.1f\n", f.octaves, f.tPeakMs);

    // 1. A QUIET pick opens the filter by a musically real amount. A threshold
    //    detector measures 0.000 here — the pedal becomes a static filter until
    //    you dig in.
    assert(q.octaves > 0.15);

    // 2. PROPORTIONAL: 2x and 4x the pick strength give 2x and 4x the sweep.
    //    This is the property a Vbe threshold destroys, and it is not automatic —
    //    the tank's own f0(p) law is nonlinear, so proportionality in OCTAVES is
    //    a prediction about the follower, not an identity.
    std::printf("     ratios: 2x -> %.3f (want 2.00), 4x -> %.3f (want 4.00)\n",
                h.octaves / q.octaves, f.octaves / q.octaves);
    assert(std::fabs(h.octaves / q.octaves - 2.0) < 0.20);
    assert(std::fabs(f.octaves / q.octaves - 4.0) < 0.40);

    // 3. Time-to-peak does not depend on how hard you pick. The measurement grid
    //    is 64 frames, so "identical" means within two blocks. docs §58.6 records
    //    that the 82.7 ms is the RECTIFIER's own behaviour on a 147 Hz note
    //    (|x| crosses zero twice a cycle) rather than the 10 ms coefficient —
    //    which is exactly why swapping the rectifier moves it.
    const double lo = std::min(q.tPeakMs, std::min(h.tPeakMs, f.tPeakMs));
    const double hi = std::max(q.tPeakMs, std::max(h.tPeakMs, f.tPeakMs));
    std::printf("     t_peak spread across 12 dB of pick strength: %.2f ms\n", hi - lo);
    assert(hi - lo <= 2.0 * 1000.0 * 64.0 / fs);
}

// -----------------------------------------------------------------------------
// (h) no zipper — measured against its own control
// -----------------------------------------------------------------------------
void testNoZipper(double fs) {
    std::printf("  [zipper @ %.0f Hz]\n", fs);
    // mode: 0 static, 1 fast full-travel sweep, 2 pathological per-block slam
    auto run = [&](int mode, double loHz, double hiHz) {
        WahModel m;
        m.prepare(fs, 128);
        m.setSensitivity(0.0f);
        m.setVoice(0.5f);
        m.setPosition(0.5f);
        const int n = static_cast<int>(1.0 * fs);
        std::vector<float> in(static_cast<size_t>(n)), out(in.size());
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.02 * std::sin(kTwoPi * 1000.0 * i / fs));
        for (int off = 0; off < n; off += 64) {
            if (mode == 1) {
                const double ph = std::fmod(off / fs, 0.2) / 0.2;
                m.setPosition(static_cast<float>(ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph));
            } else if (mode == 2) {
                m.setPosition((off / 64) % 2 ? 1.0f : 0.0f);
            }
            m.process(in.data() + off, out.data() + off, std::min(64, n - off));
        }
        for (float v : out) assert(std::isfinite(v));
        std::vector<float> t(out.begin() + n / 2, out.end());
        return 20.0 * std::log10(bandFloor(t, loHz, hiHz, 1000.0, 200.0, fs) /
                                 goertzel(t, 1000.0, fs));
    };
    const double stat = run(0, 6000.0, 20000.0);
    const double slam = run(2, 6000.0, 20000.0);
    const double sweepLo = run(1, 3000.0, 6000.0);
    const double sweepHi = run(1, 14000.0, 20000.0);
    std::printf("     static %.1f dB | per-block slam %.1f dB | sweep 3-6k %.1f dB, "
                "14-20k %.1f dB\n", stat, slam, sweepLo, sweepHi);

    // The resonator at rest is clean to the float floor — the control for
    // everything below. (Measured −153.1 dB at 44.1 k, where the top of the
    // search band is close enough to Nyquist to sit in the halfband stopband,
    // and −322.9 dB at 48 k.)
    assert(stat < -130.0);
    // The pathological slam (POSITION 0<->1 every 64 samples) stays far down.
    assert(slam < -95.0);
    // A fast full-travel sweep leaves a skirt, and the property that says it is a
    // modulation skirt rather than coefficient STEPPING is that it DECAYS with
    // frequency. A stepping artifact is broadband and flat.
    assert(sweepLo < -60.0);
    assert(sweepHi < sweepLo - 8.0);
}

// -----------------------------------------------------------------------------
// (i) alias floor
// -----------------------------------------------------------------------------
void testAliasing(double fs) {
    std::printf("  [alias floor @ %.0f Hz]\n", fs);
    const double ftone = 4186.0;  // top C, the house aliasing stimulus
    double prev = 1e9, floor1x = 0.0, floor4x = 0.0;
    for (int f : {1, 2, 4, 8}) {
        WahModel m;
        m.prepare(fs, 128);
        m.setOversampling(f);
        m.setPosition(1.0f);
        m.setSensitivity(0.0f);
        m.setVoice(0.5f);
        const int n = static_cast<int>(1.0 * fs);
        std::vector<float> in(static_cast<size_t>(n)), out(in.size());
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(1.5 * std::sin(kTwoPi * ftone * i / fs));
        m.process(in.data(), out.data(), n);
        std::vector<float> t(out.begin() + n / 2, out.end());
        const double carrier = goertzel(t, ftone, fs);
        double worst = 0.0;
        for (double fb = 24.0; fb < 0.47 * fs; fb += 24.0) {
            bool harm = false;
            for (int k = 1; k <= 5; ++k)
                if (std::fabs(fb - ftone * k) < 400.0) harm = true;
            if (harm) continue;
            worst = std::max(worst, goertzel(t, fb, fs));
        }
        const double db = 20.0 * std::log10(worst / carrier);
        std::printf("     os %dx: %.1f dB rel carrier, latency %d\n", f, db,
                    m.latencySamples());
        if (f == 1) floor1x = db;
        if (f == 4) floor4x = db;
        if (f > 1) assert(db <= prev + 1.0);  // never worse than the factor below
        prev = db;
    }
    // The bar that has teeth (docs §54): the floor must MOVE with the factor. A
    // number that does not improve with oversampling is not measuring aliasing.
    std::printf("     1x -> 4x improvement %.1f dB\n", floor4x - floor1x);
    assert(floor4x < -100.0);
    assert(floor1x - floor4x > 20.0);
}

// -----------------------------------------------------------------------------
// (j) DC offset ON SIGNAL
// -----------------------------------------------------------------------------
void testDcOffset(double fs) {
    std::printf("  [dc offset on signal @ %.0f Hz]\n", fs);
    // HONEST NOTE ON THE SECOND STIMULUS: support/DcOffset.h's +0.1 V input-offset
    // case exists because a clean input cannot fail a missing output coupling cap.
    // On THIS pedal it has no extra teeth either, and for a structural reason
    // worth stating rather than papering over: the resonator is a bandpass with a
    // zero at DC, so an input offset is removed BEFORE it can reach anything, and
    // the two rows below are identical to six decimals. The clean row is doing all
    // the work here — it catches the real risk, which is the transistor stage
    // RECTIFYING on signal.
    for (double p : {0.0, 0.5, 1.0}) {
        for (int withOffset = 0; withOffset < 2; ++withOffset) {
            WahModel m;
            m.prepare(fs, 128);
            m.setPosition(static_cast<float>(p));
            m.setSensitivity(0.0f);
            m.setVoice(0.5f);
            const int n = static_cast<int>(1.0 * fs);
            std::vector<float> in(static_cast<size_t>(n)), out(in.size());
            for (int i = 0; i < n; ++i)
                in[static_cast<size_t>(i)] =
                    static_cast<float>(0.4 * std::sin(kTwoPi * 220.0 * i / fs)) +
                    (withOffset ? clipper::test::kInputDcOffset : 0.0f);
            m.process(in.data(), out.data(), n);
            const clipper::test::DcOnSignal d = clipper::test::measureDcOnSignal(out);
            std::printf("     p=%.1f %s: DC %+.6f / peak %.4f = %.4f %%\n", p,
                        withOffset ? "offset " : "clean  ", d.mean, d.peak,
                        100.0 * d.fraction);
            assert(d.peak > 0.01);  // the test would be vacuous on silence
            assert(d.fraction < clipper::test::kDcFractionBar);
        }
    }
}

// -----------------------------------------------------------------------------
// (k) reset / NaN / denormal
// -----------------------------------------------------------------------------
void testResetAndGuards(double fs) {
    std::printf("  [reset / guards @ %.0f Hz]\n", fs);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // A NaN knob must never reach the engine's state. (The C ABI rejects
    // non-finite outright; the model clamps as the second line of defence.)
    {
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(nan);
        m.setSensitivity(nan);
        m.setVoice(nan);
        const int n = 4096;
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * i / fs));
        m.process(in.data(), out.data(), n);
        int bad = 0;
        for (float v : out) if (!std::isfinite(v)) ++bad;
        std::printf("     NaN knobs -> %d/%d non-finite output samples\n", bad, n);
        assert(bad == 0);
    }

    // A NaN INPUT poisons the recursion; reset() is the recovery seam.
    {
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(0.5f);
        const int n = 4096;
        std::vector<float> in(n, 0.0f), out(n);
        in[10] = nan;
        m.process(in.data(), out.data(), n);
        m.reset();
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.2 * std::sin(kTwoPi * 220.0 * i / fs));
        m.process(in.data(), out.data(), n);
        int bad = 0;
        for (float v : out) if (!std::isfinite(v)) ++bad;
        std::printf("     after NaN input + reset() -> %d/%d non-finite\n", bad, n);
        assert(bad == 0);
    }

    // Silence in -> exact digital silence out, from sample 0 (no turn-on thump).
    {
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(0.5f);
        std::vector<float> in(4096, 0.0f), out(4096);
        m.process(in.data(), out.data(), 4096);
        for (float v : out) assert(v == 0.0f);
    }

    // Every zero-resting state reaches EXACTLY 0.0 after a silent tail (docs §33
    // / ADR 006). The 12 s tail is not padding: the envelope follower's 166.7 ms
    // release takes 11.4 s to ring from a loud note down through the 1e-30 floor,
    // and it is measured here rather than assumed (2 s reads 2.7e-06).
    {
        WahModel m;
        m.prepare(fs, 128);
        m.setPosition(0.5f);
        m.setSensitivity(0.8f);
        std::vector<float> in(4800), out(4800);
        for (int i = 0; i < 4800; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.5 * std::sin(kTwoPi * 220.0 * i / fs));
        m.process(in.data(), out.data(), 4800);
        std::fill(in.begin(), in.end(), 0.0f);
        const int blocks = static_cast<int>(12.0 * fs / 4800.0);
        for (int b = 0; b < blocks; ++b) m.process(in.data(), out.data(), 4800);
        std::printf("     maxAbsRestingState after a 12 s silent tail = %.3e\n",
                    m.maxAbsRestingState());
        assert(m.maxAbsRestingState() == 0.0);
    }

    // Block-size independence: the stream must not depend on how it is chunked.
    {
        const int n = 8192;
        std::vector<float> in(static_cast<size_t>(n)), a(in.size()), b(in.size());
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.3 * std::sin(kTwoPi * 330.0 * i / fs));
        WahModel m1, m2;
        m1.prepare(fs, 128);
        m2.prepare(fs, 128);
        m1.setPosition(0.6f);
        m2.setPosition(0.6f);
        m1.process(in.data(), a.data(), n);
        for (int off = 0; off < n; off += 128)
            m2.process(in.data() + off, b.data() + off, std::min(128, n - off));
        double worst = 0.0;
        for (int i = 0; i < n; ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(a[i] - b[i])));
        std::printf("     one 8192 block vs 64x128 blocks: worst |diff| %.3e\n", worst);
        assert(worst == 0.0);
    }
}

// -----------------------------------------------------------------------------
// Staging: the divider is MEASURED from the stage, so the pedal's small-signal
// resonant boost is the published figure by construction. Report the level and
// the breakup the component values then predict — neither is a knob.
// -----------------------------------------------------------------------------
void testStaging(double fs) {
    std::printf("  [staging @ %.0f Hz]\n", fs);
    WahModel m;
    m.prepare(fs, 128);
    std::printf("     output-stage G0 = %.4f x, tank divider = %.6f, product = %.4f\n",
                m.outputStageGain(), m.tankDivider(),
                m.outputStageGain() * m.tankDivider());
    // The identity that says nothing was fitted: G0 * divider == 10^(18/20).
    assert(std::fabs(m.outputStageGain() * m.tankDivider() -
                     std::pow(10.0, WahModel::peakBoostDb() / 20.0)) < 1e-9);
    assert(m.outputStageGain() > 5.0);  // it is a real gain stage, not a buffer

    // The SHIPPED oversampling default. Nothing else pins it: testAliasing sets
    // the factor explicitly for every row, so dropping the default to 1x left the
    // whole suite green in the perturbation run. 4x is the house default for a
    // nonlinear stage and the latency it reports is what the UI and the plugin
    // display, so it is asserted through the observable.
    assert(m.latencySamples() == 72);

    std::printf("     pluck level (low E, POSITION 0.5): in_pk  out_pk  dRMS(dB)\n");
    for (double amp : {0.1, 0.3, 0.5}) {
        const std::vector<float> in = pluck(82.41, amp, 2.0, fs);
        std::vector<float> out(in.size());
        WahModel w;
        w.prepare(fs, 128);
        w.setPosition(0.5f);
        w.setSensitivity(0.0f);
        w.setVoice(0.5f);
        w.process(in.data(), out.data(), static_cast<int>(in.size()));
        double ip = 0, op = 0, ir = 0, orr = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            ip = std::max(ip, std::fabs(static_cast<double>(in[i])));
            op = std::max(op, std::fabs(static_cast<double>(out[i])));
            ir += static_cast<double>(in[i]) * in[i];
            orr += static_cast<double>(out[i]) * out[i];
        }
        ir = std::sqrt(ir / in.size());
        orr = std::sqrt(orr / in.size());
        std::printf("       %7.3f %7.4f %+8.2f\n", ip, op, 20.0 * std::log10(orr / ir));
        // On real playing material a bandpass is a CUT, not a level bomb: the
        // +18 dB lives at fc while the fundamental and everything above roll off.
        assert(orr < ir);
        assert(op < 4.0);
    }
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("== clipper_wah_tests (docs §58) ==\n");
    for (double fs : {44100.0, 48000.0}) {
        std::printf("-- %.0f Hz --\n", fs);
        testSweepLaw(fs);
        testResonance(fs);
        testAutoTracking(fs);
        testFollowerLevelLaw(fs);
        testNoZipper(fs);
        testDcOffset(fs);
        testStaging(fs);
    }
    // The expensive ones once, at the shipped desktop rate.
    testAliasing(48000.0);
    testResetAndGuards(48000.0);
    std::printf("ALL WAH TESTS PASSED\n");
    return 0;
}
