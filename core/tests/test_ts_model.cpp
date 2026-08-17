// Plain-assert tests for clipper::dsp::TsModel (v1.1 — TS808-style "Screamer").
// No framework: int main + <cassert>. Frequency content via a hand-rolled
// Goertzel. Mirrors test_sd_model.cpp — the TS IS the SD-1 topology — so the
// deltas ARE the tests: SYMMETRIC clipping (2nd harmonic ~absent, the mirror of
// the SD-1's asymmetry) and a lower +40.6 dB DRIVE plateau (vs the SD-1's
// +46.6 dB). Shared traits (720 Hz mid-hump, aliasing) are re-verified.
//
// The headline measurements (see docs/DEVELOPMENT.md §21):
//   1. Mid-hump corner ~= 720 Hz within +/-1.5 dB of 1 + K*HP720 (SHARED trait).
//   2. SYMMETRY: 220 Hz -> the 2nd (even) harmonic ~ABSENT, cited side-by-side
//      with the SD-1's strong even harmonic (its asymmetric warmth). The mirror
//      of the SD-1's asymmetry test.
//   3. Max-DRIVE plateau ~= 40.6 dB analytic (1 + 500k/4.7k), NOT the SD-1's 46.6.
//   4. Aliasing: shipped 4x ADAA at max DRIVE below the M2 -60 dB bar.

#include "clipper/dsp/TsModel.h"

#include "clipper/dsp/OutputPotTaper.h"
#include "clipper/dsp/SdModel.h"
#include "measure/AliasMetric.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::TsModel;
using clipper::dsp::SdModel;

// TS analytic constants — KEEP IN SYNC with TsModel.cpp.
constexpr double kMidHumpHz = 720.5;
constexpr double kDriveMinDb = 12.0;
constexpr double kDriveMaxDb = 40.6;  // 1 + 500k/4.7k ~= 107.4x

double goertzelAmp(const std::vector<float>& x, size_t start, size_t n, double f,
                   double fs) {
    const double w = kTwoPi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0, winSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double win = 0.5 * (1.0 - std::cos(kTwoPi * i / (n - 1)));
        winSum += win;
        const double s0 = x[start + i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / winSum;
}
double toDb(double amp) { return 20.0 * std::log10(amp + 1e-12); }

double driveKnobToK(double knob) {
    const double db = kDriveMinDb + (kDriveMaxDb - kDriveMinDb) * knob;
    return std::pow(10.0, db / 20.0) - 1.0;
}

struct Params { float drive, tone, level; };

// Render a mono buffer through a fresh TS model, with measurement options.
std::vector<float> render(const std::vector<float>& in, Params p, double fs,
                          int os = 4, int clipMode = TsModel::CLIP_ADAA,
                          bool idealOpAmp = false, bool symmetric = true) {
    TsModel m;
    m.prepare(fs, 128);
    m.setOversampling(os);
    m.setClipMode(clipMode);
    m.setIdealOpAmp(idealOpAmp);
    m.setSymmetric(symmetric);
    m.setParameter(TsModel::PARAM_DRIVE, p.drive);
    m.setParameter(TsModel::PARAM_TONE, p.tone);
    m.setParameter(TsModel::PARAM_LEVEL, p.level);
    std::vector<float> out(in.size(), 0.0f);
    if (!in.empty()) m.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}
double tailRms(const std::vector<float>& x, double fs) {
    const size_t skip = std::min(x.size(), static_cast<size_t>(0.2 * fs));
    double acc = 0.0; size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i) { acc += double(x[i]) * x[i]; ++n; }
    return n ? std::sqrt(acc / n) : 0.0;
}

