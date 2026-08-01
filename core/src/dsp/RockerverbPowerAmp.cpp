// Clipper — RockerverbPowerAmp (M10.7, docs §63). See RockerverbPowerAmp.h for
// the topology and the PROVENANCE banner (this half is a documented
// reconstruction). The numerics — the LTP 3x3, the per-tube plate Newton, the
// grid coupling/blocking, the OT, the sag — are the shared M9.3 machinery; only
// the QUAD's per-tube reflected load and the flat feedback loop differ.

#include "clipper/dsp/RockerverbPowerAmp.h"

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

RockerverbPowerAmp::RockerverbPowerAmp() {
    LtpInverter::Config c;
    // RECONSTRUCTION, constrained by physics rather than by a tone (see the
    // header). The PI runs off its own dropped node, and the pair is sized to the
    // project's documented targets: 0.5-0.9 mA per triode with the plates at
    // 70-85 % of B+ (docs §29's LtpProbe, §42's derivation).
    //
    //   Ra1 = 100k is the textbook pair's first leg.
    //   Ra2 is ASYMMETRIC on purpose: a real long-tailed pair with equal plate
    //   loads does NOT deliver equal legs, because the second grid sits on the
    //   tail rather than being driven. That is audit finding 8's whole subject
    //   (§45), and the doctrine there is that Ra2 is a model-parameter
    //   calibration landing the real circuit's measured property (balanced legs
    //   -> even-harmonic cancellation). Swept on THIS amp's rail and tail (the
    //   full 24-row table is in docs §63.6): 100k -> 0.828, 110k -> 0.901,
    //   120k -> 0.972, 130k -> 0.960, 140k -> 0.901, 150k -> 0.850. 120k is the
    //   optimum, and it is the SAME value docs §45's independent sweep landed on
    //   for the 2204 — which is not a coincidence and is not a copy: same tube,
    //   same 10k tail, a similar B+, so the same compensation. Reported.
    //   tailRef is finding 7's tail reference (§42): Rtail keeps setting the
    //   common-mode rejection while the reference sets the standing current.
    //
    // The CONTRAST this creates against the OR120 is the point and it is
    // reported, not asserted as the bar: a cathodyne's two legs are equal BY
    // TOPOLOGY (§57 measures 0.999965 with no calibration at all), an LTP's are
    // not and never were.
    c.Ra1 = 100.0e3;
    c.Ra2 = 120.0e3;
    c.Rtail = 10.0e3;
    c.tailRef = -12.0;
    ltp_.configure(c);
}

void RockerverbPowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void RockerverbPowerAmp::setOversampling(int factor) {
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

    solveOperatingPoint();
    parkState();
}

void RockerverbPowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    vCcUp_ = ltp_.quiescentPlate1() - kVbias;
    vCcDown_ = ltp_.quiescentPlate2() - kVbias;
    vgUp_ = vgDown_ = kVbias;
    otHpS_ = 0.0;
    otLpS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void RockerverbPowerAmp::reset() {
    ltp_.reset();
    parkState();
    os_.reset();
}

// Self-consistent idle for the QUAD, then the PI's own dropped supply node.
void RockerverbPowerAmp::solveOperatingPoint() {
    double rail = kVsupply - 0.15 * kRsupply;
    double scr = rail - 0.015 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 300; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, el34_);
        ig2 = el34ScreenCurrent(kVbias, scr, el34_);
        const double newRail = kVsupply - (kTubes * (ip + ig2)) * kRsupply;
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

    // The PI hangs off the rail through kRdropPi and draws two triodes' worth, so
    // its supply is a fixed point. LtpInverter::prepare() is a small Newton (not a
    // settle), so it is cheap to re-solve inside the loop.
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

void RockerverbPowerAmp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        default: break;
    }
}

// Per-tube plate-load Newton with the QUAD's per-tube reflected load. Identical
// numerics to Jcm800PowerAmp::solveTubePlate (docs §25's hoisted Koren base);
// only kRppPerTube differs, because two tubes share each side of the primary.
inline double RockerverbPowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
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

inline double RockerverbPowerAmp::solveEl34Grid(double vpPlateAC, double vpPlateQ,
                                                double& vCc, double& vgWarm) const {
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

inline float RockerverbPowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Long-tailed pair. V-A's grid takes the master-volume signal, V-B's takes
    //    the global feedback. The forward path from V-A is inverting and from V-B
    //    non-inverting, so -beta*vSec at V-B OPPOSES the output. There is NO
    //    presence filter in this leg (the panel has no presence control), which
    //    makes the loop flat with frequency.
    const double vfb = -feedbackBeta() * fbDelay_;
    double va1 = 0.0, va2 = 0.0;
    ltp_.processSample(x, vfb, va1, va2);
    const double va1AC = va1 - ltp_.quiescentPlate1();
    const double va2AC = va2 - ltp_.quiescentPlate2();

    // 2. PI plates -> EL34 grids (coupling + blocking, §18).
    const double vgUp = solveEl34Grid(va1AC, ltp_.quiescentPlate1(), vCcUp_, vgUp_);
    const double vgDown = solveEl34Grid(va2AC, ltp_.quiescentPlate2(), vCcDown_, vgDown_);

    // 3. The EL34 QUAD at the sagged rail.
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = el34_.kg1 / el34_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 4. Output transformer (linear v1, as all four siblings). Each SIDE carries
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

    // 5. Sag. Four tubes draw through kRsupply into the reservoir; the screens sit
    //    behind their own 1k into a shared 47uF filter.
    const double iLoad = 2.0 * (ipUp + ipDown + ig2Up + ig2Down);
    const double iScr = 2.0 * (ig2Up + ig2Down);
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iLoad) / (gRes_ + 1.0 / kRsupply);
    const double gScrR = static_cast<double>(kTubes) / kRscreen;  // four in parallel
    vScreen_ = (gScr_ * vScreen_ + vRail_ * gScrR - iScr) / (gScr_ + gScrR);

    // 6. Global feedback (unit delay for the next sample). Flat — no shaping.
    fbDelay_ = flushDenormal(vSec);

    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void RockerverbPowerAmp::process(const float* in, float* out, int numFrames) {
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
