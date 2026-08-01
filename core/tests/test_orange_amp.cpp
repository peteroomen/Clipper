// Plain-assert tests for the M10.3 Orange OR120 "Overdrive" — the MID-FORWARD head,
// AS TRANSCRIBED FROM THE OWNER-SUPPLIED SCHEMATICS (docs §57, schematic correction
// 2026-07-31). Covers OrangePreamp (V1A -> James stack -> GAIN -> 330p -> V1B ->
// 68n -> F.A.C.), OrangePowerAmp (the driver, the AC-COUPLED CATHODYNE, the H.F.
// Boost R-L-C in the driver's cathode, the EL34 QUAD, the OT, and global NFB from
// the 16 ohm tap) and the composed OrangeAmp.
//
// No framework: int main + <cassert>. Every assert compares a MEASURED number against
// something derived independently IN THE TEST — the Koren load-line DC, Ohm's law on
// the transcribed dropper chain, the EL34 fixed point, the netlist's own H(jω), the
// closed-vs-open-loop identity — or against ANOTHER AMP IN THE REPO, which is the one
// comparison that can catch a wrong topology (docs §29's standing complaint).
//
// THE LOAD-BEARING TEST IS testMidForwardVsJcm800. The OR120 is EL34 push-pull like
// the 2204 and shares its power machinery, so if the tone network came out
// Marshall-shaped the voice would have failed no matter what else passed.

#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/OrangeAmp.h"
#include "clipper/dsp/OrangePowerAmp.h"
#include "clipper/dsp/OrangePreamp.h"

#include "measure/AliasMetric.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"
#include "support/Xfail.h"

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
using clipper::dsp::FacNetwork;
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

using clipper::test::expectXfail;

// The ONE property the schematic correction cost this voice, measured rather than
// bargained down. The reconstruction's composed alias floor at 44.1 kHz / 4x was
// -59.2 dB against a -56 bar; the transcribed circuit measures -50.8 on the same
// stimulus. It is genuine foldover, not the rail-clipping signature (it improves
// 11 dB going to 8x), and it is not the new resonant H.F. Boost network (the floor
// is the same at boost 0, 0.5 and 1.0). The bar is NOT loosened.
const clipper::test::XfailDecl kXfailOrangeAlias44k1{
    "orange-schematic-alias-44k1",
    "docs §57 schematic correction (2026-07-31)",
    "the composed OR120 at maximum volume must hold a -56 dB alias floor at the "
    "shipped 4x oversampling on a 44.1 kHz grid, as it does at 48 kHz",
    "NOT the shared OS domain: that candidate was BUILT AND RUN on this amp "
    "2026-08-01 and measured WORSE (4x 44.1 kHz -50.8 -> -48.7 dB, 48 kHz -73.0 "
    "-> -67.8), while the identical change fixed the Rockerverb's twin defect "
    "(-52.7 -> -72.3). Re-owned to the CATHODYNE, whose compliance clip (Vk "
    "pinned to [0, C+/2], docs §57.3) is the hardest nonlinearity in the lineup. "
    "Docs §57.7 amendment / §63.14. NEVER a lower bar",
};
const clipper::test::XfailDecl kLedger[] = {kXfailOrangeAlias44k1};

