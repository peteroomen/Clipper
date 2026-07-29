// Plain-assert tests for the M10.1 Fender blackface "Twin-style" amp — the CLEAN
// BENCHMARK. Covers the TwinPreamp (2× 12AX7 + pre-gain Fender TMB stack + volume/
// bright), the OptoTremolo (AB763 optical "vibrato"), the TwinPowerAmp (12AT7 LTP
// PI → 6L6GC quad → OT → flat NFB → light sag), and the composed TwinAmp
// (preamp → reverb → tremolo → power). No framework: int main + <cassert>. Every
// assert compares a MEASURED number against an ANALYTIC target derived IN THE TEST
// (load-line DC, the complex nodal Fender-stack H(jω), the NFB reduction
// 1/(1+β·A_real) with A_real measured open-loop, the LDR attack/release asymmetry,
// the sag depth) — so the suite pins the physics, not itself. Same discipline as
// the M9.1/9.2/9.3 suites.

#include "clipper/dsp/TwinAmp.h"
#include "clipper/dsp/TwinPreamp.h"
#include "clipper/dsp/TwinPowerAmp.h"
#include "clipper/dsp/OptoTremolo.h"
#include "clipper/dsp/ReverbModel.h"
#include "clipper/dsp/Jcm800PowerAmp.h"

#include "measure/AliasMetric.h"
#include "support/AssertsLive.h"
#include "support/LtpProbe.h"
#include "support/Xfail.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

// --- known-bad properties (2026-07-24 audit) --------------------------------------
// 2026-07-29 (finding 7, docs §42): `finding7-twin-pi-plate-fraction` and
// `finding7-twin-pi-standing-current` XPASSed once `LtpInverter::Config::tailRef` landed
// (Rtail 22k -> 10k to a −7 V reference) and were DELETED in the same slice — all three
// PI targets are hard assertions below. Measured after: Va 332.0/323.1 V (81.0 / 78.8 %
// of B+), 0.780/0.612 mA per triode, leg ratio 0.946; before: 94.3 % of B+, 0.232 mA,
// ratio 0.990 between two nearly-cut-off legs. This suite has NO XFAILs left.
// NOTE: the OptoTremolo's SPEED-dependent depth collapse / level sag (audit Medium/DSP)
// is pinned in the new core/tests/test_opto_tremolo.cpp, which also drives it through the
// composed TwinAmp — the path no render in this file exercises, because every Twin render
// here passes INTENSITY 0 or a fixed SPEED of 0.4.
using clipper::dsp::FenderToneStack;
using clipper::dsp::Jcm800PowerAmp;
using clipper::dsp::OptoTremolo;
using clipper::dsp::ReverbModel;
using clipper::dsp::TwinAmp;
using clipper::dsp::TwinPowerAmp;
using clipper::dsp::TwinPreamp;
using clipper::measure::measureAliasing;

// --- signal + spectrum helpers ----------------------------------------------
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
double toDb(double a) { return 20.0 * std::log10(a + 1e-12); }

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

double thd(const std::vector<float>& o, double f0, double fs) {
    const size_t n = o.size(), w = n / 2, st = n - w;
    const double f1 = goertzelAmp(o, st, w, f0, fs);
    double hh = 0.0;
    for (int k = 2; k <= 8; ++k) {
        const double a = goertzelAmp(o, st, w, k * f0, fs);
        hh += a * a;
    }
    return std::sqrt(hh) / (f1 + 1e-12);
}

// Analytic |H(jω)| of the FenderToneStack netlist, derived from the SAME netlist
// via a complex nodal solve (caps = jωC). Independent of the runtime discretization.
double fenderToneH(double f, double bass, double mid, double treble, double rs) {
    using cd = std::complex<double>;
    const double w = kTwoPi * f;
    const cd s(0.0, w);
    auto Zc = [&](double C) { return 1.0 / (s * C); };
    auto cl = [](double v) { return v < 1.0 ? 1.0 : v; };
    const double RT = FenderToneStack::kRT, RB = FenderToneStack::kRB,
                 RM = FenderToneStack::kRM, R1 = FenderToneStack::kRslope;
    const double Ct = FenderToneStack::kCt, Cb = FenderToneStack::kCb,
                 Cm = FenderToneStack::kCm;
    const int N = 5;  // IN,N2,N3,N4,OUT
    cd G[5][5] = {};
    auto stamp = [&](int a, int b, cd g) { G[a][a]+=g; G[b][b]+=g; G[a][b]-=g; G[b][a]-=g; };
    G[0][0] += 1.0 / rs;
    stamp(0, 1, 1.0 / Zc(Ct));
    stamp(0, 2, cd(1.0 / R1, 0));
    stamp(1, 4, cd(1.0 / cl((1 - treble) * RT), 0));
    stamp(4, 2, cd(1.0 / cl(treble * RT), 0));
    stamp(2, 3, cd(1.0 / cl(bass * RB), 0));
    stamp(2, 3, 1.0 / Zc(Cb));
    G[3][3] += cd(1.0 / cl(mid * RM), 0);
    G[3][3] += 1.0 / Zc(Cm);
    cd A[5][6];
    for (int i = 0; i < N; ++i) { for (int j = 0; j < N; ++j) A[i][j] = G[i][j]; A[i][5] = 0; }
    A[0][5] = cd(1.0 / rs, 0);  // source injects Vs=1 through 1/rs at IN
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r) if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
        for (int j = 0; j < 6; ++j) std::swap(A[p][j], A[c][j]);
        const cd d = A[c][c];
        for (int j = 0; j < 6; ++j) A[c][j] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const cd f2 = A[r][c];
            for (int j = 0; j < 6; ++j) A[r][j] -= f2 * A[c][j];
        }
    }
    return std::abs(A[4][5]);
}

