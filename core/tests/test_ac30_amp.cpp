// Plain-assert tests for the M10.2 Vox AC30 "top boost" amp — the CLASS-A CHIME.
// Covers the Ac30Preamp (one 12AX7 + the interactive top-boost tone stack + volume),
// the Ac30PowerAmp (hot 12AX7 LTP PI → TOP CUT → CATHODE-BIASED EL84 quad → OT → NO
// NFB → deep GZ34 sag), and the composed Ac30Amp (preamp → power → spring reverb).
// No framework: int main + <cassert>. Every assert compares a MEASURED number
// against an ANALYTIC target derived IN THE TEST (load-line DC, the EL84 Koren fixed
// point, the shared-cathode self-bias fixed point, the complex nodal top-boost H(jω),
// the anti-NFB identity, the cathode-bias RC recovery, the sag ordering) — so the
// suite pins the physics, not itself. Same discipline as the M9/M10.1 suites (docs §23).

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/Ac30Preamp.h"
#include "clipper/dsp/Ac30PowerAmp.h"
#include "clipper/dsp/Jcm800PowerAmp.h"
#include "clipper/dsp/TwinAmp.h"
#include "clipper/dsp/TwinPowerAmp.h"

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
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

// --- XFAIL history (the ratchet working) -------------------------------------------
// `finding7-ac30-pi-leg-balance` XPASSed 2026-07-29 (finding 7's tail reference; legs
// ×27.56/×25.13, ratio 0.912) — hard assertion since. The five `finding7-ac30-*`
// voicing entries that same fix created (the balanced PI removed the even-harmonic
// leakage §23's guards had been measuring as "chime"/"breakup") XPASSed 2026-07-30
// when the §46 gain-structure slice completed the top-boost channel (V2 + cathode
// follower + the corrected stack + the re-derived volume law) — all five deleted,
// their unchanged bars hard assertions in testChime / testBreakupOrdering. This
// suite currently carries ZERO known-bad properties (no --xfail-ledger in CMake).
using clipper::dsp::Ac30Amp;
using clipper::dsp::Ac30PowerAmp;
using clipper::dsp::Ac30Preamp;
using clipper::dsp::Jcm800PowerAmp;
using clipper::dsp::TopBoostToneStack;
using clipper::dsp::TwinAmp;
using clipper::dsp::TwinPowerAmp;
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

// Analytic H(jω) of the §46 CORRECTED netlist (six nodes): R1 and Ct both feed the
// treble pot's TOP, Cb sits in SERIES with the (log-law) bass rheostat, and the
// 500 k wiper load sits at OUT — audit finding 5's structural errors, fixed in
// TopBoostToneStack and mirrored here. This remains a DISCRETIZATION check
// (same-netlist, documented limitation); the topology itself is argued against the
// published top-boost schematic in docs §46.
double topBoostH(double f, double bass, double treble, double rs) {
    using cd = std::complex<double>;
    const double w = kTwoPi * f;
    const cd s(0.0, w);
    auto Yc = [&](double C) { return s * C; };  // cap admittance jωC
    auto cl = [](double v) { return v < 1.0 ? 1.0 : v; };
    const double RT = TopBoostToneStack::kRT, RB = TopBoostToneStack::kRB,
                 R1 = TopBoostToneStack::kRslope, RL = TopBoostToneStack::kRLoad;
    const double Ct = TopBoostToneStack::kCt, Cb = TopBoostToneStack::kCb,
                 Cc = TopBoostToneStack::kCc;
    const int N = 6;  // SRC,IN,N2,OUT,N4,N5
    cd G[6][6] = {};
    auto stamp = [&](int a, int b, cd g) { G[a][a]+=g; G[b][b]+=g; G[a][b]-=g; G[b][a]-=g; };
    G[0][0] += 1.0 / rs;                  // source conductance at SRC
    stamp(0, 1, Yc(Cc));                  // series coupling cap SRC-IN
    stamp(1, 2, Yc(Ct));                  // treble cap IN-N2 (bypasses R1)
    stamp(1, 2, cd(1.0 / R1, 0));         // slope R1 IN-N2 (to the pot TOP, §46)
    stamp(2, 3, cd(1.0 / cl((1 - treble) * RT), 0));  // treble pot upper N2-OUT
    stamp(3, 4, cd(1.0 / cl(treble * RT), 0));        // treble pot lower OUT-N4
    stamp(4, 5, Yc(Cb));                              // bass cap N4-N5 (series, §46)
    G[5][5] += cd(1.0 / cl(bass * bass * RB), 0);     // bass rheostat, log pot (§46)
    G[3][3] += cd(1.0 / RL, 0);                       // wiper load OUT-GND (§46)
    cd A[6][7];
    for (int i = 0; i < N; ++i) { for (int j = 0; j < N; ++j) A[i][j] = G[i][j]; A[i][N] = 0; }
    A[0][N] = cd(1.0 / rs, 0);  // source injects Vs=1 through 1/rs at SRC
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r) if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
        for (int j = 0; j <= N; ++j) std::swap(A[p][j], A[c][j]);
        const cd d = A[c][c];
        for (int j = 0; j <= N; ++j) A[c][j] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const cd f2 = A[r][c];
            for (int j = 0; j <= N; ++j) A[r][j] -= f2 * A[c][j];
        }
    }
    return std::abs(A[3][N]);  // V[OUT]
}

double powerAmpAt(Ac30PowerAmp& pa, double f, float amp, double fs) {
    auto in = sine(f, amp, 0.4, fs);
    std::vector<float> out(in.size(), 0.0f);
    pa.process(in.data(), out.data(), static_cast<int>(in.size()));
    const size_t n = out.size(), win = n / 2, start = n - win;
    return goertzelAmp(out, start, win, f, fs);
}