// ===========================================================================
// 1. DC operating points, and the TRANSCRIBED dropper chain
// ===========================================================================
void testDcOperatingPoints(double fs) {
    std::printf("\n[orange] DC operating points @ %.0f Hz\n", fs);

    OrangePowerAmp pow;
    pow.setOversampling(4);
    pow.prepare(fs, 128);

    // --- the EL34 QUAD idle, against an independently-iterated fixed point ----
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
    // The screens sit behind the TRANSCRIBED per-tube 1k with no bypass cap. The
    // Ohm's-law form of this is an identity (the code computes it that way), so
    // what is asserted is the ABSOLUTE drop the transcribed resistor must produce:
    // an EL34 at this rail and bias draws 2-6 mA of screen current, so a 1 k
    // resistor must drop 2-6 V. A wrong resistor value lands outside that window.
    const double scrDrop = rail - scr;
    std::printf("  screen drop across the transcribed per-tube 1k: %.3f V "
                "(ig2 %.3f mA)\n",
                scrDrop, ig2 * 1e3);
    assert(scrDrop > 2.0 && scrDrop < 6.0);
    // Inside the EL34's plate rating, and hot enough to be class AB rather than B.
    assert(diss < 25.0 && diss > 12.0);
    assert(ip > 25.0e-3 && ip < 45.0e-3);

    // --- the transcribed 33K dropper chain -----------------------------------
    const auto& inv = pow.inverter();
    const auto& ic = inv.config();
    const double ipd = inv.quiescentDriverCurrent();
    const double ipc = inv.quiescentCathodyneCurrent();
    const double cPlus = pow.cPlusIdle();
    std::printf("  supply chain: B+ %7.3f -> 33k -> C+ %7.3f V   (driver %.4f + "
                "cathodyne %.4f mA)\n",
                rail, cPlus, ipd * 1e3, ipc * 1e3);
    // The Ohm's-law form of this is an identity (the code computes C+ that way), so
    // the assertion is the ABSOLUTE ladder the transcribed 33 K must produce. A
    // 12AX7 with a 100 k plate load cannot draw less than ~0.75 mA here and the
    // cathodyne's is pinned at C+/(4*Rsplit), so the pair must draw at least
    // 1.5 mA and the drop cannot be under 33 k x 1.5 mA = 49.5 V.
    assert(cPlus < rail - 49.5);
    assert(cPlus > rail - 150.0);
    assert(std::fabs(ic.bPlus - cPlus) < 1e-6);  // both plate loads return to C+

    std::printf("  driver:    Vp %7.2f V  Vk %6.3f V  Ip %6.4f mA\n",
                inv.quiescentDriverPlate(), inv.quiescentDriverCathode(), ipd * 1e3);
    std::printf("  cathodyne: Vk %7.2f V  Vplate %7.2f V  Ip %6.4f mA  Vgrid %7.3f V"
                "  Vgk %6.3f V  bias tap %.1f ohm\n",
                inv.quiescentCathodyneCathode(), inv.quiescentCathodynePlate(),
                ipc * 1e3, inv.quiescentCathodyneGrid(),
                inv.quiescentCathodyneGrid() - inv.quiescentCathodyneCathode(),
                inv.biasTapOhms());
    // Both currents come off the SAME transcribed 100k loads from the SAME rail, so
    // their windows are derived from the load line rather than quoted:
    //   driver    — Ra 100k, Rk 1k5 from C+ = a classic ECC83 gain stage. Ohm's law
    //               on the plate load must reproduce the solver's current.
    //   cathodyne — Vkc is C+/4 BY CONSTRUCTION (the compliance-centre bias, see
    //               OrangePowerAmp.h), so Ipc == C+/(4*Rsplit) is an identity and is
    //               asserted as one.
    // NOTE the movement this correction makes, and it is reported rather than
    // hidden: the reconstruction's 300k/180k loads from 320/400 V put both triodes
    // in the project's 0.5-0.9 mA gain-stage window; the transcribed 100k/100k from
    // 417 V put the driver at ~1.45 mA and the cathodyne at ~1.04 mA. A cathodyne is
    // not a gain stage and the window never applied to it.
    const double ipdOhm = (ic.bPlus - inv.quiescentDriverPlate()) / ic.Rad;
    assert(std::fabs(ipdOhm - ipd) < 1e-8);
    assert(ipd > 1.0e-3 && ipd < 2.0e-3);
    assert(std::fabs(ipc - ic.bPlus / (4.0 * ic.Rsplit)) < 1e-12);
    // The AC-COUPLED cathodyne's grid sits at its own bias tap, NOT at the driver's
    // plate: this is the assertion that would fail if a future edit re-introduced
    // the DC coupling the reconstruction had.
    assert(std::fabs(inv.quiescentCathodyneGrid() - inv.quiescentDriverPlate()) >
           50.0);
    const double vgkCath =
        inv.quiescentCathodyneGrid() - inv.quiescentCathodyneCathode();
    assert(vgkCath < 0.0 && vgkCath > -3.0);
    // THE ABSOLUTE ACCEPTANCE CHECK ON THE DERIVED BIAS TAP (docs §57.3): the
    // cathode can only fall to 0 V, so its idle voltage IS the available downward
    // swing, and it must clear the transcribed EL34 fixed bias or the amp could
    // never drive its output tubes to grid conduction at all.
    std::printf("  cathodyne down-swing %.2f V vs the EL34s' %.1f V of fixed bias\n",
                inv.quiescentCathodyneCathode(), -OrangePowerAmp::kVbias);
    assert(inv.quiescentCathodyneCathode() > -OrangePowerAmp::kVbias);

    // --- the preamp, on the D+ the same chain produces ------------------------
    OrangePreamp pre;
    pre.setSupplyCPlus(cPlus);
    pre.setOversampling(4);
    pre.prepare(fs, 128);
    std::printf("  supply chain: C+ %7.3f -> 33k -> D+ %7.3f V\n", cPlus,
                pre.supplyDPlus());
    double preampDraw = 0.0;
    for (int s = OrangePreamp::V1A; s <= OrangePreamp::V1B; ++s) {
        const auto& cfg = pre.stageConfig(s);
        const double va = pre.stageQuiescentPlate(s);
        const double vk = pre.stageQuiescentCathode(s);
        const double ipSolver = pre.stageQuiescentCurrent(s);
        // Standing current from OHM'S LAW on the plate load — independent of the
        // Koren law the solver used (the LtpProbe.h doctrine).
        const double ipOhm = (cfg.bPlus - va - vk) / cfg.Ra;
        const double frac = (va + vk) / cfg.bPlus;
        preampDraw += ipOhm;
        std::printf("  V1%c: Va(pk) %7.2f V  Vk %6.3f V  Ip %6.4f mA (ohm %6.4f)  "
                    "plate %.1f %% of D+\n",
                    s == OrangePreamp::V1A ? 'A' : 'B', va, vk, ipSolver * 1e3,
                    ipOhm * 1e3, frac * 100.0);
        // A 12AX7 gain stage biased in the middle of its 220k load line.
        assert(std::fabs(cfg.Ra - 220.0e3) < 1.0);
        assert(std::fabs(cfg.Rk - 2.2e3) < 1e-9);
        assert(ipOhm > 0.5e-3 && ipOhm < 1.0e-3);
        assert(frac > 0.4 && frac < 0.85);
        assert(std::fabs(ipOhm - ipSolver) < 0.05e-3);
    }
    // …and D+ is that draw through the transcribed second 33K. Two checks, neither
    // an identity: the rail-chain arithmetic must reproduce the SOLVED D+ (the two
    // use different equations — the dropper is bisected on the bare Koren law, the
    // stage's own DC solve includes its grid network, and they agree to 0.27 V),
    // and the ladder must be a real one (33 k x two triodes at >= 0.5 mA each).
    std::printf("  D+ from Ohm's law on the transcribed 33K: %.3f V (solved %.3f)\n",
                cPlus - OrangePreamp::kRdropDplus * preampDraw, pre.supplyDPlus());
    assert(std::fabs((cPlus - OrangePreamp::kRdropDplus * preampDraw) -
                     pre.supplyDPlus()) < 0.5);
    assert(pre.supplyDPlus() < cPlus - 33.0);
    assert(pre.supplyDPlus() > cPlus - 100.0);
    std::printf("  V1A plate source impedance into the James stack: %.0f ohm\n",
                pre.plateSourceImpedance());
    std::printf("  V1B plate source impedance into the F.A.C.:      %.0f ohm\n",
                pre.facSourceImpedance());
    // A real high-Z plate source — the OR120 has no cathode follower anywhere,
    // where the JCM's stack is driven from a 371 ohm follower.
    assert(pre.plateSourceImpedance() > 20.0e3 &&
           pre.plateSourceImpedance() < 80.0e3);
}