// Steady-state output amplitude of the power amp at f for a grid-drive amplitude.
double powerAmpAt(TwinPowerAmp& pa, double f, float amp, double fs) {
    auto in = sine(f, amp, 0.4, fs);
    std::vector<float> out(in.size(), 0.0f);
    pa.process(in.data(), out.data(), static_cast<int>(in.size()));
    const size_t n = out.size(), win = n / 2, start = n - win;
    return goertzelAmp(out, start, win, f, fs);
}

// ---------------------------------------------------------------------------
// Test 1 — DC operating points vs load-line analysis: V1/V2 self-bias, the PI,
// and the 6L6 quiescent (Koren fixed point). ±5 % triode, ±10 % pentode.
// ---------------------------------------------------------------------------
void testOperatingPoints(double fs) {
    TwinPreamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(4);
    // V1/V2: self-bias load line B+ = Va + Ra·Iq, Vk = Iq·Rk (Ra 100k, Rk 1.5k, B+ 410).
    for (int st : {TwinPreamp::V1, TwinPreamp::V2}) {
        const auto& cfg = pre.stageConfig(st);
        const double Va = pre.stageQuiescentPlate(st);
        const double Vk = pre.stageQuiescentCathode(st);
        const double Iq = pre.stageQuiescentCurrent(st);
        const double bplusResidual = std::fabs(cfg.bPlus - (Va + cfg.Ra * Iq));
        assert(bplusResidual < 0.05 * cfg.bPlus && "V1/V2 off the B+ = Va + Ra·Iq load line");
        assert(std::fabs(Vk - Iq * cfg.Rk) < 0.05 * std::fabs(Vk) + 0.02 &&
               "V1/V2 cathode voltage off Iq·Rk self-bias");
        assert(Iq > 0.5e-3 && Iq < 2.0e-3 && "V1/V2 idle current implausible for 12AX7");
        assert(Va > 200.0 && Va < 360.0 && "V1/V2 plate voltage implausible");
    }

    // Power section: PI + 6L6 quiescent.
    TwinPowerAmp pa;
    pa.prepare(fs, 128);
    pa.setOversampling(4);

    // 6L6 analytic fixed point (grids at Vbias; rail/screen from the 4-tube draw).
    const auto p = pa.tube();  // 6L6 fit routed through the pentode evaluators
    const double Vbias = TwinPowerAmp::kVbias;
    const int nTot = 2 * TwinPowerAmp::kTubesPerSide;
    double rail = TwinPowerAmp::kVsupply - 0.14 * TwinPowerAmp::kRsupply;
    double scr = rail - 0.012 * TwinPowerAmp::kRscreen;
    double ipA = 0.0, ig2A = 0.0;
    for (int it = 0; it < 500; ++it) {
        ipA = clipper::dsp::el34PlateCurrent(rail, Vbias, scr, p);
        ig2A = clipper::dsp::el34ScreenCurrent(Vbias, scr, p);
        const double nRail = TwinPowerAmp::kVsupply - (nTot * ipA + nTot * ig2A) * TwinPowerAmp::kRsupply;
        const double nScr = nRail - (nTot * ig2A) * TwinPowerAmp::kRscreen;
        if (std::fabs(nRail - rail) < 1e-7 && std::fabs(nScr - scr) < 1e-7) { rail = nRail; scr = nScr; break; }
        rail = nRail; scr = nScr;
    }
    const double iq = pa.tubeQuiescentPlateCurrent();
    const double railM = pa.railIdle();
    assert(std::fabs(iq - ipA) / ipA < 0.10 && "6L6 Iq off the analytic Koren fixed point >10%");
    assert(std::fabs(railM - rail) / rail < 0.10 && "rail idle off analytic >10%");
    assert(iq > 0.026 && iq < 0.036 && "6L6 Iq not in the ~30 mA design region (±10% of 32)");
    assert(railM > 450.0 && railM < 468.0 && "rail idle not in the 450–468 V window");
    const double dissW = iq * railM;
    assert(dissW < 30.0 && "6L6 plate dissipation exceeds the 30 W max");

    // PI: the phase inverter's operating point and LEG BALANCE.
    //
    // 2026-07-25 (test/assert-real-properties): this block was commented "PI: balanced
    // anti-phase legs" but asserted only `quiescentPlate1() > 250 && < 410` — a 160 V
    // window on a 410 V rail, which admits BOTH a healthy ~330 V and the starved 386.8 V
    // (94.3 % of B+) the amp actually ships with. Nothing checked the balance the comment
    // named. That is the exact hole the starved AC30 phase inverter shipped through
    // (docs §23 second amendment). Now: plate as a FRACTION of B+, standing current per
    // triode from Ohm's law on the plate load, and the measured leg-gain ratio — see
    // core/tests/support/LtpProbe.h, shared with the JCM800 and AC30 suites.
    //
    // 2026-07-29 (finding 7): ALL THREE targets are asserted for real. The plate fraction
    // and standing current used to XFAIL — the pair idled at 94 % of B+ drawing
    // 0.23 mA/triode, parked near cutoff, because the tail returned to ground and 22 kΩ
    // was the only thing setting the current. With Rtail = 10 kΩ to a −7 V reference it
    // idles at 81.0/78.8 % of B+, 0.780/0.612 mA, ratio 0.946. The balance was already a
    // hard assertion and stayed green through the change — the 100k/142k plate pair is
    // what compensates the finite tail impedance (measured: matched 100k/100k gives
    // 0.718 at the same operating point, so the audit's "142 k is an artifact of the
    // starved tail" reading does not survive measurement; see TwinPowerAmp.cpp).
    const auto ltpM = clipper::test::measureLtp(pa.inverter(), fs);
    clipper::test::assertLtpSane(ltpM, "Twin");
    clipper::test::assertLtpTargets(ltpM, fs, "Twin",
                                    /*plate fraction: holds, assert for real*/ nullptr,
                                    /*standing current: holds, assert for real*/ nullptr,
                                    /*balance: holds, assert for real*/ nullptr);

    std::printf("  [ok] DC op points @ %.0f Hz: V1 Va=%.1f Vk=%.2f Iq=%.2fmA; 6L6 Iq=%.2f mA/tube "
                "(analytic %.2f), rail=%.1f V (%.1f), screen=%.1f V, Pdiss=%.1f W\n",
                fs, pre.stageQuiescentPlate(TwinPreamp::V1), pre.stageQuiescentCathode(TwinPreamp::V1),
                pre.stageQuiescentCurrent(TwinPreamp::V1) * 1e3, iq * 1e3, ipA * 1e3, railM, rail,
                pa.screenIdle(), dissW);
    clipper::test::printLtp(ltpM, "Twin", fs);
}

