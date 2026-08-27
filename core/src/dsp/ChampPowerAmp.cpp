// Clipper — portable DSP core (M10.10). See ChampPowerAmp.h for the circuit
// rationale, the derived 6V6GT device card and the two absolute references it is
// validated against (the RCA/TAD datasheet, and Fender's own measured 5F1 nodes).

#include "clipper/dsp/ChampPowerAmp.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"
#include "clipper/dsp/TubeSolverMode.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace clipper::dsp {
namespace {

inline double softplus(double u) {
    if (u > 30.0) return u;
    return std::log1p(std::exp(u));
}
inline double sigmoid(double u) {
    if (u > 30.0) return 1.0;
    if (u < -30.0) return 0.0;
    return 1.0 / (1.0 + std::exp(-u));
}

}  // namespace

ChampPowerAmp::ChampPowerAmp() {
    tube6V6_ = to6V6(card_);
    gridRgk_ = 1500.0;   // house 6V6/EL34/6L6 grid-conduction values
    gridVgn_ = 0.5;
}

void ChampPowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    tube6V6_ = to6V6(card_);
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void ChampPowerAmp::setOversampling(int factor) {
    os_.setFactor(factor);
    const double osRate = sampleRate_ * os_.factor();
    const double T = 1.0 / osRate;

    gRes_ = kCreservoir / T;
    gCk_ = kCkCathode / T;
    gCc_ = kCoupCc / T;
    gRg_ = 1.0 / kRgl;

    auto onePoleA = [&](double fc) {
        const double g = std::tan(M_PI * fc / osRate);
        return g / (1.0 + g);
    };
    otHpA_ = onePoleA(kOtLfHz);
    otLpA_ = onePoleA(kOtHfHz);

    solveOperatingPoint();
    reset();
}

void ChampPowerAmp::setParameter(int paramId, float value) {
    if (paramId == PARAM_DRIVE) drive_ = clampParam01(static_cast<double>(value));
}

// Park all dynamic state at the ALREADY-SOLVED idle point. No re-solve: the cathode
// fixed point in solveOperatingPoint() is the expensive part and stays as prepared
// (audit finding 1 / docs §28 — reset() is the recovery seam, prepare() is not).
void ChampPowerAmp::reset() {
    vRail_ = vRailIdle_;
    vk_ = vkIdle_;
    // The grid sits at 0 V DC (1 MΩ leak to GROUND — cathode bias, no negative
    // supply), so the coupling cap holds the driving stage's own plate DC, which the
    // preamp removes before handing us its AC. Our input is therefore already AC and
    // the cap rests at 0.
    vCc_ = 0.0;
    vgWarm_ = 0.0;
    vpWarm_ = vRailIdle_;
    otLpS_ = 0.0;
    otHpS_ = 0.0;
    lastOutPeak_ = 0.0;
}

// Solve the cathode-bias fixed point. Vk and the rail are mutually dependent (the
// cathode current sets both the bias AND the supply droop), so this is a damped
// two-level iteration rather than a closed form. Runs ONCE per prepare/OS change.
void ChampPowerAmp::solveOperatingPoint() {
    double Vk = 19.0;
    double rail = kVsupply;
    for (int outer = 0; outer < 4000; ++outer) {
        // Inner: the rail at the present bias.
        for (int i = 0; i < 200; ++i) {
            const double vpk = std::max(1.0, rail - Vk);
            const double ip = el34PlateCurrent(vpk, -Vk, vpk, tube6V6_);
            const double ig = el34ScreenCurrent(-Vk, vpk, tube6V6_);
            const double next = kVsupply - (ip + ig) * kRsupply;
            if (std::fabs(next - rail) < 1e-10) { rail = next; break; }
            rail += 0.5 * (next - rail);
        }
        const double vpk = std::max(1.0, rail - Vk);
        const double ip = el34PlateCurrent(vpk, -Vk, vpk, tube6V6_);
        const double ig = el34ScreenCurrent(-Vk, vpk, tube6V6_);
        const double nextVk = (ip + ig) * kRkCathode;
        if (std::fabs(nextVk - Vk) < 1e-12) { Vk = nextVk; break; }
        Vk += 0.2 * (nextVk - Vk);
    }
    const double vpk = std::max(1.0, rail - Vk);
    vRailIdle_ = rail;
    vkIdle_ = Vk;
    iqTube_ = el34PlateCurrent(vpk, -Vk, vpk, tube6V6_);
    ig2qTube_ = el34ScreenCurrent(-Vk, vpk, tube6V6_);
}