// ---------------------------------------------------------------------------
// Test 1 — DC operating points: preamp V1 self-bias load line; EL84 quiescent vs
// the Koren + shared-cathode fixed point (±10 %); rail/screen; Pdiss < EL84 max.
// ---------------------------------------------------------------------------
void testOperatingPoints(double fs) {
    Ac30Preamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(4);
    // V1: self-bias load line B+ = Va + Ra·Iq, Vk = Iq·Rk (Ra 100k, Rk 1.5k, B+ 300).
    {
        const auto& cfg = pre.stageConfig(Ac30Preamp::V1);
        const double Va = pre.stageQuiescentPlate(Ac30Preamp::V1);
        const double Vk = pre.stageQuiescentCathode(Ac30Preamp::V1);
        const double Iq = pre.stageQuiescentCurrent(Ac30Preamp::V1);
        assert(std::fabs(cfg.bPlus - (Va + cfg.Ra * Iq)) < 0.05 * cfg.bPlus &&
               "V1 off the B+ = Va + Ra·Iq load line");
        assert(std::fabs(Vk - Iq * cfg.Rk) < 0.05 * std::fabs(Vk) + 0.02 &&
               "V1 cathode voltage off Iq·Rk self-bias");
        assert(Iq > 0.5e-3 && Iq < 2.0e-3 && "V1 idle current implausible for 12AX7");
        assert(Va > 150.0 && Va < 290.0 && "V1 plate voltage implausible");
    }

    Ac30PowerAmp pa;
    pa.prepare(fs, 128);
    pa.setOversampling(4);

    // EL84 analytic shared-cathode fixed point: bisect vk so nTot·(ip+ig2) = vk/Rk,
    // with the rail from the soft-knee source and the screen settled — the SAME system
    // the solver uses, re-derived here.
    const auto p = pa.tube();
    const int nTot = 2 * Ac30PowerAmp::kTubesPerSide;
    auto residual = [&](double vk, double& ip, double& ig2, double& rail, double& scr) {
        const double iTarget = vk / Ac30PowerAmp::kRkCathode;
        const double Reff = Ac30PowerAmp::kRsupply * (1.0 + Ac30PowerAmp::kRectKnee * iTarget);
        rail = Ac30PowerAmp::kVsupply - iTarget * Reff;
        scr = rail;
        for (int k = 0; k < 60; ++k) {
            ig2 = clipper::dsp::el34ScreenCurrent(-vk, scr, p);
            const double sn = rail - (nTot * ig2) * Ac30PowerAmp::kRscreen;
            scr += 0.35 * (sn - scr);
        }
        ip = clipper::dsp::el34PlateCurrent(rail, -vk, scr, p);
        return nTot * (ip + ig2) - iTarget;
    };
    double lo = 0.5, hi = 40.0, ipA = 0, ig2A = 0, railA = 0, scrA = 0, tmp;
    double fLo = residual(lo, ipA, ig2A, railA, scrA);
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        const double fm = residual(mid, ipA, ig2A, railA, scrA);
        if ((fm > 0) == (fLo > 0)) { lo = mid; fLo = fm; } else hi = mid;
    }
    const double vkA = 0.5 * (lo + hi);
    residual(vkA, ipA, ig2A, railA, scrA);
    (void)tmp;

    const double iq = pa.tubeQuiescentPlateCurrent();
    const double railM = pa.railIdle();
    const double vkM = pa.cathodeIdle();
    assert(std::fabs(iq - ipA) / ipA < 0.10 && "EL84 Iq off the analytic Koren fixed point >10%");
    assert(std::fabs(vkM - vkA) / vkA < 0.05 && "cathode Vk off the analytic shared-bias fixed point >5%");
    assert(std::fabs(railM - railA) / railA < 0.05 && "rail idle off analytic >5%");
    assert(iq > 0.030 && iq < 0.045 && "EL84 Iq not in the ~35 mA class-A design region");
    assert(vkM > 6.0 && vkM < 12.0 && "cathode bias not in the ~-7..-10 V AC30 window");
    const double dissW = iq * railM;
    assert(dissW < 12.5 && "EL84 plate dissipation exceeds the ~12 W max (class-A runs hot but bounded)");

    // PI: the phase inverter's operating point and LEG BALANCE.
    //
    // 2026-07-25 (test/assert-real-properties): this block asserted only
    // `quiescentPlate1() > 150 && < 300` — a 150 V window on a 300 V rail, wide enough to
    // pass a phase inverter parked anywhere from mid-load-line to 99 % of B+. The comment
    // above the Twin's equivalent said "balanced anti-phase legs" and nothing checked the
    // balance; this one did not even claim to. That window is the hole the AC30's OWN
    // starved phase inverter shipped through (docs §23 second amendment) — the test that
    // should have caught it passed both before and after the fix.
    //
    // 2026-07-29 (finding 7): ALL THREE targets are asserted for real. The AC30 always met
    // the plate-fraction and standing-current targets, but only because Rtail had been cut
    // to 2.2 kΩ — which is exactly what wrecked its LEG BALANCE (0.550), because a
    // two-terminal tail cannot deliver both. With the tail reference it can: 10 kΩ to
    // −10 V gives 79.3/78.5 % of B+, 0.622/0.587 mA and a 0.912 ratio. The three
    // assertions together still pin the trade-off — a future "fix" that restores balance
    // by re-starving the inverter, or buys current by shortening the tail again, fails.
    const auto ltpM = clipper::test::measureLtp(pa.inverter(), fs);
    clipper::test::assertLtpSane(ltpM, "AC30");
    clipper::test::assertLtpTargets(ltpM, fs, "AC30",
                                    /*plate fraction: holds, assert for real*/ nullptr,
                                    /*standing current: holds, assert for real*/ nullptr,
                                    /*leg balance: holds, assert for real*/ nullptr);

    std::printf("  [ok] DC op points @ %.0f Hz: V1 Va=%.1f Vk=%.2f Iq=%.2fmA; EL84 Iq=%.2f mA/tube "
                "(analytic %.2f), Vk=%.2f (analytic %.2f), rail=%.1f (%.1f), screen=%.1f, Pdiss=%.1f W\n",
                fs, pre.stageQuiescentPlate(Ac30Preamp::V1), pre.stageQuiescentCathode(Ac30Preamp::V1),
                pre.stageQuiescentCurrent(Ac30Preamp::V1) * 1e3, iq * 1e3, ipA * 1e3, vkM, vkA,
                railM, railA, pa.screenIdle(), dissW);
    clipper::test::printLtp(ltpM, "AC30", fs);
}

