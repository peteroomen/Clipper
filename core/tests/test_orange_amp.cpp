// Plain-assert tests for the M10.3 Orange OR120 "Overdrive" — the MID-FORWARD head.
// Covers OrangePreamp (two 12AX7 stages + the single VOLUME + the F.A.C. rotary + the
// James/passive-Baxandall stack), OrangePowerAmp (the DC-coupled driver + CATHODYNE
// phase inverter → EL34 QUAD → OT → global NFB with HF DRIVE → a stiff solid-state
// supply) and the composed OrangeAmp.
//
// No framework: int main + <cassert>. Every assert compares a MEASURED number against
// something derived independently IN THE TEST — the Koren load-line DC, the EL34
// fixed point, the netlist's own H(jω), the closed-vs-open-loop identity — or against
// ANOTHER AMP IN THE REPO, which is the one comparison that can catch a wrong
// topology (docs §29's standing complaint about same-netlist checks).
//
// THE LOAD-BEARING TEST IS testMidForwardVsJcm800. The OR120 is EL34 push-pull like
// the 2204 and shares its power machinery, so if the tone network came out
// Marshall-shaped the slice would have failed no matter what else passed. That test
// measures both stacks' own netlists and both composed amps' renders.

#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/OrangeAmp.h"
#include "clipper/dsp/OrangePowerAmp.h"
#include "clipper/dsp/OrangePreamp.h"

#include "measure/AliasMetric.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

using clipper::dsp::CathodyneInverter;
using clipper::dsp::JamesToneStack;
using clipper::dsp::Jcm800Amp;
using clipper::dsp::MarshallToneStack;
using clipper::dsp::OrangeAmp;
using clipper::dsp::OrangePowerAmp;
using clipper::dsp::OrangePreamp;
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
        s[static_cast<size_t>(i)] =
            amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
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

