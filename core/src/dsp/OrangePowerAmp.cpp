// Clipper — OrangePowerAmp (M10.3, docs §57). See OrangePowerAmp.h for the
// topology, the sourced facts and every documented simplification. This file is
// the numerics: the driver+cathodyne 3x3 Newton, the EL34 QUAD plate solve, the
// inverter->EL34 coupling/blocking, the OT + NFB + HF DRIVE, and the rail sag.

#include "clipper/dsp/OrangePowerAmp.h"

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

// Koren 12AX7 current + partials. Same device law as TriodeStage.cpp and
// Jcm800PowerAmp.cpp's LTP (both file-local there); re-stated here so the
// cathodyne shares the M9.1 fit exactly — no new triode model.
struct TriodeEval {
    double Ip, dVa, dVgk;
};
TriodeEval triodeEval(double Va, double Vgk, const TriodeStage::KorenParams& p) {
    const double S = std::sqrt(p.kvb + Va * Va);
    const double A = p.kp * (1.0 / p.mu + Vgk / S);
    const double L = softplus(A);
    const double sig = sigmoid(A);
    const double E1 = (Va / p.kp) * L;
    if (E1 <= 0.0) return {0.0, 0.0, 0.0};
    const double C = 2.0 / p.kg1;
    const double Ip = C * std::pow(E1, p.ex);
    const double dIp_dE1 = p.ex * Ip / E1;
    const double dE1_dVgk = Va * sig / S;
    const double dE1_dVa = L / p.kp - (Va * Va * Vgk * sig) / (S * S * S);
    return {Ip, dIp_dE1 * dE1_dVa, dIp_dE1 * dE1_dVgk};
}

}  // namespace

// ===========================================================================
// CathodyneInverter
// ===========================================================================
namespace {
// One Newton step of the driver+cathodyne system. Shared by prepare() (cold start,
// wide clamps, many iterations) and processSample() (warm start, tight clamps), so
// the DC point the tests read and the point the audio path tracks can never be two
// different equations. Returns false if the Jacobian is degenerate.
struct CathState {
    double vpd, vkd, vkc;
};
bool cathodyneStep(const CathodyneInverter::Config& cfg, double vg, double vfb,
                   CathState& s, double clampP, double clampK, double& maxStep) {
    const TriodeEval d = triodeEval(s.vpd - s.vkd, vg - s.vkd, cfg.tube);
    const double vpc = cfg.bPlusCathodyne - 2.0 * s.vkc;  // cathodyne plate-cathode
    const TriodeEval c = triodeEval(vpc, s.vpd - s.vkc, cfg.tube);

    const double gAd = 1.0 / cfg.Rad, gKd = 1.0 / cfg.Rkd, gFb = 1.0 / cfg.Rfb;
    const double gR = 1.0 / cfg.Rsplit;

    const double r1 = (cfg.bPlusDriver - s.vpd) * gAd - d.Ip;
    const double r2 = d.Ip - s.vkd * gKd - (s.vkd - vfb) * gFb;
    const double r3 = c.Ip - s.vkc * gR;

    // Jacobian. Driver: Ipd = f(Vpd - Vkd, vg - Vkd) so
    //   dIpd/dVpd = d.dVa ; dIpd/dVkd = -(d.dVa + d.dVgk).
    // Cathodyne: Ipc = f(B+c - 2Vkc, Vpd - Vkc) so
    //   dIpc/dVpd = c.dVgk ; dIpc/dVkc = -2*c.dVa - c.dVgk.
    const double dIpd_dVpd = d.dVa;
    const double dIpd_dVkd = -(d.dVa + d.dVgk);
    const double dIpc_dVpd = c.dVgk;
    const double dIpc_dVkc = -2.0 * c.dVa - c.dVgk;

    const double J00 = -gAd - dIpd_dVpd, J01 = -dIpd_dVkd;
    const double J10 = dIpd_dVpd, J11 = dIpd_dVkd - gKd - gFb;
    const double J20 = dIpc_dVpd, J22 = dIpc_dVkc - gR;

    // Rows 0/1 close over (vpd, vkd) only; row 2 adds vkc and couples back through
    // vpd. Solve the 2x2 first, then vkc.
    const double det = J00 * J11 - J01 * J10;
    if (std::fabs(det) < 1e-30 || std::fabs(J22) < 1e-30) return false;
    double dvpd = (-r1 * J11 - J01 * -r2) / det;
    double dvkd = (J00 * -r2 - J10 * -r1) / det;
    double dvkc = (-r3 - J20 * dvpd) / J22;

    dvpd = std::clamp(dvpd, -clampP, clampP);
    dvkd = std::clamp(dvkd, -clampK, clampK);
    dvkc = std::clamp(dvkc, -clampP, clampP);
    s.vpd += dvpd;
    s.vkd += dvkd;
    s.vkc += dvkc;
    // The cathodyne cathode is physically pinned to [0, B+c]: outside that the
    // Koren law's plate voltage would go negative and the solve can wander into a
    // non-physical branch. This clamp IS the compliance limit of a real split
    // load, not a numerical band-aid.
    s.vkc = std::clamp(s.vkc, 0.0, 0.5 * cfg.bPlusCathodyne);
    maxStep = std::max({std::fabs(dvpd), std::fabs(dvkd), std::fabs(dvkc)});
    return true;
}
}  // namespace

