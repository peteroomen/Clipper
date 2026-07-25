// Clipper — Ac30PowerAmp (M10.2). See Ac30PowerAmp.h for the model overview, the
// EL84 Koren fit, the SHARED cathode-bias network (the centerpiece), the no-NFB
// stance, the TOP CUT control, and the deep GZ34 sag rationale. This file is the
// numerics: the EL84 device law (via the shared pentode evaluators), the reused
// (hot, mildly-asymmetric) 12AX7 LTP phase inverter, the post-PI TOP CUT one-pole,
// the per-tube plate-load solve, the PI→EL84 coupling/blocking referenced to the
// dynamic cathode, the OT, and the coupled rail/screen/CATHODE integration.

#include "clipper/dsp/Ac30PowerAmp.h"

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
// Ac30PowerAmp
// ===========================================================================
Ac30PowerAmp::Ac30PowerAmp() {
    tubeEl84_ = toEl84(tubeEl84cfg_);
    LtpInverter::Config c;
    // 12AX7 phase inverter (same device fit as the JCM PI). Run HOT off a lower B+
    // node and DELIBERATELY less balanced than the Twin's laser-matched PI: a mildly
    // asymmetric plate pair (100k/110k) plus a moderate tail leaves a residual even-
    // harmonic imbalance and lets one leg reach cutoff EARLY — the AC30 PI clip +
    // part of the chime (the opposite intent from the Twin's clean, matched PI).
    c.tube.mu = 100.0; c.tube.ex = 1.4; c.tube.kg1 = 1060.0;
    c.tube.kp = 600.0; c.tube.kvb = 300.0;
    // Rtail = the OPERATING-POINT constant (M10.2 §23 second amendment — the "not
    // enough gain/breakup" field report). Our LtpInverter is a TWO-TERMINAL tail:
    // grids at 0 V DC, tail resistor straight to ground, so the standing current is
    // set entirely by Rtail. With the original 22 k the pair self-biased at 83 µA per
    // triode — plates parked at 291.7 V, i.e. 97 % of B+, essentially cut off — and
    // measured only ×9.1 gain per leg. No real 12AX7 LTP idles there: a guitar-amp
    // PI runs ~0.5–0.9 mA per triode with its plates at 70–85 % of B+ and ~×25–35 per
    // leg (the real amps get there by returning the long tail to a negative reference
    // our two-terminal tail cannot express, so the tail VALUE is that network's model
    // equivalent, not a parts-bin resistor). 2.2 k lands the textbook point: 0.53 mA,
    // plates 247.1/244.9 V (82 % of B+), gain ×32. This is what finally makes the
    // header's "run HOT, clip early" TRUE rather than aspirational — the starved PI
    // was the reason the VOLUME knob (which IS the AC30's overdrive) could not reach
    // the EL84 grids. MEASURED before/after in docs §23.
    c.bPlus = 300.0; c.Ra1 = 100.0e3; c.Ra2 = 110.0e3; c.Rtail = 2.2e3;
    ltp_.configure(c);
}

void Ac30PowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    tubeEl84_ = toEl84(tubeEl84cfg_);
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void Ac30PowerAmp::setOversampling(int factor) {
    os_.setFactor(factor);
    osRate_ = sampleRate_ * os_.factor();
    const double T = 1.0 / osRate_;

    gRes_ = kCreservoir / T;
    gScr_ = kCscreen / T;
    gCk_ = kCkCathode / T;          // SHARED cathode cap companion conductance
    gCc_ = kCoupCc / T;
    gRg_ = 1.0 / kCoupRg;

    auto onePoleA = [&](double fc) {
        const double g = std::tan(M_PI * fc / osRate_);
        return g / (1.0 + g);
    };
    otHpA_ = onePoleA(kOtLfHz);
    otLpA_ = onePoleA(kOtHfHz);
    // GZ34 demand-current envelope: asymmetric one-pole (fast attack, slow release).
    gSagAtk_ = 1.0 - std::exp(-2.0 * M_PI * kSagAtkHz / osRate_);
    gSagRel_ = 1.0 - std::exp(-2.0 * M_PI * kSagRelHz / osRate_);

    // TOP CUT corner: log map kTopCutHiHz (knob 0) -> kTopCutLoHz (knob 1), applied to
    // the SKEWED knob (knob^kTopCutSkew) so the default 0.5 is a MILD cut (docs §23).
    const double fc = kTopCutHiHz *
                      std::pow(kTopCutLoHz / kTopCutHiHz, std::pow(topCut_, kTopCutSkew));
    topCutA_ = onePoleA(fc);

    ltp_.prepare();
    solveOperatingPoint();

    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    vk_ = vkIdle_;
    iIdleTotal_ = 2.0 * kTubesPerSide * (iqTube_ + ig2qTube_);  // envelope floor
    iSagEnv_ = iIdleTotal_;
    vCcUp_ = ltp_.quiescentPlate1();   // grid DC-referenced to GROUND (0), so the
    vCcDown_ = ltp_.quiescentPlate2(); // idle coupling-cap voltage = the PI plate V.
    vgUp_ = vgDown_ = 0.0;             // grid warm starts at idle (leak to ground)
    otHpS_ = 0.0; otLpS_ = 0.0;
    topCutS1_ = 0.0; topCutS2_ = 0.0;
    lastOutPeak_ = 0.0;
}