// ---------------------------------------------------------------------------
// Test 2 — Fender tone stack vs analytic H(jω) at flat (.5,.5,.5) and scooped
// (1,0,1). The Fender mid SCOOP exists even at "flat"; assert the notch freq +
// depth vs analytic at both, ±1.5 dB.
// ---------------------------------------------------------------------------
void testFenderStack(double fs) {
    TwinPreamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(1);
    const double rs = pre.toneSourceImpedance();

    auto measured = [&](double bass, double mid, double treble, double f) {
        FenderToneStack ts;
        ts.prepare(fs);
        ts.setSourceImpedance(rs);
        ts.setKnobs(bass, mid, treble);
        auto in = sine(f, 0.1f, 0.3, fs);
        std::vector<float> out(in.size(), 0.0f);
        ts.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), w = n / 2, st = n - w;
        return goertzelAmp(out, st, w, f, fs) / 0.1;
    };

    struct Cfg { const char* name; double b, m, t; };
    for (const Cfg& c : {Cfg{"flat", 0.5, 0.5, 0.5}, Cfg{"scoop", 1.0, 0.0, 1.0}}) {
        // Compare discrete vs analytic at several frequencies (±1.5 dB).
        double worst = 0.0;
        for (double f : {100.0, 200.0, 340.0, 650.0, 1500.0, 4000.0}) {
            const double m = toDb(measured(c.b, c.m, c.t, f));
            const double a = toDb(fenderToneH(f, c.b, c.m, c.t, rs));
            worst = std::max(worst, std::fabs(m - a));
        }
        assert(worst < 1.5 && "Fender stack discrete response off analytic H(jω) >1.5 dB");
        // Analytic notch (mid scoop): search the band.
        double minDb = 1e9, notchF = 0.0;
        for (double f = 80.0; f < 1200.0; f *= 1.01) {
            const double d = toDb(fenderToneH(f, c.b, c.m, c.t, rs));
            if (d < minDb) { minDb = d; notchF = f; }
        }
        const double shoulderLo = toDb(fenderToneH(100.0, c.b, c.m, c.t, rs));
        const double shoulderHi = toDb(fenderToneH(1500.0, c.b, c.m, c.t, rs));
        // The scoop is real: the notch sits below BOTH shoulders (deeper when scooped).
        assert(notchF > 150.0 && notchF < 700.0 && "Fender mid notch not in the 150–700 Hz band");
        assert(minDb < shoulderLo - 1.0 && minDb < shoulderHi - 1.0 &&
               "Fender mid scoop absent (notch not below both shoulders)");
        // Verify the measured discrete notch depth matches analytic at the notch (±1.5 dB).
        const double mNotch = toDb(measured(c.b, c.m, c.t, notchF));
        assert(std::fabs(mNotch - minDb) < 1.5 && "measured notch depth off analytic >1.5 dB");
        std::printf("  [ok] Fender stack @ %.0f Hz (%s): notch %.0f Hz depth %.1f dB (shoulders "
                    "%.1f/%.1f), discrete-vs-analytic worst %.2f dB (<1.5)\n",
                    fs, c.name, notchF, minDb, shoulderLo, shoulderHi, worst);
    }
}