void CathodyneInverter::prepare() {
    CathState s{0.45 * cfg_.bPlusDriver, 1.0, 0.35 * cfg_.bPlusCathodyne};
    for (int it = 0; it < 400; ++it) {
        double step = 0.0;
        if (!cathodyneStep(cfg_, 0.0, 0.0, s, 40.0, 4.0, step)) break;
        if (step < 1e-10) break;
    }
    vpd_ = vpdq_ = s.vpd;
    vkd_ = vkdq_ = s.vkd;
    vkc_ = vkcq_ = s.vkc;
    ipdq_ = triodeEval(s.vpd - s.vkd, -s.vkd, cfg_.tube).Ip;
    ipcq_ = triodeEval(cfg_.bPlusCathodyne - 2.0 * s.vkc, s.vpd - s.vkc, cfg_.tube).Ip;
}

void CathodyneInverter::processSample(double vg, double vfb, double& vPlate,
                                      double& vCath) {
    const double tol = 1e-7 * tubeSolverTolScale();  // §25, regression-gated
    CathState s{vpd_, vkd_, vkc_};                   // warm start
    for (int it = 0; it < 40; ++it) {
        double step = 0.0;
        if (!cathodyneStep(cfg_, vg, vfb, s, 80.0, 20.0, step)) break;
        if (step < tol) break;
    }
    vpd_ = s.vpd;
    vkd_ = s.vkd;
    vkc_ = s.vkc;
    // The two outputs come off ONE current through TWO equal resistors, so they are
    // anti-phase and equal by construction (see the header).
    vPlate = cfg_.bPlusCathodyne - s.vkc;
    vCath = s.vkc;
}

// ===========================================================================
// OrangePowerAmp
// ===========================================================================
OrangePowerAmp::OrangePowerAmp() {
    CathodyneInverter::Config c;
    inv_.configure(c);
}

void OrangePowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void OrangePowerAmp::setOversampling(int factor) {
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
    hfLpA_ = onePoleA(kHfDriveHz);

    inv_.prepare();
    solveOperatingPoint();
    parkState();
}

void OrangePowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    // The coupling caps block the inverter's two DC levels (which are NOT the same
    // — the plate sits at B+c - Vk, the cathode at Vk), so each side parks at its
    // own DC difference to the fixed bias.
    vCcUp_ = inv_.quiescentCathodynePlate() - kVbias;
    vCcDown_ = inv_.quiescentCathodyneCathode() - kVbias;
    vgUp_ = vgDown_ = kVbias;
    otHpS_ = 0.0;
    otLpS_ = 0.0;
    hfLpS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void OrangePowerAmp::reset() {
    inv_.reset();
    parkState();
    os_.reset();
}

// Self-consistent idle for the QUAD: four tubes' plate + screen current pull the
// rail, which sets the current. Same fixed-point iterate as the 2204's pair, with
// kTubes in place of the 2.
void OrangePowerAmp::solveOperatingPoint() {
    double rail = kVsupply - 0.15 * kRsupply;
    double scr = rail - 0.015 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 300; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, el34_);
        ig2 = el34ScreenCurrent(kVbias, scr, el34_);
        const double newRail = kVsupply - (kTubes * (ip + ig2)) * kRsupply;
        const double newScr = newRail - (kTubes * ig2) * kRscreen;
        if (std::fabs(newRail - rail) < 1e-6 && std::fabs(newScr - scr) < 1e-6) {
            rail = newRail;
            scr = newScr;
            break;
        }
        rail = newRail;
        scr = newScr;
    }
    vRailIdle_ = rail;
    vScreenIdle_ = scr;
    iqTube_ = ip;
    ig2qTube_ = ig2;
}

