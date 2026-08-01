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
// SHARED family trait (same Zg leg as the SD-1). Small signal, moderate DRIVE.
// This measures STAGE 1 on its own, so the TONE NETWORK is divided out (docs §65:
// since 2026-08-01 TONE=0.5 is NOT transparent — it is the real network, whose
// 723 Hz low-pass is the other half of the family's mid hump; the old test's
// "5 kHz plateau" reference no longer exists). The tone half of the reference is
// validated on its own by testToneNetwork, against Yeh & Abel's published H(s);
// what THIS test pins is the 720.5 Hz corner of the clipping stage's Zg leg.
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
    const auto toneCfg = TsModel::toneConfig();
    auto toneDb = [&](double f) {
        return toDb(clipper::dsp::OverdriveToneStack::analyticMag(toneCfg, 0.5,
                                                                  kTwoPi * f));
    };
    auto measDbRel = [&](double f) {
        auto o = render(sine(f, amp, 0.5, fs), {(float)driveKnob, 0.5f, 1.0f}, fs);
        auto oRef = render(sine(fRef, amp, 0.5, fs), {(float)driveKnob, 0.5f, 1.0f}, fs);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
        return (toDb(goertzelAmp(o, n - win, win, f, fs)) - toneDb(f)) -
               (toDb(goertzelAmp(oRef, n - win, win, fRef, fs)) - toneDb(fRef));
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

// --- Test 2b: THE TONE NETWORK, against an OUTSIDE reference (docs §65). -----
// Yeh & Abel's DAFx-07 distortion-modelling paper publishes a closed-form H(s)
// for exactly this tone stage. It is transcribed here from a THIRD party's
// implementation of it (JamesStubbsEng/TS-808-Ultra, `ToneStage::calcCoefs`) —
// different author, different derivation, different variable names — and the
// model's own netlist-derived H(s) has to agree with it. That is the strongest
// absolute reference available for this network, and it is the bar that says the
// TOPOLOGY is right rather than merely self-consistent.
//
// ONE correction is applied to that transcription and is a finding, not a
// convenience: its `X` term reads `(Rr/(Rl+Rr)) / ((Rz+par) * Y)`, which is
// dimensionally Ohm^-3 where the other two terms of the same sum (wp, wz) are
// rad/s. Deriving the same coefficient from the netlist gives
// `(Rr/(Rl+Rr)) / (Cs*(Rz+par))` — i.e. the reference substitutes `Y` where `Cs`
// belongs. With that one fix the two forms agree EXACTLY (worst 0.000000 dB over
// the whole knob), which is itself the evidence that the fix is the right one:
// wp, wz, wp*wz, alpha and alpha*W*wz all reproduce term for term untouched.
double yehMag(double Rl, double Rpot, double w) {
    const double Cs = 0.22e-6, Rs = 1e3, Ri = 10e3, Cz = 0.22e-6, Rz = 220.0,
                 Rf = 1e3;  // the paper's TS values
    const double wp = 1.0 / (Cs * Rs * Ri / (Rs + Ri));
    const double Rr = Rpot - Rl;
    const double par = Rl * Rr / (Rl + Rr);
    const double wz = 1.0 / (Cz * (Rz + par));
    const double Y = (Rl + Rr) * (Rz + par);
    const double X = (Rr / (Rl + Rr)) / (Cs * (Rz + par));
    const double W = Y / ((Rl * Rf) + Y);
    const double alpha = (Rl * Rf + Y) / (Y * Rs * Cs);
    // H(s) = (alpha*s + alpha*W*wz) / (s^2 + (wp+wz+X)*s + wp*wz)
    const double nr = alpha * W * wz, ni = alpha * w;
    const double dr = wp * wz - w * w, di = (wp + wz + X) * w;
    return std::sqrt(nr * nr + ni * ni) / std::sqrt(dr * dr + di * di);
}

void testToneNetwork() {
    using clipper::dsp::OverdriveToneStack;
    const double fs = 48000.0;
    const auto cfg = TsModel::toneConfig();

    auto stackDb = [&](double p, double f, double rate = 48000.0) {
        OverdriveToneStack t;
        t.prepare(cfg, rate);
        t.setPot(p);
        const int n = static_cast<int>(rate * 0.4);
        std::vector<float> out(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            out[static_cast<size_t>(i)] =
                t.processSample(static_cast<float>(0.1 * std::sin(kTwoPi * f * i / rate)));
        return toDb(goertzelAmp(out, static_cast<size_t>(rate * 0.2),
                                static_cast<size_t>(rate * 0.15), f, rate) /
                    0.1);
    };
    auto pedalDb = [&](double f, double tone) {
        auto o = render(sine(f, 1e-3f, 0.6, fs), {0.5f, (float)tone, 1.0f}, fs);
        return toDb(goertzelAmp(o, static_cast<size_t>(fs * 0.3),
                                static_cast<size_t>(fs * 0.25), f, fs) /
                    1e-3);
    };

    // BAR 1 — THE OUTSIDE REFERENCE. The netlist's H(s) must be Yeh & Abel's.
    // The reference's pot value is the paper's own 20 kOhm LITERAL, not cfg.rPot:
    // this bar is the one thing in the suite that goes red if ANY of the eight
    // transcribed TS tone values is edited, so it must not track the config.
    constexpr double kYehPot = 20.0e3;
    double worstYeh = 0.0;
    for (double p : {0.0, 0.05, 0.25, 0.5, 0.75, 0.95, 1.0}) {
        for (int i = 0; i < 200; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 199.0);
            worstYeh = std::max(
                worstYeh,
                std::fabs(toDb(OverdriveToneStack::analyticMag(cfg, p, kTwoPi * f)) -
                          toDb(yehMag(kYehPot * p, kYehPot, kTwoPi * f))));
        }
    }
    assert(worstYeh < 0.05 &&
           "the TS tone network does not reproduce Yeh & Abel's published H(s)");

    // BAR 2 — THE HEADLINE: a HUMP, not a SHELF. Before docs §65 this model
    // peaked at 6467 Hz with 3 k / 6 k / 12 k at -0.15 / -0.00 / -0.11 dB re the
    // peak — a plateau. Measured now: peak 776.8 Hz, 6 kHz -11.75 dB re peak.
    double peakF = 0.0, peakDb = -1e9;
    for (int i = 0; i < 25; ++i) {
        const double f = 200.0 * std::pow(30.0, i / 24.0);
        const double v = pedalDb(f, 0.5);
        if (v > peakDb) { peakDb = v; peakF = f; }
    }
    const double at6k = pedalDb(6000.0, 0.5);
    assert(peakF > 500.0 && peakF < 1300.0 &&
           "TS small-signal peak is not in the upper mids (500..1300 Hz)");
    assert(peakDb - at6k > 8.0 &&
           "TS 6 kHz is not >= 8 dB below the upper-mid peak — the response is a "
           "SHELF, i.e. the 723 Hz low-pass is missing");

    // BAR 3 — TONE is a TREBLE control (authority at 3 kHz, ~none at 100 Hz).
    const double t3lo = pedalDb(3000.0, 0.0), t3hi = pedalDb(3000.0, 1.0);
    const double b1lo = pedalDb(100.0, 0.0), b1hi = pedalDb(100.0, 1.0);
    assert(t3hi - t3lo > 12.0 && "TS TONE has < 12 dB of authority at 3 kHz");
    assert(std::fabs(b1hi - b1lo) < 1.5 && "TS TONE moves 100 Hz by > 1.5 dB");

    // BAR 4 — THE PEDALS ARE NOT THE SAME PEDAL. They share one engine, so the
    // easiest way to get this wrong is to give them one tone stage. The TS's
    // input leg is 1k against a 10k bias return and the SD-1's is 10k against
    // 10k, so the SD-1's stage costs 5.2 dB MORE. Literal bar, not a recompute.
    const double sdDcDb = [&] {
        OverdriveToneStack t;
        t.prepare(SdModel::toneConfig(), fs);
        t.setPot(0.5);
        const int n = static_cast<int>(fs * 0.4);
        std::vector<float> out(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            out[static_cast<size_t>(i)] =
                t.processSample(static_cast<float>(0.1 * std::sin(kTwoPi * 30.0 * i / fs)));
        return toDb(goertzelAmp(out, static_cast<size_t>(fs * 0.2),
                                static_cast<size_t>(fs * 0.15), 30.0, fs) /
                    0.1);
    }();
    const double tsDcDb = stackDb(0.5, 30.0);
    assert(std::fabs(tsDcDb - (-0.828)) < 0.15 &&
           "TS tone stage DC insertion loss is not the 1k/10k divider (-0.83 dB)");
    assert(tsDcDb - sdDcDb > 4.0 &&
           "the TS and SD-1 tone stages have stopped differing — one engine, TWO "
           "configs (docs §65)");

    // BAR 5 — discretization (mapping check). Worst measured 0.96 dB at 16 kHz /
    // 44.1 kHz, 0.53 dB below 10 kHz.
    double worstDisc = 0.0;
    for (double rate : {44100.0, 96000.0})
        for (double p : {0.0, 0.25, 0.5, 0.75, 1.0})
            for (double f : {30.0, 110.0, 440.0, 880.0, 3000.0, 10000.0, 16000.0})
                worstDisc = std::max(
                    worstDisc,
                    std::fabs(stackDb(p, f, rate) -
                              toDb(OverdriveToneStack::analyticMag(cfg, p, kTwoPi * f))));
    assert(worstDisc < 1.5 && "tone network discretization drifts > 1.5 dB from H(s)");

    // BAR 6 — ADR 006: every state rests at EXACTLY zero.
    {
        OverdriveToneStack t;
        t.prepare(cfg, fs);
        t.setPot(0.5);
        for (int i = 0; i < static_cast<int>(fs * 0.2); ++i)
            t.processSample(static_cast<float>(0.3 * std::sin(kTwoPi * 220.0 * i / fs)));
        for (int i = 0; i < static_cast<int>(fs * 4.0); ++i) t.processSample(0.0f);
        assert(t.maxAbsRestingState() == 0.0 &&
               "TS tone network state did not reach EXACT zero after a silent tail");
    }

    // BAR 7 (transcription guard, deliberately last). The famous 723 Hz corner is
    // 1/(2*pi*R5*C4) and it is the LOW-PASS, not the mid-hump high-pass.
    assert(cfg.rIn == 1.0e3 && cfg.cIn == 220.0e-9 && cfg.rBias == 10.0e3);
    assert(cfg.rPot == 20.0e3 && cfg.rW == 220.0 && cfg.cW == 220.0e-9);
    assert(cfg.rFb == 1.0e3 && cfg.cFb == 0.0 &&
           "the TS has NO cap across its 1k tone feedback resistor (the SD-1 has "
           "0.01 uF) — do not 'unify' them");
    const double lpHz = 1.0 / (kTwoPi * cfg.rIn * cfg.cIn);
    assert(std::fabs(lpHz - 723.4) < 1.0 && "the TS tone low-pass is not at 723 Hz");

    std::printf(
        "  [ok] tone network: vs Yeh & Abel DAFx-07 H(s) worst %.6f dB; low-pass "
        "%.1f Hz; DC loss %.2f dB (TS) vs %.2f dB (SD-1) = %.2f dB apart; peak "
        "%.0f Hz at %+.2f dB, 6 kHz %.2f dB below it (was a FLAT shelf to 12 kHz); "
        "TONE 3 kHz span %.2f dB vs 100 Hz %.2f dB; discretization worst %.2f dB\n",
        worstYeh, lpHz, tsDcDb, sdDcDb, tsDcDb - sdDcDb, peakF, peakDb, peakDb - at6k,
        t3hi - t3lo, std::fabs(b1hi - b1lo), worstDisc);
}

// --- Test 3: max-DRIVE plateau ~= 40.6 dB analytic (1 + 500k/4.7k). ----------
// Small-signal gain well above the mid-hump corner (5 kHz, below the ~28 kHz
// op-amp corner) IS the plateau 1 + K_max. Absolute out/in gain, LEVEL unity, and
// the TONE NETWORK divided out (docs §65 — at 5 kHz it is no longer 0 dB).
// Asserts the 500k drive-pot derivation and that it is NOT the SD-1's 46.6 dB.
// (Op-amp bypassed so the plateau is the clean analytic 1+K.)
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
    const double toneDb = toDb(clipper::dsp::OverdriveToneStack::analyticMag(
        TsModel::toneConfig(), 0.5, kTwoPi * fProbe));
    const double plateauDb = toDb(gOut / gIn) - toneDb;
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
    testToneNetwork();
    testMaxGainPlateau(44100.0);
    testMaxGainPlateau(48000.0);
    testMaxGainPlateau(96000.0);
    testMinDriveClips();
    testSoftKnee();
    testAliasing(44100.0);
    testAliasing(96000.0);
    testLevelLinearity();
    testHygiene();
    std::printf("All TsModel tests passed.\n");
    return 0;
}