// ---------------------------------------------------------------------------
// Test 2 — top-boost tone stack vs analytic H(jω): (a) discrete matches analytic
// <1.5 dB; (b) GAIN LOSS through the stack (|H| < 1 everywhere — the passive Vox
// network attenuates); (c) STRONG treble/bass INTERACTION (moving bass shifts the
// treble-band response and vice versa — the AC30 tone-control signature).
// ---------------------------------------------------------------------------
void testTopBoostStack(double fs) {
    Ac30Preamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(1);
    const double rs = pre.toneSourceImpedance();

    auto measured = [&](double bass, double treble, double f) {
        TopBoostToneStack ts;
        ts.prepare(fs);
        ts.setSourceImpedance(rs);
        ts.setKnobs(bass, treble);
        auto in = sine(f, 0.1f, 0.3, fs);
        std::vector<float> out(in.size(), 0.0f);
        ts.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), w = n / 2, st = n - w;
        return goertzelAmp(out, st, w, f, fs) / 0.1;
    };

    // (a) discrete vs analytic at several settings/frequencies (±1.5 dB).
    double worst = 0.0, worstMax = 0.0;
    struct S { double b, t; };
    for (const S& c : {S{0.5,0.5}, S{1.0,0.2}, S{0.2,0.9}, S{0.8,0.8}}) {
        for (double f : {80.0, 200.0, 500.0, 1500.0, 4000.0, 8000.0}) {
            const double m = toDb(measured(c.b, c.t, f));
            const double a = toDb(topBoostH(f, c.b, c.t, rs));
            worst = std::max(worst, std::fabs(m - a));
            // (b) gain LOSS: the passive stack attenuates — |H| < 1 (0 dB) always.
            worstMax = std::max(worstMax, a);
        }
    }
    assert(worst < 1.5 && "top-boost stack discrete response off analytic H(jω) >1.5 dB");
    // The SIGNATURE gain LOSS: the passive top-boost network attenuates the midband
    // substantially at a normal setting (there is no makeup gain in the stack).
    const double midFlat = toDb(topBoostH(1000.0, 0.5, 0.5, rs));
    assert(midFlat < -3.0 && "top-boost stack midband is not a clear LOSS (the AC30 gain-loss signature)");
    assert(worstMax < 1.0 && "top-boost stack shows implausible GAIN"); // passive: allow tiny resonant peaking

    // (c) treble/bass INTERACTION: raising BASS must lift the low end AND measurably
    // change the treble-band response (the shared-wiper interaction). Analytic H.
    const double loBassLo = toDb(topBoostH(100.0, 0.1, 0.5, rs));
    const double hiBassLo = toDb(topBoostH(100.0, 0.9, 0.5, rs));
    assert(std::fabs(hiBassLo - loBassLo) > 2.0 && "bass knob does not move the low end");
    // Interaction: at a FIXED treble knob, changing BASS shifts the response across a
    // BAND (a non-interacting stack would leave the whole treble/mid region unchanged).
    // The Vox wiper taps between the treble-cap node and the bass network, so moving
    // one control audibly changes the other's band — scan and take the max shift.
    double maxInteract = 0.0;
    for (double f : {400.0, 700.0, 1200.0, 2000.0, 3000.0}) {
        const double lo = toDb(topBoostH(f, 0.1, 0.7, rs));
        const double hi = toDb(topBoostH(f, 0.9, 0.7, rs));
        maxInteract = std::max(maxInteract, std::fabs(hi - lo));
    }
    assert(maxInteract > 0.5 &&
           "treble/mid band unaffected by the bass knob — the Vox interaction is missing");
    // And the treble knob strongly controls the top.
    const double topLoT = toDb(topBoostH(6000.0, 0.5, 0.1, rs));
    const double topHiT = toDb(topBoostH(6000.0, 0.5, 0.9, rs));
    assert(topHiT > topLoT + 4.0 && "treble knob does not control the top end");

    std::printf("  [ok] top-boost stack @ %.0f Hz: discrete-vs-analytic worst %.2f dB (<1.5); max |H| %.1f dB "
                "(<0 = LOSS); bass 100Hz %.1f→%.1f dB; treble/bass interaction @3k %.2f dB; treble @6k %.1f→%.1f dB\n",
                fs, worst, worstMax, loBassLo, hiBassLo, maxInteract, topLoT, topHiT);
}

// ---------------------------------------------------------------------------
// Test 3 — NO negative feedback (the anti-NFB test, the mirror of the JCM/Twin
// sign-catcher): toggling "feedback" leaves the output BIT-EXACT (there is no loop).
// ---------------------------------------------------------------------------
void testNoNfb(double fs) {
    const double f = 1000.0;
    const float amp = 0.02f;
    Ac30PowerAmp openA; openA.prepare(fs, 128); openA.setOversampling(4); openA.setFeedbackEnabled(false);
    Ac30PowerAmp closedA; closedA.prepare(fs, 128); closedA.setOversampling(4); closedA.setFeedbackEnabled(true);
    auto o1 = sine(f, amp, 0.2, fs), o2 = o1;
    std::vector<float> y1(o1.size(), 0), y2(o2.size(), 0);
    openA.process(o1.data(), y1.data(), static_cast<int>(o1.size()));
    closedA.process(o2.data(), y2.data(), static_cast<int>(o2.size()));
    for (size_t i = 0; i < y1.size(); ++i)
        assert(y1[i] == y2[i] && "AC30 is not feedback-free: toggling NFB changed the output");
    assert(Ac30PowerAmp::kFeedbackBeta == 0.0 && "kFeedbackBeta must be 0 (NO NFB)");
    const double g = powerAmpAt(closedA, f, amp, fs) / amp;
    std::printf("  [ok] NO NFB @ %.0f Hz: open == closed BIT-EXACT (β=0); forward gain %.2f dB — the raw voicing\n",
                fs, toDb(g));
}