// ---------------------------------------------------------------------------
// Test 3 — global NFB (NO presence): closed-loop gain BELOW open-loop by the
// analytic 1/(1+β·A_real). Sign-catcher (an inverted loop would RAISE the gain).
// ---------------------------------------------------------------------------
void testNfb(double fs) {
    const double f = 1000.0;
    const float amp = 0.02f;  // small-signal grid drive

    TwinPowerAmp openA;
    openA.prepare(fs, 128); openA.setOversampling(4);
    openA.setFeedbackEnabled(false);
    const double gOpen = powerAmpAt(openA, f, amp, fs) / amp;

    TwinPowerAmp closedA;
    closedA.prepare(fs, 128); closedA.setOversampling(4);
    closedA.setFeedbackEnabled(true);
    const double gClosed = powerAmpAt(closedA, f, amp, fs) / amp;

    assert(gClosed < gOpen && "NFB SIGN INVERTED: closed-loop gain not below open-loop");
    const double Areal = gOpen * TwinPowerAmp::kFullScaleSecV;
    const double predRedDb = toDb(1.0 / (1.0 + TwinPowerAmp::kFeedbackBeta * Areal));
    const double measRedDb = toDb(gClosed / gOpen);
    assert(std::fabs(measRedDb - predRedDb) < 1.0 &&
           "NFB reduction off analytic 1/(1+β·A_real) >1 dB");
    assert(-measRedDb > 1.5 && -measRedDb < 8.0 &&
           "NFB loop not in the meaningful window");
    std::printf("  [ok] NFB @ %.0f Hz: open %.2f dB -> closed %.2f dB (%.2f dB, analytic %.2f, "
                "A_real=%.2f) — gain goes DOWN, no presence\n",
                fs, toDb(gOpen), toDb(gClosed), measRedDb, predRedDb, Areal);
}