// --- Test 1: the mid-hump shelf matches 1 + K*HP720 within +/-1.5 dB. --------
// SHARED family trait (same Zg leg as the SD-1). Small signal, moderate DRIVE,
// TONE flat; measure dB relative to a 5 kHz plateau reference vs the analytic
// non-inverting shelf. Pins the 720 Hz corner.
void testMidHumpCorner(double fs) {
    const double driveKnob = 0.5;
    const double K = driveKnobToK(driveKnob);
    const double tau = 1.0 / (kTwoPi * kMidHumpHz);
    auto analyticDbRel = [&](double f, double fRef) {
        auto A = [&](double fr) {
            const double w = kTwoPi * fr;
            std::complex<double> hp = std::complex<double>(0.0, w * tau) /
                                      std::complex<double>(1.0, w * tau);
            return std::abs(std::complex<double>(1.0, 0.0) + K * hp);
        };
        return 20.0 * std::log10(A(f) / A(fRef));
    };
    const double fRef = 5000.0;
    const float amp = 1e-5f;
    auto measDbRel = [&](double f) {
        auto o = render(sine(f, amp, 0.5, fs), {(float)driveKnob, 0.5f, 1.0f}, fs);
        auto oRef = render(sine(fRef, amp, 0.5, fs), {(float)driveKnob, 0.5f, 1.0f}, fs);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
        return toDb(goertzelAmp(o, n - win, win, f, fs)) -
               toDb(goertzelAmp(oRef, n - win, win, fRef, fs));
    };
    const double probes[] = {82.4, 220.0, 440.0, 720.5, 1500.0, 3000.0, 5000.0};
    double worst = 0.0;
    for (double f : probes) {
        const double dev = std::fabs(measDbRel(f) - analyticDbRel(f, fRef));
        worst = std::max(worst, dev);
        assert(dev < 1.5 && "TS mid-hump shelf deviates >1.5 dB from analytic 1+K*HP720");
    }
    const double atCorner = measDbRel(kMidHumpHz);  // ~ -3 dB rel 5 kHz
    const double atBass = measDbRel(82.4);          // strongly shelved down
    assert(atCorner < -1.5 && atCorner > -4.5 && "720 Hz not ~3 dB below plateau");
    assert(atBass < atCorner - 3.0 && "bass not shelved below the corner");
    std::printf(
        "  [ok] mid-hump @ %.0f Hz: worst dev %.2f dB; 720Hz=%.2f dB, 82Hz=%.2f dB "
        "rel plateau (K=%.1f)\n",
        fs, worst, atCorner, atBass, K);
}

// --- Test 2: SYMMETRY — 2nd harmonic ~absent; cite the SD-1 side-by-side. -----
// The TS's 1-vs-1 diodes make an ODD (symmetric) curve, so the even (2nd)
// harmonic is ~absent — the MIRROR of the SD-1's 2-vs-1 asymmetry. Measured on
// the same 220 Hz / 0.30 V / moderate-drive stimulus the SD-1 test uses, cited
// against (a) the TS forced to the SD-1's asymmetric knees and (b) a real SD-1.
void testSymmetry(double fs) {
    const double f0 = 220.0;
    const float amp = 0.30f;
    // TS production (symmetric) harmonics.
    auto tsH = [&](double mult, bool sym) {
        auto o = render(sine(f0, amp, 1.0, fs), {0.5f, 0.5f, 1.0f}, fs, 4,
                        TsModel::CLIP_ADAA, false, sym);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
        return goertzelAmp(o, n - win, win, f0 * mult, fs);
    };
    const double f1 = toDb(tsH(1.0, true));
    const double h2Sym = toDb(tsH(2.0, true));   // TS production: symmetric
    const double h2Asym = toDb(tsH(2.0, false)); // TS forced asymmetric (SD-1 knees)
    const double h3Sym = toDb(tsH(3.0, true));

    // A REAL SD-1 on the identical stimulus, for the side-by-side citation.
    auto sdH2 = [&]() {
        SdModel m; m.prepare(fs, 128); m.setOversampling(4);
        m.setParameter(SdModel::PARAM_DRIVE, 0.5f);
        m.setParameter(SdModel::PARAM_TONE, 0.5f);
        m.setParameter(SdModel::PARAM_LEVEL, 1.0f);
        auto in = sine(f0, amp, 1.0, fs);
        std::vector<float> o(in.size(), 0.0f);
        m.process(in.data(), o.data(), static_cast<int>(in.size()));
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
        const double sf1 = toDb(goertzelAmp(o, n - win, win, f0, fs));
        return toDb(goertzelAmp(o, n - win, win, f0 * 2.0, fs)) - sf1;  // dBc
    };
    const double sd1H2dBc = sdH2();

    // The TS's symmetric 2nd harmonic sits FAR below its own asymmetric reference
    // (>= 20 dB) — the even harmonic is entirely asymmetry-driven, exactly as for
    // the SD-1 but with the roles reversed (here symmetric is production).
    assert(h2Asym - h2Sym > 20.0 && "TS 2nd harmonic not driven by asymmetry (sym ~= asym)");
    // Odd harmonic present (it IS a clipper): sanity that we are clipping.
    assert(f1 - h3Sym < 45.0 && "no 3rd harmonic — TS not clipping?");
    // And the TS's even harmonic is well below the SD-1's (its warmth we lack).
    assert((h2Sym - f1) < sd1H2dBc - 20.0 &&
           "TS 2nd harmonic not far below the SD-1's even-harmonic warmth");
    std::printf(
        "  [ok] symmetry @ %.0f Hz: TS 2nd harmonic %.1f dBc (symmetric, ~absent) vs "
        "%.1f dBc forced-asym vs SD-1 %.1f dBc (asymmetric warmth); TS delta %.1f dB\n",
        fs, h2Sym - f1, h2Asym - f1, sd1H2dBc, h2Asym - h2Sym);
}

