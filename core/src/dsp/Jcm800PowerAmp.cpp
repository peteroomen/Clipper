// Clipper — Jcm800PowerAmp (M9.3). See Jcm800PowerAmp.h for the model overview,
// the Koren pentode citation, the EL34 fit, and every documented simplification.
// This file is the numerics: the EL34 device law, the LTP phase-inverter 3×3
// Newton, the per-tube plate-load solve, the PI→EL34 coupling/blocking, the OT +
// NFB + presence, and the B+ rail/screen sag integration.

#include "clipper/dsp/Jcm800PowerAmp.h"

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

// Koren 12AX7 triode current + partials dIp/dVa, dIp/dVgk (for the LTP Jacobian).
// Mirrors TriodeStage.cpp's korenEval (that one is file-local; re-used here so the
// PI shares the exact M9.1 device law — no new triode fit).
struct TriodeEval { double Ip, dVa, dVgk; };
TriodeEval triodeEval(double Va, double Vgk, const TriodeStage::KorenParams& p) {
    const double S = std::sqrt(p.kvb + Va * Va);
    const double A = p.kp * (1.0 / p.mu + Vgk / S);
    const double L = softplus(A);
    const double sig = sigmoid(A);
    const double E1 = (Va / p.kp) * L;
    if (E1 <= 0.0) return {0.0, 0.0, 0.0};
    const double C = 2.0 / p.kg1;
    const double Ip = C * std::pow(E1, p.ex);
    // dIp/dE1 = C*ex*E1^(ex-1) == ex*Ip/E1 — one pow instead of two (§25); the
    // Jacobian only steers the iterates, Newton converges to the same root.
    const double dIp_dE1 = p.ex * Ip / E1;
    const double dE1_dVgk = Va * sig / S;
    const double dE1_dVa = L / p.kp - (Va * Va * Vgk * sig) / (S * S * S);
    return {Ip, dIp_dE1 * dE1_dVa, dIp_dE1 * dE1_dVgk};
}

}  // namespace

// ===========================================================================
// EL34 Koren pentode device law.
// ===========================================================================
double el34PlateCurrent(double Vp, double Vg1k, double Vg2, const El34Params& p) {
    if (Vp < 0.0) Vp = 0.0;
    if (Vg2 <= 0.0) return 0.0;
    const double E1 = (Vg2 / p.kp) * softplus(p.kp * (1.0 / p.mu + Vg1k / Vg2));
    if (E1 <= 0.0) return 0.0;
    return (2.0 / p.kg1) * std::pow(E1, p.ex) * std::atan(Vp / p.kvb);
}
double el34ScreenCurrent(double Vg1k, double Vg2, const El34Params& p) {
    if (Vg2 <= 0.0) return 0.0;
    const double E1 = (Vg2 / p.kp) * softplus(p.kp * (1.0 / p.mu + Vg1k / Vg2));
    if (E1 <= 0.0) return 0.0;
    return (2.0 / p.kg2) * std::pow(E1, p.ex);
}

// Vp-independent Koren factor: Ip = base·atan(Vp/kvb). See the header (§25).
double el34PlateBase(double Vg1k, double Vg2, const El34Params& p) {
    if (Vg2 <= 0.0) return 0.0;
    const double E1 = (Vg2 / p.kp) * softplus(p.kp * (1.0 / p.mu + Vg1k / Vg2));
    if (E1 <= 0.0) return 0.0;
    return (2.0 / p.kg1) * std::pow(E1, p.ex);
}