// ---------------------------------------------------------------------------
// Test 4 — OptoTremolo: DEPTH grows with INTENSITY, RATE follows SPEED (log map),
// the opto gain-waveform is ASYMMETRIC (fast dip / slow recovery — NOT a sine), AND
// the ON/OFF ENABLE (M10.1 amendment, docs §20): OFF is a BIT-EXACT bypass, the
// toggle is CLICK-FREE, and intensity=0 stays a unity escape hatch.
// ---------------------------------------------------------------------------
void testTremolo(double fs) {
    // DEPTH vs INTENSITY: feed DC, measure the gain-envelope excursion. Trem ENABLED.
    auto envDepth = [&](float intensity) {
        OptoTremolo tr; tr.prepare(fs); tr.setEnabled(true); tr.setSpeed(0.4f); tr.setIntensity(intensity);
        std::vector<float> in(static_cast<size_t>(fs * 2.0), 1.0f), out(in.size(), 0.0f);
        tr.process(in.data(), out.data(), static_cast<int>(in.size()));
        double mn = 1e9, mx = -1e9;
        for (size_t i = static_cast<size_t>(fs); i < out.size(); ++i) {
            mn = std::min(mn, static_cast<double>(out[i]));
            mx = std::max(mx, static_cast<double>(out[i]));
        }
        return mx - mn;
    };
    const double d0 = envDepth(0.0f), d5 = envDepth(0.5f), d9 = envDepth(0.9f);
    assert(d0 < 1e-4 && "intensity=0 not a bit-exact passthrough (gain not held at unity)");
    assert(d5 > d0 && d9 > d5 && "tremolo depth not monotonic with INTENSITY");
    assert(d9 > 0.4 && "tremolo depth at intensity 0.9 implausibly small");

    // ENABLE = OFF is a BIT-EXACT bypass (the trem on/off report): even at full
    // intensity, a settled-DISABLED trem passes the input through unchanged. The enable
    // is snapped to 0 by prepare()/reset() (enabled_ default false), so there is no
    // ramp — every sample multiplies by exactly 1.0.
    {
        OptoTremolo tr; tr.prepare(fs); tr.setSpeed(0.4f); tr.setIntensity(0.9f);  // NOT enabled
        assert(tr.enableRamp() == 0.0 && "disabled trem did not snap the enable ramp to 0");
        auto in = sine(220.0, 0.3f, 0.3, fs);
        std::vector<float> out(in.size(), 0.0f);
        tr.process(in.data(), out.data(), static_cast<int>(in.size()));
        for (size_t i = 0; i < in.size(); ++i)
            assert(out[i] == in[i] && "trem OFF not a BIT-EXACT bypass (gain not exactly 1.0)");
    }

    // CLICK-FREE toggle: enable mid-stream on a DC input; the gain must not JUMP — the
    // linear enable-ramp bounds the per-sample step. (A hard switch would drop from 1.0
    // to a mid-dip gain in one sample.) Also confirm it ends fully engaged (ramp → 1).
    {
        OptoTremolo tr; tr.prepare(fs); tr.setSpeed(0.4f); tr.setIntensity(0.9f);
        const int n = static_cast<int>(0.2 * fs);
        std::vector<float> in(static_cast<size_t>(n), 1.0f), out(static_cast<size_t>(n), 0.0f);
        // Run a bit disabled, toggle ON, keep running.
        const int half = n / 2;
        tr.process(in.data(), out.data(), half);
        tr.setEnabled(true);
        tr.process(in.data() + half, out.data() + half, n - half);
        double maxStep = 0.0;
        for (int i = 1; i < n; ++i)
            maxStep = std::max(maxStep, std::fabs(static_cast<double>(out[i]) - out[i - 1]));
        // The enable ramp spans ~10 ms; on a unit DC input the largest legitimate
        // per-sample change is tiny. A hard toggle would show a step ~depth·cell (0.1+).
        assert(maxStep < 0.02 && "tremolo enable toggle is not click-free (gain jumped)");
        assert(tr.enableRamp() > 0.99 && "enable ramp did not reach fully engaged");
    }

    // RATE vs SPEED: log map kMinHz..kMaxHz.
    auto rateAt = [&](float speed) {
        OptoTremolo tr; tr.setSpeed(speed); tr.setIntensity(0.5f); tr.prepare(fs);
        return tr.rateHz();  // prepare() snaps the rate smoother to the speed target
    };
    const double r0 = rateAt(0.0f), r1 = rateAt(1.0f), rMid = rateAt(0.5f);
    assert(std::fabs(r0 - OptoTremolo::kMinHz) < 0.05 && "speed=0 rate not kMinHz");
    assert(std::fabs(r1 - OptoTremolo::kMaxHz) < 0.05 && "speed=1 rate not kMaxHz");
    const double geoMid = OptoTremolo::kMinHz * std::sqrt(OptoTremolo::kMaxHz / OptoTremolo::kMinHz);
    assert(std::fabs(rMid - geoMid) / geoMid < 0.05 && "speed=0.5 rate not the log-map midpoint");
    assert(rMid > r0 && r1 > rMid && "rate not monotonic with SPEED");

    // ASYMMETRY: the LDR darkens fast, recovers slow — so the gain envelope FALLS
    // fast and RISES slow (more rising samples than falling per cycle). A pure sine
    // would give ~1.0; assert a clear asymmetry consistent with τ_attack < τ_release.
    OptoTremolo tr; tr.prepare(fs); tr.setEnabled(true); tr.setSpeed(0.4f); tr.setIntensity(0.9f);
    std::vector<float> in(static_cast<size_t>(fs * 2.0), 1.0f), out(in.size(), 0.0f);
    tr.process(in.data(), out.data(), static_cast<int>(in.size()));
    int rising = 0, falling = 0;
    for (size_t i = static_cast<size_t>(fs) + 1; i < out.size(); ++i) {
        if (out[i] > out[i - 1]) ++rising;
        else if (out[i] < out[i - 1]) ++falling;
    }
    const double ratio = static_cast<double>(rising) / std::max(1, falling);
    assert(ratio > 1.3 && "tremolo gain waveform not asymmetric (fast dip / slow recovery)");
    // Consistency with the constants: τ_release > τ_attack, so rising dominates.
    assert(OptoTremolo::kReleaseMs > OptoTremolo::kAttackMs && "attack/release constants inverted");

    // COMPOSED TwinAmp on/off (the "fender needs on/off for its trem" report): with the
    // trem OFF the output is INDEPENDENT of INTENSITY (the effect is truly bypassed —
    // bit-exact), and turning it ON with a real intensity MODULATES the output (a moving
    // envelope the off path does not have). Both amps set params BEFORE prepare so the
    // enable snaps (no init ramp) — trem-off is bit-exact vs the no-trem reference chain.
    auto twinRender = [&](bool tremOn, float intensity) {
        TwinAmp a; a.setOversampling(4);
        a.setParameter(TwinAmp::PARAM_VOLUME, 0.5f);
        a.setParameter(TwinAmp::PARAM_BASS, 0.5f);
        a.setParameter(TwinAmp::PARAM_MID, 0.5f);
        a.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        a.setParameter(TwinAmp::PARAM_SPEED, 0.6f);
        a.setParameter(TwinAmp::PARAM_INTENSITY, intensity);
        a.setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, tremOn ? 1.0f : 0.0f);
        a.prepare(fs, 128);
        auto in = sine(180.0, 0.1f, 0.4, fs);
        std::vector<float> out(in.size(), 0.0f);
        a.process(in.data(), out.data(), static_cast<int>(in.size()));
        return out;
    };
    const auto offLo = twinRender(false, 0.0f);
    const auto offHi = twinRender(false, 0.9f);  // intensity ignored when OFF
    for (size_t i = 0; i < offLo.size(); ++i)
        assert(offLo[i] == offHi[i] &&
               "trem OFF is not intensity-invariant — the effect is not truly bypassed");
    const auto onHi = twinRender(true, 0.9f);
    double offEnvMin = 1e9, offEnvMax = -1e9, onEnvMin = 1e9, onEnvMax = -1e9;
    // Compare the per-period peak envelope (steady window) off vs on.
    const int per = static_cast<int>(fs / 180.0);
    for (size_t i = static_cast<size_t>(0.15 * fs); i + per < offLo.size(); i += per) {
        double poff = 0.0, pon = 0.0;
        for (int j = 0; j < per; ++j) {
            poff = std::max(poff, std::fabs(static_cast<double>(offLo[i + j])));
            pon = std::max(pon, std::fabs(static_cast<double>(onHi[i + j])));
        }
        offEnvMin = std::min(offEnvMin, poff); offEnvMax = std::max(offEnvMax, poff);
        onEnvMin = std::min(onEnvMin, pon);    onEnvMax = std::max(onEnvMax, pon);
    }
    const double offRipple = (offEnvMax - offEnvMin) / (offEnvMax + 1e-9);
    const double onRipple = (onEnvMax - onEnvMin) / (onEnvMax + 1e-9);
    assert(offRipple < 0.02 && "trem OFF still shows amplitude ripple (not bypassed)");
    assert(onRipple > 0.2 && "trem ON did not modulate the output (throb missing)");

    std::printf("  [ok] tremolo @ %.0f Hz: depth %.3f/%.3f/%.3f (int 0/.5/.9), rate %.2f/%.2f/%.2f Hz "
                "(speed 0/.5/1), asymmetry %.2f; ON/OFF: off ripple %.3f (bypassed) vs on %.3f (throb)\n",
                fs, d0, d5, d9, r0, rMid, r1, ratio, offRipple, onRipple);
}