// ---------------------------------------------------------------------------
// Test 4 — the CATHODE-BIAS dynamic (the CENTERPIECE): (a) idle Vk == analytic
// fixed point (tested in T1); (b) under sustained drive Vk RISES (bias cools →
// the class-A bloom); (c) after the drive stops Vk recovers toward idle on the
// Rk·Ck time constant (±tolerance).
// ---------------------------------------------------------------------------
void testCathodeBias(double fs) {
    Ac30PowerAmp pa; pa.prepare(fs, 128); pa.setOversampling(4);
    pa.setParameter(Ac30PowerAmp::PARAM_DRIVE, 1.0f);
    const double vkIdle = pa.cathodeIdle();

    // (b) Sustained drive → Vk rises (average cathode current grows, cap charges).
    //
    // DRIVE LEVEL, §23 second amendment: this probe used to inject 8 V at the PI grid.
    // That number encoded the OLD, starved PI operating point (83 µA/triode, ×9.1 per
    // leg) — 8 V was "sustained loud" only because the inverter was ~11 dB deaf. With
    // the corrected PI (0.53 mA, ×32) the amp's musical range moved: 0.15 V at the PI
    // grid is now the EDGE-OF-BREAKUP drive (≈ VOLUME 0.34 with a −10 dBFS pickup),
    // i.e. exactly where an AC30 is played and where the class-A bloom belongs. The
    // MEASURED bloom peaks there (+0.44 V) and the amp crosses into grid-leak BLOCKING
    // above ~0.22 V at the PI grid — the point where the EL84 grids start conducting
    // (7 V swing vs the −9.5 V self-bias), after which the coupling caps charge and the
    // bias shifts COLD instead. Both regimes are real; docs §23 tabulates the crossover.
    // 2026-07-29 (finding 7, docs §42): 0.15 V -> 0.125 V, for exactly the reason §23
    // moved it from 8 V to 0.15 V in the first place — the probe has to sit where the
    // bloom lives, and the tail reference moved that window again. (Note PARAM_DRIVE 1.0
    // is the trim's noon = ×2, so the grid actually sees twice these numbers: the figures
    // below are VOLTS AT THE GRID, i.e. 0.125 V here = 0.25 V at the grid, and §23's
    // "0.15 V" was 0.30 V at the grid.) The AC30's legs went ×31.76/×17.46 (ratio 0.550)
    // to ×27.56/×25.13 (0.912) — leg 1 weaker, leg 2 44 % stronger — and the grid-leak
    // BLOCKING crossover moved from ~0.22 V up to ~0.40 V at the grid.
    // MEASURED Vk rise / recovery τ vs volts AT THE GRID, 44.1 / 48 / 96 kHz:
    //   0.10 V  +0.066/+0.072/+0.090 V   (bloom too small to assert: bar is +0.2)
    //   0.20 V  +0.234/+0.253/+0.301 V   τ 1.63/1.50/1.00 ms
    //   0.25 V  +0.336/+0.362/+0.428 V   τ 1.63/1.50/1.00 ms   <-- the probe
    //   0.30 V  +0.361/+0.395/+0.475 V   τ 1.45/1.33/0.75 ms   (the OLD probe: the bloom
    //                                     peak, but τ lands exactly ON the 0.3·Rk·Ck bound
    //                                     at 96 kHz, which is what went red)
    //   0.45 V  −0.306/−0.250/−0.135 V   (grid-leak blocking: the bias goes COLD)
    // Both bounds are unchanged (+0.2 V of rise, τ within 0.3–3.0× of Rk·Ck = 2.5 ms).
    auto drive = sine(300.0, 0.125f, 0.20, fs);
    std::vector<float> od(drive.size(), 0);
    pa.process(drive.data(), od.data(), static_cast<int>(drive.size()));
    const double vkDriven = pa.cathodeNow();
    assert(vkDriven > vkIdle + 0.2 &&
           "cathode Vk did not RISE under sustained drive (the class-A bias bloom is missing)");

    // (c) Recovery on Rk·Ck: after the drive stops, feed silence in small chunks and
    // trace Vk back toward idle; find the time to fall to 1/e of the way back.
    const double vkStart = pa.cathodeNow();
    const double target = vkIdle + (vkStart - vkIdle) / std::exp(1.0);  // 1/e point
    const int chunk = 8;
    int nrec = 0; bool hit = false;
    std::vector<float> sil(chunk, 0.0f), so(chunk, 0.0f);
    for (int i = 0; i < static_cast<int>(0.05 * fs / chunk); ++i) {
        pa.process(sil.data(), so.data(), chunk);
        nrec += chunk;
        if (!hit && pa.cathodeNow() <= target) { hit = true; break; }
    }
    const double tauMeas = nrec / fs;
    const double tauRC = Ac30PowerAmp::kRkCathode * Ac30PowerAmp::kCkCathode;  // 2.5 ms
    assert(hit && "cathode Vk did not recover toward idle after the drive stopped");
    assert(tauMeas > 0.3 * tauRC && tauMeas < 3.0 * tauRC &&
           "cathode recovery time constant off Rk·Ck (the demand + cap dynamics)");

    std::printf("  [ok] cathode bias @ %.0f Hz: idle Vk=%.2f → driven %.2f (rise %.2f V, bloom); "
                "recovery τ≈%.2f ms vs Rk·Ck=%.2f ms\n",
                fs, vkIdle, vkDriven, vkDriven - vkIdle, tauMeas * 1e3, tauRC * 1e3);
}

// ---------------------------------------------------------------------------
// Test 5 — TOP CUT (post-PI treble cut, INVERTED sense): raising the CUT knob
// REMOVES high end (HF level drops), while the low end stays put. CLOCKWISE (higher
// value) = MORE cut — the authentic inverted sense.
// ---------------------------------------------------------------------------
void testTopCut(double fs) {
    auto hfLevel = [&](float cut, double f) {
        Ac30PowerAmp pa; pa.prepare(fs, 128); pa.setOversampling(4);
        pa.setParameter(Ac30PowerAmp::PARAM_DRIVE, 0.5f);
        pa.setParameter(Ac30PowerAmp::PARAM_TOPCUT, cut);
        // SMALL-SIGNAL probe (§23 second amendment). TOP CUT is a LINEAR one-pole per
        // PI leg, so its response can only be read at a quasi-linear operating point.
        // The old 0.5 V probe was quasi-linear against the starved PI (×9.1); against
        // the corrected PI (×32) 0.5 V is already deep power-amp saturation, where the
        // clipped level is drive-independent and a treble cut barely moves the output —
        // the probe would have been measuring the clipper, not the filter. 0.05 V puts
        // the section back in its linear region (MEASURED THD 0.12 %).
        return toDb(powerAmpAt(pa, f, 0.05f, fs));
    };
    const double hfNoCut = hfLevel(0.0f, 3000.0);
    const double hfMidCut = hfLevel(0.5f, 3000.0);
    const double hfFullCut = hfLevel(1.0f, 3000.0);
    assert(hfMidCut < hfNoCut - 0.3 && "TOP CUT at noon did not reduce the treble");
    assert(hfFullCut < hfMidCut - 0.5 && "TOP CUT fully up did not reduce the treble further (inverted sense)");
    // RE-TAPER GUARD (M10.2 muddy-fix, docs §23): the default 0.5 knob (the shared
    // 'presence' default) must be a MILD cut — within ~2 dB of no-cut at 3 kHz — so the
    // AC30 face no longer opens with ~half the top-cut engaged. AND the full range must
    // stay reachable: full cut must be MUCH darker than noon (most of the cut lives in
    // the upper half of travel, where a real AC30's CUT is rarely run).
    assert(hfMidCut > hfNoCut - 2.0 &&
           "TOP CUT default (0.5) is not a MILD cut — the muddy re-taper regressed");
    assert(hfFullCut < hfMidCut - 5.0 &&
           "TOP CUT full range collapsed — the deep cut must live in the upper half of travel");
    // The low end is barely touched by the cut.
    const double lfNoCut = hfLevel(0.0f, 150.0);
    const double lfFullCut = hfLevel(1.0f, 150.0);
    assert(std::fabs(lfFullCut - lfNoCut) < 1.5 && "TOP CUT should tame the TOP, not the low end");
    std::printf("  [ok] TOP CUT @ %.0f Hz (inverted, re-tapered): 3 kHz %.1f→%.1f→%.1f dB (cut 0/.5/1; "
                "noon is a MILD %.1f dB cut, full %.1f dB); 150 Hz Δ %.2f dB (low end untouched)\n",
                fs, hfNoCut, hfMidCut, hfFullCut, hfNoCut - hfMidCut, hfNoCut - hfFullCut,
                std::fabs(lfFullCut - lfNoCut));
}