// Self-consistent idle: co-solve the SHARED cathode voltage vk (bias = -vk), the
// rail, and the screen. Four tubes (2 per side) draw through the supply and share
// ONE cathode network, so all four sit at the same operating point at idle.
void Ac30PowerAmp::solveOperatingPoint() {
    const int nTot = 2 * kTubesPerSide;  // 4 tubes
    // The self-consistent class-A idle: the shared cathode voltage vk sets the bias
    // (Vg1k = −vk, grids at 0), and vk must equal Rk·(total cathode current). Higher
    // vk ⇒ colder bias ⇒ less current, while the "self-bias target" vk/Rk rises with
    // vk, so the residual (actual − target current) is monotone in vk — BISECTION is
    // robust where the fixed-point iteration oscillates (the cathode loop is tight).
    auto currentsAt = [&](double vk, double& ip, double& ig2, double& rail, double& scr) {
        const double iTarget = vk / kRkCathode;               // cathode current for this vk
        const double Reff = kRsupply * (1.0 + kRectKnee * iTarget);  // GZ34 soft knee
        rail = kVsupply - iTarget * Reff;                     // rail droop from the draw
        scr = rail;
        for (int k = 0; k < 40; ++k) {                        // settle the screen node (damped)
            ig2 = el34ScreenCurrent(-vk, scr, tubeEl84_);
            const double scrNew = rail - (nTot * ig2) * kRscreen;
            scr = scr + 0.35 * (scrNew - scr);                // damping keeps it stable
        }
        ip = el34PlateCurrent(rail, -vk, scr, tubeEl84_);
        return nTot * (ip + ig2) - iTarget;                   // residual
    };
    double lo = 0.5, hi = 40.0, ip = 0.0, ig2 = 0.0, rail = 0.0, scr = 0.0;
    double fLo, il, ig, rl, sc;
    fLo = currentsAt(lo, il, ig, rl, sc);
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        const double fMid = currentsAt(mid, ip, ig2, rail, scr);
        if ((fMid > 0.0) == (fLo > 0.0)) { lo = mid; fLo = fMid; }
        else hi = mid;
        if (hi - lo < 1e-8) break;
    }
    const double vk = 0.5 * (lo + hi);
    currentsAt(vk, ip, ig2, rail, scr);
    vkIdle_ = vk;
    vRailIdle_ = rail;
    vScreenIdle_ = scr;
    iqTube_ = ip;
    ig2qTube_ = ig2;
}

void Ac30PowerAmp::setParameter(int paramId, float value) {
    const double v = std::clamp(static_cast<double>(value), 0.0, 1.0);
    switch (paramId) {
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        case PARAM_TOPCUT: {
            topCut_ = v;
            // Skewed log map (docs §23): knob^kTopCutSkew so default 0.5 is a mild cut.
            const double fc = kTopCutHiHz *
                              std::pow(kTopCutLoHz / kTopCutHiHz, std::pow(topCut_, kTopCutSkew));
            const double g = std::tan(M_PI * fc / osRate_);
            topCutA_ = g / (1.0 + g);
            break;
        }
        default: break;
    }
}

// Plate-load Newton with the hoisted Koren base and exact dIp/dVp (see the JCM's
// solveTubePlate; §25, regression-gated): one atan per iteration, same root.
inline double Ac30PowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                           double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, tubeEl84_);
    const double kvb = tubeEl84_.kvb;
    double Vp = rail;
    for (int it = 0; it < 40; ++it) {
        const double u = Vp / kvb;
        const double i = base * std::atan(u);
        const double f = Vp - (rail - (i - iqTube_) * kRppReflected);
        const double df = 1.0 + (base / (kvb * (1.0 + u * u))) * kRppReflected;
        double dv = -f / df;
        dv = std::clamp(dv, -120.0, 120.0);
        Vp += dv;
        if (Vp < 0.0) Vp = 0.01;
        if (std::fabs(dv) < tol) break;
    }
    vpOut = Vp;
    baseOut = base;
    return base * std::atan(Vp / kvb);
}

