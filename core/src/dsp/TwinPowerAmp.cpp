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
    // BALANCED large-signal legs: a LONG tail (22k) plus an asymmetric plate-load
    // pair (Ra1 100k input side, Ra2 142k feedback side) equalize the two anti-
    // phase plate swings to within ~1% (measured ratio 1.007) — this cancellation
    // is what keeps the push-pull's EVEN harmonics down, the key to a CLEAN power
    // stage (an unbalanced PI leaks a strong 2nd harmonic even with matched tubes).
    c.bPlus = 410.0; c.Ra1 = 100.0e3; c.Ra2 = 142.0e3; c.Rtail = 22.0e3;
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
