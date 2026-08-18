// Clipper — MesaPowerAmp (M10.4, docs §69). See MesaPowerAmp.h for the topology,
// the PROVENANCE banner and both halves of the acceptance bar. The numerics —
// the LTP, the per-tube plate Newton, the grid coupling/blocking, the OT and the
// Thevenin supply — are the shared M9.3 / §55 machinery; what differs here is
// the 6L6 device card, the TRANSCRIBED unequal plate loads, the three-state
// feedback and the selectable rectifier.

#include "clipper/dsp/MesaPowerAmp.h"

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

MesaPowerAmp::MesaPowerAmp() {
    LtpInverter::Config c;
    // TRANSCRIBED, and this is the one place this voice differs in KIND from its
    // two siblings' power sections. §45 swept Ra2 on the JCM to balance the legs
    // and §63.6 swept it again on the Rockerverb — both landed on 120k as a
    // model-parameter CALIBRATION. Here the factory drawing already carries
    // unequal loads (82k / 90k), so nothing is swept: the balance this produces
    // is a PREDICTION, reported in docs §69.6 rather than fitted.
    c.Ra1 = kRa1;
    c.Ra2 = kRa2;
    c.Rtail = kRtail;
    // Finding 7's tail reference (§42): Rtail sets the common-mode rejection
    // while the reference sets the standing current. RECONSTRUCTED — the sheet
    // shows the tail returning to the bias supply, whose value it gives as the
    // -51 V node, but the divider that sets the PI's own reference is not drawn.
    c.tailRef = -12.0;
    ltp_.configure(c);
    tube6L6_ = to6L6(tube6L6cfg_);
}

double MesaPowerAmp::feedbackBeta() const {
    const MesaModeRow& row = mesaModeRow(mode_);
    if (!row.ldr19_feedback) return 0.0;  // THE MODERN MODES: open loop
    const double rfb = row.ldr20_moreFeedback ? kRfbDouble : kRfbSingle;
    return kRfbLower / (kRfbLower + rfb);
}

double MesaPowerAmp::supplyOpenCircuit() const {
    const double v =
        (rect_ == MesaRectifier::Silicon) ? kVsupplySilicon : kVsupplyValve;
    // SPONGY drops the primary too, not just its impedance: the same series
    // element that softens the supply also lowers the no-load rail.
    return powerMode_ == MesaPowerMode::Spongy ? v * 0.94 : v;
}

double MesaPowerAmp::supplyResistance() const {
    double r = kRptSecondary;
    if (rect_ == MesaRectifier::Valve5U4) r += kRrect5U4Pair;
    if (powerMode_ == MesaPowerMode::Spongy) r += kRspongyExtra;
    return r;
}

void MesaPowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void MesaPowerAmp::setOversampling(int factor) {
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
    presA_ = onePoleA(kPresenceCornerHz);

    solveOperatingPoint();
    parkState();
}

void MesaPowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    vCcUp_ = ltp_.quiescentPlate1() - kVbias;
    vCcDown_ = ltp_.quiescentPlate2() - kVbias;
    vgUp_ = vgDown_ = kVbias;
    otHpS_ = 0.0;
    otLpS_ = 0.0;
    presS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void MesaPowerAmp::reset() {
    ltp_.reset();
    parkState();
    os_.reset();
}

void MesaPowerAmp::setMode(MesaMode m) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= static_cast<int>(MesaMode::MESA_MODE_COUNT)) return;
    mode_ = m;  // feedback depth only — nothing else in this class depends on it
}

void MesaPowerAmp::setRectifier(MesaRectifier r) {
    if (r == rect_) return;
    rect_ = r;
    solveOperatingPoint();
    // Deliberately NOT parkState(): a rectifier flip on a sustaining chord is a
    // real player action and the reservoir should walk to its new rail through
    // the same RC the sag uses, not jump. The composed amp brackets the switch
    // with the house declick fade for the discontinuity that remains.
}

void MesaPowerAmp::setPowerMode(MesaPowerMode p) {
    if (p == powerMode_) return;
    powerMode_ = p;
    solveOperatingPoint();
}