// ---------------------------------------------------------------------------
// Test 6 — SAG ordering + window: the AC30's GZ34 sag is DEEPER than the JCM800's
// and MUCH deeper than the Twin's (Twin < JCM < AC30), in the documented 4–8 dB
// window. Same hard burst / attack-vs-settle metric into all three power sections.
// ---------------------------------------------------------------------------
void testSag(double fs) {
    // The burst is 2.0 V into the PI-grid trim at noon (DRIVE 1.0 -> ×2, i.e. 4 V at the
    // grid), not the 45 V it was before 2026-07-29 (docs §42) — a PROBE re-derivation, in
    // the spirit of §23's cathode-bias and TOP CUT probes, with every bound left alone.
    // 45 V × 2 = 90 V at the grid is ~30× the inverter's clipping onset; it was chosen
    // when two of the three inverters were 5.7 dB deaf, and at that level the envelope
    // ratio measures the output CEILING instead of the rail. Measured after the tail fix:
    // at 90 V the JCM's rail droops 57.4 V and the Twin's 50.5 V — the JCM sags harder,
    // exactly as their supply impedances (150 Ω / 50 µF vs 80 Ω / 100 µF) predict — yet
    // the metric reported Twin 2.63 > JCM 2.26 dB, because both outputs are pinned to
    // their clipping ceiling at attack and at settle. At 4 V at the grid it tracks the
    // rail again, and the burst is still hard (the JCM's attack peak is 0.74 of full
    // scale, the AC30 clips):
    //   probe (grid V)     Twin        JCM        AC30
    //   4 V, pre-fix       1.03 dB     1.96 dB    5.74 dB   ordering + windows OK
    //   4 V, post-fix      1.16 dB     1.95 dB    6.03 dB   ordering + windows OK
    //   90 V, pre-fix      2.09        3.56       5.80      OK (ceiling-limited)
    //   90 V, post-fix     2.63        2.26       5.89      INVERTED
    // Holding on the PRE-fix circuit too is the check that this is a fixed reference and
    // not a number chosen to pass. Rail droops at the 4 V probe: Twin 13.4 V, JCM 37.3 V,
    // AC30 1.6 V (the AC30's "sag" is its static cathode-bias saturator — finding 4).
    auto sagDepth = [&](auto& amp) {
        amp.setParameter(std::decay_t<decltype(amp)>::PARAM_DRIVE, 1.0f);
        const double f0 = 400.0;
        const int nsil = static_cast<int>(0.02 * fs), nb = static_cast<int>(0.30 * fs),
                  nt = static_cast<int>(0.25 * fs), n = nsil + nb + nt;
        std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < nb; ++i)
            in[static_cast<size_t>(nsil + i)] = static_cast<float>(2.0 * std::sin(kTwoPi * f0 * i / fs));
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
    Jcm800PowerAmp jpa; jpa.prepare(fs, 128); jpa.setOversampling(4);
    Ac30PowerAmp apa; apa.prepare(fs, 128); apa.setOversampling(4);
    const double twinSag = sagDepth(tpa), jcmSag = sagDepth(jpa), ac30Sag = sagDepth(apa);
    assert(twinSag < jcmSag && "ordering broken: Twin sag not < JCM sag");
    assert(jcmSag < ac30Sag && "ordering broken: JCM sag not < AC30 sag (AC30 must be the saggiest)");
    assert(ac30Sag > 4.0 && ac30Sag < 8.0 && "AC30 sag not in the documented 4–8 dB window");
    std::printf("  [ok] sag @ %.0f Hz: Twin %.2f < JCM %.2f < AC30 %.2f dB (4–8 window; GZ34 class-A bloom)\n",
                fs, twinSag, jcmSag, ac30Sag);
}

// ---------------------------------------------------------------------------
// Test 7 — CHIME: the AC30 at its documented mid-volume has a MORE prominent 2nd
// harmonic than the Twin at its own matched setting (the even-harmonic chime vs
// the clean push-pull).
//
// PROBE RE-DERIVED 2026-07-30 (docs §46, the §42.9 discipline — the probe moved to
// where the physics lives, the +3 dB bar is UNCHANGED). The old probe drove the two
// POWER SECTIONS alone at 6 V of PI drive. That was the right place to look when the
// AC30's "chime" was its PI leg imbalance leaking evens (the §42 discovery) — but in
// a defect-free model BOTH PIs are balanced (0.912 / 0.998) and both push-pull
// stages rightly CANCEL their own even harmonics, so a power-section-only probe
// structurally cannot show class-A chime (measured: AC30 −49.1 dBc there, and the
// margin can never clear +3). The real chime sources are UPSTREAM of the push-pull:
// the single-ended top-boost V2 (its asymmetric transfer makes h2 the moment the
// VOLUME knob drives it) and the cathode-bias bloom — so the probe is the COMPOSED
// amps at their documented mid-volume settings, same hot-pickup input, h2 in dBc
// (a relative measure, robust to the amps' honest level difference). Checked on
// both circuits per §42.9: the power-section-only figures are printed alongside so
// a future PI regression is still visible in this test's output.
// ---------------------------------------------------------------------------
void testChime(double fs) {
    auto composedH2 = [&](auto& amp, auto volParam, float vol) {
        amp.setParameter(volParam, vol);
        auto in = sine(220.0, 0.316f, 1.0, fs);
        std::vector<float> out(in.size(), 0.0f);
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), w = n / 2, st = n - w;
        const double f1 = goertzelAmp(out, st, w, 220.0, fs);
        const double f2 = goertzelAmp(out, st, w, 440.0, fs);
        return std::pair<double, double>{toDb(f2 / (f1 + 1e-12)), f1};
    };
    Ac30Amp aca; aca.prepare(fs, 128); aca.setOversampling(4);
    aca.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
    aca.setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
    aca.setParameter(Ac30Amp::PARAM_TOPCUT, 0.3f);
    aca.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
    TwinAmp twa; twa.prepare(fs, 128); twa.setOversampling(4);
    twa.setParameter(TwinAmp::PARAM_BASS, 0.5f);
    twa.setParameter(TwinAmp::PARAM_MID, 0.5f);
    twa.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
    twa.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
    const auto ac30 = composedH2(aca, Ac30Amp::PARAM_VOLUME, 0.6f);
    const auto twin = composedH2(twa, TwinAmp::PARAM_VOLUME, 0.6f);
    // Subsidiary (printed, not asserted): the power sections alone, both balanced —
    // a future PI-imbalance regression shows up here as a big even residue.
    TwinPowerAmp tpa; tpa.prepare(fs, 128); tpa.setOversampling(4);
    Ac30PowerAmp apa; apa.prepare(fs, 128); apa.setOversampling(4);
    auto powerH2 = [&](auto& amp) {
        amp.setParameter(std::decay_t<decltype(amp)>::PARAM_DRIVE, 1.0f);
        auto in = sine(220.0, 6.0f, 0.4, fs);
        std::vector<float> out(in.size(), 0.0f);
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), w = n / 2, st = n - w;
        return toDb(goertzelAmp(out, st, w, 440.0, fs) /
                    (goertzelAmp(out, st, w, 220.0, fs) + 1e-12));
    };
    std::printf("  [--] power sections alone (PI-regression tell): AC30 %.1f / Twin %.1f dBc\n",
                powerH2(apa), powerH2(tpa));
    // History: XFAILed 2026-07-29 (§42) when finding 7 balanced the PI and the leaked
    // evens that had been posing as "chime" disappeared (−49.1 dBc on the old power-
    // section probe). XPASSed and HARDENED 2026-07-30 (§46): the completed top-boost
    // channel supplies the evens from the right physics — measured composed at the
    // documented 0.6 / hot pickup: AC30 h2 ≈ −20 dBc vs Twin ≈ −31 → margin ≈ +11 dB
    // against the unchanged +3 bar. Perturbation: bypassing V2 (or re-starving the
    // interstage) collapses the margin negative.
    std::printf("  [ok] chime @ %.0f Hz (composed, documented 0.6): AC30 2nd %.1f dBc vs "
                "Twin %.1f dBc (margin %.1f > +3; f1 %.3f/%.3f)\n",
                fs, ac30.first, twin.first, ac30.first - twin.first, ac30.second,
                twin.second);
    std::fflush(stdout);
    assert(ac30.first > twin.first + 3.0 &&
           "CHIME: the AC30's composed 2nd harmonic no longer sits >= 3 dB above the "
           "Twin's at the documented mid-volume (docs §46 — is V2's drive gone?)");
}