// --- Test 3: max-DRIVE plateau ~= 40.6 dB analytic (1 + 500k/4.7k). ----------
// Small-signal gain well above the mid-hump corner (5 kHz, below the ~28 kHz
// op-amp corner) IS the plateau 1 + K_max. Absolute out/in gain, TONE + LEVEL
// flat/unity. Asserts the 500k drive-pot derivation and that it is NOT the SD-1's
// 46.6 dB. (Op-amp bypassed so the plateau is the clean analytic 1+K.)
void testMaxGainPlateau(double fs) {
    const double Kmax = driveKnobToK(1.0);
    const double plateauDbAnalytic = 20.0 * std::log10(1.0 + Kmax);  // ~40.6 dB
    const double fProbe = 5000.0;
    const float amp = 1e-5f;
    auto in = sine(fProbe, amp, 0.5, fs);
    auto o = render(in, {1.0f, 0.5f, 1.0f}, fs, 4, TsModel::CLIP_ADAA, /*ideal*/ true);
    const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
    const double gIn = goertzelAmp(in, n - win, win, fProbe, fs);
    const double gOut = goertzelAmp(o, n - win, win, fProbe, fs);
    const double plateauDb = toDb(gOut / gIn);
    assert(std::fabs(plateauDb - plateauDbAnalytic) < 1.5 &&
           "TS max-DRIVE plateau not ~ 1 + 500k/4.7k (+40.6 dB)");
    // And distinctly BELOW the SD-1's +46.6 dB plateau (the 500k-vs-1M pot).
    assert(plateauDb < 44.0 && "TS plateau too high — reads like the SD-1's 1M pot");
    std::printf(
        "  [ok] max-gain plateau @ %.0f Hz: %.2f dB (analytic 1+500k/4.7k = %.2f dB; "
        "SD-1 is +46.6 dB), K_max=%.1f\n",
        fs, plateauDb, plateauDbAnalytic, Kmax);
}

// --- Test 4a: min DRIVE still grinds lightly at a hot input. ------------------
void testMinDriveClips() {
    const double fs = 48000.0, f0 = 220.0;
    auto o = render(sine(f0, 0.30f, 0.5, fs), {0.0f, 0.5f, 1.0f}, fs);  // DRIVE 0
    const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.4 * fs));
    const double f1 = goertzelAmp(o, n - win, win, f0, fs);
    double e = 0.0;
    for (int hh : {2, 3, 4, 5}) {
        const double ah = goertzelAmp(o, n - win, win, f0 * hh, fs);
        e += ah * ah;
    }
    const double thd = std::sqrt(e) / (f1 + 1e-12);
    assert(thd > 0.005 && "min DRIVE produced no clipping at a hot input (too clean)");
    assert(thd < 0.35 && "min DRIVE clipping is not LIGHT (too dirty)");
    std::printf("  [ok] min-drive clip: THD %.1f%% at a 0.30 V input (light, not clean)\n",
                thd * 100);
}

// --- Test 4b: THD rises monotonically with input (soft, progressive). --------
void testSoftKnee() {
    const double fs = 48000.0, f0 = 220.0;
    auto thd = [&](float a) {
        auto o = render(sine(f0, a, 0.5, fs), {0.5f, 0.5f, 1.0f}, fs);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.4 * fs));
        const double f1 = goertzelAmp(o, n - win, win, f0, fs);
        double e = 0.0;
        for (int hh : {2, 3, 4, 5, 6}) {
            const double ah = goertzelAmp(o, n - win, win, f0 * hh, fs);
            e += ah * ah;
        }
        return std::sqrt(e) / (f1 + 1e-12);
    };
    const double t05 = thd(0.05f), t15 = thd(0.15f), t30 = thd(0.30f);
    assert(t05 < t15 && t15 < t30 && "TS THD did not rise monotonically with input");
    std::printf("  [ok] soft knee: THD 0.05/0.15/0.30 = %.1f%%/%.1f%%/%.1f%% (monotone)\n",
                t05 * 100, t15 * 100, t30 * 100);
}