// Render one steady tone through a fresh, prepared OrangeAmp at the given knobs.
struct OrangeKnobs {
    double volume = 0.5, bass = 0.5, treble = 0.5, fac = 0.2, hfDrive = 0.5,
           reverb = 0.0;
};
std::vector<float> renderOrange(const OrangeKnobs& k, const std::vector<float>& in,
                                double fs) {
    OrangeAmp amp;
    amp.setOversampling(4);
    amp.prepare(fs, 128);
    amp.setParameter(OrangeAmp::PARAM_VOLUME, static_cast<float>(k.volume));
    amp.setParameter(OrangeAmp::PARAM_BASS, static_cast<float>(k.bass));
    amp.setParameter(OrangeAmp::PARAM_TREBLE, static_cast<float>(k.treble));
    amp.setParameter(OrangeAmp::PARAM_FAC, static_cast<float>(k.fac));
    amp.setParameter(OrangeAmp::PARAM_HF_DRIVE, static_cast<float>(k.hfDrive));
    amp.setParameter(OrangeAmp::PARAM_REVERB, static_cast<float>(k.reverb));
    std::vector<float> out(in.size(), 0.0f);
    amp.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

double rmsTail(const std::vector<float>& x) {
    const size_t st = x.size() / 2;
    double a = 0.0;
    for (size_t i = st; i < x.size(); ++i) a += double(x[i]) * double(x[i]);
    return std::sqrt(a / double(x.size() - st));
}

// The house probe level: 0.15 V peak at 220 Hz is a normal unity-trim pickup
// (docs §51 established it as the level a field report reproduces at).
constexpr double kProbeHz = 220.0;
constexpr float kProbeV = 0.15f;

// ===========================================================================
// 1. DC operating points
// ===========================================================================
void testDcOperatingPoints(double fs) {
    std::printf("\n[orange] DC operating points @ %.0f Hz\n", fs);
    OrangePreamp pre;
    pre.setOversampling(4);
    pre.prepare(fs, 128);

    for (int s = OrangePreamp::V1A; s <= OrangePreamp::V1B; ++s) {
        const auto& cfg = pre.stageConfig(s);
        const double va = pre.stageQuiescentPlate(s);
        const double vk = pre.stageQuiescentCathode(s);
        const double ipSolver = pre.stageQuiescentCurrent(s);
        // Standing current from OHM'S LAW on the plate load — independent of the
        // Koren law the solver used (the LtpProbe.h doctrine).
        const double ipOhm = (cfg.bPlus - va - vk) / cfg.Ra;
        const double frac = (va + vk) / cfg.bPlus;
        std::printf("  V1%c: Va(pk) %7.2f V  Vk %6.3f V  Ip %6.4f mA (ohm %6.4f)  "
                    "plate %.1f %% of B+\n",
                    s == OrangePreamp::V1A ? 'A' : 'B', va, vk, ipSolver * 1e3,
                    ipOhm * 1e3, frac * 100.0);
        // A 12AX7 gain stage biased in the middle of its load line.
        assert(ipOhm > 0.5e-3 && ipOhm < 1.6e-3);
        assert(frac > 0.4 && frac < 0.85);
        assert(std::fabs(ipOhm - ipSolver) < 0.05e-3);
    }
    std::printf("  V1B plate source impedance into the stack: %.0f ohm\n",
                pre.plateSourceImpedance());
    assert(pre.plateSourceImpedance() > 20.0e3 &&
           pre.plateSourceImpedance() < 80.0e3);

    OrangePowerAmp pow;
    pow.setOversampling(4);
    pow.prepare(fs, 128);
    const auto& inv = pow.inverter();
    const auto& ic = inv.config();
    const double ipd = inv.quiescentDriverCurrent();
    const double ipc = inv.quiescentCathodyneCurrent();
    std::printf("  driver:    Vp %7.2f V  Vk %6.3f V  Ip %6.4f mA\n",
                inv.quiescentDriverPlate(), inv.quiescentDriverCathode(), ipd * 1e3);
    std::printf("  cathodyne: Vk %7.2f V  Vplate %7.2f V  Ip %6.4f mA  Vgk %6.3f V\n",
                inv.quiescentCathodyneCathode(), inv.quiescentCathodynePlate(),
                ipc * 1e3,
                inv.quiescentDriverPlate() - inv.quiescentCathodyneCathode());
    // Both triodes in the project's documented window (0.5-0.9 mA/triode).
    assert(ipd > 0.5e-3 && ipd < 0.9e-3);
    assert(ipc > 0.5e-3 && ipc < 0.9e-3);
    // The split load is loaded symmetrically BY CONSTRUCTION: Ip*Rk == Vk and the
    // plate sits exactly B+ - Vk above it. Verified as an identity so a future edit
    // that breaks the equal-R assumption is caught here rather than in the audio.
    assert(std::fabs(ipc * ic.Rsplit - inv.quiescentCathodyneCathode()) < 1e-6);
    assert(std::fabs((ic.bPlusCathodyne - inv.quiescentCathodyneCathode()) -
                     inv.quiescentCathodynePlate()) < 1e-9);
    // The cathodyne is DC-coupled: its grid IS the driver's plate, so Vgk must be a
    // small negative bias, not an AC-coupled 0 V.
    const double vgkCath =
        inv.quiescentDriverPlate() - inv.quiescentCathodyneCathode();
    assert(vgkCath < 0.0 && vgkCath > -3.0);

    // The EL34 QUAD idle, against an independently-iterated fixed point.
    const double rail = pow.railIdle(), scr = pow.screenIdle();
    const double ip = pow.el34QuiescentPlateCurrent();
    const double ig2 = pow.el34QuiescentScreenCurrent();
    const double diss = rail * ip;
    std::printf("  EL34 quad: rail %7.2f V  screen %7.2f V  Ip %6.2f mA  Ig2 %5.2f mA"
                "  Pdiss %5.2f W (%.0f %% of 25 W)\n",
                rail, scr, ip * 1e3, ig2 * 1e3, diss, diss / 25.0 * 100.0);
    // Self-consistency of the rail against the four tubes' draw (Ohm's law on the
    // supply impedance, not the iterate).
    const double railCheck =
        OrangePowerAmp::kVsupply -
        OrangePowerAmp::kTubes * (ip + ig2) * OrangePowerAmp::kRsupply;
    assert(std::fabs(railCheck - rail) < 1e-3);
    // Inside the EL34's plate rating, and hot enough to be class AB rather than B.
    assert(diss < 25.0 && diss > 12.0);
    assert(ip > 25.0e-3 && ip < 45.0e-3);
}

// ===========================================================================
// 2. The cathodyne: anti-phase, EQUAL legs, by topology
// ===========================================================================
void testCathodyneBalance(double fs) {
    std::printf("\n[orange] cathodyne phase inverter\n");
    OrangePowerAmp pow;
    pow.setOversampling(4);
    pow.prepare(fs, 128);
    // Probe a FRESH inverter configured exactly like the shipped one (the LtpProbe
    // doctrine: measure the leg gains, do not read a constant).
    CathodyneInverter inv;
    inv.configure(pow.inverter().config());
    inv.prepare();
    const double vpq = inv.quiescentCathodynePlate();
    const double vkq = inv.quiescentCathodyneCathode();

    double gPlate = 0.0, gCath = 0.0, gSplit = 0.0, gDriver = 0.0;
    const double drive = 0.05;  // small signal
    {
        double vpA = 0, vkA = 0, vpB = 0, vkB = 0;
        // Settle the warm start at each operating point before reading.
        for (int i = 0; i < 64; ++i) inv.processSample(+drive, 0.0, vpA, vkA);
        const double vpdA = inv.driverPlateNow();
        for (int i = 0; i < 64; ++i) inv.processSample(-drive, 0.0, vpB, vkB);
        const double vpdB = inv.driverPlateNow();
        gPlate = (vpA - vpB) / (2.0 * drive);
        gCath = (vkA - vkB) / (2.0 * drive);
        // The DRIVER's own gain and the SPLIT LOAD's own gain, separated at the
        // node they share. CathodyneInverter is the pair, so the composite above
        // is the product of these two.
        gDriver = (vpdA - vpdB) / (2.0 * drive);
        gSplit = (vkA - vkB) / (vpdA - vpdB);
    }
    std::printf("  driver gain %+.3f   split-load gain %+.4f   composite %+.3f\n",
                gDriver, gSplit, gCath);
    // The driver carries ALL the gain (its 300 k plate load is why)...
    assert(std::fabs(gDriver) > 30.0 && std::fabs(gDriver) < 110.0);
    // ...and the split load has gain just under unity, which is the defining
    // property of a cathodyne and the reason the driver has to be that hot.
    assert(std::fabs(gSplit) > 0.85 && std::fabs(gSplit) < 1.0);
    const double ratio =
        std::min(std::fabs(gPlate), std::fabs(gCath)) /
        std::max(std::fabs(gPlate), std::fabs(gCath));
    std::printf("  leg gains: plate %+.5f  cathode %+.5f  |ratio| %.6f\n", gPlate,
                gCath, ratio);
    std::printf("  quiescent: plate %.2f V  cathode %.2f V (sum %.2f = B+c %.2f)\n",
                vpq, vkq, vpq + vkq, inv.config().bPlusCathodyne);
    // ANTI-PHASE: opposite signs, no exception.
    assert(gPlate * gCath < 0.0);
    // BALANCED BY TOPOLOGY, not by a fitted plate resistor. The 2204's LTP needed
    // audit finding 8 and a resistor sweep to reach 0.988 (docs §45); a split load
    // reads both legs off ONE current through TWO equal resistors, so the only way
    // this can drift is if the model stops implementing a split load.
    assert(ratio > 0.9999);
    // The plate and cathode nodes always sum to B+c (one current, two equal R).
    assert(std::fabs((vpq + vkq) - inv.config().bPlusCathodyne) < 1e-9);

    // COMPLIANCE clipping, the "stiffer, fuzzier" Orange clip: drive the grid far
    // past what the stage can follow and the cathode pins at its rails instead of
    // continuing to swing.
    double vpHi = 0, vkHi = 0, vpLo = 0, vkLo = 0;
    for (int i = 0; i < 256; ++i) inv.processSample(+400.0, 0.0, vpHi, vkHi);
    for (int i = 0; i < 256; ++i) inv.processSample(-400.0, 0.0, vpLo, vkLo);
    const double swingUp = vkHi - vkq, swingDown = vkq - vkLo;
    std::printf("  compliance: Vk slams to %.2f / %.2f V (idle %.2f) -> +%.1f / -%.1f V\n",
                vkHi, vkLo, vkq, swingUp, swingDown);
    assert(vkHi <= 0.5 * inv.config().bPlusCathodyne + 1e-9);
    assert(vkLo >= -1e-9);
    // Asymmetric by construction: the cutoff side has further to fall than the
    // conduction side has to rise. This is the number that differs from an LTP.
    assert(swingDown > swingUp);
}

// ===========================================================================
// 3. The James stack — discretization vs its own netlist
// ===========================================================================
void testJamesStackVsAnalytic(double fs) {
    std::printf("\n[orange] James stack: discrete MNA vs analytic H(jw) @ %.0f Hz\n",
                fs);
    // NOTE the standing limitation (docs §29): the analytic reference is derived
    // from the SAME netlist, so this validates the DISCRETIZATION only. What
    // validates the TOPOLOGY is testMidForwardVsJcm800, which compares against a
    // different amp's stack.
    const double rs = 38.0e3;
    const int facPos = 1;
    struct Case {
        double bass, treble;
    };
    const Case cases[] = {{0.5, 0.5}, {0.0, 0.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 1.0}};
    const double freqs[] = {82.0, 220.0, 440.0, 1000.0, 3000.0, 6000.0};
    double worst = 0.0;
    for (const Case& c : cases) {
        JamesToneStack st;
        st.prepare(fs);
        st.setSourceImpedance(rs);
        st.setFacPosition(facPos);
        st.setKnobs(c.bass, c.treble);
        st.snapKnobs();
        std::printf("  b=%.1f t=%.1f:", c.bass, c.treble);
        for (double f : freqs) {
            const std::vector<float> in = sine(f, 1.0f, 0.5, fs);
            std::vector<float> out(in.size(), 0.0f);
            st.process(in.data(), out.data(), static_cast<int>(in.size()));
            const size_t n = out.size(), w = n / 2, s0 = n - w;
            const double meas = goertzelAmp(out, s0, w, f, fs);
            // The MNA clamps the pot fraction to [1e-3, 1-1e-3]; mirror that in the
            // analytic call so the two describe the same network.
            const double b = std::min(std::max(c.bass, 1.0e-3), 1.0 - 1.0e-3);
            const double t = std::min(std::max(c.treble, 1.0e-3), 1.0 - 1.0e-3);
            const double want = JamesToneStack::magnitudeAt(f, b, t, rs, facPos);
            const double errDb = toDb(meas) - toDb(want);
            worst = std::max(worst, std::fabs(errDb));
            std::printf(" %6.0f:%+.2f", f, errDb);
        }
        std::printf("\n");
    }
    std::printf("  worst |error| = %.3f dB\n", worst);
    assert(worst < 0.35);
}

// ===========================================================================
// 4. THE BAR — mid-forward vs the JCM800
// ===========================================================================
// Metric: at NOON, the minimum response across 300-800 Hz relative to the mean of
// the 100 Hz and 4 kHz responses. Negative = a mid SCOOP (the FMV signature),
// positive = a mid BUMP. Scale-free, so the two stacks' different insertion losses
// cannot flatter either one.
double midNotchDb(double (*mag)(double)) {
    const double lo = toDb(mag(100.0));
    const double hi = toDb(mag(4000.0));
    double mid = 1e30;
    for (double f : {300.0, 400.0, 500.0, 600.0, 700.0, 800.0})
        mid = std::min(mid, toDb(mag(f)));
    return mid - 0.5 * (lo + hi);
}

double gJamesRs = 38.0e3;
int gJamesFac = 1;
double jamesMag(double f) {
    return JamesToneStack::magnitudeAt(f, 0.5, 0.5, gJamesRs, gJamesFac);
}

// Analytic H(jw) of the Marshall FMV netlist, written from MarshallToneStack's own
// documented component constants (docs §14) — an independent evaluation of the
// OTHER amp's stack, driven from its cathode follower's real 371 ohm impedance.
double marshallMag(double f) {
    using cd = std::complex<double>;
    const cd s(0.0, kTwoPi * f);
    auto cl = [](double v) { return v < 1.0 ? 1.0 : v; };
    const double t = 0.5, m = 0.5, b = 0.5, rs = 371.0;
    const double RT = MarshallToneStack::kRT, RB = MarshallToneStack::kRB,
                 RM = MarshallToneStack::kRM, R1 = MarshallToneStack::kRslope;
    const double Ct = MarshallToneStack::kCt, Cb = MarshallToneStack::kCb,
                 Cm = MarshallToneStack::kCm;
    constexpr int N = 5;  // IN, N2, N3, N4, OUT
    cd G[N][N + 1];
    for (auto& row : G)
        for (auto& e : row) e = cd(0.0, 0.0);
    auto stamp = [&](int a, int bb, cd g) {
        G[a][a] += g;
        G[bb][bb] += g;
        G[a][bb] -= g;
        G[bb][a] -= g;
    };
    G[0][0] += cd(1.0 / rs, 0.0);
    stamp(0, 1, s * Ct);
    stamp(0, 2, cd(1.0 / R1, 0.0));
    stamp(1, 4, cd(1.0 / cl((1.0 - t) * RT), 0.0));
    stamp(4, 2, cd(1.0 / cl(t * RT), 0.0));
    stamp(2, 3, cd(1.0 / cl(b * RB), 0.0));
    stamp(2, 3, s * Cb);
    G[3][3] += cd(1.0 / cl(m * RM), 0.0);
    G[3][3] += s * Cm;
    G[0][N] = cd(1.0 / rs, 0.0);
    for (int c = 0; c < N; ++c) {
        int piv = c;
        for (int r = c + 1; r < N; ++r)
            if (std::abs(G[r][c]) > std::abs(G[piv][c])) piv = r;
        for (int j = 0; j <= N; ++j) std::swap(G[piv][j], G[c][j]);
        const cd d = G[c][c];
        for (int j = c; j <= N; ++j) G[c][j] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const cd fq = G[r][c];
            for (int j = c; j <= N; ++j) G[r][j] -= fq * G[c][j];
        }
    }
    return std::abs(G[4][N]);
}

