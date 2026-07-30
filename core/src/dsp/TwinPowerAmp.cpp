// Clipper — TwinPowerAmp (M10.1). See TwinPowerAmp.h for the model overview, the
// Koren 6L6GC + 12AT7 citations, the four-tube (paralleled push-pull) reflection,
// the no-presence NFB, and the light-sag rationale. This file is the numerics:
// the 6L6 device law (via the shared pentode evaluators), the reused 12AT7 LTP
// phase inverter, the per-tube plate-load solve, the PI→6L6 coupling/blocking,
// the OT + flat NFB, and the B+ rail/screen sag integration.

#include "clipper/dsp/TwinPowerAmp.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"
#include "clipper/dsp/TubeSolverMode.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
inline double softplus(double u) {
    if (u > 30.0) return u;
    if (u < -30.0) return std::exp(u);
    return std::log1p(std::exp(u));
}
inline double sigmoid(double u) {
    if (u > 30.0) return 1.0;
    if (u < -30.0) return std::exp(u);
    const double e = std::exp(u);
    return e / (1.0 + e);
}
}  // namespace

// ===========================================================================
// TwinPowerAmp
// ===========================================================================
TwinPowerAmp::TwinPowerAmp() {
    tube6L6_ = to6L6(tube6L6cfg_);
    LtpInverter::Config c;
    // 12AT7 phase inverter: lower-mu, higher-current than the JCM's 12AX7 PI.
    c.tube.mu = 60.0; c.tube.ex = 1.35; c.tube.kg1 = 460.0;
    c.tube.kp = 300.0; c.tube.kvb = 300.0;
    // BALANCED large-signal legs: an asymmetric plate-load pair (Ra1 100k input side,
    // Ra2 119k feedback side) compensates the finite tail impedance and equalizes the
    // two anti-phase plate swings — this cancellation is what keeps the push-pull's
    // EVEN harmonics down, the key to a CLEAN power stage (an unbalanced PI leaks a
    // strong 2nd harmonic even with matched tubes).
    //
    // TAIL (2026-07-24 audit finding 7, docs §42). A real long-tailed pair returns its
    // tail through a LARGE resistor to a NEGATIVE reference: the resistor sets the
    // COMMON-MODE REJECTION, the reference sets the standing current, and the two are
    // independent. Modelled as two terminals (Rtail straight to ground) they collapse
    // into one number, and this PI paid for its standing current with cutoff: 22 k to
    // ground idled the pair at 0.232 mA/triode with its plates at 94.3 % of B+, at
    // ×7.4/×7.5 per leg (the old "ratio 0.990" was balance between two nearly-dead legs).
    //
    // The tail stays LONG — Rtail = 22 k, the shipped value — and gains a reference:
    //   Rtail 22 k, tailRef = -26 V -> Va 330.8/326.4 V (80.7 / 79.6 % of B+),
    //   0.792/0.703 mA per triode, 1.494 mA tail, legs ×13.93/×13.90, ratio 0.9978
    //   (identical at 44.1 / 48 / 96 kHz — the LTP solve is memoryless).
    // All three project targets (70-85 % of B+, 0.5-0.9 mA/triode, ratio >= 0.90) are met
    // and are HARD assertions in core/tests/test_twin_amp.cpp. tailRef = -26 V is the
    // value that maximises the MINIMUM normalised margin to those three bounds (scored
    // -20 V 0.15 · -22 V 0.29 · -24 V 0.44 · -26 V 0.54 · -28 V 0.32 · -30 V 0.10, each
    // with Ra2 re-balanced), the same scoring the JCM's -12 V was chosen by.
    //
    // WHY THE TAIL IS NOT SHORTENED TO THE 10 k ON THE SCHEMATIC (docs §42, phase 2 —
    // this is the one place this amp deliberately departs from a parts-bin value, and it
    // is departed from BY MEASUREMENT). The audit and the slice plan both said to take
    // Rtail 22 k -> 10 k. Measured, that costs 6.8 dB of tail impedance, i.e. 6.8 dB of
    // common-mode rejection — and this amp injects its global NFB SINGLE-ENDEDLY into the
    // cold grid, so half of the feedback signal IS common mode. What leaks through the
    // tail arrives IN PHASE on both 6L6 grids, which is exactly the drive a push-pull
    // pair cannot cancel, so it comes out as 2nd harmonic. Power section alone, 110 Hz,
    // 0.5 V at the PI grid, h2 relative to the fundamental:
    //   config                          open loop     closed loop
    //   22 k to ground (starved)         -45.9 dBc     -47.3 dBc   (NFB reduces h2)
    //   10 k / -7 V                      -61.9 dBc     -33.0 dBc   (NFB ADDS 24 dB)
    //   22 k / -26 V (shipped)           -73.7 dBc     -39.9 dBc
    // The corrected PI is 16-28 dB cleaner OPEN loop; with the loop closed the short
    // tail throws all of that away and then some, and the composed amp's documented
    // clean-headroom bar (< 4 % THD at VOLUME 0.5 / hot 0.10 V DI) went 2.96 % -> 4.5 %
    // FAIL at 10 k, against 3.40 % PASS at 22 k. A model whose CMRR is set by one
    // resistor has to keep that resistor long; the 10 k on the drawing sits above a
    // 470 Ω-per-cathode network and a real negative return that this two-node tail does
    // not represent.
    //
    // Ra2 was NOT changed to 100 k, which is what the audit proposed: measured with the
    // tail fixed, matched 100k/100k loads give a leg ratio of 0.718, and no tailRef
    // meeting the DC targets gets past 0.79. Unequal plate loads are how a finite-tail
    // LTP is balanced — that is what this resistor is for.
    //
    // Ra2 WAS re-derived, 142 k -> 119 k (docs §42, phase 2). 142 k was fitted for
    // balance against the OLD ground-referenced tail (§20 recorded the result: "measured
    // swing ratio 1.007", i.e. balanced to ~1 %), so it is a constant fitted around the
    // finding-7 defect, and it is un-fitted here in the same slice as the fix (the
    // ADR 008 precedent). Re-derived BY MEASUREMENT against the corrected tail, to the
    // SAME documented convention — legs balanced to ~1 % — with the DC windows held: at
    // Rtail 22 k / tailRef -26 V the ratio peaks at Ra2 118k 0.9905 · 119k 0.9978 ·
    // 120k 0.9949 · 125k 0.968, so 119 k is the balance optimum. (With the tail fixed the
    // balance crossover moves BELOW 142 k, not above it: a longer effective tail needs
    // less plate-load compensation, which is the same physics as the CMRR note above.)
    c.bPlus = 410.0; c.Ra1 = 100.0e3; c.Ra2 = 119.0e3; c.Rtail = 22.0e3; c.tailRef = -26.0;
    ltp_.configure(c);
}

void TwinPowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    tube6L6_ = to6L6(tube6L6cfg_);
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void TwinPowerAmp::setOversampling(int factor) {
    os_.setFactor(factor);
    osRate_ = sampleRate_ * os_.factor();
    const double T = 1.0 / osRate_;

    gRes_ = kCreservoir / T;
    gScr_ = kCscreen / T;
    gCc_ = kCoupCc / T;
    gRg_ = 1.0 / kCoupRg;

    auto onePoleA = [&](double fc) {
        const double g = std::tan(M_PI * fc / osRate_);
        return g / (1.0 + g);
    };
    otHpA_ = onePoleA(kOtLfHz);
    otLpA_ = onePoleA(kOtHfHz);

    ltp_.prepare();
    solveOperatingPoint();
    parkState();
}

// Park all dynamic state at the operating point. Called after the DC solve at
// prepare/OS change, and by reset() WITHOUT a re-solve.
void TwinPowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    vCcUp_ = ltp_.quiescentPlate1() - kVbias;
    vCcDown_ = ltp_.quiescentPlate2() - kVbias;
    vgUp_ = vgDown_ = kVbias;   // grid warm starts at idle
    otHpS_ = 0.0; otLpS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void TwinPowerAmp::reset() {
    ltp_.reset();
    parkState();
    os_.reset();
}

// Self-consistent idle. Four tubes (2 per side): total plate/screen draw = 4×tube.
void TwinPowerAmp::solveOperatingPoint() {
    const int nTot = 2 * kTubesPerSide;  // 4 tubes
    double rail = kVsupply - 0.14 * kRsupply;   // guess ~140 mA total draw
    double scr = rail - 0.012 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 300; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, tube6L6_);
        ig2 = el34ScreenCurrent(kVbias, scr, tube6L6_);
        const double newRail = kVsupply - (nTot * ip + nTot * ig2) * kRsupply;
        const double newScr = newRail - (nTot * ig2) * kRscreen;
        if (std::fabs(newRail - rail) < 1e-6 && std::fabs(newScr - scr) < 1e-6) {
            rail = newRail; scr = newScr; break;
        }
        rail = newRail; scr = newScr;
    }
    vRailIdle_ = rail;
    vScreenIdle_ = scr;
    iqTube_ = ip;
    ig2qTube_ = ig2;
}

void TwinPowerAmp::setParameter(int paramId, float value) {
    // NaN-rejecting (ParamGuard.h) — audit finding 1.
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        default: break;
    }
}

