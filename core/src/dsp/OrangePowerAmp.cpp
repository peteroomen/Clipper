// Clipper — OrangePowerAmp (M10.3, docs §57; SCHEMATIC CORRECTION 2026-07-31).
// See OrangePowerAmp.h for the topology, the transcribed values and every
// documented gap. This file is the numerics: the driver 2x2 Newton, the 68n/1M
// coupling, the cathodyne 1-D solve, the H.F. Boost R-L-C branch, the EL34 QUAD
// plate solve, the inverter->EL34 coupling/blocking, the OT + NFB, and the sag.

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

// The cathodyne alone: one current through two equal resistors, its grid driven
// by vgc. Returns the solved cathode voltage.
static double solveCathodyne(const CathodyneInverter::Config& cfg, double vgc,
                             double vkStart, double tol, int maxIt) {
    const double gR = 1.0 / cfg.Rsplit;
    double vk = vkStart;
    for (int it = 0; it < maxIt; ++it) {
        const double vak = cfg.bPlus - 2.0 * vk;
        const TriodeEval c = triodeEval(vak, vgc - vk, cfg.tube);
        const double r = c.Ip - vk * gR;
        // dIp/dVk = -2*dVa - dVgk ; d(vk*gR)/dVk = gR
        const double d = (-2.0 * c.dVa - c.dVgk) - gR;
        if (std::fabs(d) < 1e-30) break;
        double dv = -r / d;
        dv = std::clamp(dv, -40.0, 40.0);
        vk += dv;
        // The cathode is physically pinned to [0, C+/2]: outside that the plate
        // would sit below the cathode. This clamp IS the split load's compliance
        // limit, not a numerical band-aid — it is the "stiffer, fuzzier" clip.
        vk = std::clamp(vk, 0.0, 0.5 * cfg.bPlus);
        if (std::fabs(dv) < tol) break;
    }
    return vk;
}

void CathodyneInverter::prepare(double osRate) {
    osRate_ = osRate > 0.0 ? osRate : 176400.0;
    const double T = 1.0 / osRate_;
    geqAd_ = 2.0 * cfg_.Cad / T;
    geqCc_ = 2.0 * cfg_.Ccoup / T;
    rL_ = 2.0 * cfg_.Lboost / T;
    rC_ = T / (2.0 * cfg_.Cboost);
    solveDc();
    reset();
}

// The DC operating point, and with it the ONE element the schematic does not
// carry: where the cathodyne's 1M grid leak returns to.
//
// The cathodyne is placed at the CENTRE of its own compliance — Vkc = C+/4, so
// Vak = C+/2 and the cathode has as far to fall as the plate has to rise. That
// is the cathodyne's published design rule and it is checked in-test against an
// ABSOLUTE number from the same schematic: the resulting swing must exceed the
// EL34s' 48 V of fixed bias, or the amp cannot reach full power at all.
void CathodyneInverter::solveDc() {
    // --- cathodyne -----------------------------------------------------------
    vkcq_ = 0.25 * cfg_.bPlus;
    ipcq_ = vkcq_ / cfg_.Rsplit;
    const double vak = cfg_.bPlus - 2.0 * vkcq_;
    // Newton for the Vgk that draws exactly ipcq_ at that plate voltage.
    double vgk = -1.0;
    for (int it = 0; it < 200; ++it) {
        const TriodeEval e = triodeEval(vak, vgk, cfg_.tube);
        const double r = e.Ip - ipcq_;
        if (std::fabs(e.dVgk) < 1e-30) break;
        double dv = -r / e.dVgk;
        dv = std::clamp(dv, -2.0, 2.0);
        vgk += dv;
        if (std::fabs(dv) < 1e-12) break;
    }
    vgkcq_ = vgk;
    vTap_ = vkcq_ + vgk;                 // the grid-leak return node's DC
    rkTap_ = (-vgk) / ipcq_;             // cathode leg above that tap (Ohm)

    // --- driver (2x2 DC: caps open, so no Cad and no coupling current) --------
    const double gAd = 1.0 / cfg_.Rad;
    const double gK = 1.0 / cfg_.Rkd + 1.0 / cfg_.RkdShunt + 1.0 / cfg_.RboostBleed +
                      1.0 / cfg_.Rfb;
    double vpd = 0.5 * cfg_.bPlus, vkd = 1.5;
    for (int it = 0; it < 400; ++it) {
        const TriodeEval d = triodeEval(vpd - vkd, -vkd, cfg_.tube);
        const double r1 = (cfg_.bPlus - vpd) * gAd - d.Ip;
        const double r2 = d.Ip - vkd * gK;
        const double J00 = -gAd - d.dVa, J01 = d.dVa + d.dVgk;
        const double J10 = d.dVa, J11 = -(d.dVa + d.dVgk) - gK;
        const double det = J00 * J11 - J01 * J10;
        if (std::fabs(det) < 1e-30) break;
        double dvp = (-r1 * J11 + J01 * r2) / det;
        double dvk = (-J10 * -r1 + J00 * -r2) / det;
        dvp = std::clamp(dvp, -40.0, 40.0);
        dvk = std::clamp(dvk, -4.0, 4.0);
        vpd += dvp;
        vkd += dvk;
        if (std::max(std::fabs(dvp), std::fabs(dvk)) < 1e-10) break;
    }
    vpdq_ = vpd;
    vkdq_ = vkd;
    ipdq_ = triodeEval(vpd - vkd, -vkd, cfg_.tube).Ip;
}

