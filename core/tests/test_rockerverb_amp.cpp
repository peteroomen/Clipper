// Plain-assert tests for the M10.7 Orange Rockerverb 100 dirty channel (docs §63)
// — the MODERN Orange, and the deliberate counterweight to the OR120 (§57) the way
// the OR120 was to the JCM800.
//
// Covers RockerverbPreamp (four 12AX7 stages, the GANGED dual GAIN pot, the FMV
// stack with its VOLUME inside the same MNA), RockerverbPowerAmp (the LONG-TAILED
// PAIR, the EL34 quad, the OT, the flat feedback loop) and the composed
// RockerverbAmp.
//
// No framework: int main + <cassert>. Every assert compares a MEASURED number
// against something derived independently IN THE TEST — the Koren load line, Ohm's
// law on the transcribed 10k droppers, the EL34 fixed point, each netlist's own
// H(jw) — or against ANOTHER AMP IN THE REPO, which is the one comparison that can
// catch a wrong topology (docs §29's standing complaint).
//
// THE LOAD-BEARING TESTS ARE testNotAReskinnedOr120 AND testMasterVolumeDecouples.
// This amp is the same manufacturer, the same output valves and a shared design
// lineage as the OR120 — so if it did not measure DIFFERENT on a structural axis,
// the voice would have failed no matter what else passed.

#include "clipper/dsp/OrangeAmp.h"
#include "clipper/dsp/OrangePreamp.h"
#include "clipper/dsp/RockerverbAmp.h"
#include "clipper/dsp/RockerverbPowerAmp.h"
#include "clipper/dsp/RockerverbPreamp.h"

#include "measure/AliasMetric.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"
#include "support/Xfail.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

using clipper::dsp::JamesToneStack;
using clipper::dsp::LtpInverter;
using clipper::dsp::OrangeAmp;
using clipper::dsp::OrangePreamp;
using clipper::dsp::RockerverbAmp;
using clipper::dsp::RockerverbInterstage;
using clipper::dsp::RockerverbPowerAmp;
using clipper::dsp::RockerverbPreamp;
using clipper::dsp::RockerverbToneStack;
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

// The ONE known-bad property this voice ships with, and it is the SAME defect the
// OR120 carries (docs §57.7's `orange-schematic-alias-44k1`) — which is itself the
// evidence for what owns it. On a 48 kHz grid the composed cranked alias floor is
// -80.1 dB at the shipped 4x; on a 44.1 kHz grid it is -52.7 dB against the same
// -56 bar. It is genuine foldover, not the rail-clipping signature docs §54
// describes: it improves 11.3 dB going to 8x and beats 1x by 28.1 dB. Two amps
// failing the same bar at the same rate, both with per-triode oversampling domains
// feeding a separately-oversampled power section, is the architecture speaking.
// The bar is NOT loosened.
const clipper::test::XfailDecl kXfailAlias44k1{
    "rockerverb-alias-44k1",
    "docs §63.8 (M10.7, 2026-08-01) — shared with docs §57.7",
    "the composed Rockerverb at maximum GAIN and VOLUME must hold a -56 dB alias "
    "floor at the shipped 4x oversampling on a 44.1 kHz grid, as it does at 48 kHz",
    "a shared-oversampling-domain slice: ONE domain around the whole preamp+power "
    "cascade (docs §57.13 names the same fix for the OR120), NEVER a lower bar",
};
const clipper::test::XfailDecl kLedger[] = {kXfailAlias44k1};