// ---------------------------------------------------------------------------
// Test 5 — reverb placement: reverb=0 is a bit-exact passthrough (the ReverbModel
// contract that makes reverb-off unchanged), and with reverb>0 the wet tail is
// present pre-PI (it persists in the OUTPUT after the input stops — proving it
// runs THROUGH the power amp).
// ---------------------------------------------------------------------------
void testReverbPlacement(double fs) {
    // (a) reverb=0 bit-exact passthrough (standalone contract).
    {
        ReverbModel rv; rv.prepare(fs); rv.setMix(0.0f);
        auto in = sine(220.0, 0.3f, 0.2, fs);
        std::vector<float> out(in.size(), 0.0f);
        rv.process(in.data(), out.data(), static_cast<int>(in.size()));
        for (size_t i = 0; i < in.size(); ++i)
            assert(out[i] == in[i] && "reverb mix=0 not a bit-exact passthrough");
    }
    // (b) wet tail present pre-PI: a pluck then silence, compare tail energy.
    auto tailRms = [&](float reverbMix) {
        TwinAmp amp; amp.prepare(fs, 128); amp.setOversampling(4);
        amp.setParameter(TwinAmp::PARAM_VOLUME, 0.6f);
        amp.setParameter(TwinAmp::PARAM_BASS, 0.5f);
        amp.setParameter(TwinAmp::PARAM_MID, 0.5f);
        amp.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        amp.setParameter(TwinAmp::PARAM_INTENSITY, 0.0f);  // trem off for a clean tail
        amp.setParameter(TwinAmp::PARAM_REVERB, reverbMix);
        const int nsig = static_cast<int>(0.15 * fs), ntail = static_cast<int>(0.5 * fs);
        std::vector<float> in(static_cast<size_t>(nsig + ntail), 0.0f), out(in.size(), 0.0f);
        for (int i = 0; i < nsig; ++i)
            in[static_cast<size_t>(i)] = 0.25f * static_cast<float>(std::sin(kTwoPi * 220.0 * i / fs)) *
                                         static_cast<float>(std::exp(-3.0 * i / (nsig)));
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        double e = 0.0;
        for (int i = nsig + static_cast<int>(0.1 * fs); i < nsig + ntail; ++i)
            e += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
        return std::sqrt(e / (ntail - static_cast<int>(0.1 * fs)));
    };
    const double dry = tailRms(0.0f), wet = tailRms(0.5f);
    assert(wet > dry * 3.0 && "reverb tail not present pre-PI (wet tail not above the dry residual)");
    std::printf("  [ok] reverb @ %.0f Hz: mix=0 bit-exact passthrough; wet tail RMS %.2e vs dry %.2e "
                "(present pre-PI, persists through the power amp)\n", fs, wet, dry);
}