// ===========================================================================
// LtpInverter — long-tailed-pair phase inverter.
// ===========================================================================
void LtpInverter::prepare() {
    // DC: grids at 0 V (no input, no feedback). Solve Va1, Va2, Vk from the same
    // 3×3 system the per-sample solver uses (Newton from a mid-load-line guess).
    double Va1 = 0.6 * cfg_.bPlus, Va2 = 0.6 * cfg_.bPlus, Vk = 1.0;
    for (int it = 0; it < 300; ++it) {
        const double Vgk1 = 0.0 - Vk, Vgk2 = 0.0 - Vk;
        const TriodeEval k1 = triodeEval(Va1, Vgk1, cfg_.tube);
        const TriodeEval k2 = triodeEval(Va2, Vgk2, cfg_.tube);
        const double r1 = (cfg_.bPlus - Va1) / cfg_.Ra1 - k1.Ip;
        const double r2 = (cfg_.bPlus - Va2) / cfg_.Ra2 - k2.Ip;
        const double r3 = k1.Ip + k2.Ip - Vk / cfg_.Rtail;
        // Jacobian rows [Va1, Va2, Vk]; dVgk/dVk = -1.
        const double J[3][3] = {
            {-1.0 / cfg_.Ra1 - k1.dVa, 0.0, k1.dVgk},
            {0.0, -1.0 / cfg_.Ra2 - k2.dVa, k2.dVgk},
            {k1.dVa, k2.dVa, -(k1.dVgk + k2.dVgk) - 1.0 / cfg_.Rtail},
        };
        const double b[3] = {-r1, -r2, -r3};
        const double det =
            J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) -
            J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
            J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
        if (std::fabs(det) < 1e-30) break;
        auto cramer = [&](int col) {
            double c[3][3];
            for (int r = 0; r < 3; ++r) {
                c[r][0] = J[r][0]; c[r][1] = J[r][1]; c[r][2] = J[r][2];
                c[r][col] = b[r];
            }
            return (c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
                    c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                    c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0])) / det;
        };
        const double d0 = std::clamp(cramer(0), -40.0, 40.0);
        const double d1 = std::clamp(cramer(1), -40.0, 40.0);
        const double d2 = std::clamp(cramer(2), -5.0, 5.0);
        Va1 += d0; Va2 += d1; Vk += d2;
        if (std::fabs(d0) < 1e-9 && std::fabs(d1) < 1e-9 && std::fabs(d2) < 1e-9) break;
    }
    va1q_ = va1_ = Va1;
    va2q_ = va2_ = Va2;
    vkq_ = vk_ = Vk;
    ip1q_ = triodeEval(Va1, -Vk, cfg_.tube).Ip;
}

void LtpInverter::processSample(double vg1, double vg2, double& va1, double& va2) {
    const double tol = 1e-7 * tubeSolverTolScale();  // §25, regression-gated
    const double gRa1 = 1.0 / cfg_.Ra1, gRa2 = 1.0 / cfg_.Ra2;
    const double gTail = 1.0 / cfg_.Rtail;
    double Va1 = va1_, Va2 = va2_, Vk = vk_;  // warm start
    for (int it = 0; it < 40; ++it) {
        const double Vgk1 = vg1 - Vk, Vgk2 = vg2 - Vk;
        const TriodeEval k1 = triodeEval(Va1, Vgk1, cfg_.tube);
        const TriodeEval k2 = triodeEval(Va2, Vgk2, cfg_.tube);
        const double r1 = (cfg_.bPlus - Va1) * gRa1 - k1.Ip;
        const double r2 = (cfg_.bPlus - Va2) * gRa2 - k2.Ip;
        const double r3 = k1.Ip + k2.Ip - Vk * gTail;
        // Jacobian sparsity is FIXED (each plate row couples only its own plate and
        // the shared tail; J01 == J10 == 0), so the 3x3 solves by direct elimination
        // instead of general Cramer with column copies (§25) — same solution:
        //   J00·d0            + J02·d2 = b0
        //             J11·d1  + J12·d2 = b1
        //   J20·d0  + J21·d1  + J22·d2 = b2
        const double J00 = -gRa1 - k1.dVa, J02 = k1.dVgk;
        const double J11 = -gRa2 - k2.dVa, J12 = k2.dVgk;
        const double J20 = k1.dVa, J21 = k2.dVa;
        const double J22 = -(k1.dVgk + k2.dVgk) - gTail;
        const double b0 = -r1, b1 = -r2, b2 = -r3;
        if (std::fabs(J00) < 1e-30 || std::fabs(J11) < 1e-30) break;
        const double q0 = J20 / J00, q1 = J21 / J11;
        const double denom = J22 - q0 * J02 - q1 * J12;
        if (std::fabs(denom) < 1e-30) break;
        const double dk = (b2 - q0 * b0 - q1 * b1) / denom;
        const double d0 = std::clamp((b0 - J02 * dk) / J00, -60.0, 60.0);
        const double d1 = std::clamp((b1 - J12 * dk) / J11, -60.0, 60.0);
        const double d2 = std::clamp(dk, -20.0, 20.0);
        Va1 += d0; Va2 += d1; Vk += d2;
        if (std::fabs(d0) < tol && std::fabs(d1) < tol && std::fabs(d2) < tol) break;
    }
    va1_ = Va1; va2_ = Va2; vk_ = Vk;
    va1 = Va1; va2 = Va2;
}

// ===========================================================================
// Jcm800PowerAmp
// ===========================================================================
Jcm800PowerAmp::Jcm800PowerAmp() {
    LtpInverter::Config c;
    ltp_.configure(c);
}

void Jcm800PowerAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    setOversampling(os_.factor());
}

