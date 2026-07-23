// Plain-assert tests for clipper::dsp::SdModel (M8 — Boss SD-1 Super Overdrive).
// No framework: int main + <cassert>. Frequency content via a hand-rolled
// Goertzel. Each assert fails if the corresponding behaviour is broken.
//
// The four headline measurements (see docs/DEVELOPMENT.md §M8):
//   1. Mid-hump corner ~= 720 Hz, within +/-1.5 dB of the analytic 1 + K*HP720
//      shelf, at 44.1k and 96k.
//   2. Asymmetry: 220 Hz -> a real 2nd harmonic (even), vs a forced-symmetric
//      reference where it is ~absent.
//   3. Soft knee: THD rises gradually with input; the compression knee is
//      quantitatively WIDER than the RAT's hard shunt clipper.
//   4. 4558 op-amp closed-loop corner ~= GBW/A ~= 14 kHz at max DRIVE; the
//      shipped 4x ADAA path aliases below the M2 bar at max drive.

#include "clipper/dsp/SdModel.h"

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/RatModel.h"
#include "measure/AliasMetric.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::SdModel;
using clipper::dsp::RatModel;

// Mid-hump analytic constants — KEEP IN SYNC with SdModel.cpp.
constexpr double kMidHumpHz = 720.5;
constexpr double kDriveMinDb = 12.0;
constexpr double kDriveMaxDb = 46.6;

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

// Render a mono buffer through a fresh SD-1 model, with measurement options.
std::vector<float> render(const std::vector<float>& in, Params p, double fs,
                          int os = 4, int clipMode = SdModel::CLIP_ADAA,
                          bool idealOpAmp = false, bool symmetric = false) {
    SdModel m;
    m.prepare(fs, 128);
    m.setOversampling(os);
    m.setClipMode(clipMode);
    m.setIdealOpAmp(idealOpAmp);
    m.setSymmetric(symmetric);
    m.setParameter(SdModel::PARAM_DRIVE, p.drive);
    m.setParameter(SdModel::PARAM_TONE, p.tone);
    m.setParameter(SdModel::PARAM_LEVEL, p.level);
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
// Small signal (diodes ~linear), moderate DRIVE (op-amp corner far above audio),
// TONE=0.5 (transparent). Measure dB relative to a 5 kHz reference (the plateau)
// and compare to the analytic non-inverting shelf. Pins the 720 Hz corner.
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
        assert(dev < 1.5 && "mid-hump shelf deviates >1.5 dB from analytic 1+K*HP720");
    }
    // The corner sits ~3 dB below the plateau; bass is well below it (unity-ward).
    const double atCorner = measDbRel(kMidHumpHz);       // ~ -3 dB rel 5 kHz
    const double atBass = measDbRel(82.4);               // strongly shelved down
    assert(atCorner < -1.5 && atCorner > -4.5 && "720 Hz not ~3 dB below plateau");
    assert(atBass < atCorner - 3.0 && "bass not shelved below the corner");
    std::printf(
        "  [ok] mid-hump @ %.0f Hz: worst dev %.2f dB; 720Hz=%.2f dB, 82Hz=%.2f dB "
        "rel plateau (K=%.1f)\n",
        fs, worst, atCorner, atBass, K);
}

// --- Test 2: asymmetry -> a real 2nd harmonic (even); symmetric ~absent. -----
void testAsymmetry(double fs) {
    const double f0 = 220.0;
    const float amp = 0.30f;  // hot input, moderate drive -> clear soft clip
    auto h = [&](bool sym, double mult) {
        auto o = render(sine(f0, amp, 1.0, fs), {0.5f, 0.5f, 1.0f}, fs, 4,
                        SdModel::CLIP_ADAA, false, sym);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
        return goertzelAmp(o, n - win, win, f0 * mult, fs);
    };
    const double f1 = toDb(h(false, 1.0));
    const double h2Asym = toDb(h(false, 2.0));
    const double h2Sym = toDb(h(true, 2.0));
    const double h3Asym = toDb(h(false, 3.0));
    // The 2nd harmonic is present (even harmonic — the SD-1 warmth) ...
    assert(f1 - h2Asym < 40.0 && "asymmetric 2nd harmonic not within 40 dB of fundamental");
    // ... and stands well above the symmetric reference (>= 20 dB) where the odd
    // curve produces essentially no 2nd harmonic.
    assert(h2Asym - h2Sym > 20.0 && "2nd harmonic not driven by asymmetry (sym ~= asym)");
    // Odd harmonic present in BOTH (it's a clipper): sanity that we are clipping.
    assert(f1 - h3Asym < 45.0 && "no 3rd harmonic — not clipping?");
    std::printf(
        "  [ok] asymmetry @ %.0f Hz: 2nd harmonic %.1f dBc (asym) vs %.1f dBc (sym), "
        "delta %.1f dB\n",
        fs, h2Asym - f1, h2Sym - f1, h2Asym - h2Sym);
}