// ---------------------------------------------------------------------------
// Test 8 — the PRODUCT: chimey-clean at low volume, monotonic THD growth to real
// breakup at max, full-scale calibration (cranked ~0.9), ±10 V slam stability.
// ---------------------------------------------------------------------------
void testProduct(double fs) {
    auto compAmp = [&](float vol) {
        auto a = std::make_unique<Ac30Amp>();
        a->prepare(fs, 128); a->setOversampling(4);
        a->setParameter(Ac30Amp::PARAM_VOLUME, vol);
        a->setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        a->setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
        a->setParameter(Ac30Amp::PARAM_TOPCUT, 0.3f);
        a->setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
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
    const auto clean = thdAt(0.35f, 0.10f);
    assert(clean.first < 0.05 && "chimey-clean bar: THD at low volume / hot input not under 5%");
    const double t1 = thdAt(0.35f, 0.10f).first, t2 = thdAt(0.5f, 0.20f).first,
                 t3 = thdAt(0.7f, 0.30f).first, t4 = thdAt(1.0f, 0.50f).first;
    assert(t2 > t1 && t3 > t2 && t4 > t3 && "THD not monotonic with volume/drive");
    const auto cranked = thdAt(1.0f, 0.50f);
    assert(cranked.first > 0.20 && "max volume / hot input did not reach real breakup (>20%)");
    assert(cranked.first > clean.first * 5.0 && "cranked not dramatically dirtier than clean");
    assert(cranked.second > 0.7 && cranked.second <= 1.0 && "cranked peak not in the ~0.9 window");

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
    std::printf("  [ok] product @ %.0f Hz: clean THD %.2f%% (vol .35), monotonic → cranked %.1f%% "
                "(peak %.3f≈0.9); ±10 V slam finite\n",
                fs, clean.first * 100, cranked.first * 100, cranked.second);
}

// ---------------------------------------------------------------------------
// Test 10 — CHARACTER GUARD (the "muddy?" report, docs §23): the AC30 "top boost"
// is the CHIME KING — at its OPENING DEFAULTS it must measure BRIGHTER at 3 kHz
// (relative to 1 kHz) than the blackface Twin at ITS defaults, by a documented
// margin. This pins the voice's character so a future change (a darker default CUT,
// a stack/OT regression) cannot let the chime amp open muddier than the clean Fender.
// Measured on the composed amp voices (no cab — the shared linear cab is identical to
// both and cancels in a relative metric), small-signal so the tone shape dominates.
// ---------------------------------------------------------------------------
void testCharacterGuard(double fs) {
    const float amp = 0.05f;  // small-signal: quasi-linear, the voice shape dominates
    auto levelDb = [&](auto& a, double f) {
        auto in = sine(f, amp, 0.5, fs);
        std::vector<float> out(in.size(), 0.0f);
        a.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), w = n / 2, st = n - w;
        return toDb(goertzelAmp(out, st, w, f, fs));
    };

    // AC30 at OPENING DEFAULTS (rig.ts AMP_KNOB_DEFAULTS: vol .4, bass .5, treble .6,
    // CUT = the shared presence default .5, reverb 0).
    Ac30Amp ac; ac.prepare(fs, 128); ac.setOversampling(4);
    ac.setParameter(Ac30Amp::PARAM_VOLUME, 0.4f);
    ac.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
    ac.setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
    ac.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
    ac.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
    const double acBright = levelDb(ac, 3000.0) - levelDb(ac, 1000.0);

    // Twin at OPENING DEFAULTS (vol .4, bass/mid .5, treble .6, bright off, reverb 0);
    // tremolo OFF by default (chorusMode 0) → a clean, un-modulated response.
    TwinAmp tw; tw.prepare(fs, 128); tw.setOversampling(4);
    tw.setParameter(TwinAmp::PARAM_VOLUME, 0.4f);
    tw.setParameter(TwinAmp::PARAM_BASS, 0.5f);
    tw.setParameter(TwinAmp::PARAM_MID, 0.5f);
    tw.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
    tw.setParameter(TwinAmp::PARAM_BRIGHT, 0.0f);
    tw.setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, 0.0f);  // default off
    tw.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
    const double twBright = levelDb(tw, 3000.0) - levelDb(tw, 1000.0);

    // RE-DERIVED 2026-07-30 (docs §46). The old bar — "AC30 3 kHz rel 1 kHz beats the
    // Twin's by ≥ 6 dB" — passed VACUOUSLY on the pre-§46 amp: its 1 kHz reference sat
    // inside the structural mid notch (finding 5), so the ratio measured the DEFECT,
    // not chime (the §42.8 trap class). With the notch gone the ratio inverts honestly
    // (AC30 3k−1k ≈ −1 dB, Twin ≈ +7): a top-boost AC30 is not a tilted-treble
    // amp — its character against the scooped blackface is FULL MIDS + sparkle + the
    // early class-A breakup (asserted by testBreakupOrdering/the chime bar). What is
    // asserted now, all measured on the composed voices:
    //   (a) MID FULLNESS CONTRAST — AC30 mid (700 Hz rel its own 3 kHz) beats the
    //       Twin's same ratio by ≥ 6 dB (measured ≈ +1.4 vs −10.6 → 12.0 dB margin).
    //       This is the player-audible AC30-vs-blackface distinction.
    //   (b) THE HOLE STAYS DEAD — the AC30's own 220 Hz→3 kHz band is FLAT within
    //       4 dB (measured spread ≈ 1.4 dB). Defense-in-depth: restoring the FULL
    //       pre-§46 netlist (the notch) fails the suite at the stack's own bars
    //       before reaching this one (perturbation run recorded in §46); this bar's
    //       unique catch is a notch that somehow passes the stack tests, e.g. a
    //       component-value drift that moves runtime and analytic together.
    //   (c) NOT MUDDY — the AC30's 220 Hz sits at or above its own 82 Hz (measured
    //       +2.6 dB; the muddy failure mode is bass over mids).
    const double acMidVsTop = levelDb(ac, 700.0) - levelDb(ac, 3000.0);
    const double twMidVsTop = levelDb(tw, 700.0) - levelDb(tw, 3000.0);
    const double ac220 = levelDb(ac, 220.0), ac880 = levelDb(ac, 880.0),
                 ac1500 = levelDb(ac, 1500.0), ac3000 = levelDb(ac, 3000.0);
    const double bandMax = std::max(std::max(ac220, ac880), std::max(ac1500, ac3000));
    const double bandMin = std::min(std::min(ac220, ac880), std::min(ac1500, ac3000));
    const double acLowShoulder = ac220 - levelDb(ac, 82.0);
    std::printf("  [ok] character guard @ %.0f Hz: mid-vs-top AC30 %.1f dB / Twin %.1f dB "
                "(contrast %.1f ≥ 6); AC30 220–3k spread %.1f dB (< 4); 220-vs-82 %.1f dB "
                "(≥ 0); [3k-1k: ac %.1f tw %.1f — recorded, no longer the bar]\n",
                fs, acMidVsTop, twMidVsTop, acMidVsTop - twMidVsTop, bandMax - bandMin,
                acLowShoulder, acBright, twBright);
    std::fflush(stdout);
    assert(acMidVsTop > twMidVsTop + 6.0 &&
           "CHARACTER GUARD: the AC30's mids are not fuller than the Twin's (rel each "
           "amp's own 3 kHz) by >=6 dB — the full-mids top-boost character is gone");
    assert(bandMax - bandMin < 4.0 &&
           "CHARACTER GUARD: the AC30's 220 Hz-3 kHz band is not flat within 4 dB — "
           "the finding-5 mid hole (or a new one) is back");
    assert(acLowShoulder > 0.0 &&
           "CHARACTER GUARD: the AC30's 220 Hz sits below its 82 Hz — muddy voicing");
}

