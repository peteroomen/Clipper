// Clipper — test support: measure a long-tailed-pair phase inverter's operating point
// and leg balance.
//
// WHY THIS EXISTS (2026-07-25, test/assert-real-properties). All three valve amps share
// `clipper::dsp::LtpInverter`, and all three tests used to assert the same near-useless
// thing about it: `quiescentPlate1() > 250 && < 410`, an absolute window 160 V wide on a
// 410 V rail. Both a healthy ~330 V and the starved 386.8 V (94.3 % of B+) pass it — which
// is precisely how the starved AC30 phase inverter shipped (docs §23 second amendment).
// Nothing anywhere asserted the LEG RATIO, the property the push-pull even-harmonic
// cancellation actually depends on (2026-07-24 audit findings 7 and 8).
//
// The three things worth measuring, defined ONCE here so the three amps cannot drift:
//
//   plateFraction  Va / B+. The SCALE-FREE form. An absolute voltage window cannot
//                  distinguish "biased in the middle of the load line" from "parked at
//                  cutoff", because the answer depends on B+; a fraction can.
//   ipOhm          (B+ − Va)/Ra — the standing current per triode from OHM'S LAW on the
//                  plate load. Deliberately independent of the Koren device law the
//                  solver used, so a self-consistent-but-wrong device fit cannot satisfy
//                  it. Cross-checked against the solver's own Ip.
//   legGain        |Δva| / |Δvg1| per leg, MEASURED by driving a fresh LtpInverter
//                  configured from the shipped `config()` with a small sine. The ratio of
//                  the two is the balance: a real long tail gives ~1.0, and an unbalanced
//                  pair leaks even harmonics no matter how well matched the output tubes
//                  are.
//
// Player-observable, not academic: a starved PI clips asymmetrically at low signal level
// and cannot swing the output tubes at all; an unbalanced PI puts 2nd harmonic where the
// push-pull topology is supposed to have cancelled it.
//
// Portable C++17, header-only.

#ifndef CLIPPER_TESTS_SUPPORT_LTP_PROBE_H
#define CLIPPER_TESTS_SUPPORT_LTP_PROBE_H

#include "clipper/dsp/Jcm800PowerAmp.h"  // LtpInverter lives here
#include "support/Xfail.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace clipper::test {

struct LtpMeasurement {
    double bPlus = 0.0;
    double va1 = 0.0, va2 = 0.0;          // quiescent plate voltages (V)
    double frac1 = 0.0, frac2 = 0.0;      // Va / B+
    double ip1 = 0.0, ip2 = 0.0;          // standing current per triode (A), Ohm's law
    double ipSolver = 0.0;                // the solver's own leg-1 Ip (A), for cross-check
    double iTail = 0.0;                   // total tail current (A)
    double g1 = 0.0, g2 = 0.0;            // small-signal leg gains (V/V)
    double ratio = 0.0;                   // min(g)/max(g) — 1.0 is balanced
    double ra1 = 0.0, ra2 = 0.0, rTail = 0.0;
};