// --- Test 3: soft knee. THD rises gradually; knee WIDER than the RAT. ---------
// Compression metric c(a) = H1(a) / (a * G0), G0 = small-signal gain. Knee width
// = a(c=0.5) / a(c=0.9): the input ratio over which compression deepens from
// -0.9 to -6 dB. A soft clipper spreads this over a wide ratio; the RAT's hard
// shunt clamp collapses it. Scale-free, so the two pedals compare fairly despite
// very different gains.
void testSoftKnee() {
    const double fs = 48000.0, f0 = 220.0;
    // Log-swept input amplitudes.
    std::vector<double> amps;
    for (double a = 1e-4; a <= 1.0 + 1e-9; a *= std::pow(10.0, 0.1)) amps.push_back(a);

    auto sweep = [&](auto renderOne) {
        std::vector<double> c(amps.size());
        double G0 = 0.0;
        for (size_t i = 0; i < amps.size(); ++i) {
            auto o = renderOne(static_cast<float>(amps[i]));
            const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.4 * fs));
            const double h1 = goertzelAmp(o, n - win, win, f0, fs);
            if (i == 0) G0 = h1 / amps[0];
            c[i] = h1 / (amps[i] * G0);
        }
        // Interpolate the input amplitude at which c first crosses a threshold.
        auto crossing = [&](double thr) {
            for (size_t i = 1; i < c.size(); ++i) {
                if (c[i] <= thr) {
                    const double t = (c[i - 1] - thr) / (c[i - 1] - c[i] + 1e-18);
                    const double la = std::log(amps[i - 1]) +
                                      t * (std::log(amps[i]) - std::log(amps[i - 1]));
                    return std::exp(la);
                }
            }
            return amps.back();
        };
        const double a90 = crossing(0.9), a50 = crossing(0.5);
        return a50 / a90;  // knee width (input ratio)
    };

    const double kneeSd = sweep([&](float a) {
        return render(sine(f0, a, 0.5, fs), {0.5f, 0.5f, 1.0f}, fs);
    });
    const double kneeRat = sweep([&](float a) {
        RatModel m; m.prepare(fs, 128); m.setOversampling(4);
        m.setParameter(RatModel::PARAM_DISTORTION, 0.5f);
        m.setParameter(RatModel::PARAM_FILTER, 0.0f);
        m.setParameter(RatModel::PARAM_LEVEL, 1.0f);
        auto in = sine(f0, a, 0.5, fs);
        std::vector<float> o(in.size(), 0.0f);
        m.process(in.data(), o.data(), static_cast<int>(in.size()));
        return o;
    });

    // THD vs input rises gradually (monotone) for the SD-1.
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
    assert(t05 < t15 && t15 < t30 && "SD-1 THD did not rise monotonically with input");

    assert(kneeSd > kneeRat * 1.2 && "SD-1 knee not measurably softer (wider) than the RAT");
    std::printf(
        "  [ok] soft knee: knee-width SD-1 = %.2fx input vs RAT = %.2fx (%.2fx softer); "
        "THD 0.05/0.15/0.30 = %.1f%%/%.1f%%/%.1f%%\n",
        kneeSd, kneeRat, kneeSd / kneeRat, t05 * 100, t15 * 100, t30 * 100);
}

// --- Test 4a: min DRIVE still clips lightly at a hot input. -------------------
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
    assert(thd > 0.01 && "min DRIVE produced no clipping at a hot input (too clean)");
    assert(thd < 0.35 && "min DRIVE clipping is not LIGHT (too dirty)");
    std::printf("  [ok] min-drive clip: THD %.1f%% at a 0.30 V input (light, not clean)\n",
                thd * 100);
}