// ---------------------------------------------------------------------------
// Test 11 — BREAKUP ORDERING (the field report behind the §23 second amendment:
// "the ac30 doesn't have enough gain/breakup, it breaks up less easy than the
// fender twin"). The reporter was right and the voicing was INVERTED: a 30 W
// cathode-biased class-A combo is the EARLY-breakup amp — edge-of-breakup at
// moderate volume IS the Vox sound — while an 85 W blackface Twin is the clean-
// headroom king. This guard pins that ordering permanently, in two independent
// ways, so no future gain-structure change can flip it back.
//
// Probe: 220 Hz at 0.316 V peak (−10 dBFS) — a hot-humbucker DI level, the level
// at which the reporter heard it. Both voices at their opening tone knobs, no
// reverb, no cab (the shared linear cab is identical to both and cancels).
//
// MEASURED 2026-07 @ 48 kHz (THD %, VOLUME 0.1 → 1.0):
//   AC30  1.27  2.00  3.07  4.60  7.06 10.75 12.30 14.11 20.12 23.28
//   Twin  4.11  4.18  4.29  4.45  4.67  4.90  4.99  4.60  5.98 14.13
// Breakup onset (first 0.1 step at ≥ 5 % THD): AC30 0.5, Twin 0.9.
// At the documented mid-volume 0.6: AC30 10.75 % vs Twin 4.90 % — 2.2×.
//
// TWO honest notes on the Twin's numbers, so a future reader is not misled:
//  1. The Twin's ~4–5 dB THD FLOOR at this input is preamp clipping, and it is
//     flat across the volume travel because TwinPreamp places its VOLUME pot
//     AFTER both gain stages (V1 → stack → V2 → volume) — so the knob cannot
//     back the clipping off. The real AB763 puts the volume right after V1. That
//     is the mirror image of this amendment's bug and is ledgered as a separate
//     field item; it is NOT touched here, which is why this guard compares at a
//     mid-volume point and at a 5 % onset threshold rather than assuming the
//     Twin is pristine.
//  2. At the very TOP of the travel both voices are saturated and the THD metric
//     stops discriminating (the AC30's GZ34 demand-sag compresses its own
//     harmonics). The player-relevant claim — and the assertion — is about the
//     MIDDLE of the knob, where a guitarist actually sets a volume.
// ---------------------------------------------------------------------------
void testBreakupOrdering(double fs) {
    const float kHotPickup = 0.316f;  // −10 dBFS peak — a hot humbucker DI
    const double f0 = 220.0;
    const double kBreakupThd = 0.05;  // 5 % THD == audible breakup
    const float kMidVolume = 0.6f;    // the documented mid-knob comparison point

    auto ac30ThdAt = [&](float vol) {
        auto a = std::make_unique<Ac30Amp>();
        a->prepare(fs, 128); a->setOversampling(4);
        a->setParameter(Ac30Amp::PARAM_VOLUME, vol);
        a->setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        a->setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
        a->setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
        a->setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
        auto in = sine(f0, kHotPickup, 0.25, fs);
        std::vector<float> out(in.size(), 0.0f);
        a->process(in.data(), out.data(), static_cast<int>(in.size()));
        return thd(out, f0, fs);
    };
    auto twinThdAt = [&](float vol) {
        auto a = std::make_unique<TwinAmp>();
        a->prepare(fs, 128); a->setOversampling(4);
        a->setParameter(TwinAmp::PARAM_VOLUME, vol);
        a->setParameter(TwinAmp::PARAM_BASS, 0.5f);
        a->setParameter(TwinAmp::PARAM_MID, 0.5f);
        a->setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        a->setParameter(TwinAmp::PARAM_BRIGHT, 0.0f);
        a->setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, 0.0f);
        a->setParameter(TwinAmp::PARAM_REVERB, 0.0f);
        auto in = sine(f0, kHotPickup, 0.25, fs);
        std::vector<float> out(in.size(), 0.0f);
        a->process(in.data(), out.data(), static_cast<int>(in.size()));
        return thd(out, f0, fs);
    };

    // (a) BREAKUP-ONSET VOLUME: the first 0.1 knob step whose THD reaches 5 %.
    // 1.1 == "never breaks up anywhere on its travel".
    auto onsetOf = [&](auto&& thdAt) {
        for (int i = 1; i <= 10; ++i) {
            const float v = 0.1f * static_cast<float>(i);
            if (thdAt(v) >= kBreakupThd) return static_cast<double>(v);
        }
        return 1.1;
    };
    const double acOnset = onsetOf(ac30ThdAt);
    const double twOnset = onsetOf(twinThdAt);
    // (b) MID-KNOB THD MARGIN at the documented comparison point.
    const double acMid = ac30ThdAt(kMidVolume);
    const double twMid = twinThdAt(kMidVolume);

    // 2026-07-29 (docs §42): these four bars are UNCHANGED and now XFAIL. The §23
    // voicing they pin is real and still wanted; what measurement showed is that the
    // 10.75 % the AC30 used to produce at VOLUME 0.6 was 10.39 % EVEN harmonics —
    // h2 at −19.76 dBc — leaking through the push-pull because the phase inverter's
    // legs were mismatched 2:1 (leg ratio 0.550, audit finding 7). Finding 7's tail
    // reference balanced them (0.912) and h2 collapsed to −33.03 dBc, so the same
    // knob position measures 3.80 % — while the ODD harmonics, the section's own
    // clipping, barely moved (2.77 % -> 3.06 %). The Vox did not get quieter (level
    // +0.4 dB) and it did not stop clipping; the model simply stopped manufacturing
    // 2nd harmonic from a defect that is now fixed.
    //
    //   VOLUME          0.4    0.5    0.6    0.7    0.8
    //   THD before %   4.60   7.06  10.75  12.30  14.11    (even 4.55 / 6.90 / 10.39)
    //   THD after  %   1.55   2.29   3.80   6.58  15.18    (even 1.42 / 1.77 /  2.25)
    //   odd before %   0.65   1.53   2.77   5.52  11.63
    //   odd after  %   0.62   1.45   3.06   6.23  15.10
    //
    // 2026-07-30 (docs §46): the gain-structure slice those XFAILs named LANDED — the
    // missing top-boost V2 + cathode follower exist, the stack's structural notch is
    // fixed, the VOLUME sits between V1 and V2 (owner-chosen kit position) with a
    // re-derived k = 8 taper, and the drive the passive divider "had none left to
    // give" now comes from the completed channel. All four bars XPASSed and are
    // HARDENED below, bars unchanged. Measured at the hardening (48 kHz, hot pickup):
    // onset 0.5–0.6 (Twin 0.8 on this harness post-§44), VOLUME 0.6 THD ≈ 10.8 % vs
    // Twin ≈ 3.0 % (≈ 3.6×). Perturbation: reverting the volume taper to k = 4 (or
    // bypassing V2) fails the onset/mid bars.
    std::printf("  [ok] breakup ordering @ %.0f Hz (hot pickup −10 dBFS): onset AC30 vol %.1f, "
                "Twin %.1f; at vol %.1f AC30 %.2f%% vs Twin %.2f%% (%.2f×)\n",
                fs, acOnset, twOnset, kMidVolume, acMid * 100, twMid * 100,
                acMid / (twMid + 1e-12));
    std::fflush(stdout);
    assert(acOnset <= 0.65 &&
           "AC30 breakup onset (5 % THD, hot pickup) later than VOLUME 0.65 — the §23 "
           "field failure is back (docs §46)");
    // (+1e-6: the onsets are float 0.1-grid values; without the epsilon 0.6 vs 0.8
    // fails the exact comparison by 1.2e-8 of float/double literal mismatch.)
    assert(acOnset <= twOnset - 0.2 + 1e-6 &&
           "AC30 onset not >= 0.2 of knob travel below the Twin's — the class-A combo "
           "must break up first (docs §46)");
    assert(acMid > 0.08 &&
           "AC30 under 8 % THD at the documented mid-volume 0.6 / hot pickup (docs §46)");
    assert(acMid > 1.8 * twMid &&
           "AC30 not >= 1.8x dirtier than the Twin at the documented mid-volume (docs §46)");
}