// Measure the shipped inverter. `fs` only sets the length of the small-signal probe; the
// LTP itself is memoryless apart from its Newton warm start.
inline LtpMeasurement measureLtp(const clipper::dsp::LtpInverter& shipped, double fs) {
    const auto cfg = shipped.config();
    LtpMeasurement m;
    m.bPlus = cfg.bPlus;
    m.ra1 = cfg.Ra1;
    m.ra2 = cfg.Ra2;
    m.rTail = cfg.Rtail;
    m.va1 = shipped.quiescentPlate1();
    m.va2 = shipped.quiescentPlate2();
    m.frac1 = m.va1 / cfg.bPlus;
    m.frac2 = m.va2 / cfg.bPlus;
    m.ip1 = (cfg.bPlus - m.va1) / cfg.Ra1;
    m.ip2 = (cfg.bPlus - m.va2) / cfg.Ra2;
    m.ipSolver = shipped.quiescentCurrent1();
    // The tail returns to cfg.tailRef, not to ground (finding 7): the current through
    // Rtail is (Vk − tailRef)/Rtail. Reading it as Vk/Rtail was correct only while every
    // amp had tailRef = 0, and would UNDER-report the tail current on all three amps now.
    m.iTail = (shipped.quiescentTail() - cfg.tailRef) / cfg.Rtail;

    // Small-signal leg gains, from the SHIPPED configuration. Grid 2 is held at its
    // quiescent 0 (single-ended drive, the way every one of the three amps drives it).
    clipper::dsp::LtpInverter pi;
    pi.configure(cfg);
    pi.prepare();
    constexpr double kTwoPi = 6.283185307179586;
    const double f = 1000.0, amp = 0.02;  // 20 mV: well inside the linear region
    const int n = static_cast<int>(0.1 * fs);
    double mn1 = 1e30, mx1 = -1e30, mn2 = 1e30, mx2 = -1e30;
    for (int i = 0; i < n; ++i) {
        double a1 = 0.0, a2 = 0.0;
        pi.processSample(amp * std::sin(kTwoPi * f * i / fs), 0.0, a1, a2);
        if (i > n / 2) {  // second half only — past the warm-start transient
            mn1 = std::min(mn1, a1); mx1 = std::max(mx1, a1);
            mn2 = std::min(mn2, a2); mx2 = std::max(mx2, a2);
        }
    }
    m.g1 = (mx1 - mn1) / (2.0 * amp);
    m.g2 = (mx2 - mn2) / (2.0 * amp);
    m.ratio = std::min(m.g1, m.g2) / std::max(m.g1, m.g2);
    return m;
}

// The assertions that hold TODAY for every amp and must keep holding. The three project
// targets (plate fraction, standing current, leg balance) are asserted per-amp by the
// caller, because two of the three amps currently violate them (findings 7 and 8) and are
// recorded as XFAILs there.
inline void assertLtpSane(const LtpMeasurement& m, const char* amp) {
    assert(m.frac1 > 0.0 && m.frac1 < 1.0 && m.frac2 > 0.0 && m.frac2 < 1.0 &&
           "PI plate voltage outside (0, B+) — the load line is broken");
    assert(m.iTail > 0.0 && m.ipSolver > 0.0 && "PI tail/leg current not positive (cut off)");
    // Both legs must actually amplify. A 12AX7/12AT7 LTP leg is never below ×5, and even
    // in the shipped unbalanced state the pair stays inside 4:1 — a guard against a leg
    // going completely dead.
    assert(m.g1 > 5.0 && m.g2 > 5.0 && "a PI leg is not amplifying (gain below x5)");
    assert(m.ratio > 0.25 && "one PI leg is effectively dead (leg-gain ratio worse than 4:1)");
    // Ohm's law on Ra1 and the solver's own Ip must agree to 1 %. Catches a plate-load
    // resistor that does not match the solved operating point.
    assert(std::fabs(m.ip1 - m.ipSolver) <= 0.01 * m.ip1 + 1e-9 &&
           "PI leg-1 current from Ohm's law disagrees with the solved Ip");
    (void)amp;
}