// ===========================================================================
// 2. The cathodyne: anti-phase, EQUAL legs, by topology — and AC-coupled
// ===========================================================================
void testCathodyneBalance(double fs) {
    std::printf("\n[orange] cathodyne phase inverter (AC-coupled driver)\n");
    OrangePowerAmp pow;
    pow.setOversampling(4);
    pow.prepare(fs, 128);
    // Probe a FRESH inverter configured exactly like the shipped one (the LtpProbe
    // doctrine: measure the leg gains, do not read a constant).
    CathodyneInverter inv;
    inv.configure(pow.inverter().config());
    inv.prepare(fs * 4.0);
    const double vpq = inv.quiescentCathodynePlate();
    const double vkq = inv.quiescentCathodyneCathode();

    double gPlate = 0.0, gCath = 0.0, gSplit = 0.0, gDriver = 0.0;
    const double drive = 0.05;  // small signal
    {
        // A SINE, not a DC step. The stage is AC-COUPLED now (68n into 1M, tau ~68
        // ms), so a step decays through the coupling cap and a step probe would
        // report whatever settle length it happened to use — measured 0.95 at 3 ms
        // and 0.76 at 23 ms from identical code. A steady tone has no such
        // ambiguity. Signed in-phase amplitudes, so ANTI-PHASE is still visible.
        const double osRate = fs * 4.0;
        const double f = 440.0;
        const int cycles = 64;
        const int n = static_cast<int>(cycles * osRate / f);
        std::vector<double> sp(n), sc(n), sd(n);
        double vp = 0, vk = 0;
        for (int i = 0; i < n; ++i) {  // settle first
            const double x = drive * std::sin(kTwoPi * f * i / osRate);
            inv.processSample(x, 0.0, vp, vk);
        }
        for (int i = 0; i < n; ++i) {
            const double x = drive * std::sin(kTwoPi * f * i / osRate);
            inv.processSample(x, 0.0, vp, vk);
            sp[i] = vp;
            sc[i] = vk;
            sd[i] = inv.driverPlateNow();
        }
        auto inPhase = [&](const std::vector<double>& s) {
            double acc = 0.0;
            for (int i = 0; i < n; ++i) acc += s[i] * std::sin(kTwoPi * f * i / osRate);
            return 2.0 * acc / n;
        };
        gPlate = inPhase(sp) / drive;
        gCath = inPhase(sc) / drive;
        gDriver = inPhase(sd) / drive;
        gSplit = inPhase(sc) / inPhase(sd);
    }
    std::printf("  driver gain %+.3f   split-load gain %+.4f   composite %+.3f\n",
                gDriver, gSplit, gCath);
    // The driver carries ALL the gain. Its transcribed plate load is 100k (against
    // the reconstruction's invented 300k), so this number is ~30 % lower than the
    // first release measured — reported, not compensated.
    assert(std::fabs(gDriver) > 25.0 && std::fabs(gDriver) < 80.0);
    // …and the split load has gain just under unity, which is the defining
    // property of a cathodyne.
    assert(std::fabs(gSplit) > 0.85 && std::fabs(gSplit) < 1.0);
    const double ratio =
        std::min(std::fabs(gPlate), std::fabs(gCath)) /
        std::max(std::fabs(gPlate), std::fabs(gCath));
    std::printf("  leg gains: plate %+.5f  cathode %+.5f  |ratio| %.6f\n", gPlate,
                gCath, ratio);
    std::printf("  quiescent: plate %.2f V  cathode %.2f V (sum %.2f = C+ %.2f)\n",
                vpq, vkq, vpq + vkq, inv.config().bPlus);
    // ANTI-PHASE: opposite signs, no exception.
    assert(gPlate * gCath < 0.0);
    // BALANCED BY TOPOLOGY, not by a fitted plate resistor. The 2204's LTP needed
    // audit finding 8 and a resistor sweep to reach 0.988 (docs §45); a split load
    // reads both legs off ONE current through TWO equal resistors, so the only way
    // this can drift is if the model stops implementing a split load. Splitting the
    // joint Newton into an AC-coupled pair did NOT move it.
    assert(ratio > 0.9999);
    // The plate and cathode nodes always sum to C+ (one current, two equal R).
    assert(std::fabs((vpq + vkq) - inv.config().bPlus) < 1e-9);

    // COMPLIANCE clipping. Drive the grid far past what the stage can follow and
    // the cathode pins at its rails instead of continuing to swing. The rails are
    // the TRANSCRIBED ones — 0 and C+/2 — and are asserted as exact pins.
    double vpHi = 0, vkHi = 0, vpLo = 0, vkLo = 0;
    for (int i = 0; i < 1024; ++i) inv.processSample(+400.0, 0.0, vpHi, vkHi);
    for (int i = 0; i < 1024; ++i) inv.processSample(-400.0, 0.0, vpLo, vkLo);
    // The driver INVERTS, so a positive grid slam drives the cathodyne's cathode DOWN.
    const double swingDown = vkq - vkHi, swingUp = vkLo - vkq;
    std::printf("  compliance: Vk slams to %.4f / %.4f V (idle %.4f) -> -%.2f / +%.2f V"
                "  (asymmetry %.2f %%)\n",
                vkHi, vkLo, vkq, swingDown, swingUp,
                100.0 * std::fabs(swingUp - swingDown) / swingDown);
    // The conduction side pins EXACTLY at 0 (the clamp bites); the cutoff side
    // approaches C+/2 from below and can never exceed it, because past that the
    // plate would sit under the cathode.
    assert(vkHi >= 0.0 && vkHi < 1e-9);
    assert(vkLo <= 0.5 * inv.config().bPlus + 1e-9);
    assert(vkLo > 0.5 * inv.config().bPlus - 1.0);
    // A FIRST-RELEASE CLAIM THIS CORRECTION REFUTES, reported rather than kept:
    // the DC-coupled reconstruction clipped hard asymmetrically (-132.7 / +67.3 V)
    // because the driver's plate pinned its grid off-centre. The transcribed
    // AC-coupled stage is biased at the CENTRE of its own compliance, so the two
    // limits are within a fraction of a percent of each other. Assert what is now
    // TRUE — the excursions are symmetric to better than 1 % — so a future edit
    // that de-centres the bias tap is caught here.
    assert(std::fabs(swingUp - swingDown) / swingDown < 0.01);
    // …and both must clear the EL34s' fixed bias, or the amp cannot make power.
    assert(swingDown > -OrangePowerAmp::kVbias);
    assert(swingUp > -OrangePowerAmp::kVbias);
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
    const double rs = 45.0e3;
    struct Case {
        double bass, treble, gain;
    };
    const Case cases[] = {{0.5, 0.5, 0.5}, {0.0, 0.0, 0.5}, {1.0, 1.0, 0.5},
                          {1.0, 0.0, 1.0}, {0.0, 1.0, 0.2}};
    const double freqs[] = {82.0, 220.0, 440.0, 1000.0, 3000.0, 6000.0};
    double worst = 0.0;
    for (const Case& c : cases) {
        JamesToneStack st;
        st.prepare(fs);
        st.setSourceImpedance(rs);
        st.setKnobs(c.bass, c.treble, c.gain);
        st.snapKnobs();
        std::printf("  b=%.1f t=%.1f g=%.1f:", c.bass, c.treble, c.gain);
        for (double f : freqs) {
            const std::vector<float> in = sine(f, 1.0f, 0.5, fs);
            std::vector<float> out(in.size(), 0.0f);
            st.process(in.data(), out.data(), static_cast<int>(in.size()));
            const size_t n = out.size(), w = n / 2, s0 = n - w;
            const double meas = goertzelAmp(out, s0, w, f, fs);
            // The MNA clamps the pot fraction to [1e-3, 1-1e-3]; mirror that in the
            // analytic call so the two describe the same network.
            auto cl = [](double v) { return std::min(std::max(v, 1.0e-3), 1.0 - 1.0e-3); };
            const double want = JamesToneStack::magnitudeAt(
                f, cl(c.bass), cl(c.treble), cl(c.gain), rs,
                JamesToneStack::Probe::Grid);
            const double errDb = toDb(meas) - toDb(want);
            worst = std::max(worst, std::fabs(errDb));
            std::printf(" %6.0f:%+.2f", f, errDb);
        }
        std::printf("\n");
    }
    std::printf("  worst |error| = %.3f dB\n", worst);
    // The worst cell is always TREBLE at minimum, 6 kHz, 44.1 kHz — i.e. BILINEAR
    // FREQUENCY WARPING, not a modelling error. tan(pi*f/fs)/(pi*f/fs) at 6 kHz on
    // a 44.1 kHz grid is 1.0672, so the discrete pole sits 6.7 % high, which on the
    // treble branch's rolloff is ~0.5 dB. Reported movement: the reconstruction's
    // network measured 0.291 dB worst and shipped a 0.35 bound; the transcribed one
    // measures 0.481 because its treble pot is 1M (not 250k) and the extreme is
    // sharper. The bound is the warp figure with margin.
    assert(worst < 0.55);

    // The F.A.C. network gets the same treatment, on its own netlist.
    std::printf("  F.A.C. network: discrete vs analytic\n");
    double worstFac = 0.0;
    for (int pos = 0; pos < FacNetwork::kPositions; ++pos) {
        FacNetwork fn;
        fn.prepare(fs);
        fn.setSourceImpedance(rs);
        fn.setPosition(pos);
        std::printf("    pos %d:", pos);
        for (double f : {82.0, 220.0, 1000.0, 6000.0}) {
            const std::vector<float> in = sine(f, 1.0f, 0.4, fs);
            std::vector<float> out(in.size(), 0.0f);
            fn.process(in.data(), out.data(), static_cast<int>(in.size()));
            const size_t n = out.size(), w = n / 2, s0 = n - w;
            const double errDb = toDb(goertzelAmp(out, s0, w, f, fs)) -
                                 toDb(FacNetwork::magnitudeAt(f, pos, rs));
            worstFac = std::max(worstFac, std::fabs(errDb));
            std::printf(" %6.0f:%+.2f", f, errDb);
        }
        std::printf("\n");
    }
    std::printf("  worst |error| = %.3f dB\n", worstFac);
    assert(worstFac < 0.35);
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

double gJamesRs = 45.0e3;
double gJamesGain = 0.5;
double jamesMag(double f) {
    // Probe::Out — the TONE NETWORK's own output node (the treble wiper, loaded by
    // the GAIN pot), which is the fair analogue of the FMV's output node.
    return JamesToneStack::magnitudeAt(f, 0.5, 0.5, gJamesGain, gJamesRs,
                                       JamesToneStack::Probe::Out);
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
    {
        // Drive the James network from the real V1A plate impedance and the real
        // noon GAIN wiper, both measured rather than assumed.
        OrangePreamp pre;
        pre.setOversampling(4);
        pre.prepare(fs, 128);
        gJamesRs = pre.plateSourceImpedance();
        gJamesGain = OrangePreamp::volumeTaper(0.5);
    }

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
    // REPORTED MOVEMENT, and it is the one place the schematic correction cost this
    // voice a margin. The reconstruction's invented James measured +2.32 dB and
    // shipped a +1.0 bound; the TRANSCRIBED network measures +0.75, because its
    // bass pot sits on a 22k leg (not 100k) and its treble branch is flatter. §57.4
    // said in terms that these one-sided margins were recorded rather than snugged
    // and that "a future component-value correction inside either stack is allowed
    // to move them" — so the SIGN stays hard here and the teeth move to the
    // CONTRAST bar below, which is UNCHANGED at 6.0 and now has less margin, not
    // more.
    assert(orangeNotch > 0.0);
    // …and the CONTRAST, the hard bar in dB, UNCHANGED by this correction.
    assert(orangeNotch - marshallNotch > 6.0);

    // KNOB AUTHORITY, added by the schematic correction because the perturbation
    // run found the mid-notch metric alone could not see a collapsed bass pot: with
    // kRB at 10 k instead of the transcribed 1 M the network is swamped by the two
    // 100 k series resistors, the BASS control does almost nothing, and every bar
    // above still passed. A control that does nothing is dead UI (CLAUDE.md's own
    // rule), so both pots' travel is now measured at the frequency each one owns.
    {
        auto at = [&](double f, double b, double t) {
            return toDb(JamesToneStack::magnitudeAt(f, b, t, gJamesGain, gJamesRs,
                                                    JamesToneStack::Probe::Out));
        };
        const double bassTravel = at(82.0, 0.999, 0.5) - at(82.0, 0.001, 0.5);
        const double trebTravel = at(5000.0, 0.5, 0.999) - at(5000.0, 0.5, 0.001);
        std::printf("  knob authority: BASS %+.2f dB at 82 Hz   TREBLE %+.2f dB at "
                    "5 kHz\n",
                    bassTravel, trebTravel);
        assert(bassTravel > 8.0);
        assert(trebTravel > 20.0);
    }

    // --- (b) the COMPOSED AMPS, rendered -----------------------------------
    // Same input, both amps at their tone knobs noon and at a clean level, and THE
    // SAME METRIC as (a) — min(330..660 Hz) relative to the mean of 110 Hz and
    // 4.4 kHz. Deliberately not "dB re 1 kHz": the FMV's own notch minimum sits at
    // ~1 kHz, so normalizing there would hide the very thing being measured.
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
    // the Marshall sits mid-scooped, and the gap has to be real. The correction
    // IMPROVED this half (6.24 -> 7.76 dB), because the transcribed 330p at V1B's
    // grid and the transcribed F.A.C. take the bottom out of the Orange where the
    // FMV is boosting it. Bars UNCHANGED.
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
    // same window the AC30's volume law was chosen against (docs §46). On the
    // corrected circuit this is NOT set by a constant: kInterstageScale is 1.0 (an
    // un-fitting — the model carries the whole physical path now), and the onset
    // lands here on its own.
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
    std::printf("\n[orange] F.A.C. — six TRANSCRIBED positions "
                "(through / 4n7 / 4n7 / 2n2 / 1n / 330p)\n");
    const double lowE = 82.0;
    double prevLf = 1e30, prevTilt = -1e30;
    for (int pos = 0; pos < FacNetwork::kPositions; ++pos) {
        const double knob = double(pos) / double(FacNetwork::kPositions - 1);
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
                    pos, FacNetwork::kCaps[pos] * 1e12, aLo, aHi, aHi - aLo);
        // Every click to the right takes low end away, and never adds any.
        assert(aLo < prevLf + 0.05);
        // …and the amp gets THINNER, i.e. the 1 kHz-to-low-E tilt only grows.
        assert(aHi - aLo > prevTilt - 0.05);
        prevLf = aLo;
        prevTilt = aHi - aLo;
    }
    // TRANSCRIPTION NOTE, asserted so it cannot be silently "tidied": positions 1
    // and 2 carry the SAME 4n7 on BOTH factory sheets (C19 and C20). SW2 is a
    // 2-pole 6-way, so the second pole plausibly does something the single-line
    // transcription does not carry; the ladder is shipped literally.
    assert(FacNetwork::kCaps[1] == FacNetwork::kCaps[2]);
    assert(FacNetwork::kCaps[0] == 0.0);  // the straight-through click
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
// 7. Global feedback + the H.F. BOOST choke network
// ===========================================================================
void testFeedbackAndHfBoost(double fs) {
    std::printf("\n[orange] global NFB (16 ohm tap -> 15k -> driver cathode) + "
                "H.F. BOOST\n");
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
    {
        OrangePowerAmp p;
        p.setOversampling(4);
        p.prepare(fs, 128);
        std::printf("  open-loop %.5f   closed-loop %.5f   loop depth %.2f dB "
                    "(divider beta = %.4f, x%.4f for the 16 ohm tap)\n",
                    open, closed, depth, p.feedbackDivider(),
                    OrangePowerAmp::kFbTapRatio);
    }
    // NEGATIVE feedback: the loop must REDUCE the gain. This is what pins the
    // injection sign at the driver cathode (see OrangePowerAmp.h §3).
    assert(closed < open);
    // A moderately fed-back British amp: present, not a hi-fi loop.
    assert(depth > 2.0 && depth < 12.0);

    // The H.F. BOOST is a series R-L-C in the driver's CATHODE, resonant at
    //   1/(2*pi*sqrt(L*C)) = 5.19 kHz
    // — an ABSOLUTE number straight off the two transcribed component values, and
    // the reason this control cannot be a one-pole shelf.
    {
        CathodyneInverter::Config c;
        const double fRes = 1.0 / (kTwoPi * std::sqrt(c.Lboost * c.Cboost));
        std::printf("  boost branch: L %.2f mH + C %.3f uF -> series resonance "
                    "%.1f Hz, Q %.2f at full boost\n",
                    c.Lboost * 1e3, c.Cboost * 1e6, fRes,
                    std::sqrt(c.Lboost / c.Cboost) / c.RchokeDcr);
        assert(c.Lboost > 0.0);  // there IS an inductor here
        assert(fRes > 4500.0 && fRes < 6000.0);
    }

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
    std::printf("  H.F. BOOST 0: 220 Hz %.2f  5 kHz %.2f (tilt %+.2f)\n", lo0, hi0,
                hi0 - lo0);
    std::printf("  H.F. BOOST 1: 220 Hz %.2f  5 kHz %.2f (tilt %+.2f)\n", lo1, hi1,
                hi1 - lo1);
    std::printf("  HF lift across the control: %+.2f dB   (220 Hz moved %+.2f dB)\n",
                (hi1 - lo1) - (hi0 - lo0), lo1 - lo0);
    // The control must move the TILT, not just the level (a level-only "presence"
    // is dead UI — CLAUDE.md's own rule).
    assert((hi1 - lo1) - (hi0 - lo0) > 0.75);
    // …and it must leave the low end where it was: the branch is a cathode bypass
    // whose own reactance keeps it out of the bass.
    assert(std::fabs(lo1 - lo0) < 2.0);
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
    // 4x must be a large improvement on 1x — the clause that would catch an
    // oversampler that stopped working, which an absolute bar on a compound floor
    // cannot. Hard at BOTH rates.
    assert(a[2] < a[0] - 12.0);
    // The absolute bar is the one the reconstruction shipped, -56 dB, and it is NOT
    // loosened: at 48 kHz the corrected amp clears it with room (-73.0). At
    // 44.1 kHz it does NOT — see the XFAIL. Attribution was measured, not guessed:
    // the floor is -52.0 / -50.8 / -50.7 dB at H.F. BOOST 0 / 0.5 / 1.0, so the new
    // resonant cathode network is NOT the cause; and it MOVES with the factor
    // (1x -15.2, 4x -50.8, 8x -61.8), so it is genuine foldover rather than the
    // rail-clipping signature docs §54 describes.
    if (fs > 46000.0) {
        assert(a[2] < -56.0);
    } else {
        expectXfail(a[2] < -56.0, kXfailOrangeAlias44k1,
                    "4x = -50.8 dB at 44.1 kHz (48 kHz measures -73.0); 8x reaches "
                    "-61.8, so the shortfall is foldover the shipped factor does not "
                    "clear");
    }
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
    std::printf("       f    Orange    Brit   (absolute, both unity-peak)\n");
    for (double f : {60.0, 100.0, 200.0, 500.0, 1000.0, 1200.0, 3000.0, 8000.0})
        std::printf("  %6.0f   %+6.2f  %+6.2f\n", f, mag(o, f), mag(b, f));
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
    const double oBark = mag(o, 1200.0) - mag(o, 200.0);
    const double bBark = mag(b, 1200.0) - mag(b, 200.0);
    std::printf("  1.2 kHz-minus-200 Hz: Orange %+.2f dB   Brit %+.2f dB\n", oBark,
                bBark);
    assert(oBark > bBark + 2.0);
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
// 11. reset() re-parks without re-solving, blocking, and the denormal rests
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

    // Every cap companion whose rest value is ZERO (docs §33, ADR 006). The James
    // network has TEN of them (two bass caps, the treble series and shunt caps and
    // the 330p, each as a v/i pair) and the F.A.C. two; a subnormal parked there is
    // invisible in the float output and costs ~68x on the Marshall stack. Driven,
    // then silenced — a never-driven network would prove nothing.
    JamesToneStack stack;
    stack.prepare(fs);
    stack.setSourceImpedance(45.0e3);
    stack.setKnobs(0.5, 0.5, 0.5);
    stack.snapKnobs();
    FacNetwork fn;
    fn.prepare(fs);
    fn.setSourceImpedance(45.0e3);
    fn.setPosition(1);
    {
        const std::vector<float> tone = sine(220.0, 1.0f, 0.2, fs);
        std::vector<float> o(tone.size(), 0.0f);
        stack.process(tone.data(), o.data(), static_cast<int>(tone.size()));
        fn.process(tone.data(), o.data(), static_cast<int>(tone.size()));
        assert(stack.maxAbsRestingState() > 0.0);  // it really was excited
        assert(fn.maxAbsRestingState() > 0.0);
        std::vector<float> quiet(static_cast<size_t>(4.0 * fs), 0.0f);
        std::vector<float> o2(quiet.size(), 0.0f);
        stack.process(quiet.data(), o2.data(), static_cast<int>(quiet.size()));
        fn.process(quiet.data(), o2.data(), static_cast<int>(quiet.size()));
    }
    std::printf("  James stack max |resting state| after 4 s of silence: %.3e\n",
                stack.maxAbsRestingState());
    std::printf("  F.A.C.       max |resting state| after 4 s of silence: %.3e\n",
                fn.maxAbsRestingState());
    assert(stack.maxAbsRestingState() == 0.0);
    assert(fn.maxAbsRestingState() == 0.0);

    // The H.F. Boost branch, measured rather than assumed (ADR 006). Its 0.47 uF
    // rests at the DRIVER'S CATHODE voltage — a real operating point that looks
    // like a zero-resting state — and its current and choke voltage, which really
    // do rest at zero, FLOOR at ~1.4e-16 because they are a cancellation of two
    // ~2 V node quantities. 292 decades above subnormal, so neither is flushed;
    // this asserts the floor so a future edit that lets them decay into the
    // subnormal range is caught. Driven, then silenced.
    {
        OrangePowerAmp pw;
        pw.setOversampling(4);
        pw.prepare(fs, 128);
        CathodyneInverter probe;
        probe.configure(pw.inverter().config());
        probe.prepare(fs * 4.0);
        probe.setBoost(1.0);
        double vp = 0, vk = 0;
        for (int i = 0; i < 20000; ++i)
            probe.processSample(2.0 * std::sin(kTwoPi * 5000.0 * i / (fs * 4.0)), 0.0,
                                vp, vk);
        assert(probe.maxAbsRestingState() > 0.0);  // it really was excited
        for (int i = 0; i < static_cast<int>(4.0 * fs * 4.0); ++i)
            probe.processSample(0.0, 0.0, vp, vk);
        std::printf("  H.F. Boost branch max |resting state| after 4 s of silence: "
                    "%.3e   (its 0.47 uF rests at the driver cathode, %.4f V)\n",
                    probe.maxAbsRestingState(), probe.boostCapRestVolts());
        assert(probe.maxAbsRestingState() < 1e-12);
        // …and the cap really is at a real operating point, which is why it is not
        // guarded (ADR 006's scope rule, applied by measurement).
        assert(probe.boostCapRestVolts() > 1.0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: an assert() aborts, and a buffered table is exactly the
    // measurement you need to see when it does.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_orange_tests");
    if (ledger >= 0) return ledger;
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
    testFeedbackAndHfBoost(fs);
    testAliasing(fs);
    testAliasing(44100.0);
    testDcOffsetOnSignal(fs);
    testOrangeCab(48000.0);
    testOrangeCab(44100.0);
    std::printf("\n[orange] all OR120 tests passed (the XFAIL below is a known open "
                "defect, not a regression)\n");
    return clipper::test::reportXfails();
}