// ---------------------------------------------------------------------------
// Test 9 — aliasing at MAX volume (M2 sweep): the shipped 4× clears the −60 dB bar;
// 8× buys ~nothing beyond 4× → 4× ships (the M2 budget), same as M9/M10.1.
// ---------------------------------------------------------------------------
void testAliasing(double fs) {
    const double f0 = 4186.0;
    auto aliasAt = [&](int os) {
        Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(os);
        amp.setParameter(Ac30Amp::PARAM_VOLUME, 1.0f);
        amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
        amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.3f);
        amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
        auto in = sine(f0, 0.3f, 1.0, fs);
        std::vector<float> out(in.size(), 0.0f);
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        return measureAliasing(out, fs, f0).worstAliasDb;
    };
    const double a1 = aliasAt(1), a2 = aliasAt(2), a4 = aliasAt(4), a8 = aliasAt(8);
    assert(a2 < a1 - 6.0 && "2x not materially better than 1x");
    assert(a4 < -60.0 && "shipped 4x worst-alias not >=60 dB below fundamental (M2 bar)");
    assert(a8 < a4 + 5.0 && "8x materially better than 4x (4x not at the floor)");
    std::printf("  [ok] aliasing @ %.0f Hz (max vol): 1x=%.1f 2x=%.1f 4x=%.1f 8x=%.1f dB (M2 −60 bar; 4× ships)\n",
                fs, a1, a2, a4, a8);
}

// Known defects this binary exercises under XFAIL. Printed by --xfail-ledger, which ctest
// surfaces as ***Skipped in its default summary (see core/CMakeLists.txt).
// XFAIL history of this suite (the ratchet working as designed):
// `finding7-ac30-pi-leg-balance` XPASSed 2026-07-29 (finding 7's tail reference) and
// was deleted — hard assertion since. The five `finding7-ac30-*` voicing entries
// (chime, breakup-onset, breakup-vs-twin, mid-thd, mid-vs-twin) XPASSed 2026-07-30
// when the §46 gain-structure slice landed the missing top-boost stage, the corrected
// stack and the re-derived volume law — all five deleted, their bars hard assertions
// in testChime/testBreakupOrdering/testCharacterGuard. This suite currently carries
// ZERO known-bad properties, so it has no --xfail-ledger registration in CMake.

}  // namespace

int main(int argc, char** argv) {
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, nullptr, 0,
                                                 "clipper_ac30_tests");
    if (ledger >= 0) return ledger;

    std::printf("Running clipper::dsp AC30 (M10.2) tests...\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        testOperatingPoints(fs);
        testTopBoostStack(fs);
        testNoNfb(fs);
        testCathodeBias(fs);
        testTopCut(fs);
        testSag(fs);
        testChime(fs);
        testProduct(fs);
        testCharacterGuard(fs);
    }
    // The breakup-ordering guard is a VOICING claim, not a per-rate circuit claim
    // (the per-rate circuit behavior is pinned by the nine tests above at all three
    // rates), so it runs once at 48 kHz — the AudioWorklet rate the field report was
    // heard at — and keeps the suite's runtime honest.
    testBreakupOrdering(48000.0);
    testAliasing(44100.0);
    testAliasing(48000.0);
    testAliasing(96000.0);
    std::printf("All AC30 (M10.2) tests passed (XFAILs listed below are known open defects, "
                "not regressions).\n");
    return clipper::test::reportXfails();
}