// EL84 grid node: PI plate AC through the coupling cap into the grid leak (to
// GROUND — cathode bias has no fixed negative supply) + grid conduction (relative
// to the shared cathode vk). Returns the grid voltage Vg (absolute, ref. ground).
inline double Ac30PowerAmp::solveTubeGrid(double vpPlateAC, double vpPlateQ,
                                          double& vCc, double& vgWarm) const {
    const double vp = vpPlateQ + vpPlateAC;
    const double ig0 = gridVgn_ / gridRgk_;
    const double tol = 1e-7 * tubeSolverTolScale();
    double Vg = vgWarm;  // warm start from the previous sample's solution (§25)
    for (int it = 0; it < 40; ++it) {
        const double u = (Vg - vk_) / gridVgn_;       // conduction when Vg > Vk
        const double Igk = ig0 * softplus(u);
        const double dIgk = (ig0 / gridVgn_) * sigmoid(u);
        const double r = gCc_ * ((vp - Vg) - vCc) - Vg * gRg_ - Igk;
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

inline float Ac30PowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Phase inverter (HOT). NO feedback — the cold-side grid sees 0 (there is no
    //    loop; fbEnabled_ is inert, the anti-NFB test relies on this being a no-op).
    double va1 = 0.0, va2 = 0.0;
    ltp_.processSample(x, 0.0, va1, va2);
    double va1AC = va1 - ltp_.quiescentPlate1();
    double va2AC = va2 - ltp_.quiescentPlate2();

    // 2. TOP CUT: one-pole low-pass on each anti-phase leg (a cap across the PI
    //    outputs). Higher knob -> lower corner -> more treble removed (inverted sense).
    {
        const double lp1 = topCutS1_ + topCutA_ * (va1AC - topCutS1_);
        topCutS1_ = 2.0 * lp1 - topCutS1_;
        va1AC = lp1;
        const double lp2 = topCutS2_ + topCutA_ * (va2AC - topCutS2_);
        topCutS2_ = 2.0 * lp2 - topCutS2_;
        va2AC = lp2;
    }

    // 3. PI plates → EL84 grids (coupling + blocking, grid leak to ground).
    const double vgUp = solveTubeGrid(va1AC, ltp_.quiescentPlate1(), vCcUp_, vgUp_);
    const double vgDown = solveTubeGrid(va2AC, ltp_.quiescentPlate2(), vCcDown_, vgDown_);

    // 4. EL84 pair at the sagged rail. Grid-cathode bias rides on the DYNAMIC shared
    //    cathode (previous sample's vk_ — decoupled like the rail/screen). This is
    //    where the class-A bias-shift compression comes from.
    const double vg1kUp = vgUp - vk_;
    const double vg1kDown = vgDown - vk_;
    //    Screen currents reuse the plate solve's hoisted base: Ig2 = base·kg1/kg2.
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vg1kUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vg1kDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = tubeEl84_.kg1 / tubeEl84_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 5. Output transformer (linear): differential primary → secondary.
    const double vPriDiff = (ipUp - ipDown) * kRppReflected;
    double vSec = vPriDiff / otTurnsRatio();
    {
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = 2.0 * vLp - otLpS_;
        vSec = vLp;
        const double vLp2 = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = 2.0 * vLp2 - otHpS_;
        vSec = vSec - vLp2;
    }

    // 6a. Physical rail + screen + CATHODE integration (backward Euler). The plate rail
    //     droops modestly from the near-constant class-A B+ draw (the soft-knee on the
    //     actual cathode current); the cathode cap charges/discharges on the summed
    //     cathode current with τ = Rk·Ck — the measured DYNAMIC BIAS SHIFT (bloom).
    const double iCath = static_cast<double>(kTubesPerSide) *
                         (ipUp + ipDown + ig2Up + ig2Down);
    const double iScr = static_cast<double>(kTubesPerSide) * (ig2Up + ig2Down);
    const double Reff = kRsupply * (1.0 + kRectKnee * iCath);
    vRail_ = (gRes_ * vRail_ + kVsupply / Reff - iCath) / (gRes_ + 1.0 / Reff);
    vScreen_ = (gScr_ * vScreen_ + vRail_ / kRscreen - iScr) /
               (gScr_ + 1.0 / kRscreen);
    // SHARED cathode node: Ck·dVk/dt + Vk/Rk = iCath  (backward Euler). Vk RISES under
    // sustained drive as the average cathode current grows → bias cools → the bloom.
    vk_ = (gCk_ * vk_ + iCath) / (gCk_ + 1.0 / kRkCathode);

    // 6b. GZ34 SAG (§6). A BALANCED class-A push-pull draws a near-CONSTANT total B+
    //     current, so its PLATE rail barely sags — the deep AC30 sag is the valve
    //     rectifier failing to supply the DELIVERED SIGNAL CURRENT (the differential
    //     push-pull current into the OT primary, which swells with output). We model
    //     that as a demand-envelope COMPRESSION of the delivered output (a documented
    //     voicing of the rectifier's peak-current limit — the rail's lost headroom
    //     applied to the secondary, decoupled from the tube DC bias so the class-A
    //     bloom above is preserved). Demand = idle draw + |differential current|; a
    //     fast-attack / slow-release envelope drives the compression, which recovers
    //     on the release RC (the audible bloom → squash → recover).
    const double iDemand = iIdleTotal_ +
        static_cast<double>(kTubesPerSide) * std::fabs(ipUp - ipDown);
    const double aEnv = (iDemand > iSagEnv_) ? gSagAtk_ : gSagRel_;
    iSagEnv_ += aEnv * (iDemand - iSagEnv_);
    const double sagComp = 1.0 / (1.0 + kSagCompGain * (iSagEnv_ - iIdleTotal_));
    vSec *= sagComp;

    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void Ac30PowerAmp::process(const float* in, float* out, int numFrames) {
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