// ---------------------------------------------------------------------------
// Test 6 — the PRODUCT: clean-headroom bar + monotonic THD growth to breakup; the
// LIGHT sag (smaller than the JCM800's); and ±10 V slam stability.
// ---------------------------------------------------------------------------
void testHeadroomSagStability(double fs) {
    auto compAmp = [&](float vol) {
        auto a = std::make_unique<TwinAmp>();
        a->prepare(fs, 128); a->setOversampling(4);
        a->setParameter(TwinAmp::PARAM_VOLUME, vol);
        a->setParameter(TwinAmp::PARAM_BASS, 0.5f);
        a->setParameter(TwinAmp::PARAM_MID, 0.5f);
        a->setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        a->setParameter(TwinAmp::PARAM_INTENSITY, 0.0f);
        a->setParameter(TwinAmp::PARAM_REVERB, 0.0f);
        return a;
    };
    auto thdAt = [&](float vol, float in) {
        auto a = compAmp(vol);
        auto sig = sine(110.0, in, 0.4, fs);
        std::vector<float> out(sig.size(), 0.0f);
        a->process(sig.data(), out.data(), static_cast<int>(sig.size()));
        double pk = 0.0;
        for (float v : out) pk = std::max(pk, std::fabs(static_cast<double>(v)));
        return std::pair<double, double>{thd(out, 110.0, fs), pk};
    };

    // Clean-headroom bar: volume 0.5, a hot single-coil DI (0.1) stays CLEAN.
    const auto clean = thdAt(0.5f, 0.10f);
    assert(clean.first < 0.04 && "clean bar: THD at volume 0.5 / hot input not under 4%");
    // Monotonic THD growth with input at volume 0.5, all the way to breakup at max.
    const double t1 = thdAt(0.5f, 0.10f).first, t2 = thdAt(0.5f, 0.20f).first,
                 t3 = thdAt(0.5f, 0.30f).first, t4 = thdAt(0.5f, 0.50f).first;
    assert(t2 > t1 && t3 > t2 && t4 > t3 && "THD not monotonic with input at volume 0.5");
    // Real breakup at max volume + hot input, and it is MUCH dirtier than clean.
    const auto cranked = thdAt(1.0f, 0.50f);
    assert(cranked.first > 0.25 && "max volume / hot input did not reach real breakup (>25%)");
    assert(cranked.first > clean.first * 6.0 && "cranked not dramatically dirtier than clean (headroom)");
    // Full-scale calibration: fully cranked ≈ 0.9 peak, inside full scale.
    assert(cranked.second > 0.7 && cranked.second <= 1.0 && "cranked peak not in the ~0.9 window");

    // LIGHT sag, SMALLER than the JCM800's. Same hard burst into both amps.
    auto sagDepth = [&](auto& amp) {
        amp.setParameter(std::decay_t<decltype(amp)>::PARAM_DRIVE, 1.0f);
        const double f0 = 400.0;
        const int nsil = static_cast<int>(0.02 * fs), nb = static_cast<int>(0.30 * fs),
                  nt = static_cast<int>(0.25 * fs), n = nsil + nb + nt;
        std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < nb; ++i)
            in[static_cast<size_t>(nsil + i)] = static_cast<float>(45.0 * std::sin(kTwoPi * f0 * i / fs));
        amp.process(in.data(), out.data(), n);
        for (float v : out) assert(std::isfinite(v) && "non-finite in the sag burst");
        const int per = static_cast<int>(fs / f0);
        std::vector<double> env;
        for (int i = nsil; i + per <= nsil + nb; i += per) {
            double pk = 0.0;
            for (int j = 0; j < per; ++j) pk = std::max(pk, std::fabs(static_cast<double>(out[i + j])));
            env.push_back(pk);
        }
        double attack = 0.0;
        for (size_t k = 0; k < env.size() && k < 3; ++k) attack = std::max(attack, env[k]);
        return toDb(attack / env.back());
    };
    TwinPowerAmp tpa; tpa.prepare(fs, 128); tpa.setOversampling(4);
    const double twinSag = sagDepth(tpa);
    Jcm800PowerAmp jpa; jpa.prepare(fs, 128); jpa.setOversampling(4);
    const double jcmSag = sagDepth(jpa);
    assert(twinSag > 0.5 && twinSag < 2.5 && "Twin sag not in the LIGHT ~1–2 dB window");
    assert(twinSag < jcmSag && "Twin sag not SMALLER than the JCM800's (the Twin is the stiff one)");

    // ±10 V slam stability inside a sustained tone.
    {
        auto a = compAmp(0.8f);
        const int n = static_cast<int>(0.2 * fs);
        std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) {
            double s = 3.0 * std::sin(kTwoPi * 180.0 * i / fs);
            if (i % 3000 < 16) s += (i & 1) ? 10.0 : -10.0;
            in[static_cast<size_t>(i)] = static_cast<float>(s);
        }
        a->process(in.data(), out.data(), n);
        double pk = 0.0;
        for (float v : out) { assert(std::isfinite(v) && "non-finite on ±10 V slam"); pk = std::max(pk, std::fabs((double)v)); }
        assert(pk < 100.0 && "output not bounded on ±10 V slam");
    }
    std::printf("  [ok] product @ %.0f Hz: clean THD %.2f%% (vol .5), monotonic → cranked %.1f%% "
                "(peak %.3f≈0.9); sag Twin %.2f dB < JCM %.2f dB (light+stiff); ±10 V slam finite\n",
                fs, clean.first * 100, cranked.first * 100, cranked.second, twinSag, jcmSag);
}