// SINGLE-ENDED plate load line: Vp = rail − (Ip − Iq)·kRload, the AC load line
// through the quiescent point. This is the SAME solver shape the push-pull siblings
// use and — per the header's (b) — it is EXACTLY CORRECT here and approximate there
// (audit finding 9). One tube, one primary winding, no differential term.
inline double ChampPowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                            double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, tube6V6_);
    const double kvb = tube6V6_.kvb;
    double Vp = vpWarm_;
    if (!(Vp > 0.0)) Vp = rail;
    for (int it = 0; it < 40; ++it) {
        const double u = Vp / kvb;
        const double i = base * std::atan(u);
        const double f = Vp - (rail - (i - iqTube_) * kRload);
        const double df = 1.0 + (base / (kvb * (1.0 + u * u))) * kRload;
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

double ChampPowerAmp::plateAtCurrent(double vg1k, double vg2, double rail,
                                     double& ipOut) const {
    double vp = 0.0, base = 0.0;
    ipOut = solveTubePlate(vg1k, vg2, rail, vp, base);
    return vp;
}

// 6V6 grid node: the driving stage's AC through the coupling cap into the grid leak
// (to GROUND — cathode bias has no fixed negative supply) + grid conduction relative
// to the DYNAMIC cathode. This is where blocking distortion lives on a hard pick.
inline double ChampPowerAmp::solveTubeGrid(double vDriveAC, double& vCc,
                                           double& vgWarm) const {
    const double ig0 = gridVgn_ / gridRgk_;
    const double tol = 1e-7 * tubeSolverTolScale();
    double Vg = vgWarm;
    for (int it = 0; it < 40; ++it) {
        const double u = (Vg - vk_) / gridVgn_;      // conduction when Vg > Vk
        const double Igk = ig0 * softplus(u);
        const double dIgk = (ig0 / gridVgn_) * sigmoid(u);
        const double r = gCc_ * ((vDriveAC - Vg) - vCc) - Vg * gRg_ - Igk;
        const double dr = -gCc_ - gRg_ - dIgk;
        double dVg = -r / dr;
        dVg = std::clamp(dVg, -100.0, 100.0);
        Vg += dVg;
        if (std::fabs(dVg) < tol) break;
    }
    vCc = (vDriveAC - Vg);
    vgWarm = Vg;
    return Vg;
}

inline float ChampPowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Grid node (coupling + blocking). NO phase inverter — the volume signal
    //    drives this grid directly. There is nothing to balance and nothing to cancel.
    const double vg = solveTubeGrid(x, vCc_, vgWarm_);

    // 2. THE ONE 6V6, at the sagged rail. Grid-cathode bias rides the DYNAMIC cathode
    //    (previous sample's vk_ — ms time constants against a µs step, the same
    //    decoupling the JCM/Twin/AC30 sections use). Screen shares the plate node in
    //    the 5F1 (no screen dropping resistor), so vg2 is the rail.
    //    EVERYTHING HERE IS CATHODE-REFERRED, because the Koren law's Vp and Vg2 are
    //    plate- and screen-to-CATHODE. With a cathode-biased tube whose cathode sits
    //    at ~19 V and MOVES under drive, that distinction is worth 19 V and is not
    //    optional: solveOperatingPoint() solves in the same frame, and mixing the two
    //    puts the idle solve and the run loop on different load lines (measured: a
    //    0.23 startup transient into a silent render, which is how this was caught).
    const double vg1k = vg - vk_;
    const double railK = vRail_ - vk_;      // plate/screen node, cathode-referred
    double vp = 0.0, base = 0.0;
    const double ip = solveTubePlate(vg1k, railK, railK, vp, base);
    vpWarm_ = vp;
    const double ig2 = base * (tube6V6_.kg1 / tube6V6_.kg2);

    // 3. Output transformer. SINGLE-ENDED: the signal is this tube's OWN deviation
    //    from idle — there is no (ipUp − ipDown) difference to take, which is exactly
    //    why the even harmonics survive (header (a)).
    double vSec = ((ip - iqTube_) * kRload) / otTurnsRatio();
    {
        // Anti-denormal (Denormal.h, ADR 006). Measured scope, not assumed — see
        // maxAbsRestingState() below: with no NFB loop and no opposite leg, this
        // amp's secondary settles at EXACTLY zero on silence, so these two TPT states
        // ring down into the double subnormal range and stick, the AC30's case.
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = flushDenormal(2.0 * vLp - otLpS_);
        vSec = vLp;
        const double vHp = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = flushDenormal(2.0 * vHp - otHpS_);
        vSec = vSec - vHp;
    }

    // 4. THE DYNAMIC SUPPLY AND THE DYNAMIC BIAS (docs §55's form). Both integrate
    //    backward-Euler from THIS sample's currents and are read on the NEXT one.
    //    (a) Reservoir: Cres·dV/dt = (kVsupply − V)/kRsupply − Ik. kRsupply is 680 Ω
    //        against the AC30's 134.6 — a 5Y3 in a very small amp — so this is the
    //        saggiest supply in the lineup and that is the Champ's bloom.
    const double iCath = ip + ig2;
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iCath) / (gRes_ + 1.0 / kRsupply);
    //    (b) The cathode: Ck·dV/dt = Ik − V/Rk. Class-A bias shift — the cathode
    //        cap charges under sustained drive, cooling the tube toward cutoff. NOT
    //        denormal-guarded: it rests at a real operating point (see below).
    vk_ = (gCk_ * vk_ + iCath) / (gCk_ + 1.0 / kRkCathode);

    const double outV = vSec / kFullScaleSecV;
    const double a = std::fabs(outV);
    if (a > lastOutPeak_) lastOutPeak_ = a;
    return static_cast<float>(outV);
}