// --- Test 5: aliasing. Shipped ADAA 4x at max DRIVE below the M2 bar. ---------
void testAliasing(double fs) {
    using clipper::measure::measureAliasing;
    const double f0 = 4186.0;  // C8
    auto in = sine(f0, 0.3f, 1.0, fs);
    const double w4 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 4), fs, f0).worstAliasDb;
    assert(w4 < -60.0 &&
           "TS 4x ADAA worst-alias at max DRIVE not >= 60 dB below fundamental");
    const double wAdaa1 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 1, TsModel::CLIP_ADAA), fs, f0)
            .worstAliasDb;
    const double wNaive1 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 1, TsModel::CLIP_NAIVE), fs, f0)
            .worstAliasDb;
    assert(wAdaa1 < wNaive1 - 3.0 && "ADAA did not improve on naive at 1x");
    std::printf(
        "  [ok] aliasing @ %.0f Hz base: 4x ADAA worst-alias %.1f dB (bar -60); "
        "1x ADAA %.1f vs naive %.1f dB\n",
        fs, w4, wAdaa1, wNaive1);
}

// --- Test 6: the LEVEL pot's law. ---------------------------------------------
//
// REWRITTEN 2026-08-10 (docs §68). This was `testLevelLinearity` and it asserted
// the identity map §66.3 registered as a lineup-wide approximation. Rewritten,
// not deleted.
//
// The TS is the pedal where the LOADING matters, and that is what separates this
// test from the RAT's. Sourced: `ts808_tube_screamer.asc` annotates
// "R15 is level pot (1-100k, log)" (cross-checked independently by the
// IceScreamer clone's build doc, "100K logarithmic"), and the same file's
// WIRE/FLAG records put R19 = 510 k on the wiper plus C8 into (R18 = 510 k ∥ the
// 2SC4081 follower's ~2 M base) — a 226 k wiper load. A loaded pot does NOT
// deliver its bare taper: 11.391 % at half rotation against the RAT's 11.920 %.
void testOutputPotLaw() {
    const double fs = 48000.0;
    auto in = sine(220.0, 0.3f, 0.5, fs);
    auto rmsAt = [&](float lvl) { return tailRms(render(in, {0.6f, 0.5f, lvl}, fs), fs); };
    const double r100 = rmsAt(1.0f), r75 = rmsAt(0.75f);
    const double r50 = rmsAt(0.50f), r25 = rmsAt(0.25f);

    // BAR 1 — inside §58's documented 10-20 % audio-taper band. A linear pot
    // measures 50.0 %, which is what this model did until this slice.
    const double halfFraction = r50 / r100;
    assert(halfFraction >= 0.10 && halfFraction <= 0.20 &&
           "LEVEL at half rotation is outside the 10-20 % audio-taper band the "
           "netlist's 'R15 is level pot (1-100k, log)' implies");

    // BAR 2 — the SHAPE: even dB per quarter turn (9.81 / 9.06 / 11.13 dB ->
    // 1.229) where the linear map it replaced gives 2.50 / 3.52 / 6.02 -> 2.409.
    const double s1 = std::fabs(toDb(r75 / r100));
    const double s2 = std::fabs(toDb(r50 / r75));
    const double s3 = std::fabs(toDb(r25 / r50));
    const double stepRatio =
        std::max(s1, std::max(s2, s3)) / std::min(s1, std::min(s2, s3));
    assert(stepRatio < 1.60 &&
           "LEVEL does not spend its dB evenly across the travel — that is a "
           "linear pot's signature, not an audio taper's");

    // BAR 3 — THE LOAD IS MODELLED, and this is the clause that separates a
    // DERIVED law from "the house audioTaper applied a fifth time". The output
    // buffer's base network shunts the pot's bottom leg, so this pedal must sit
    // measurably BELOW the bare taper — and by the derived amount, not just
    // "less". Bare 11.920 %, loaded 11.391 %; the window brackets it on BOTH
    // sides so neither dropping the load nor inventing a bigger one passes.
    const double bare = clipper::dsp::pot::audioTaperWiper(0.5);
    assert(halfFraction < bare - 0.003 &&
           "LEVEL at half rotation is the BARE taper — the output buffer's load "
           "on the wiper is not being modelled");
    assert(halfFraction > bare - 0.010 &&
           "LEVEL at half rotation is further below the bare taper than a 226 k "
           "load on a 100 k pot can explain");

    // BAR 4 — the top of the knob does not move (§51's rule). The loading leaves
    // law(1) exactly 1 because the top leg is zero there, so LEVEL 1.0 renders
    // bit-identically to the pre-slice build (hashes in docs §68.4).
    assert(clipper::dsp::pot::tsLevel(1.0) == 1.0 &&
           "LEVEL 1.0 is no longer exactly unity — the taper has become a trim");
    assert(clipper::dsp::pot::tsLevel(0.0) == 0.0 && "LEVEL 0 is not silence");

    std::printf("  [ok] LEVEL pot law (100k LOG, wiper loaded 226k by the output "
                "buffer): half rotation %.3f %% vs the BARE taper's %.3f %% "
                "(band 10-20), steps %.2f/%.2f/%.2f dB ratio %.3f (bar 1.60)\n",
                100.0 * halfFraction, 100.0 * bare, s1, s2, s3, stepRatio);
}