// Self-consistent idle for the quad, then the PI's own dropped supply node.
void MesaPowerAmp::solveOperatingPoint() {
    const double vsup = supplyOpenCircuit();
    const double rsup = supplyResistance();
    double rail = vsup - 0.15 * rsup;
    double scr = rail - 0.015 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 300; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, tube6L6_);
        ig2 = el34ScreenCurrent(kVbias, scr, tube6L6_);
        const double newRail = vsup - (kTubes * (ip + ig2)) * rsup;
        const double newScr = newRail - (kTubes * ig2) * kRscreen / kTubes;
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

    LtpInverter::Config c = ltp_.config();
    double bp = rail;
    for (int it = 0; it < 80; ++it) {
        c.bPlus = bp;
        ltp_.configure(c);
        ltp_.prepare();
        const double itail = (ltp_.quiescentTail() - c.tailRef) / c.Rtail;
        const double next = rail - kRdropPi * itail;
        if (std::fabs(next - bp) < 1e-6) {
            bp = next;
            break;
        }
        bp = next;
    }
    c.bPlus = bp;
    ltp_.configure(c);
    ltp_.prepare();
}

void MesaPowerAmp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_PRESENCE: presence_ = v; break;
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        default: break;
    }
}

inline double MesaPowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                           double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, tube6L6_);
    const double kvb = tube6L6_.kvb;
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

inline double MesaPowerAmp::solveGrid(double vpPlateAC, double vpPlateQ, double& vCc,
                                      double& vgWarm) const {
    const double vp = vpPlateQ + vpPlateAC;
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

inline float MesaPowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Long-tailed pair. V5A's grid takes the master signal, V5B's takes the
    //    global feedback. In the two MODERN modes beta is EXACTLY zero, so this
    //    term vanishes and the power stage runs open-loop — see the header.
    //
    //    PRESENCE shunts the loop's lower leg through 100n, so it removes HF
    //    feedback (a lift) above the transcribed parts' own 338.7 Hz corner.
    const double beta = feedbackBeta();
    double vfb = 0.0;
    if (beta > 0.0) {
        const double lp = presS_ + presA_ * (fbDelay_ - presS_);
        presS_ = flushDenormal(2.0 * lp - presS_);
        // At LF the cap blocks and the full beta applies; at HF the presence pot
        // shunts it toward beta*(1-p).
        const double shaped = (1.0 - presence_) * fbDelay_ + presence_ * lp;
        vfb = -beta * shaped;
    } else {
        presS_ = 0.0;
    }
    double va1 = 0.0, va2 = 0.0;
    ltp_.processSample(x, vfb, va1, va2);
    const double va1AC = va1 - ltp_.quiescentPlate1();
    const double va2AC = va2 - ltp_.quiescentPlate2();

    // 2. PI plates -> 6L6 grids (47n coupling + blocking, §18).
    const double vgUp = solveGrid(va1AC, ltp_.quiescentPlate1(), vCcUp_, vgUp_);
    const double vgDown = solveGrid(va2AC, ltp_.quiescentPlate2(), vCcDown_, vgDown_);

    // 3. The 6L6 quad at the sagged rail.
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = tube6L6_.kg1 / tube6L6_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 4. Output transformer. Each SIDE carries two tubes into Raa/4.
    const double vPriDiff = (2.0 * ipUp - 2.0 * ipDown) * (kRaa / 4.0);
    double vSec = vPriDiff / otTurnsRatio();
    {
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = flushDenormal(2.0 * vLp - otLpS_);
        vSec = vLp;
        const double vLp2 = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = flushDenormal(2.0 * vLp2 - otHpS_);
        vSec = vSec - vLp2;
    }

    // 5. Sag, on the SELECTED rectifier's Thevenin source (§55's machinery).
    //    This is the second half of the acceptance bar: the valve setting has
    //    both a lower open-circuit voltage and ~3x the source resistance, so the
    //    rail walks further under a sustained chord.
    const double vsup = supplyOpenCircuit();
    const double rsup = supplyResistance();
    const double iLoad = 2.0 * (ipUp + ipDown + ig2Up + ig2Down);
    const double iScr = 2.0 * (ig2Up + ig2Down);
    vRail_ = (gRes_ * vRail_ + vsup / rsup - iLoad) / (gRes_ + 1.0 / rsup);
    const double gScrR = static_cast<double>(kTubes) / kRscreen;
    vScreen_ = (gScr_ * vScreen_ + vRail_ * gScrR - iScr) / (gScr_ + gScrR);

    // 6. Global feedback (unit delay for the next sample).
    fbDelay_ = flushDenormal(vSec);

    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void MesaPowerAmp::process(const float* in, float* out, int numFrames) {
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