struct RvKnobs {
    double gain = 0.5, volume = 0.5, bass = 0.5, mid = 0.5, treble = 0.5,
           reverb = 0.0;
};
std::vector<float> renderRv(const RvKnobs& k, const std::vector<float>& in, double fs,
                            int os = 4) {
    RockerverbAmp amp;
    amp.setOversampling(os);
    amp.prepare(fs, 128);
    amp.setParameter(RockerverbAmp::PARAM_GAIN, static_cast<float>(k.gain));
    amp.setParameter(RockerverbAmp::PARAM_VOLUME, static_cast<float>(k.volume));
    amp.setParameter(RockerverbAmp::PARAM_BASS, static_cast<float>(k.bass));
    amp.setParameter(RockerverbAmp::PARAM_MID, static_cast<float>(k.mid));
    amp.setParameter(RockerverbAmp::PARAM_TREBLE, static_cast<float>(k.treble));
    amp.setParameter(RockerverbAmp::PARAM_REVERB, static_cast<float>(k.reverb));
    std::vector<float> out(in.size(), 0.0f);
    amp.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

struct OrKnobs {
    double volume = 0.5, bass = 0.5, treble = 0.5, fac = 0.2, hf = 0.5;
};
std::vector<float> renderOr(const OrKnobs& k, const std::vector<float>& in,
                            double fs) {
    OrangeAmp amp;
    amp.setOversampling(4);
    amp.prepare(fs, 128);
    amp.setParameter(OrangeAmp::PARAM_VOLUME, static_cast<float>(k.volume));
    amp.setParameter(OrangeAmp::PARAM_BASS, static_cast<float>(k.bass));
    amp.setParameter(OrangeAmp::PARAM_TREBLE, static_cast<float>(k.treble));
    amp.setParameter(OrangeAmp::PARAM_FAC, static_cast<float>(k.fac));
    amp.setParameter(OrangeAmp::PARAM_HF_DRIVE, static_cast<float>(k.hf));
    amp.setParameter(OrangeAmp::PARAM_REVERB, 0.0f);
    std::vector<float> out(in.size(), 0.0f);
    amp.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

// ===========================================================================
// 1. DC operating points — the TRANSCRIBED supply chain and stage load lines
// ===========================================================================
void testDcOperatingPoints(double fs) {
    std::printf("\n[rockerverb] DC operating points @ %.0f Hz\n", fs);

    RockerverbAmp amp;
    amp.setOversampling(4);
    amp.prepare(fs, 128);
    RockerverbPreamp& pre = amp.preamp();
    RockerverbPowerAmp& pw = amp.powerAmp();

    // --- the transcribed 400 V / 10k / 10k dropper chain ---------------------
    // Ohm's law on the two transcribed 10k resistors is NOT asserted as an
    // identity (the solver computes the rails that way, so such an assert could
    // not fail — docs §57.10's own finding). What is asserted is an ABSOLUTE
    // window: four 12AX7s on 100k plate loads draw ~1 mA each, so R9 must drop
    // 30-60 V and R42 (carrying only S1+S2) must drop 15-35 V. A wrong dropper or
    // a wrong plate load lands outside.
    const double b1 = pre.supplyB1(), b2 = pre.supplyB2();
    const double drop1 = RockerverbPreamp::kVsupplyRaw - b1, drop2 = b1 - b2;
    std::printf("  supply: 400.00 V -> R9 10k -> B1 %.3f V (drop %.2f) -> R42 10k -> "
                "B2 %.3f V (drop %.2f)\n",
                b1, drop1, b2, drop2);
    assert(drop1 > 30.0 && drop1 < 60.0);
    assert(drop2 > 15.0 && drop2 < 35.0);
    assert(b1 > b2);

    // --- the four stages, each cross-checked by Ohm's law on its 100k load ----
    const char* nm[4] = {"S1", "S2", "S3", "S4"};
    for (int s = RockerverbPreamp::S1; s <= RockerverbPreamp::S4; ++s) {
        const double va = pre.stageQuiescentPlate(s);
        const double vk = pre.stageQuiescentCathode(s);
        const double ip = pre.stageQuiescentCurrent(s);
        const double bp = pre.stageConfig(s).bPlus;
        const double ipOhm = (bp - va - vk) / RockerverbPreamp::kRa;
        std::printf("  %s  Va %7.2f V  Vk %6.3f V  Ip %.4f mA (Ohm %.4f)  plate "
                    "%.1f %% of B+  rout %.1f k\n",
                    nm[s], va, vk, ip * 1e3, ipOhm * 1e3, 100.0 * (va + vk) / bp,
                    pre.plateSourceImpedance(s) / 1e3);
        // The solver's current and Ohm's law on the transcribed plate load agree.
        assert(std::fabs(ip - ipOhm) < 0.05e-3);
        // A 12AX7 on a 100k load from ~340 V: the classic British gain stage. This
        // is NOT the project's 0.5-0.9 mA window (docs §57.3 makes the same point
        // for the OR120's driver) — that window is for 220k-load stages. What must
        // hold is that the tube is conducting properly and the plate has room.
        assert(ip > 0.7e-3 && ip < 1.6e-3);
        assert(va > 0.5 * bp && va < 0.85 * bp);
        // Every stage's cathode is self-biased by its transcribed resistor.
        assert(std::fabs(vk - ip * pre.stageConfig(s).Rk) < 0.05);
    }
    // S4 is the COLD stage: 1k5 UNBYPASSED. Its config must say so — an unbypassed
    // cathode is the whole reason the fourth stage tightens instead of adding fizz,
    // and it is the one stage whose Ck is zero.
    assert(pre.stageConfig(RockerverbPreamp::S4).Ck == 0.0);
    assert(pre.stageConfig(RockerverbPreamp::S1).Ck > 0.0);
    assert(pre.stageConfig(RockerverbPreamp::S2).Ck > 0.0);
    assert(pre.stageConfig(RockerverbPreamp::S3).Ck > 0.0);

    // --- the EL34 QUAD -------------------------------------------------------
    const double rail = pw.railIdle(), scr = pw.screenIdle();
    const double ip = pw.el34QuiescentPlateCurrent();
    const double ig2 = pw.el34QuiescentScreenCurrent();
    const double diss = rail * ip;
    std::printf("  EL34 quad: rail %7.2f V  screen %7.2f V  Ip %6.2f mA  Ig2 %5.2f mA"
                "  Pdiss %5.2f W (%.0f %% of 25 W)\n",
                rail, scr, ip * 1e3, ig2 * 1e3, diss, diss / 25.0 * 100.0);
    const double railCheck = RockerverbPowerAmp::kVsupply -
                             RockerverbPowerAmp::kTubes * (ip + ig2) *
                                 RockerverbPowerAmp::kRsupply;
    assert(std::fabs(railCheck - rail) < 1e-3);
    // Absolute window on the reconstructed bias: inside the 25 W plate rating and
    // hot enough to be class AB rather than B.
    assert(diss < 20.0 && diss > 13.0);
    assert(ip > 25.0e-3 && ip < 45.0e-3);
    const double scrDrop = rail - scr;
    std::printf("  screen drop across the per-tube 1k: %.3f V\n", scrDrop);
    assert(scrDrop > 2.0 && scrDrop < 6.0);

    // --- the LONG-TAILED PAIR ------------------------------------------------
    const LtpInverter& ltp = pw.inverter();
    const double bp = ltp.config().bPlus;
    std::printf("  LTP: B+ %7.2f V  Va1 %7.2f (%.1f %%)  Va2 %7.2f (%.1f %%)  "
                "Vk %.3f V  Ip1 %.4f mA\n",
                bp, ltp.quiescentPlate1(), 100.0 * ltp.quiescentPlate1() / bp,
                ltp.quiescentPlate2(), 100.0 * ltp.quiescentPlate2() / bp,
                ltp.quiescentTail(), ltp.quiescentCurrent1() * 1e3);
    // The docs §29 / §42 phase-inverter health targets, on a REAL long tail.
    assert(ltp.quiescentPlate1() > 0.70 * bp && ltp.quiescentPlate1() < 0.85 * bp);
    assert(ltp.quiescentPlate2() > 0.70 * bp && ltp.quiescentPlate2() < 0.85 * bp);
    assert(ltp.quiescentCurrent1() > 0.5e-3 && ltp.quiescentCurrent1() < 0.9e-3);
    // The PI hangs off the main rail through its own dropper: an absolute window,
    // not the identity the solver used.
    assert(bp < rail - 8.0 && bp > rail - 30.0);
}

// ===========================================================================
// 2. The LONG-TAILED PAIR — anti-phase, and the structural contrast with the
//    OR120's cathodyne, measured on both amps in the same run.
// ===========================================================================
void testInverterBalance(double fs) {
    std::printf("\n[rockerverb] phase inverter @ %.0f Hz\n", fs);
    RockerverbPowerAmp pw;
    pw.setOversampling(4);
    pw.prepare(fs, 128);

    LtpInverter probe;
    probe.configure(pw.inverter().config());
    probe.prepare();
    const double osRate = fs * 4.0;
    const int n = 8192;
    double a1 = 0.0, a2 = 0.0, den = 0.0;
    const double amp = 0.02;
    for (int i = 0; i < n; ++i) {
        const double w = kTwoPi * 440.0 * i / osRate;
        double p1 = 0.0, p2 = 0.0;
        probe.processSample(amp * std::sin(w), 0.0, p1, p2);
        if (i > n / 2) {
            a1 += (p1 - probe.quiescentPlate1()) * std::sin(w);
            a2 += (p2 - probe.quiescentPlate2()) * std::sin(w);
            den += std::sin(w) * std::sin(w);
        }
    }
    // Signed in-phase amplitudes: a steady tone, not a DC step (docs §57.3's trap —
    // a step decays through the coupling network and reads a different gain at
    // every settle time).
    const double g1 = a1 / den / amp, g2 = a2 / den / amp;
    const double bal = std::min(std::fabs(g1), std::fabs(g2)) /
                       std::max(std::fabs(g1), std::fabs(g2));
    std::printf("  legs %+.4f / %+.4f   balance %.6f\n", g1, g2, bal);
    // ANTI-PHASE: the product of the two signed gains must be negative. That is
    // the property a phase inverter exists for, and it cannot be faked by a
    // magnitude bar.
    assert(g1 * g2 < 0.0);
    // Balanced legs -> even-harmonic cancellation in the push-pull pair. Audit
    // finding 8 / docs §45's bar for the 2204's LTP is 0.90; the same bar here.
    assert(bal > 0.90);
    // BOTH legs must actually amplify — a long-tailed pair is a gain stage, unlike
    // the OR120's split load (which measures 0.9733 and cannot).
    assert(std::fabs(g1) > 10.0 && std::fabs(g2) > 10.0);
    std::printf("  (structural contrast, REPORTED not asserted: the OR120's "
                "cathodyne is balanced BY TOPOLOGY at 0.999965 with no calibration "
                "and its legs have a gain below unity; docs §57.3)\n");
}

// ===========================================================================
// 3. The networks against their own H(jw) — the discretization check.
//    Docs §29's standing limitation applies: an analytic reference derived from
//    the same netlist validates the DISCRETIZATION and structurally cannot catch
//    a wrong topology. What validates the topology here is test 4.
// ===========================================================================
void testNetworksVsAnalytic(double fs) {
    std::printf("\n[rockerverb] networks vs their own H(jw) @ %.0f Hz\n", fs);
    RockerverbAmp amp;
    amp.setOversampling(4);
    amp.prepare(fs, 128);
    RockerverbPreamp& pre = amp.preamp();

    // Bilinear frequency warping is the whole error budget: at 6 kHz on a 44.1 kHz
    // grid tan(pi f/fs)/(pi f/fs) = 1.0672, i.e. the discrete pole sits 6.7 % high.
    const double bound = 0.60;
    double worst = 0.0;
    // --- the three interstage networks ---------------------------------------
    for (int i = 0; i < 3; ++i) {
        const RockerverbInterstage::Config& c = RockerverbPreamp::interstageConfig(i);
        const double rs = pre.plateSourceImpedance(i);
        for (double g : {0.15, 0.5, 0.9}) {
            const double wiper = RockerverbPreamp::logTaper(g);
            RockerverbInterstage net;
            net.configure(c);
            net.prepare(fs);
            net.setSourceImpedance(rs);
            net.setWiper(wiper);
            net.snapKnobs();
            for (double f : {82.0, 220.0, 660.0, 2000.0, 6000.0}) {
                std::vector<float> in = sine(f, 1.0f, 0.30, fs);
                std::vector<float> out(in.size(), 0.0f);
                net.process(in.data(), out.data(), static_cast<int>(in.size()));
                const double meas =
                    goertzelAmp(out, out.size() / 2, out.size() / 2, f, fs);
                const double an = RockerverbInterstage::magnitudeAt(f, c, wiper, rs);
                worst = std::max(worst, std::fabs(toDb(meas) - toDb(an)));
            }
            if (c.Rpot <= 0.0) break;  // the S3->S4 divider has no pot to sweep
        }
    }
    std::printf("  interstage worst |error| %.4f dB (bound %.2f)\n", worst, bound);
    assert(worst < bound);

    // --- the FMV tone stack + VOLUME -----------------------------------------
    double worstTs = 0.0;
    const double rs = pre.plateSourceImpedance(RockerverbPreamp::S4);
    const double combos[5][4] = {{0.5, 0.5, 0.5, 0.5}, {0.0, 0.5, 1.0, 0.5},
                                 {1.0, 0.0, 0.0, 0.7}, {0.3, 0.9, 0.7, 0.3},
                                 {0.8, 0.2, 0.4, 1.0}};
    for (const auto& k : combos) {
        RockerverbToneStack ts;
        ts.prepare(fs);
        ts.setSourceImpedance(rs);
        ts.setKnobs(RockerverbPreamp::logTaper(k[0]), k[1], k[2], k[3]);
        ts.snapKnobs();
        for (double f : {82.0, 220.0, 660.0, 2000.0, 6000.0}) {
            std::vector<float> in = sine(f, 1.0f, 0.30, fs);
            std::vector<float> out(in.size(), 0.0f);
            ts.process(in.data(), out.data(), static_cast<int>(in.size()));
            const double meas =
                goertzelAmp(out, out.size() / 2, out.size() / 2, f, fs);
            const double an = RockerverbToneStack::magnitudeAt(
                f, RockerverbPreamp::logTaper(k[0]), k[1], k[2], k[3], rs);
            worstTs = std::max(worstTs, std::fabs(toDb(meas) - toDb(an)));
        }
    }
    std::printf("  tone stack worst |error| %.4f dB (bound %.2f)\n", worstTs, bound);
    assert(worstTs < bound);

    // --- ABSOLUTE interstage divider windows ---------------------------------
    // ADDED BECAUSE THE PERTURBATION RUN FOUND THESE NETWORKS UNGUARDED (docs
    // §63.13, P5): flattening the S3->S4 divider from the transcribed 470k/220k
    // to 470k/2M2 left every bar above GREEN, because an H(jw) check compares the
    // network against its OWN netlist and cannot see a wrong component value
    // (docs §29's standing complaint), and the composed bars are scale-free.
    //
    // So each network's mid-band magnitude is compared against a number written
    // out HERE from the transcribed resistors, NOT read back from the Config. The
    // numbers below are computed in the comment and the windows are absolute:
    //
    //   net 2 (S3->S4): a plain divider. R5 / (rout + R6 + R5)
    //                 = 220k / (35.1k + 470k + 220k) = 0.3034. Measured 0.30293.
    //                 At Rgnd 2M2 it would read 0.814; at 22k, 0.042.
    //   net 0 (S1->S2): R30 220k into R1 220k || GAIN 1M, then the pot's noon
    //                 wiper (the house k=4 taper puts it at 0.1192) and the 470p
    //                 shunt. Measured 0.04598.
    //   net 1 (S2->S3): R31 220k into R32 470k || GAIN 1M, same wiper, no shunt
    //                 cap and no bright cap. Measured 0.06626.
    //
    // The two GAIN networks are asserted at ±25 % (they carry a pot and a taper);
    // the fixed divider at ±8 %, because nothing about it is free.
    {
        const double expect[3] = {0.04598, 0.06626, 0.30293};
        const double tol[3] = {0.25, 0.25, 0.08};
        for (int i = 0; i < 3; ++i) {
            const RockerverbInterstage::Config& c =
                RockerverbPreamp::interstageConfig(i);
            RockerverbInterstage net;
            net.configure(c);
            net.prepare(fs);
            net.setSourceImpedance(pre.plateSourceImpedance(i));
            net.setWiper(RockerverbPreamp::logTaper(0.5));
            net.snapKnobs();
            std::vector<float> in = sine(1000.0, 1.0f, 0.30, fs);
            std::vector<float> out(in.size(), 0.0f);
            net.process(in.data(), out.data(), static_cast<int>(in.size()));
            const double m = goertzelAmp(out, out.size() / 2, out.size() / 2, 1000.0, fs);
            std::printf("  interstage %d mid-band magnitude %.5f (transcribed %.5f "
                        "+/- %.0f %%)\n",
                        i, m, expect[i], 100.0 * tol[i]);
            assert(std::fabs(m - expect[i]) < tol[i] * expect[i]);
        }
    }
}

// ===========================================================================
// 4. THE BAR, HALF ONE — measurably NOT a re-skinned OR120 (the TONE NETWORK)
//
// Metric (docs §57.4's, verbatim, so the two voices are on ONE scale): at noon,
// the minimum response across 300-800 Hz relative to the mean of the 100 Hz and
// 4 kHz responses. Negative = a mid SCOOP, positive = a mid BUMP. Scale-free, so
// the two stacks' very different insertion losses cannot flatter either one.
//
// The two amps are the same manufacturer, the same output valves and a shared
// design lineage — so the bar has to rest on a STRUCTURAL difference. It does:
// the OR120's network is a JAMES / passive Baxandall (two parallel shelving
// branches, nothing acting on the middle) and the Rockerverb's is a
// Marshall-lineage FMV (a slope resistor and a treble branch that between them
// put a notch in the middle). That is topology, and it is what this measures.
// ===========================================================================
double gRvRs = 32.4e3;
double rvNetMag(double f) {
    return RockerverbToneStack::magnitudeAt(f, 0.5, 0.5, 0.5, 0.5, gRvRs,
                                            RockerverbToneStack::Probe::Out);
}
double gJamesRs = 45.5e3, gJamesGain = 0.5;
double jamesNetMag(double f) {
    return JamesToneStack::magnitudeAt(f, 0.5, 0.5, gJamesGain, gJamesRs,
                                       JamesToneStack::Probe::Out);
}
double midNotchDb(double (*mag)(double)) {
    const double lo = toDb(mag(100.0));
    const double hi = toDb(mag(4000.0));
    double mid = 1e30;
    for (double f : {300.0, 400.0, 500.0, 600.0, 700.0, 800.0})
        mid = std::min(mid, toDb(mag(f)));
    return mid - 0.5 * (lo + hi);
}

void testNotAReskinnedOr120(double fs) {
    std::printf("\n[rockerverb] THE BAR (1/2): NOT a re-skinned OR120 @ %.0f Hz\n",
                fs);
    {
        RockerverbAmp a;
        a.setOversampling(4);
        a.prepare(fs, 128);
        gRvRs = a.preamp().plateSourceImpedance(RockerverbPreamp::S4);
        OrangeAmp o;
        o.setOversampling(4);
        o.prepare(fs, 128);
        gJamesRs = o.preamp().plateSourceImpedance();
        gJamesGain = OrangePreamp::volumeTaper(0.5);
    }

    // --- (a) the tone NETWORKS, at noon, each from its own netlist ------------
    const double rvNotch = midNotchDb(&rvNetMag);
    const double orNotch = midNotchDb(&jamesNetMag);
    std::printf("  network mid-notch @ noon:  Rockerverb FMV %+.2f dB   "
                "OR120 James %+.2f dB   contrast %.2f dB\n",
                rvNotch, orNotch, orNotch - rvNotch);
    std::printf("       f     FMV    James\n");
    for (double f : {82.0, 220.0, 440.0, 660.0, 1000.0, 2200.0, 5000.0})
        std::printf("  %6.0f   %+6.2f  %+6.2f\n", f,
                    toDb(rvNetMag(f)) - toDb(rvNetMag(1000.0)),
                    toDb(jamesNetMag(f)) - toDb(jamesNetMag(1000.0)));
    // Two SIGNS, asserted separately, so a change that moved both together could
    // not hide behind the contrast.
    assert(rvNotch < -3.0);   // the FMV is a SCOOP
    assert(orNotch > 0.0);    // the James is a BUMP (docs §57.4's own bound)
    // …and the CONTRAST, the hard bar in dB. Shipped at 5.0 against a measured
    // 6.76 — 1.76 dB of margin, RECORDED not snugged. It is deliberately not set
    // just under the measurement: §57.4 shipped 6.0 against 8.35 and a later
    // component correction took the measurement to 6.78, which would have failed a
    // snugged bound for a reason that was not a regression.
    assert(orNotch - rvNotch > 5.0);

    // --- (b) the COMPOSED amps, rendered -------------------------------------
    // Deliberately NOT normalized at 1 kHz: the FMV's own notch minimum sits near
    // there, so normalizing at 1 kHz would hide the very thing being measured.
    // Both amps at tone knobs noon, a clean level, the same input.
    const double freqs[8] = {110.0, 220.0, 330.0, 440.0, 660.0, 1000.0, 2200.0,
                             4400.0};
    double rvDb[8], orDb[8];
    for (int i = 0; i < 8; ++i) {
        std::vector<float> in = sine(freqs[i], 0.02f, 0.30, fs);
        RvKnobs rk;
        rk.gain = 0.08;
        rk.volume = 0.5;
        const std::vector<float> ro = renderRv(rk, in, fs);
        rvDb[i] = toDb(goertzelAmp(ro, ro.size() / 2, ro.size() / 2, freqs[i], fs));
        OrKnobs ok;
        ok.volume = 0.3;
        const std::vector<float> oo = renderOr(ok, in, fs);
        orDb[i] = toDb(goertzelAmp(oo, oo.size() / 2, oo.size() / 2, freqs[i], fs));
    }
    std::printf("       f    Rockerverb   OR120   (dB re each amp's own 660 Hz)\n");
    for (int i = 0; i < 8; ++i)
        std::printf("  %6.0f     %+7.2f   %+7.2f\n", freqs[i], rvDb[i] - rvDb[4],
                    orDb[i] - orDb[4]);
    auto composedNotch = [&](const double* d) {
        const double lo = d[0], hi = d[7];
        const double mid = std::min(std::min(d[2], d[3]), d[4]);
        return mid - 0.5 * (lo + hi);
    };
    const double rvC = composedNotch(rvDb), orC = composedNotch(orDb);
    std::printf("  composed mid-notch: Rockerverb %+.2f dB   OR120 %+.2f dB   "
                "contrast %.2f dB\n",
                rvC, orC, orC - rvC);
    assert(rvC < 0.0);
    assert(orC > 0.0);
    assert(orC - rvC > 3.0);
}

// ===========================================================================
// 5. THE BAR, HALF TWO — the MASTER VOLUME decouples drive from level.
//
// The OR120 has ONE knob and its power section IS the overdrive (docs §57.5,
// §46's no-master convention), so at any given output level it delivers a fixed
// amount of distortion. The Rockerverb's VOLUME sits AFTER the tone stack, so
// GAIN and level are independent — it can be filthy and quiet. That is a property
// a no-master amp CANNOT have at any setting, which makes it the right second
// half of a bar against a sibling that shares this one's power machinery.
//
// Measured LEVEL-MATCHED: both amps are bisected onto the same output RMS at the
// same input, then their THD is compared.
// ===========================================================================
void testMasterVolumeDecouples(double fs) {
    std::printf("\n[rockerverb] THE BAR (2/2): the master volume decouples drive "
                "from level @ %.0f Hz\n",
                fs);
    const std::vector<float> in = sine(kProbeHz, kProbeV, 0.40, fs);
    const double targetDb = -20.0;

    // Bisect the Rockerverb's VOLUME at a normal dirty GAIN.
    auto rvAt = [&](double v) {
        RvKnobs k;
        k.gain = 0.70;
        k.volume = v;
        return renderRv(k, in, fs);
    };
    double lo = 1.0e-3, hi = 1.0;
    for (int i = 0; i < 22; ++i) {
        const double m = 0.5 * (lo + hi);
        if (toDb(rmsTail(rvAt(m))) < targetDb)
            lo = m;
        else
            hi = m;
    }
    const double rvV = 0.5 * (lo + hi);
    const std::vector<float> rvOut = rvAt(rvV);
    const double rvThd = 100.0 * thd(rvOut, kProbeHz, fs);
    const double rvLvl = toDb(rmsTail(rvOut));

    // Bisect the OR120's ONE knob onto the same level.
    auto orAt = [&](double v) {
        OrKnobs k;
        k.volume = v;
        return renderOr(k, in, fs);
    };
    lo = 1.0e-3;
    hi = 1.0;
    for (int i = 0; i < 22; ++i) {
        const double m = 0.5 * (lo + hi);
        if (toDb(rmsTail(orAt(m))) < targetDb)
            lo = m;
        else
            hi = m;
    }
    const double orV = 0.5 * (lo + hi);
    const std::vector<float> orOut = orAt(orV);
    const double orThd = 100.0 * thd(orOut, kProbeHz, fs);
    const double orLvl = toDb(rmsTail(orOut));

    std::printf("  level-matched at %.2f dBFS (0.15 V / 220 Hz):\n", targetDb);
    std::printf("    Rockerverb  GAIN 0.70, VOLUME %.4f  ->  %.2f dBFS, THD %6.2f %%\n",
                rvV, rvLvl, rvThd);
    std::printf("    OR120       VOLUME %.4f            ->  %.2f dBFS, THD %6.2f %%\n",
                orV, orLvl, orThd);
    std::printf("    distortion ratio at equal level: %.2fx (%.2f dB)\n",
                rvThd / orThd, 20.0 * std::log10(rvThd / orThd));
    // The bisection really did level-match (otherwise the ratio means nothing).
    assert(std::fabs(rvLvl - targetDb) < 0.5);
    assert(std::fabs(orLvl - targetDb) < 0.5);
    // THE BAR: at the SAME quiet output level the master-volume amp delivers at
    // least 5x the harmonic distortion. Shipped at 5.0 against a measured ~13x;
    // margin RECORDED, not snugged.
    assert(rvThd > 5.0 * orThd);

    // …and the MECHANISM, asserted separately so the ratio above cannot be
    // produced by an amp that is simply dirty everywhere. With GAIN held at 0.70,
    // sweeping VOLUME across the range where the POWER section is not itself
    // clipping must move the LEVEL a great deal and the THD essentially not at
    // all. (Above ~0.15 the power amp saturates and the level stops moving — that
    // is the amp working, and it is why the window is the bottom of the knob.)
    double lvlLo = 0.0, lvlHi = 0.0, thdLo = 0.0, thdHi = 0.0;
    {
        const std::vector<float> a = rvAt(0.01), b = rvAt(0.10);
        lvlLo = toDb(rmsTail(a));
        lvlHi = toDb(rmsTail(b));
        thdLo = 100.0 * thd(a, kProbeHz, fs);
        thdHi = 100.0 * thd(b, kProbeHz, fs);
    }
    std::printf("    VOLUME 0.01 -> 0.10 at GAIN 0.70: level %+.2f -> %+.2f dBFS "
                "(%.2f dB), THD %.2f -> %.2f %% (ratio %.3f)\n",
                lvlLo, lvlHi, lvlHi - lvlLo, thdLo, thdHi, thdHi / thdLo);
    assert(lvlHi - lvlLo > 15.0);  // the knob is a real level control (~20 dB)
    assert(thdLo > 20.0);          // …and the amp is ALREADY dirty at the bottom
    // …so the knob is NOT a gain control: 20 dB of level for under 10 % relative
    // change in distortion. A pre-stack volume pot (the OR120's arrangement, and
    // the docs §44 defect the Twin had) cannot satisfy this.
    assert(thdHi / thdLo > 0.90 && thdHi / thdLo < 1.10);
}

// ===========================================================================
// 6. Knob authority — every control must do something (CLAUDE.md: no dead UI).
//    Added for the reason docs §57.10 found the hard way: the mid-notch metric
//    alone cannot see a collapsed pot.
// ===========================================================================
void testKnobAuthority(double fs) {
    std::printf("\n[rockerverb] knob authority @ %.0f Hz\n", fs);
    RockerverbAmp amp;
    amp.setOversampling(4);
    amp.prepare(fs, 128);
    const double rs = amp.preamp().plateSourceImpedance(RockerverbPreamp::S4);
    auto at = [&](double f, double b, double m, double t) {
        return toDb(RockerverbToneStack::magnitudeAt(
            f, RockerverbPreamp::logTaper(b), m, t, 0.5, rs,
            RockerverbToneStack::Probe::Out));
    };
    const double bassTravel = at(82.0, 0.999, 0.5, 0.5) - at(82.0, 0.001, 0.5, 0.5);
    const double midTravel = at(650.0, 0.5, 0.999, 0.5) - at(650.0, 0.5, 0.001, 0.5);
    const double trebTravel =
        at(5000.0, 0.5, 0.5, 0.999) - at(5000.0, 0.5, 0.5, 0.001);
    std::printf("  BASS %+.2f dB @82 Hz   MIDDLE %+.2f dB @650 Hz   TREBLE %+.2f dB "
                "@5 kHz\n",
                bassTravel, midTravel, trebTravel);
    assert(bassTravel > 6.0);
    assert(midTravel > 6.0);
    assert(trebTravel > 6.0);

    // THE MID POT IS NOT A RHEOSTAT, and that is transcribed, not a simplification:
    // both of its sections sit between node C and ground, so C->GND is 25k at EVERY
    // position and what the knob moves is where the 22n taps in. Asserted so a
    // later session cannot quietly "tidy" it into the canonical FMV rheostat.
    // Measured: the resistance seen looking from node C to ground is knob-invariant,
    // which shows up as the DC (0 Hz-ish) response being unmoved by MIDDLE while
    // the 650 Hz response moves 6+ dB.
    const double dcSpread = std::fabs(at(20.0, 0.5, 0.999, 0.5) - at(20.0, 0.5, 0.001, 0.5));
    std::printf("  MIDDLE at 20 Hz moves %.3f dB (the 25k leg is knob-invariant) vs "
                "%.2f dB at 650 Hz\n",
                dcSpread, midTravel);
    assert(dcSpread < 0.5 * midTravel);

    // The GAIN pot is a GANG (Group=\"Gain\" in the netlist file): ONE knob, TWO
    // wipers, so the drive it delivers is the product of two dividers. Measured as
    // preamp output level across the travel.
    RockerverbPreamp pre;
    const std::vector<float> in = sine(kProbeHz, 0.01f, 0.30, fs);
    double prev = -1e9;
    std::printf("  GAIN travel (preamp out, 0.01 V in):");
    for (double g : {0.1, 0.3, 0.5, 0.7, 1.0}) {
        RockerverbPreamp p;
        p.setParameter(RockerverbPreamp::PARAM_GAIN, static_cast<float>(g));
        p.setParameter(RockerverbPreamp::PARAM_VOLUME, 0.5f);
        p.prepare(fs, 128);
        std::vector<float> out(in.size(), 0.0f);
        p.process(in.data(), out.data(), static_cast<int>(in.size()));
        const double d = toDb(rmsTail(out));
        std::printf("  %.1f:%+.1f", g, d);
        assert(d > prev);  // monotone
        prev = d;
    }
    std::printf("\n");
    (void)pre;
}

// ===========================================================================
// 7. Breakup — GAIN drives, VOLUME does not; both monotone.
// ===========================================================================
void testBreakup(double fs) {
    std::printf("\n[rockerverb] breakup @ %.0f Hz (0.15 V / 220 Hz)\n", fs);
    const std::vector<float> in = sine(kProbeHz, kProbeV, 0.40, fs);
    std::printf("   GAIN    THD %%     RMS dBFS   (VOLUME 0.5)\n");
    double prevThd = -1.0, onset = -1.0;
    for (double g : {0.05, 0.10, 0.15, 0.20, 0.30, 0.40, 0.50, 0.70, 1.00}) {
        RvKnobs k;
        k.gain = g;
        k.volume = 0.5;
        const std::vector<float> o = renderRv(k, in, fs);
        const double t = 100.0 * thd(o, kProbeHz, fs);
        std::printf("   %.2f   %7.3f    %8.2f\n", g, t, toDb(rmsTail(o)));
        if (onset < 0.0 && t >= 5.0) onset = g;
        assert(t > prevThd - 1.0);  // monotone within the measurement's own noise
        prevThd = t;
    }
    std::printf("  >=5 %% THD onset at GAIN %.2f\n", onset);
    // A four-stage high-gain preamp with the GAIN pot AFTER the first stage: the
    // onset is early and that is what the netlist says. The window is wide and
    // absolute (it is not snugged to the measurement) — what it forbids is an amp
    // that is either clean at half gain or filthy at a tenth of it.
    assert(onset > 0.08 && onset < 0.35);
}

// ===========================================================================
// 8. Global feedback — closed loop must be quieter than open, and FLAT.
// ===========================================================================
void testFeedback(double fs) {
    std::printf("\n[rockerverb] global feedback @ %.0f Hz\n", fs);
    auto gainAt = [&](bool fb, double f) {
        RockerverbPowerAmp pw;
        pw.setOversampling(4);
        pw.prepare(fs, 128);
        pw.setFeedbackEnabled(fb);
        pw.setParameter(RockerverbPowerAmp::PARAM_DRIVE, 0.5f);
        const std::vector<float> in = sine(f, 0.05f, 0.30, fs);
        std::vector<float> out(in.size(), 0.0f);
        pw.process(in.data(), out.data(), static_cast<int>(in.size()));
        return goertzelAmp(out, out.size() / 2, out.size() / 2, f, fs) / 0.05;
    };
    const double open = gainAt(false, 440.0), closed = gainAt(true, 440.0);
    const double depth = toDb(open) - toDb(closed);
    std::printf("  open-loop %.5f  closed-loop %.5f  depth %.2f dB\n", open, closed,
                depth);
    assert(closed < open);            // it really is NEGATIVE feedback
    assert(depth > 3.0 && depth < 12.0);
    // …and the loop is FLAT, because this amp has no presence control in it. The
    // 2204's presence pot lifts its HF by several dB and the OR120's H.F. Boost
    // R-L-C peaks 6+ dB; here the loop depth must be the same at 220 Hz and 5 kHz.
    const double dLo = toDb(gainAt(false, 220.0)) - toDb(gainAt(true, 220.0));
    const double dHi = toDb(gainAt(false, 5000.0)) - toDb(gainAt(true, 5000.0));
    std::printf("  loop depth 220 Hz %.2f dB   5 kHz %.2f dB   spread %.2f dB\n", dLo,
                dHi, std::fabs(dHi - dLo));
    assert(std::fabs(dHi - dLo) < 1.0);
}

// ===========================================================================
// 9. Aliasing — the house composed probe (docs §18/§57.7's stimulus, so the
//    numbers are comparable across amps).
// ===========================================================================
void testAliasing(double fs) {
    std::printf("\n[rockerverb] alias floor @ %.0f Hz (4186 Hz / 0.3 V, cranked)\n",
                fs);
    double f1 = 0.0, f4 = 0.0;
    for (int factor : {1, 2, 4, 8}) {
        RockerverbAmp amp;
        amp.setOversampling(factor);
        amp.prepare(fs, 128);
        amp.setParameter(RockerverbAmp::PARAM_GAIN, 1.0f);
        amp.setParameter(RockerverbAmp::PARAM_VOLUME, 1.0f);
        amp.setParameter(RockerverbAmp::PARAM_BASS, 0.5f);
        amp.setParameter(RockerverbAmp::PARAM_MID, 0.5f);
        amp.setParameter(RockerverbAmp::PARAM_TREBLE, 0.5f);
        const std::vector<float> in = sine(4186.0, 0.3f, 0.5, fs);
        std::vector<float> out(in.size(), 0.0f);
        amp.process(in.data(), out.data(), static_cast<int>(in.size()));
        const double a = measureAliasing(out, fs, 4186.0).worstAliasDb;
        std::printf("   %dx: %.1f dB\n", factor, a);
        if (factor == 1) f1 = a;
        if (factor == 4) f4 = a;
    }
    // The shipped factor is 4x. Two bars: an absolute floor, and — the clause that
    // actually catches a broken oversampler — 4x must BEAT 1x by a wide margin, so
    // a floor that stops depending on the factor (docs §54's rail-clipping
    // signature) fails even if the absolute number looks fine.
    //
    // The absolute floor is HARD at 48 kHz and an XFAIL at 44.1 kHz — see
    // kXfailAlias44k1. The improvement clause stays hard at BOTH rates, because it
    // is what separates "aliasing this oversampler cannot yet reach" from "not
    // aliasing at all".
    if (fs > 46000.0) {
        assert(f4 < -56.0);
    } else {
        expectXfail(f4 < -56.0, kXfailAlias44k1,
                    "composed cranked 4x alias floor at 44.1 kHz");
    }
    assert(f1 - f4 > 12.0);
}

// ===========================================================================
// 10. DC offset ON SIGNAL, with and without an input offset (docs §29).
// ===========================================================================
void testDcOffsetOnSignal(double fs) {
    std::printf("\n[rockerverb] DC offset on signal @ %.0f Hz\n", fs);
    for (int withOffset = 0; withOffset < 2; ++withOffset) {
        std::vector<float> in = sine(kProbeHz, kProbeV, 0.40, fs);
        if (withOffset)
            for (auto& s : in) s += 0.1f;
        RvKnobs k;
        k.gain = 0.7;
        k.volume = 0.7;
        const std::vector<float> out = renderRv(k, in, fs);
        const double pct =
            100.0 * clipper::test::measureDcOnSignal(out).fraction;
        std::printf("  %s: DC %.4f %% of peak\n",
                    withOffset ? "+0.1 V input offset" : "clean input", pct);
        assert(pct < 2.0);
    }
}

// ===========================================================================
// 11. reset(), ragged blocking, rate independence, denormals.
// ===========================================================================
void testHygiene(double fs) {
    std::printf("\n[rockerverb] hygiene @ %.0f Hz\n", fs);
    const std::vector<float> in = sine(kProbeHz, kProbeV, 0.30, fs);

    // --- whole-buffer vs reset() + ragged 128-frame blocks -------------------
    RockerverbAmp a;
    a.setOversampling(4);
    a.prepare(fs, 128);
    a.setParameter(RockerverbAmp::PARAM_GAIN, 0.6f);
    a.setParameter(RockerverbAmp::PARAM_VOLUME, 0.6f);
    std::vector<float> whole(in.size(), 0.0f);
    a.process(in.data(), whole.data(), static_cast<int>(in.size()));
    a.reset();
    std::vector<float> blocked(in.size(), 0.0f);
    for (size_t off = 0; off < in.size();) {
        const int n = static_cast<int>(std::min<size_t>(128, in.size() - off));
        a.process(in.data() + off, blocked.data() + off, n);
        off += static_cast<size_t>(n);
    }
    double worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i) {
        assert(std::isfinite(blocked[i]));
        worst = std::max(worst, std::fabs(double(whole[i]) - double(blocked[i])));
    }
    std::printf("  reset() + 128-frame blocking vs one buffer: %.3e\n", worst);
    assert(worst == 0.0);

    // --- every zero-resting network state must settle to EXACTLY zero --------
    // (ADR 006's scope rule, applied by measurement: these companions rest at
    // zero, so on silence they ring down into the double subnormal range and
    // stick — invisible in the float output, expensive in the audio thread, and
    // WASM has no flush-to-zero at all.)
    {
        RockerverbToneStack ts;
        ts.prepare(fs);
        ts.setSourceImpedance(32.0e3);
        ts.setKnobs(0.5, 0.5, 0.5, 0.5);
        ts.snapKnobs();
        const std::vector<float> hot = sine(1000.0, 20.0f, 0.20, fs);
        std::vector<float> o(hot.size(), 0.0f);
        ts.process(hot.data(), o.data(), static_cast<int>(hot.size()));
        assert(ts.maxAbsRestingState() > 0.0);  // it really was excited
        const std::vector<float> quiet(static_cast<size_t>(4.0 * fs), 0.0f);
        std::vector<float> o2(quiet.size(), 0.0f);
        ts.process(quiet.data(), o2.data(), static_cast<int>(quiet.size()));
        std::printf("  tone stack max |resting state| after 4 s of silence: %.3e\n",
                    ts.maxAbsRestingState());
        assert(ts.maxAbsRestingState() == 0.0);

        for (int i = 0; i < 3; ++i) {
            RockerverbInterstage net;
            net.configure(RockerverbPreamp::interstageConfig(i));
            net.prepare(fs);
            net.setSourceImpedance(32.0e3);
            net.setWiper(0.5);
            net.snapKnobs();
            std::vector<float> ho(hot.size(), 0.0f);
            net.process(hot.data(), ho.data(), static_cast<int>(hot.size()));
            assert(net.maxAbsRestingState() > 0.0);
            std::vector<float> qo(quiet.size(), 0.0f);
            net.process(quiet.data(), qo.data(), static_cast<int>(quiet.size()));
            std::printf("  interstage %d max |resting state|: %.3e\n", i,
                        net.maxAbsRestingState());
            assert(net.maxAbsRestingState() == 0.0);
        }
    }
}

void testRateIndependence() {
    std::printf("\n[rockerverb] rate independence\n");
    double lo = 1e30, hi = -1e30;
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        const std::vector<float> in = sine(kProbeHz, kProbeV, 0.40, fs);
        RvKnobs k;
        k.gain = 0.5;
        k.volume = 0.5;
        const std::vector<float> o = renderRv(k, in, fs);
        const double d = toDb(rmsTail(o));
        std::printf("  %.0f Hz: %.3f dBFS  THD %.2f %%\n", fs, d,
                    100.0 * thd(o, kProbeHz, fs));
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    std::printf("  spread %.3f dB\n", hi - lo);
    assert(hi - lo < 1.0);
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: an assert() aborts, and a buffered table is exactly the
    // measurement you need to see when it does.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_rockerverb_tests");
    if (ledger >= 0) return ledger;
    for (double fs : {44100.0, 48000.0}) {
        std::printf("\n=========== Rockerverb 100, fs = %.0f ===========\n", fs);
        testDcOperatingPoints(fs);
        testInverterBalance(fs);
        testNetworksVsAnalytic(fs);
        testHygiene(fs);
    }
    const double fs = 48000.0;
    testNotAReskinnedOr120(fs);
    testMasterVolumeDecouples(fs);
    testKnobAuthority(fs);
    testBreakup(fs);
    testFeedback(fs);
    testAliasing(fs);
    testAliasing(44100.0);
    testDcOffsetOnSignal(fs);
    testRateIndependence();
    std::printf("\n[rockerverb] all Rockerverb tests passed (the XFAIL below is a "
                "known open defect shared with the OR120, not a regression)\n");
    return clipper::test::reportXfails();
}