void CathodyneInverter::reset() {
    vpd_ = vpdq_;
    vkd_ = vkdq_;
    vkc_ = vkcq_;
    vgc_ = vTap_;
    // The 1n across the plate load parks at C+ - Vpdq and the 68n coupling cap at
    // Vpdq - Vtap: both are REAL operating points, never zero, so neither is
    // flushed (ADR 006's scope rule) — see the header.
    vAd_ = cfg_.bPlus - vpdq_;
    iAd_ = 0.0;
    vCc_ = vpdq_ - vTap_;
    iCc_ = 0.0;
    // The H.F. Boost branch is DC-blocked by its own 0.47 uF, so all three of its
    // states rest at EXACTLY zero and all three are guarded.
    ib_ = vL_ = vC_ = 0.0;
}

void CathodyneInverter::processSample(double vg, double vfb, double& vPlate,
                                      double& vCath) {
    const double tol = 1e-7 * tubeSolverTolScale();

    // --- companions for this sample -----------------------------------------
    const double IeqAd = geqAd_ * vAd_ + iAd_;   // cap across the plate load
    const double IeqCc = geqCc_ * vCc_ + iCc_;   // 68n coupling to the cathodyne
    const double gRg = 1.0 / cfg_.Rgc;
    const double Gc = geqCc_ * gRg / (geqCc_ + gRg);
    const double Ihc = IeqCc * gRg / (geqCc_ + gRg);
    // H.F. Boost: series (Rpot + choke DCR) + L + C. The pot is a rheostat, so
    // boost = 1 removes the damping and boost = 0 inserts the whole 1k.
    const double Rb = (1.0 - boost_) * cfg_.RboostPot + cfg_.RchokeDcr;
    const double Rtot = Rb + rL_ + rC_;
    const double eL = rL_ * ib_ + vL_;
    const double eC = vC_ + rC_ * ib_;
    const double Eh = eC - eL;
    const double Gb = 1.0 / Rtot;

    const double gAd = 1.0 / cfg_.Rad;
    const double gK = 1.0 / cfg_.Rkd + 1.0 / cfg_.RkdShunt + 1.0 / cfg_.RboostBleed;
    const double gFb = 1.0 / cfg_.Rfb;

    // --- driver: 2x2 Newton in (Vpd, Vkd) ------------------------------------
    double vpd = vpd_, vkd = vkd_;
    for (int it = 0; it < 40; ++it) {
        const TriodeEval d = triodeEval(vpd - vkd, vg - vkd, cfg_.tube);
        const double r1 = (cfg_.bPlus - vpd) * gAd + (geqAd_ * (cfg_.bPlus - vpd) - IeqAd) -
                          d.Ip - (Gc * (vpd - vTap_) - Ihc);
        const double r2 = d.Ip - vkd * gK - (vkd - vfb) * gFb - Gb * (vkd - Eh);
        const double J00 = -gAd - geqAd_ - d.dVa - Gc;
        const double J01 = d.dVa + d.dVgk;
        const double J10 = d.dVa;
        const double J11 = -(d.dVa + d.dVgk) - gK - gFb - Gb;
        const double det = J00 * J11 - J01 * J10;
        if (std::fabs(det) < 1e-30) break;
        double dvp = (-r1 * J11 + J01 * r2) / det;
        double dvk = (-J10 * -r1 + J00 * -r2) / det;
        dvp = std::clamp(dvp, -120.0, 120.0);
        dvk = std::clamp(dvk, -30.0, 30.0);
        vpd += dvp;
        vkd += dvk;
        if (std::max(std::fabs(dvp), std::fabs(dvk)) < tol) break;
    }
    vpd_ = vpd;
    vkd_ = vkd;

    // --- update the driver's reactive states ---------------------------------
    {
        const double vNew = cfg_.bPlus - vpd;            // across the 1n
        iAd_ = geqAd_ * vNew - IeqAd;
        vAd_ = vNew;                                     // rests at C+ - Vpdq: no flush
        const double ib = Gb * (vkd - Eh);               // H.F. Boost branch current
        const double vLn = rL_ * ib - eL;
        const double vCn = rC_ * ib + eC;
        // DENORMAL SCOPE (ADR 006), decided BY MEASUREMENT and it is the docs §59
        // case, not the §56.4b one. The 0.47 uF rests at the DRIVER'S CATHODE
        // (1.9453 V measured after a 4 s silent tail), not at zero, because the
        // branch carries no DC current. The current and the choke voltage do rest
        // at zero mathematically — but they are computed as (Vkd - Eh), a
        // cancellation of two ~2 V quantities, so they FLOOR at 1.386e-16 and can
        // never reach the subnormal range at all. 292 decades of head-room means a
        // flush here is unreachable code in the hottest loop in the file: comment,
        // not guard. clipper_orange_tests asserts the measured floor.
        ib_ = ib;
        vL_ = vLn;
        vC_ = vCn;
    }

    // --- 68n / 1M coupling -> the cathodyne grid ------------------------------
    vgc_ = (geqCc_ * vpd - IeqCc + gRg * vTap_) / (geqCc_ + gRg);
    {
        const double vNew = vpd - vgc_;
        iCc_ = geqCc_ * vNew - IeqCc;
        vCc_ = vNew;  // rests at Vpdq - Vtap: a real operating point, no flush
    }

    // --- cathodyne ------------------------------------------------------------
    vkc_ = solveCathodyne(cfg_, vgc_, vkc_, tol, 40);

    // The two outputs come off ONE current through TWO equal resistors, so they
    // are anti-phase and equal by construction (see the header).
    vPlate = cfg_.bPlus - vkc_;
    vCath = vkc_;
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
    gCc_ = kCoupCc / T;
    gRg_ = 1.0 / kCoupRg;

    auto onePoleA = [&](double fc) {
        const double g = std::tan(M_PI * fc / osRate_);
        return g / (1.0 + g);
    };
    otHpA_ = onePoleA(kOtLfHz);
    otLpA_ = onePoleA(kOtHfHz);

    solveOperatingPoint();
    parkState();
}

void OrangePowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreenUp_ = vScreenDown_ = vScreenIdle_;
    // The coupling caps block the inverter's two DC levels (which are NOT the same
    // — the plate sits at C+ - Vkc, the cathode at Vkc), so each side parks at its
    // own DC difference to the fixed bias.
    vCcUp_ = inv_.quiescentCathodynePlate() - kVbias;
    vCcDown_ = inv_.quiescentCathodyneCathode() - kVbias;
    vgUp_ = vgDown_ = kVbias;
    otHpS_ = 0.0;
    otLpS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void OrangePowerAmp::reset() {
    inv_.reset();
    parkState();
    os_.reset();
}

// Self-consistent idle for the QUAD, then the transcribed 33K dropper down to C+.
// The screens hang off the same node as the OT centre tap (the choke is ideal at
// DC — see the header), each behind its own transcribed 1k.
void OrangePowerAmp::solveOperatingPoint() {
    double rail = kVsupply - 0.15 * kRsupply;
    double scr = rail - 0.015 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 300; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, el34_);
        ig2 = el34ScreenCurrent(kVbias, scr, el34_);
        const double newRail = kVsupply - (kTubes * (ip + ig2)) * kRsupply;
        const double newScr = newRail - ig2 * kRscreen;  // per-tube 1k
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

    // C+ = B+ - 33K * (driver + cathodyne current). Both currents depend on C+,
    // so it is a fixed point; the inverter is re-configured and re-solved each
    // pass (its DC solve is a pair of small Newtons, not a settle).
    CathodyneInverter::Config c = inv_.config();
    double cp = rail;
    for (int it = 0; it < 60; ++it) {
        c.bPlus = cp;
        inv_.configure(c);
        inv_.prepare(osRate_);
        const double next =
            rail - kRdropCplus * (inv_.quiescentDriverCurrent() +
                                  inv_.quiescentCathodyneCurrent());
        if (std::fabs(next - cp) < 1e-6) {
            cp = next;
            break;
        }
        cp = next;
    }
    c.bPlus = cp;
    inv_.configure(c);
    inv_.prepare(osRate_);
    vCplusIdle_ = cp;
}

void OrangePowerAmp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_HF_DRIVE:
            hfDrive_ = v;
            inv_.setBoost(v);
            break;
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

    // 1. Driver + cathodyne. The feedback voltage from the 16 ohm tap is presented
    //    to the DRIVER's cathode through Rfb: a POSITIVE secondary lifts the
    //    cathode, lowering Vgk — i.e. it opposes a positive grid signal, which is
    //    what makes the loop negative (asserted closed < open in-test).
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

    // 3. The EL34 QUAD: per-tube plate current at the sagged rail, each tube
    //    behind its own transcribed 1k screen resistor (no screen bypass cap on
    //    the sheet, so the drop is algebraic and uses the previous sample's ig2).
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreenUp_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreenDown_, vRail_, vpDown, baseDown);
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

    // 5. Sag. Four tubes draw through kRsupply into the reservoir. The screens sit
    //    on the same node (the choke is ideal at DC — see the header) behind their
    //    own 1k each.
    const double iLoad = 2.0 * (ipUp + ipDown + ig2Up + ig2Down);
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iLoad) / (gRes_ + 1.0 / kRsupply);
    vScreenUp_ = vRail_ - ig2Up * kRscreen;
    vScreenDown_ = vRail_ - ig2Down * kRscreen;

    // 6. Global feedback (unit delay for the next sample), from the 16 OHM TAP.
    //    There is no filter in this path: the presence control on this amp is the
    //    H.F. Boost R-L-C in the driver's cathode, which is INSIDE the loop.
    fbDelay_ = flushDenormal(vSec * kFbTapRatio);

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