// --- Test 7: hygiene (finite, silence->silence, deterministic). --------------
void testHygiene() {
    const double fs = 48000.0;
    auto in = sine(330.0, 0.4f, 0.2, fs);
    for (float dr = 0.0f; dr <= 1.0f; dr += 0.25f)
        for (float tn = 0.0f; tn <= 1.0f; tn += 0.25f)
            for (float lv = 0.0f; lv <= 1.0f; lv += 0.5f) {
                auto o = render(in, {dr, tn, lv}, fs);
                for (float v : o) assert(std::isfinite(v) && "non-finite output");
            }
    {  // silence -> silence (the model does not self-oscillate)
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {0.9f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-6 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }
    {  // DC offset ON SIGNAL — the property that matters (audit finding 16).
        //
        // 2026-07-25 (test/assert-real-properties): the silence check above was the only DC
        // assertion here and it is trivially true. The TS clips SYMMETRICALLY, so it does
        // not rectify — which means a CLEAN-input DC test on this pedal cannot fail even if
        // the output coupling cap is deleted outright (verified by doing exactly that). So
        // the second stimulus, a +0.1 V DC offset ON THE INPUT, is the one that makes
        // `dcBlockHz = 12.0` load-bearing here. See core/tests/support/DcOffset.h.
        const auto tone = sine(220.0, 0.2f, 1.0, fs);
        for (float drive : {0.5f, 1.0f}) {
            for (float dcIn : {0.0f, clipper::test::kInputDcOffset}) {
                auto stim = tone;
                for (float& s : stim) s += dcIn;
                const auto o = render(stim, {drive, 0.5f, 1.0f}, fs);
                const auto d = clipper::test::measureDcOnSignal(o);
                std::printf("  [ok] DC on SIGNAL (drive %.2f, input DC %+.2f V): mean %+.6f V, "
                            "peak %.4f V -> %.4f %% of peak (bar %.1f %%)\n",
                            drive, dcIn, d.mean, d.peak, 100.0 * d.fraction,
                            100.0 * clipper::test::kDcFractionBar);
                assert(d.peak > 0.01 && "no signal to measure DC against");
                assert(d.fraction < clipper::test::kDcFractionBar &&
                       "DC offset ON SIGNAL exceeds 1 % of peak — the output coupling cap is "
                       "missing or mistuned, or the clipper is rectifying");
            }
        }
    }
    {  // determinism
        auto a = render(in, {0.6f, 0.4f, 0.8f}, fs);
        auto b = render(in, {0.6f, 0.4f, 0.8f}, fs);
        assert(a.size() == b.size());
        assert(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0 &&
               "processing is not deterministic");
    }
    std::printf("  [ok] hygiene: finite grid, silence->silence, deterministic\n");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("Running clipper::dsp::TsModel tests (v1.1 — TS808 Screamer)...\n");
    testMidHumpCorner(44100.0);
    testMidHumpCorner(96000.0);
    testSymmetry(44100.0);
    testSymmetry(96000.0);
    testMaxGainPlateau(44100.0);
    testMaxGainPlateau(48000.0);
    testMaxGainPlateau(96000.0);
    testMinDriveClips();
    testSoftKnee();
    testAliasing(44100.0);
    testAliasing(96000.0);
    testOutputPotLaw();
    testHygiene();
    std::printf("All TsModel tests passed.\n");
    return 0;
}