void testMidForwardVsJcm800(double fs) {
    std::printf("\n[orange] ===== THE BAR: mid-forward vs the JCM800 =====\n");

    // --- (a) the tone NETWORKS, at noon ------------------------------------
    const double orangeNotch = midNotchDb(&jamesMag);
    const double marshallNotch = midNotchDb(&marshallMag);
    std::printf("  network mid-notch @ noon:  Orange James %+.2f dB   "
                "Marshall FMV %+.2f dB   contrast %.2f dB\n",
                orangeNotch, marshallNotch, orangeNotch - marshallNotch);
    std::printf("       f     James    FMV\n");
    for (double f : {82.0, 220.0, 440.0, 660.0, 1000.0, 2200.0, 5000.0})
        std::printf("  %6.0f   %+6.2f  %+6.2f\n", f,
                    toDb(jamesMag(f)) - toDb(jamesMag(1000.0)),
                    toDb(marshallMag(f)) - toDb(marshallMag(1000.0)));
    // The FMV must measure as a SCOOP and the James as a BUMP. Two separate signs,
    // asserted separately, so a change that moved both together could not hide.
    assert(marshallNotch < -3.0);
    assert(orangeNotch > +1.0);
    // …and the CONTRAST, the hard bar in dB. Measured 8.28 dB; the bar is 6.0 with
    // the margin recorded here rather than snugged, because a future component-value
    // correction inside either stack is allowed to move it.
    assert(orangeNotch - marshallNotch > 6.0);

    // --- (b) the COMPOSED AMPS, rendered -----------------------------------
    // Same input, both amps at their tone knobs noon and at a clean level, and THE
    // SAME METRIC as (a) — min(330..660 Hz) relative to the mean of 110 Hz and
    // 4.4 kHz. Deliberately not "dB re 1 kHz": the FMV's own notch minimum sits at
    // ~1 kHz, so normalizing there would hide the very thing being measured.
    // This is the player-observable half of the bar: it includes every stage, the
    // power section and the feedback loop, not just the network.
    const double bands[] = {110.0, 220.0, 330.0, 440.0, 660.0, 1000.0, 2200.0, 4400.0};
    constexpr int kNb = 8;
    double oj[kNb] = {0}, mj[kNb] = {0};
    for (int i = 0; i < kNb; ++i) {
        const double f = bands[i];
        const std::vector<float> in = sine(f, 0.05f, 0.5, fs);
        OrangeKnobs k;
        k.volume = 0.35;
        const std::vector<float> o = renderOrange(k, in, fs);
        Jcm800Amp jcm;
        jcm.setOversampling(4);
        jcm.prepare(fs, 128);
        jcm.setParameter(Jcm800Amp::PARAM_GAIN, 0.35f);
        jcm.setParameter(Jcm800Amp::PARAM_MASTER, 0.5f);
        jcm.setParameter(Jcm800Amp::PARAM_BASS, 0.5f);
        jcm.setParameter(Jcm800Amp::PARAM_MID, 0.5f);
        jcm.setParameter(Jcm800Amp::PARAM_TREBLE, 0.5f);
        jcm.setParameter(Jcm800Amp::PARAM_PRESENCE, 0.5f);
        std::vector<float> m(in.size(), 0.0f);
        jcm.process(in.data(), m.data(), static_cast<int>(in.size()));
        const size_t n = o.size(), w = n / 2, s0 = n - w;
        oj[i] = toDb(goertzelAmp(o, s0, w, f, fs));
        mj[i] = toDb(goertzelAmp(m, s0, w, f, fs));
    }
    auto composedNotch = [&](const double* a) {
        double mid = 1e30;
        for (int i = 2; i <= 4; ++i) mid = std::min(mid, a[i]);  // 330/440/660
        return mid - 0.5 * (a[0] + a[7]);                        // 110 and 4400
    };
    std::printf("  composed amps, dB re each amp's own 660 Hz:\n");
    std::printf("       f    Orange     JCM\n");
    for (int i = 0; i < kNb; ++i)
        std::printf("  %6.0f   %+6.2f  %+6.2f\n", bands[i], oj[i] - oj[4],
                    mj[i] - mj[4]);
    const double oNotch = composedNotch(oj), mNotch = composedNotch(mj);
    std::printf("  composed mid-notch:  Orange %+.2f dB   JCM %+.2f dB   "
                "contrast %.2f dB\n",
                oNotch, mNotch, oNotch - mNotch);
    // The whole amp, not just the network: the Orange must sit mid-FORWARD where
    // the Marshall sits mid-scooped, and the gap has to be real. Measured 5.9 dB;
    // the bar is 4.0, with the margin recorded rather than snugged.
    assert(oNotch > 0.0);
    assert(mNotch < -1.5);
    assert(oNotch - mNotch > 4.0);
}