void OrangePowerAmp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_HF_DRIVE: hfDrive_ = v; break;
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        default: break;
    }
}

// Per-tube plate-load Newton, with the QUAD's per-tube reflected load. Identical
// numerics to Jcm800PowerAmp::solveTubePlate (docs §25's hoisted Koren base); only
// kRppPerTube differs, because two tubes share each side of the primary.
inline double OrangePowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                             double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, el34_);
    const double kvb = el34_.kvb;
    double Vp = rail;
    for (int it = 0; it < 40; ++it) {
        const double u = Vp / kvb;
        const double i = base * std::atan(u);
        const double f = Vp - (rail - (i - iqTube_) * kRppPerTube);
        const double df = 1.0 + (base / (kvb * (1.0 + u * u))) * kRppPerTube;
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

// EL34 grid node: the inverter output through the coupling cap into the grid leak
// (to Vbias) + grid conduction. Same structure as the 2204's; the DC reference
// vDriveQ differs per side here (plate vs cathode).
inline double OrangePowerAmp::solveEl34Grid(double vDriveAC, double vDriveQ,
                                            double& vCc, double& vgWarm) const {
    const double vp = vDriveQ + vDriveAC;
    const double ig0 = gridVgn_ / gridRgk_;
    const double tol = 1e-7 * tubeSolverTolScale();
    double Vg = vgWarm;
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

inline float OrangePowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Driver + cathodyne, solved together (direct-coupled). The feedback voltage
    //    is presented to the driver's CATHODE through kRfb: a POSITIVE secondary
    //    lifts the cathode, lowering Vgk — i.e. it opposes a positive grid signal,
    //    which is what makes the loop negative (asserted closed < open in-test).
    const double vfb = fbEnabled_ ? fbDelay_ : 0.0;
    double vPlate = 0.0, vCath = 0.0;
    inv_.processSample(x, vfb, vPlate, vCath);
    const double vPlateAC = vPlate - inv_.quiescentCathodynePlate();
    const double vCathAC = vCath - inv_.quiescentCathodyneCathode();

    // 2. Inverter legs -> EL34 grids (coupling + blocking). Anti-phase by topology.
    const double vgUp = solveEl34Grid(vPlateAC, inv_.quiescentCathodynePlate(),
                                      vCcUp_, vgUp_);
    const double vgDown = solveEl34Grid(vCathAC, inv_.quiescentCathodyneCathode(),
                                        vCcDown_, vgDown_);

    // 3. The EL34 QUAD: per-tube plate current at the sagged rail. Two tubes per
    //    side, so the side currents are 2x and the screen draw is 4x.
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = el34_.kg1 / el34_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 4. Output transformer (linear v1, as the other three amps). Each SIDE carries
    //    two tubes into Raa/4.
    const double vPriDiff = (2.0 * ipUp - 2.0 * ipDown) * (kRaa / 4.0);
    double vSec = vPriDiff / otTurnsRatio();
    {
        // Anti-denormal (Denormal.h, docs §33): both TPT states rest at zero.
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = flushDenormal(2.0 * vLp - otLpS_);
        vSec = vLp;
        const double vLp2 = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = flushDenormal(2.0 * vLp2 - otHpS_);
        vSec = vSec - vLp2;
    }

    // 5. Sag. Four tubes draw through kRsupply into the reservoir; the screen node
    //    follows more slowly. Backward Euler, same form as the 2204.
    const double iLoad = 2.0 * (ipUp + ipDown + ig2Up + ig2Down);
    const double iScr = 2.0 * (ig2Up + ig2Down);
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iLoad) / (gRes_ + 1.0 / kRsupply);
    vScreen_ = (gScr_ * vScreen_ + vRail_ / kRscreen - iScr) / (gScr_ + 1.0 / kRscreen);

    // 6. HF DRIVE-shaped feedback (unit delay for the next sample). p = 0 feeds the
    //    whole band back (dark); p = 1 removes the HF feedback (top comes up).
    const double vLpP = hfLpS_ + hfLpA_ * (vSec - hfLpS_);
    hfLpS_ = flushDenormal(2.0 * vLpP - hfLpS_);
    // The divider into the driver cathode is part of the nodal solve, so what is
    // stored here is the shaped SECONDARY voltage, not a pre-scaled one.
    fbDelay_ = flushDenormal((1.0 - hfDrive_) * vSec + hfDrive_ * vLpP);

    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void OrangePowerAmp::process(const float* in, float* out, int numFrames) {
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