// --- Test 4b: 4558 closed-loop corner ~= GBW/A ~= 14 kHz at max DRIVE. --------
// Isolate the op-amp one-pole by ratioing the small-signal response REAL vs
// IDEAL (op-amp bypassed) at the same DRIVE: the mid-hump shelf, tone and DC
// block all cancel, leaving the op-amp low-pass. Extract fc from the rolloff.
void testOpAmpCorner(double fs) {
    const double GBW = 3.0e6;
    const double Kmax = driveKnobToK(1.0);
    const double fcAnalytic = GBW / (Kmax + 1.0);  // ~14 kHz
    const float amp = 1e-5f;
    auto respDb = [&](bool ideal, double f) {
        auto o = render(sine(f, amp, 0.5, fs), {1.0f, 0.5f, 1.0f}, fs, 4,
                        SdModel::CLIP_ADAA, ideal);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
        return toDb(goertzelAmp(o, n - win, win, f, fs));
    };
    auto extractFc = [&](double f) {
        const double att = respDb(true, f) - respDb(false, f);  // positive dB down
        const double r = std::pow(10.0, att / 10.0) - 1.0;      // (f/fc)^2
        return f / std::sqrt(r > 1e-9 ? r : 1e-9);
    };
    const double fc = 0.5 * (extractFc(10000.0) + extractFc(15000.0));
    assert(std::fabs(fc / fcAnalytic - 1.0) < 0.20 &&
           "4558 closed-loop corner at max DRIVE not ~ GBW/A (~14 kHz)");
    std::printf("  [ok] op-amp corner @ %.0f Hz base: fc=%.0f Hz (GBW/A=%.0f, A=%.0f)\n",
                fs, fc, fcAnalytic, Kmax + 1.0);
}

// --- Test 5: aliasing. Shipped ADAA 4x at max DRIVE below the M2 bar; ADAA
//     beats naive. ---------------------------------------------------------
void testAliasing(double fs) {
    using clipper::measure::measureAliasing;
    const double f0 = 4186.0;  // C8: harmonics fold to predictable inharmonic bins
    auto in = sine(f0, 0.3f, 1.0, fs);
    // Shipped path: ADAA, 4x, max DRIVE.
    const double w4 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 4), fs, f0).worstAliasDb;
    assert(w4 < -60.0 &&
           "SD-1 4x ADAA worst-alias at max DRIVE not >= 60 dB below fundamental");
    // ADAA vs naive at 1x (same curve): ADAA must reduce the worst alias.
    const double wAdaa1 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 1, SdModel::CLIP_ADAA), fs, f0)
            .worstAliasDb;
    const double wNaive1 =
        measureAliasing(render(in, {1.0f, 0.5f, 0.9f}, fs, 1, SdModel::CLIP_NAIVE), fs, f0)
            .worstAliasDb;
    assert(wAdaa1 < wNaive1 - 3.0 && "ADAA did not improve on naive at 1x");
    std::printf(
        "  [ok] aliasing @ %.0f Hz base: 4x ADAA worst-alias %.1f dB (bar -60); "
        "1x ADAA %.1f vs naive %.1f dB\n",
        fs, w4, wAdaa1, wNaive1);
}

// --- Test 6: level linearity. -------------------------------------------------
void testLevelLinearity() {
    const double fs = 48000.0;
    auto in = sine(220.0, 0.3f, 0.5, fs);
    auto rmsAt = [&](float lvl) { return tailRms(render(in, {0.6f, 0.5f, lvl}, fs), fs); };
    const double r25 = rmsAt(0.25f), r50 = rmsAt(0.50f), r100 = rmsAt(1.0f);
    assert(std::fabs(r50 / r25 - 2.0) < 0.05 && "RMS not linear 0.25 -> 0.5");
    assert(std::fabs(r100 / r50 - 2.0) < 0.05 && "RMS not linear 0.5 -> 1.0");
    std::printf("  [ok] level linearity: r50/r25=%.3f r100/r50=%.3f\n",
                r50 / r25, r100 / r50);
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
    {  // silence -> silence, no DC pumping
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {0.9f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-6 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
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
    std::printf("Running clipper::dsp::SdModel tests (M8 — SD-1)...\n");
    testMidHumpCorner(44100.0);
    testMidHumpCorner(96000.0);
    testAsymmetry(44100.0);
    testAsymmetry(96000.0);
    testSoftKnee();
    testMinDriveClips();
    testOpAmpCorner(44100.0);
    testOpAmpCorner(96000.0);
    testAliasing(44100.0);
    testAliasing(96000.0);
    testLevelLinearity();
    testHygiene();
    std::printf("All SdModel tests passed.\n");
    return 0;
}