// ===========================================================================
// 5. No master volume: breakup tracks the VOLUME knob (the docs §46 convention)
// ===========================================================================
void testBreakupTracksVolume(double fs) {
    std::printf("\n[orange] breakup onset vs VOLUME (0.15 V / 220 Hz probe)\n");
    const std::vector<float> in = sine(kProbeHz, kProbeV, 0.6, fs);
    const double knobs[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.85, 1.0};
    double prevThd = -1.0, prevRms = -1.0;
    double onset = -1.0;
    for (double v : knobs) {
        OrangeKnobs k;
        k.volume = v;
        const std::vector<float> o = renderOrange(k, in, fs);
        const double t = thd(o, kProbeHz, fs) * 100.0;
        const double r = 20.0 * std::log10(rmsTail(o) + 1e-12);
        std::printf("  VOLUME %.2f: THD %6.2f %%   RMS %7.2f dBFS\n", v, t, r);
        // MONOTONIC in both: a knob that goes backwards is a defect a player finds
        // immediately, and it is exactly what a bad taper or a mis-placed volume
        // pot produces (docs §43/§44).
        if (prevThd >= 0.0) {
            assert(t > prevThd - 0.25);
            assert(r > prevRms - 0.30);
        }
        if (onset < 0.0 && t >= 5.0) onset = v;
        prevThd = t;
        prevRms = r;
    }
    std::printf("  >=5 %% THD onset at VOLUME %.2f\n", onset);
    // The amp has NO master volume, so the onset IS the design statement: clean in
    // the bottom third, breaking up around the middle, roaring at the top — the
    // same window the AC30's volume law was chosen against (docs §46).
    assert(onset > 0.40 && onset <= 0.70);
    // Clean at the bottom and genuinely saturated at the top.
    {
        OrangeKnobs lo;
        lo.volume = 0.15;
        OrangeKnobs hi;
        hi.volume = 1.0;
        const double tLo = thd(renderOrange(lo, in, fs), kProbeHz, fs) * 100.0;
        const double tHi = thd(renderOrange(hi, in, fs), kProbeHz, fs) * 100.0;
        std::printf("  clean end %.2f %%   cranked end %.2f %%\n", tLo, tHi);
        assert(tLo < 2.0);
        assert(tHi > 15.0);
    }
}