// Plate-load Newton with the hoisted Koren base and exact dIp/dVp (see the JCM's
// solveTubePlate; §25, regression-gated): one atan per iteration, same root.
inline double TwinPowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                           double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, tube6L6_);
    const double kvb = tube6L6_.kvb;
    double Vp = rail;
    for (int it = 0; it < 40; ++it) {
        const double u = Vp / kvb;
        const double i = base * std::atan(u);
        const double f = Vp - (rail - (i - iqTube_) * kRppReflected);
        const double df = 1.0 + (base / (kvb * (1.0 + u * u))) * kRppReflected;
        double dv = -f / df;
        dv = std::clamp(dv, -150.0, 150.0);
        Vp += dv;
        if (Vp < 0.0) Vp = 0.01;
        if (std::fabs(dv) < tol) break;
    }
    vpOut = Vp;
    baseOut = base;
    return base * std::atan(Vp / kvb);
}

// 6L6 grid node: PI plate AC through the coupling cap into the grid leak (to
// Vbias) + grid conduction. Same structure as the M9.3 EL34 grid solve.
inline double TwinPowerAmp::solveTubeGrid(double vpPlateAC, double vpPlateQ,
                                          double& vCc, double& vgWarm) const {
    const double vp = vpPlateQ + vpPlateAC;
    const double ig0 = gridVgn_ / gridRgk_;
    const double tol = 1e-7 * tubeSolverTolScale();
    double Vg = vgWarm;  // warm start from the previous sample's solution (§25)
    for (int it = 0; it < 40; ++it) {
        const double u = Vg / gridVgn_;
        const double Igk = ig0 * softplus(u);
        const double dIgk = (ig0 / gridVgn_) * sigmoid(u);
        const double r = gCc_ * ((vp - Vg) - vCc) - (Vg - kVbias) * gRg_ - Igk;
        const double dr = -gCc_ - gRg_ - dIgk;
        double dVg = -r / dr;
        dVg = std::clamp(dVg, -100.0, 100.0);
        Vg += dVg;
        if (std::fabs(dVg) < tol) break;
    }
    vCc = (vp - Vg);
    vgWarm = Vg;
    return Vg;
}

inline float TwinPowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Phase inverter. V3A grid = input; V3B grid = FLAT feedback (no presence
    //    shaping — the Twin has no presence control). NEGATIVE feedback opposes
    //    the output: inject −β·(secondary) at the cold grid.
    const double vfb = -feedbackBeta() * fbDelay_;
    double va1 = 0.0, va2 = 0.0;
    ltp_.processSample(x, vfb, va1, va2);
    const double va1AC = va1 - ltp_.quiescentPlate1();
    const double va2AC = va2 - ltp_.quiescentPlate2();

    // 2. PI plates → 6L6 grids (coupling + blocking). Anti-phase legs.
    const double vgUp = solveTubeGrid(va1AC, ltp_.quiescentPlate1(), vCcUp_, vgUp_);
    const double vgDown = solveTubeGrid(va2AC, ltp_.quiescentPlate2(), vCcDown_, vgDown_);

    // 3. 6L6 pair (each = kTubesPerSide paralleled tubes) at the sagged rail.
    //    Screen currents reuse the plate solve's hoisted base: Ig2 = base·kg1/kg2.
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = tube6L6_.kg1 / tube6L6_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 4. Output transformer (linear): differential primary → secondary. The per-
    //    tube diff × kRppReflected already carries the kTubesPerSide factor (Raa/2).
    const double vPriDiff = (ipUp - ipDown) * kRppReflected;
    double vSec = vPriDiff / otTurnsRatio();
    {
        // Anti-denormal (Denormal.h) on both TPT states — vSec rests at zero, so on
        // silence these sink into the double subnormal range and stick. `vLp`/`vLp2`
        // are untouched, so the tripping sample stays bit-identical. Finding 11, §33.
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = flushDenormal(2.0 * vLp - otLpS_);
        vSec = vLp;
        const double vLp2 = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = flushDenormal(2.0 * vLp2 - otHpS_);
        vSec = vSec - vLp2;
    }

    // 5. Sag (LIGHT): pull rail + screen down from the total 4-tube draw.
    const double iLoad = static_cast<double>(kTubesPerSide) *
                         (ipUp + ipDown + ig2Up + ig2Down);
    const double iScr = static_cast<double>(kTubesPerSide) * (ig2Up + ig2Down);
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iLoad) /
             (gRes_ + 1.0 / kRsupply);
    vScreen_ = (gScr_ * vScreen_ + vRail_ / kRscreen - iScr) /
               (gScr_ + 1.0 / kRscreen);

    // 6. Flat feedback (unit delay for the next sample) — NO presence low-pass.
    // Anti-denormal (Denormal.h): a FEEDBACK node resting at zero, so a subnormal
    // parked here is re-multiplied by beta and re-injected at the cold grid every
    // sample, forever. Audit finding 11, docs §33.
    fbDelay_ = flushDenormal(vSec);

    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void TwinPowerAmp::process(const float* in, float* out, int numFrames) {
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        os_.upsample(in + off, n);
        float* w = os_.buffer();
        const int osN = os_.bufferLength();
        for (int i = 0; i < osN; ++i) w[i] = processSampleOS(w[i]);
        os_.downsample(out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