// ADR 006 scope, DECIDED BY MEASUREMENT — and the measurement said something other
// than what was expected, so it is recorded rather than asserted away. This is the
// §59 / §69 case, not the §56.4b one.
//
// These three states would rest at exactly zero if their input did. It does not:
// TriodeStage's grid Newton exits at a RESIDUAL tolerance, so vCc_ = (drive − Vg)
// carries the solver's own floor rather than a true zero, and that floor is
// re-injected into the plate solve and thence into the OT pair every sample. Over
// 41 s of silence, measured at 1 / 4 / 8 / 20 / 41 s, they decay and then PLATEAU —
// identical to five digits at every one of those times:
//
//     otLpS_  2.7159e-12     otHpS_  2.7159e-12     vCc_  5.4341e-11
//     output  5.223e-27      subnormal output samples  0 / 128
//
// 2.7e-12 is ~27 decades above the float subnormal boundary (1.18e-38), so these
// can never reach it and the flushDenormal calls above are GUARD-RAILS (for the
// post-reset() path, and for uniformity with the sibling power sections) rather
// than a fix for a measured cliff. What the anti-denormal policy is actually about
// is that NO SUBNORMAL FLOAT LEAVES THE MODEL, and that is what the test asserts.
//
// vRail_ (rests at 305 V) and vk_ (rests at 18.9 V) are deliberately NOT covered
// and NOT guarded: they are real DC operating points, where a flush would be
// unreachable code in the hottest loop in this file.
double ChampPowerAmp::maxAbsRestingState() const {
    return std::max({std::fabs(otLpS_), std::fabs(otHpS_), std::fabs(vCc_)});
}

void ChampPowerAmp::process(const float* in, float* out, int numFrames) {
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