// ===========================================================================
// 6. The F.A.C. rotary — a real high-pass that walks
// ===========================================================================
void testFacSwitch(double fs) {
    std::printf("\n[orange] F.A.C. — six positions\n");
    const double lowE = 82.0;
    double prevLf = 1e30, prevTilt = -1e30;
    for (int pos = 0; pos < JamesToneStack::kFacPositions; ++pos) {
        const double knob =
            double(pos) / double(JamesToneStack::kFacPositions - 1);
        OrangeKnobs k;
        k.volume = 0.3;
        k.fac = knob;
        const std::vector<float> lo = renderOrange(k, sine(lowE, 0.1f, 0.5, fs), fs);
        const std::vector<float> hi =
            renderOrange(k, sine(1000.0, 0.1f, 0.5, fs), fs);
        const size_t n = lo.size(), w = n / 2, s0 = n - w;
        const double aLo = toDb(goertzelAmp(lo, s0, w, lowE, fs));
        const double aHi = toDb(goertzelAmp(hi, s0, w, 1000.0, fs));
        std::printf("  pos %d (C = %8.1f pF): low E %7.2f dB   1 kHz %7.2f dB   "
                    "tilt %+6.2f dB\n",
                    pos, JamesToneStack::kFacCaps[pos] * 1e12, aLo, aHi, aHi - aLo);
        // Every click to the right takes low end away, and never adds any.
        assert(aLo < prevLf + 0.05);
        // …and the amp gets THINNER, i.e. the 1 kHz-to-low-E tilt only grows.
        assert(aHi - aLo > prevTilt - 0.05);
        prevLf = aLo;
        prevTilt = aHi - aLo;
    }
    // The two ends must be a real switch, not a nuance.
    OrangeKnobs k0;
    k0.volume = 0.3;
    k0.fac = 0.0;
    OrangeKnobs k5;
    k5.volume = 0.3;
    k5.fac = 1.0;
    const std::vector<float> a = renderOrange(k0, sine(lowE, 0.1f, 0.5, fs), fs);
    const std::vector<float> b = renderOrange(k5, sine(lowE, 0.1f, 0.5, fs), fs);
    const size_t n = a.size(), w = n / 2, s0 = n - w;
    const double drop =
        toDb(goertzelAmp(a, s0, w, lowE, fs)) - toDb(goertzelAmp(b, s0, w, lowE, fs));
    std::printf("  low-E drop across the whole switch: %.2f dB\n", drop);
    assert(drop > 10.0);
}