// The three PROJECT TARGETS (docs/DEVELOPMENT.md, phase-inverter design note):
// 0.5–0.9 mA/triode, plates at 70–85 % of B+, and gain-balanced legs. Each is checked
// through `expectXfail`, so an amp that meets it passes for real and an amp that does not
// prints its real measured number and is recorded against its audit finding.
//
// Balance bar: 0.90. A real long-tailed pair with a proper tail reference and plate loads
// that compensate the finite tail impedance lands within a few per cent; the Twin measures
// 0.998 and the AC30 0.912, so 0.90 is not an aspiration.
//
// Each `*Decl` is NULLABLE and that is the whole point: pass `nullptr` for a target this
// amp MEETS and it becomes a hard `assert` — a real, load-bearing assertion that goes red
// if the amp ever regresses. Pass a declaration for a target this amp VIOLATES and it
// becomes a printed XFAIL against that finding.
//
// As checked out at 9923af7 (the ground-referenced tail, before finding 7):
//
//   amp     plate %B+        Ip/triode         leg balance
//   JCM800  94.7 % XFAIL 7   0.179 mA XFAIL 7  0.607 XFAIL 8
//   Twin    94.3 % XFAIL 7   0.232 mA XFAIL 7  0.990 ASSERTED
//   AC30    82.4 % ASSERTED  0.529 mA ASSERTED 0.550 XFAIL 7
//
// After finding 7 (2026-07-29 — `LtpInverter::Config::tailRef`, docs §42; every number
// below is identical at 44.1 / 48 / 96 kHz, the LTP being memoryless):
//
//   amp     plate %B+           Ip/triode              leg balance
//   JCM800  80.0/81.6 ASSERTED  0.680/0.763 ASSERTED   0.703 XFAIL 8
//   Twin    80.7/79.6 ASSERTED  0.792/0.703 ASSERTED   0.998 ASSERTED
//   AC30    79.3/78.5 ASSERTED  0.622/0.587 ASSERTED   0.912 ASSERTED
//
// Eight of the nine targets are now hard assertions. The one remaining XFAIL is the
// JCM800's leg balance, which is audit finding 8 (Ra2 = 82 kΩ compensates the finite tail
// impedance the wrong way) and NOT a tail problem: the tail fix moved it 0.607 -> 0.703,
// and the Twin/AC30 show the bar is reachable once the plate pair compensates.
inline void assertLtpTargets(const LtpMeasurement& m, double fs, const char* amp,
                             const XfailDecl* plateDecl, const XfailDecl* currentDecl,
                             const XfailDecl* balanceDecl) {
    char detail[256];

    const bool plateOk =
        m.frac1 > 0.70 && m.frac1 < 0.85 && m.frac2 > 0.70 && m.frac2 < 0.85;
    std::snprintf(detail, sizeof detail,
                  "%s @ %.0f Hz: Va1 = %.1f V (%.1f %% of B+ = %.0f V), Va2 = %.1f V (%.1f %%) "
                  "-- target 70-85 %%",
                  amp, fs, m.va1, 100.0 * m.frac1, m.bPlus, m.va2, 100.0 * m.frac2);
    if (plateDecl) {
        expectXfail(plateOk, *plateDecl, detail);
    } else {
        if (!plateOk) std::printf("  [FAIL] %s\n", detail);
        assert(plateOk && "PI plates not parked at 70-85 % of B+ (starved / saturated inverter)");
    }

    const bool ipOk = m.ip1 > 0.5e-3 && m.ip1 < 0.9e-3 && m.ip2 > 0.5e-3 && m.ip2 < 0.9e-3;
    std::snprintf(detail, sizeof detail,
                  "%s @ %.0f Hz: Ip1 = %.3f mA, Ip2 = %.3f mA (Ohm's law on Ra1/Ra2), tail = "
                  "%.3f mA -- target 0.5-0.9 mA/triode",
                  amp, fs, m.ip1 * 1e3, m.ip2 * 1e3, m.iTail * 1e3);
    if (currentDecl) {
        expectXfail(ipOk, *currentDecl, detail);
    } else {
        if (!ipOk) std::printf("  [FAIL] %s\n", detail);
        assert(ipOk && "PI standing current per triode outside the 0.5-0.9 mA design target");
    }

    const bool balOk = m.ratio >= 0.90;
    std::snprintf(detail, sizeof detail,
                  "%s @ %.0f Hz: leg gains x%.2f / x%.2f -> ratio %.3f (need >=0.90); "
                  "Ra1 = %.0fk, Ra2 = %.0fk, Rtail = %.1fk",
                  amp, fs, m.g1, m.g2, m.ratio, m.ra1 / 1e3, m.ra2 / 1e3, m.rTail / 1e3);
    if (balanceDecl) {
        expectXfail(balOk, *balanceDecl, detail);
    } else {
        if (!balOk) std::printf("  [FAIL] %s\n", detail);
        assert(balOk && "PI anti-phase legs not gain-balanced within 10 % (even harmonics leak)");
    }
}

inline void printLtp(const LtpMeasurement& m, const char* amp, double fs) {
    std::printf("  [ok] %s PI @ %.0f Hz: Va1=%.1f (%.1f%% B+) Va2=%.1f (%.1f%% B+), Ip "
                "%.3f/%.3f mA/triode (tail %.3f mA), legs x%.2f/x%.2f (ratio %.3f)\n",
                amp, fs, m.va1, 100.0 * m.frac1, m.va2, 100.0 * m.frac2, m.ip1 * 1e3,
                m.ip2 * 1e3, m.iTail * 1e3, m.g1, m.g2, m.ratio);
}

}  // namespace clipper::test

#endif  // CLIPPER_TESTS_SUPPORT_LTP_PROBE_H