void Jcm800PowerAmp::setOversampling(int factor) {
    os_.setFactor(factor);
    osRate_ = sampleRate_ * os_.factor();
    const double T = 1.0 / osRate_;

    // Sag companion conductances (backward Euler: C/T).
    gRes_ = kCreservoir / T;
    gScr_ = kCscreen / T;
    // PI→EL34 coupling companions.
    gCc_ = kCoupCc / T;
    gRg_ = 1.0 / kCoupRg;

    // One-pole TPT coefficients (a = tan(pi*fc/fs)/(1+tan)) for the OT + presence.
    auto onePoleA = [&](double fc) {
        const double g = std::tan(M_PI * fc / osRate_);
        return g / (1.0 + g);
    };
    otHpA_ = onePoleA(kOtLfHz);
    otLpA_ = onePoleA(kOtHfHz);
    presLpA_ = onePoleA(kPresenceHz);

    ltp_.prepare();
    solveOperatingPoint();
    parkState();
}

// Park all dynamic state at the operating point (no turn-on thump). Called after
// the DC solve at prepare/OS change, and by reset() WITHOUT a re-solve.
void Jcm800PowerAmp::parkState() {
    vRail_ = vRailIdle_;
    vScreen_ = vScreenIdle_;
    vCcUp_ = ltp_.quiescentPlate1() - kVbias;   // cap blocks DC: grid at Vbias
    vCcDown_ = ltp_.quiescentPlate2() - kVbias;
    vgUp_ = vgDown_ = kVbias;                   // grid warm starts at idle
    otHpS_ = 0.0; otLpS_ = 0.0; presLpS_ = 0.0;
    fbDelay_ = 0.0;
    lastOutPeak_ = 0.0;
}

void Jcm800PowerAmp::reset() {
    ltp_.reset();
    parkState();
    os_.reset();
}