// ===========================================================================
// 7. Global feedback + HF DRIVE
// ===========================================================================
void testFeedbackAndHfDrive(double fs) {
    std::printf("\n[orange] global NFB (driver cathode) + HF DRIVE\n");
    const std::vector<float> in = sine(440.0, 0.02f, 0.4, fs);
    auto gainOf = [&](bool fb) {
        OrangePowerAmp p;
        p.setOversampling(4);
        p.prepare(fs, 128);
        p.setFeedbackEnabled(fb);
        p.setParameter(OrangePowerAmp::PARAM_HF_DRIVE, 0.0f);
        std::vector<float> o(in.size(), 0.0f);
        p.process(in.data(), o.data(), static_cast<int>(in.size()));
        const size_t n = o.size(), w = n / 2, s0 = n - w;
        return goertzelAmp(o, s0, w, 440.0, fs);
    };
    const double closed = gainOf(true), open = gainOf(false);
    const double depth = toDb(open) - toDb(closed);
    std::printf("  open-loop %.5f   closed-loop %.5f   loop depth %.2f dB "
                "(divider beta = %.4f)\n",
                open, closed, depth, 1.5e3 / (1.5e3 + 27.0e3));
    // NEGATIVE feedback: the loop must REDUCE the gain. This is what pins the
    // injection sign at the driver cathode (see OrangePowerAmp.h §3).
    assert(closed < open);
    // A moderately fed-back British amp: present, not a hi-fi loop.
    assert(depth > 2.0 && depth < 12.0);

    // HF DRIVE lifts the top by removing HF feedback.
    auto hfAt = [&](double p, double f) {
        OrangeKnobs k;
        k.volume = 0.3;
        k.hfDrive = p;
        const std::vector<float> o = renderOrange(k, sine(f, 0.05f, 0.5, fs), fs);
        const size_t n = o.size(), w = n / 2, s0 = n - w;
        return toDb(goertzelAmp(o, s0, w, f, fs));
    };
    const double lo0 = hfAt(0.0, 220.0), hi0 = hfAt(0.0, 5000.0);
    const double lo1 = hfAt(1.0, 220.0), hi1 = hfAt(1.0, 5000.0);
    std::printf("  HF DRIVE 0: 220 Hz %.2f  5 kHz %.2f (tilt %+.2f)\n", lo0, hi0,
                hi0 - lo0);
    std::printf("  HF DRIVE 1: 220 Hz %.2f  5 kHz %.2f (tilt %+.2f)\n", lo1, hi1,
                hi1 - lo1);
    std::printf("  HF lift across the control: %+.2f dB\n",
                (hi1 - lo1) - (hi0 - lo0));
    // The control must move the TILT, not just the level (a level-only "presence"
    // is dead UI — CLAUDE.md's own rule).
    assert((hi1 - lo1) - (hi0 - lo0) > 0.75);
}