// ---------------------------------------------------------------------------
// Test 7 — aliasing at MAX volume (M2 sweep). The Twin is a CLEAN amp, so it
// aliases far less than a distortion amp; the shipped 4× clears the −60 dB bar.
// ---------------------------------------------------------------------------
void testAliasing(double fs) {
    const double f0 = 4186.0;
    auto aliasAt = [&](int os) {
        TwinAmp amp; amp.prepare(fs, 128); amp.setOversampling(os);
        amp.setParameter(TwinAmp::PARAM_VOLUME, 1.0f);
        amp.setParameter(TwinAmp::PARAM_BASS, 0.5f);
        amp.setParameter(TwinAmp::PARAM_MID, 0.5f);
        amp.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        amp.setParameter(TwinAmp::PARAM_INTENSITY, 0.0f);
        amp.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
        auto in = sine(f0, 0.3f, 1.0, fs);
        std::vector<float> out(in.size(), 0.0f);
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        return measureAliasing(out, fs, f0).worstAliasDb;
    };
    const double a1 = aliasAt(1), a2 = aliasAt(2), a4 = aliasAt(4), a8 = aliasAt(8);
    assert(a2 < a1 - 8.0 && "2x not >=8 dB better than 1x");
    assert(a4 < -60.0 && "shipped 4x worst-alias not >=60 dB below fundamental (M2 bar)");
    assert(a8 < a4 + 4.0 && "8x materially better than 4x (4x not at the floor)");
    std::printf("  [ok] aliasing @ %.0f Hz (max vol): 1x=%.1f 2x=%.1f 4x=%.1f 8x=%.1f dB (M2 −60 bar)\n",
                fs, a1, a2, a4, a8);
}

// Known defects this binary exercises under XFAIL. Printed by --xfail-ledger, which ctest
// surfaces as ***Skipped in its default summary (see core/CMakeLists.txt).
// EMPTY since 2026-07-29 (finding 7): the Twin's two PI XFAILs XPASSed and were deleted,
// and this suite now has no known-bad property. `--xfail-ledger` still answers (with a
// count of zero); the ctest ledger entry was removed in core/CMakeLists.txt.
const clipper::test::XfailDecl* const kLedger = nullptr;
constexpr std::size_t kLedgerCount = 0;

}  // namespace

int main(int argc, char** argv) {
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger, kLedgerCount,
                                                 "clipper_twin_tests");
    if (ledger >= 0) return ledger;

    std::printf("Running clipper::dsp Twin (M10.1) tests...\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        testOperatingPoints(fs);
        testFenderStack(fs);
        testNfb(fs);
        testTremolo(fs);
        testReverbPlacement(fs);
        testHeadroomSagStability(fs);
    }
    testAliasing(44100.0);
    testAliasing(48000.0);
    testAliasing(96000.0);
    std::printf("All Twin (M10.1) tests passed (XFAILs listed below are known open defects, "
                "not regressions).\n");
    return clipper::test::reportXfails();
}