// Self-consistent idle: EL34 bias current depends on rail+screen, which depend on
// the current draw. Fixed-point iterate (grids at Vbias, plates ~ rail, no swing).
void Jcm800PowerAmp::solveOperatingPoint() {
    double rail = kVsupply - 0.09 * kRsupply;   // guess ~90 mA draw
    double scr = rail - 0.008 * kRscreen;
    double ip = 0.0, ig2 = 0.0;
    for (int it = 0; it < 200; ++it) {
        ip = el34PlateCurrent(rail, kVbias, scr, el34_);
        ig2 = el34ScreenCurrent(kVbias, scr, el34_);
        const double newRail = kVsupply - (2.0 * ip + 2.0 * ig2) * kRsupply;
        const double newScr = newRail - (2.0 * ig2) * kRscreen;
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

void Jcm800PowerAmp::setParameter(int paramId, float value) {
    // NaN-rejecting (ParamGuard.h) — audit finding 1.
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_PRESENCE: presence_ = v; break;
        // PARAM_DRIVE maps 0..1 -> 0..2x PI input trim (1.0 == unity at noon-ish).
        case PARAM_DRIVE: drive_ = 2.0 * v; break;
        default: break;
    }
}

// Per-tube plate-load Newton: solve Ip with Vp = rail − (Ip − Iq)·Rpp. The grids
// are FIXED during this solve, so the Koren base factor (softplus + pow) hoists out
// of the loop (Ip = base·atan(Vp/kvb), see el34PlateBase) and dIp/dVp is exact:
// base/(kvb·(1+(Vp/kvb)²)). Same residual equation and exit tolerance as the
// central-difference version it replaced (§25, regression-gated at −120 dBFS);
// each iteration now costs one atan instead of three full pentode evaluations.
inline double Jcm800PowerAmp::solveTubePlate(double vg1k, double vg2, double rail,
                                             double& vpOut, double& baseOut) const {
    const double tol = 1e-4 * tubeSolverTolScale();
    const double base = el34PlateBase(vg1k, vg2, el34_);
    const double kvb = el34_.kvb;
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

// EL34 grid node: PI plate AC through the coupling cap Cc into the grid leak Rg
// (to Vbias) + grid conduction (soft clamp to the grounded cathode). Same Thévenin
// + conduction structure as the M9.1 triode input. Returns Vg1k (= grid volts, the
// cathode is grounded); updates the coupling-cap blocking state vCc.
inline double Jcm800PowerAmp::solveEl34Grid(double vpPlateAC, double vpPlateQ,
                                            double& vCc, double& vgWarm) const {
    // Source node driving the cap is the PI plate: absolute plate = vpPlateQ + AC.
    const double vp = vpPlateQ + vpPlateAC;
    // Cap companion: current into grid = gCc*((vp - Vg) - vCc_prev). Grid KCL:
    //   gCc*((vp - Vg) - vCc) = (Vg - Vbias)*gRg + Igk(Vg)
    // Newton in Vg. Igk = (Vgn/Rgk)*softplus(Vg/Vgn) (conduction for Vg ≳ 0).
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
    vCc = (vp - Vg);  // update cap voltage (blocking state)
    vgWarm = Vg;      // next sample's warm start
    return Vg;        // grid-cathode (cathode grounded)
}

inline float Jcm800PowerAmp::processSampleOS(float xf) {
    const double x = static_cast<double>(xf) * drive_;

    // 1. Phase inverter: V3A grid = input; V3B grid = feedback (unit-delayed). The
    //    forward path vg1→secondary is inverting and vg2→secondary is non-inverting,
    //    so NEGATIVE feedback injects −β·(shaped secondary) at V3B (opposes output).
    const double vfb = -feedbackBeta() * fbDelay_;  // fbDelay_ = shaped secondary V
    double va1 = 0.0, va2 = 0.0;
    ltp_.processSample(x, vfb, va1, va2);
    const double va1AC = va1 - ltp_.quiescentPlate1();
    const double va2AC = va2 - ltp_.quiescentPlate2();

    // 2. PI plates → EL34 grids (coupling + blocking). Anti-phase legs.
    const double vgUp = solveEl34Grid(va1AC, ltp_.quiescentPlate1(), vCcUp_, vgUp_);
    const double vgDown = solveEl34Grid(va2AC, ltp_.quiescentPlate2(), vCcDown_, vgDown_);

    // 3. EL34 pair: plate currents (with plate-load saturation) at the sagged rail.
    //    The screen currents reuse the plate solve's hoisted Koren base — same E1,
    //    Ig2 = base·(kg1/kg2) (§25).
    double vpUp = 0.0, vpDown = 0.0, baseUp = 0.0, baseDown = 0.0;
    const double ipUp = solveTubePlate(vgUp, vScreen_, vRail_, vpUp, baseUp);
    const double ipDown = solveTubePlate(vgDown, vScreen_, vRail_, vpDown, baseDown);
    const double kScr = el34_.kg1 / el34_.kg2;
    const double ig2Up = baseUp * kScr;
    const double ig2Down = baseDown * kScr;

    // 4. Output transformer (linear): differential primary voltage → secondary.
    const double vPriDiff = (ipUp - ipDown) * kRppReflected;  // volts
    double vSec = vPriDiff / otTurnsRatio();
    // OT bandwidth: HF low-pass then LF high-pass (one-pole TPT).
    {
        const double vLp = otLpS_ + otLpA_ * (vSec - otLpS_);
        otLpS_ = 2.0 * vLp - otLpS_;
        vSec = vLp;
        const double vLp2 = otHpS_ + otHpA_ * (vSec - otHpS_);
        otHpS_ = 2.0 * vLp2 - otHpS_;
        vSec = vSec - vLp2;  // high-pass = input − low-pass
    }

    // 5. Sag: pull rail + screen down from the total current draw (backward Euler).
    //    Rail:  gRes*(Vrail − Vrail_prev) = (Vsupply − Vrail)/Rsupply − Iload
    //    Screen: gScr*(Vscr − Vscr_prev)  = (Vrail − Vscr)/Rscreen − Iscreen
    const double iLoad = ipUp + ipDown + ig2Up + ig2Down;
    const double iScr = ig2Up + ig2Down;
    vRail_ = (gRes_ * vRail_ + kVsupply / kRsupply - iLoad) /
             (gRes_ + 1.0 / kRsupply);
    vScreen_ = (gScr_ * vScreen_ + vRail_ / kRscreen - iScr) /
               (gScr_ + 1.0 / kRscreen);

    // 6. Presence-shaped feedback (unit delay for the next sample). β(f) reduces HF
    //    feedback: shaped = (1−p)·vSec + p·LP(vSec). p=0 → full (dark), p=1 → HF free.
    const double vLpP = presLpS_ + presLpA_ * (vSec - presLpS_);
    presLpS_ = 2.0 * vLpP - presLpS_;
    fbDelay_ = (1.0 - presence_) * vSec + presence_ * vLpP;

    // Normalize to 1.0 == full scale.
    const double outNorm = vSec / kFullScaleSecV;
    lastOutPeak_ = std::max(lastOutPeak_, std::fabs(outNorm));
    return static_cast<float>(outNorm);
}

void Jcm800PowerAmp::process(const float* in, float* out, int numFrames) {
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