// ===========================================================================
// 8. Antialiasing at the shipped 4x
// ===========================================================================
void testAliasing(double fs) {
    // The house composed-amp probe (test_jcm800_power.cpp's): 4186 Hz at 0.3 V peak
    // into a fully cranked amp. Deliberately the SAME stimulus as the JCM's so the
    // two floors are comparable numbers rather than two conventions.
    std::printf("\n[orange] alias floor @ %.0f Hz (cranked, 4186 Hz / 0.3 V)\n", fs);
    const double f0 = 4186.0;
    const std::vector<float> in = sine(f0, 0.3f, 1.0, fs);
    double a[4] = {0, 0, 0, 0};
    int idx = 0;
    for (int factor : {1, 2, 4, 8}) {
        OrangeAmp amp;
        amp.setOversampling(factor);
        amp.prepare(fs, 128);
        amp.setParameter(OrangeAmp::PARAM_VOLUME, 1.0f);
        amp.setParameter(OrangeAmp::PARAM_BASS, 0.5f);
        amp.setParameter(OrangeAmp::PARAM_TREBLE, 0.5f);
        amp.setParameter(OrangeAmp::PARAM_FAC, 0.2f);
        amp.setParameter(OrangeAmp::PARAM_HF_DRIVE, 0.5f);
        std::vector<float> o(in.size(), 0.0f);
        amp.process(in.data(), o.data(), static_cast<int>(in.size()));
        a[idx] = measureAliasing(o, fs, f0).worstAliasDb;
        std::printf("  %dx: worst alias %.1f dB\n", factor, a[idx]);
        ++idx;
    }
    // Same COMPOUND floor the JCM documents (docs §18/§45): at max everything the
    // amp's own harmonics reach past Nyquist and intermodulate, so this is not a
    // pure foldover number and 8x does not rescue it. The bar is set at the MEASURED
    // floor with margin, and 4x must still be a large improvement on 1x — that
    // second clause is what would catch an oversampler that stopped working, which
    // an absolute bar on a compound floor cannot.
    // Measured on the shipped 4x: 48 kHz -67.1 dB, 44.1 kHz -59.2 dB. The bar is
    // -56, i.e. 3 dB under the WORSE of the two rates — the same margin structure
    // docs §45 uses for the JCM, whose identical probe measures -54.7 at 48 kHz and
    // carries a -52 bar. This voice therefore clears the M2 -60 target at 48 k and
    // sits just under it at 44.1 k, on a COMPOSED amp at maximum volume.
    assert(a[2] < -56.0);
    assert(a[2] < a[0] - 12.0);
}

// ===========================================================================
// 9. DC offset ON SIGNAL (docs §29 / support/DcOffset.h)
// ===========================================================================
void testDcOffsetOnSignal(double fs) {
    std::printf("\n[orange] DC offset on SIGNAL\n");
    const int n = static_cast<int>(0.6 * fs);
    for (float offset : {0.0f, 0.1f}) {
        std::vector<float> in(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                offset + 0.15f * static_cast<float>(std::sin(kTwoPi * 220.0 * i / fs));
        OrangeKnobs k;
        k.volume = 0.7;
        const std::vector<float> o = renderOrange(k, in, fs);
        const auto d = clipper::test::measureDcOnSignal(o);
        std::printf("  input offset %+.2f V: out mean %+.5f  peak %.5f  %.3f %% of peak\n",
                    offset, d.mean, d.peak, d.fraction * 100.0);
        // Every coupling cap in the chain plus the OT's own LF corner: the amp must
        // not pass DC on to the cab, with OR without an offset at its input.
        assert(d.fraction < 0.01);
    }
}

// ===========================================================================
// 10. The Orange 4x12 cab — different from the Brit, on purpose
// ===========================================================================
void testOrangeCab(double fs) {
    std::printf("\n[orange] Orange 4x12 IR vs Brit 4x12 @ %.0f Hz\n", fs);
    const std::vector<float> o = clipper::dsp::generateOrange4x12IR(fs);
    const std::vector<float> b = clipper::dsp::generateBrit4x12IR(fs);
    auto mag = [&](const std::vector<float>& h, double f) {
        const double w = kTwoPi * f / fs;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < h.size(); ++i) {
            re += h[i] * std::cos(w * double(i));
            im -= h[i] * std::sin(w * double(i));
        }
        return toDb(std::sqrt(re * re + im * im));
    };
    // Both IRs are peak-normalized to unity (M6.6), so ABSOLUTE dB is the fair
    // comparison between them; the "re its own 1 kHz" column would be read through
    // whichever cab happens to peak there, which for the Orange is the bark itself.
    std::printf("       f    Orange    Brit   (absolute, both unity-peak)\n");
    for (double f : {60.0, 100.0, 200.0, 500.0, 1000.0, 1200.0, 3000.0, 8000.0})
        std::printf("  %6.0f   %+6.2f  %+6.2f\n", f, mag(o, f), mag(b, f));
    // The LOW-CUT CORNER: the frequency at which each IR has fallen 6 dB below its
    // own 300 Hz level. That is a property of the box's high-pass alone and is not
    // confounded by either cab's midrange voicing, which a "60 Hz re 1 kHz" number
    // very much is.
    auto corner6 = [&](const std::vector<float>& h) {
        const double ref = mag(h, 300.0) - 6.0;
        double best = 20.0;
        for (int i = 0; i < 400; ++i) {
            const double f = 20.0 * std::pow(300.0 / 20.0, i / 399.0);
            if (mag(h, f) >= ref) { best = f; break; }
        }
        return best;
    };
    const double oC = corner6(o), bC = corner6(b);
    std::printf("  -6 dB low corner: Orange %.1f Hz   Brit %.1f Hz\n", oC, bC);
    assert(oC < bC - 3.0);
    // The BARK: more 1.2 kHz relative to 200 Hz than the Brit has — the cab voiced
    // in the same direction as the amp's stack.
    const double oBark = mag(o, 1200.0) - mag(o, 200.0);
    const double bBark = mag(b, 1200.0) - mag(b, 200.0);
    std::printf("  1.2 kHz-minus-200 Hz: Orange %+.2f dB   Brit %+.2f dB\n", oBark,
                bBark);
    assert(oBark > bBark + 2.0);
    // Still a 4x12: dark up top, and peak-normalized to unity (M6.6).
    assert(mag(o, 8000.0) - mag(o, 1000.0) < -30.0);
    double peak = 0.0;
    for (int i = 0; i < 512; ++i) {
        const double f = 40.0 * std::pow(16000.0 / 40.0, i / 511.0);
        peak = std::max(peak, std::pow(10.0, mag(o, f) / 20.0));
    }
    std::printf("  spectral peak %.6f (M6.6 unity)\n", peak);
    assert(peak > 0.98 && peak < 1.02);
}

// ===========================================================================
// 11. reset() re-parks without re-solving, and the block size does not matter
// ===========================================================================
void testResetAndBlocking(double fs) {
    std::printf("\n[orange] reset() + ragged blocking\n");
    const std::vector<float> in = sine(220.0, 0.15f, 0.4, fs);
    OrangeAmp a;
    a.setOversampling(4);
    a.prepare(fs, 128);
    a.setParameter(OrangeAmp::PARAM_VOLUME, 0.6f);
    std::vector<float> whole(in.size(), 0.0f);
    a.process(in.data(), whole.data(), static_cast<int>(in.size()));

    // Same amp, reset, re-rendered in 128-frame blocks — the worklet's convention.
    a.reset();
    std::vector<float> blocked(in.size(), 0.0f);
    for (size_t off = 0; off < in.size(); off += 128) {
        const int n = static_cast<int>(std::min<size_t>(128, in.size() - off));
        a.process(in.data() + off, blocked.data() + off, n);
    }
    double worst = 0.0;
    const size_t st = blocked.size() / 4;  // past the settle
    for (size_t i = st; i < blocked.size(); ++i)
        worst = std::max(worst, std::fabs(double(whole[i]) - double(blocked[i])));
    std::printf("  reset + 128-frame blocks vs one call: worst |diff| %.3e\n", worst);
    assert(worst < 2e-3);

    // Everything finite, always.
    for (float v : blocked) assert(std::isfinite(v));

    // The tone stack's cap companions all rest at EXACTLY zero (docs §33): a
    // subnormal parked there is invisible in the float output and costs ~68x on the
    // Marshall stack, and this network has EIGHT of them. Driven, then silenced —
    // a never-driven stack would prove nothing.
    JamesToneStack stack;
    stack.prepare(fs);
    stack.setSourceImpedance(38.0e3);
    stack.setKnobs(0.5, 0.5);
    stack.snapKnobs();
    {
        const std::vector<float> tone = sine(220.0, 1.0f, 0.2, fs);
        std::vector<float> o(tone.size(), 0.0f);
        stack.process(tone.data(), o.data(), static_cast<int>(tone.size()));
        assert(stack.maxAbsRestingState() > 0.0);  // it really was excited
        std::vector<float> quiet(static_cast<size_t>(4.0 * fs), 0.0f);
        std::vector<float> o2(quiet.size(), 0.0f);
        stack.process(quiet.data(), o2.data(), static_cast<int>(quiet.size()));
    }
    const double rest = stack.maxAbsRestingState();
    std::printf("  James stack max |resting state| after 4 s of silence: %.3e\n",
                rest);
    assert(rest == 0.0);
}

}  // namespace

int main() {
    // Unbuffered: an assert() aborts, and a buffered table is exactly the
    // measurement you need to see when it does.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clipper::test::requireAssertsLive();
    for (double fs : {44100.0, 48000.0}) {
        std::printf("\n================ Orange OR120, fs = %.0f ================\n",
                    fs);
        testDcOperatingPoints(fs);
        testCathodyneBalance(fs);
        testJamesStackVsAnalytic(fs);
        testResetAndBlocking(fs);
    }
    const double fs = 48000.0;
    testMidForwardVsJcm800(fs);
    testBreakupTracksVolume(fs);
    testFacSwitch(fs);
    testFeedbackAndHfDrive(fs);
    testAliasing(fs);
    testAliasing(44100.0);
    testDcOffsetOnSignal(fs);
    testOrangeCab(48000.0);
    testOrangeCab(44100.0);
    std::printf("\n[orange] all OR120 tests passed\n");
    return 0;
}
