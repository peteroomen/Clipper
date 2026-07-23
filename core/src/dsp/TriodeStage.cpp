// Clipper — TriodeStage (M9.1). See TriodeStage.h for the model overview, the
// Koren parameter citation, and the circuit topology. This file is the numerics:
// the Koren device law + its analytic derivatives, the per-sample 3x3 nodal
// Newton solve, the reactive companions, and the DC operating-point solver.
//
// ---------------------------------------------------------------------------
// Per-sample nodal system (all currents in A, voltages in V).
//
// Unknowns: Va (plate), Vg (grid), Vk (cathode).  Vgk = Vg - Vk.
//   Ip  = korenPlateCurrent(Va, Vgk)                       (plate current)
//   Igk = gridCurrent(Vgk)                                 (grid current, soft)
//
// Reactive elements as backward-Euler companions at the oversampled rate:
//   * Coupling cap Cc + grid leak Rgl collapse to a Thevenin (Vth, Rth) driving
//     the grid through the stopper Rg:
//        Vth = gCc*(x - vCc_prev) / (gCc + 1/Rgl),   Rth = 1/(gCc + 1/Rgl) + Rg
//     After the solve the cap voltage updates to vCc = x - vGL, where
//     vGL = Vg + Igk*Rg is the grid-leak-node voltage. Grid conduction charging
//     this cap, recovering through Rgl, IS the blocking distortion (tau=Rgl*Cc).
//   * Cathode: Rk || Ck -> conductance Gk = 1/Rk + gCk, history current gCk*vCk.
//
// Residuals:
//   r1 = (B+ - Va)/Ra - Ip                                  (plate KCL)
//   r2 = Ip + Igk - Gk*Vk + gCk*vCk_prev                    (cathode KCL)
//   r3 = (Vth - Vg)/Rth - Igk                               (grid KCL)
//
// Jacobian (dIp_dVa, dIp_dVgk from korenDerivs; dIg from gridCurrentDeriv):
//   J = | -1/Ra - dIp_dVa     -dIp_dVgk               +dIp_dVgk               |
//       |  dIp_dVa             dIp_dVgk + dIg          -dIp_dVgk - dIg - Gk    |
//       |  0                  -1/Rth - dIg             +dIg                    |
// Newton: solve J * delta = -r, damp the step, iterate to |delta|<tol or the cap.
// ---------------------------------------------------------------------------

#include "clipper/dsp/TriodeStage.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {

// Numerically safe softplus log(1+exp(u)) and its derivative sigmoid(u).
inline double softplus(double u) {
    if (u > 30.0) return u;             // 1+exp(u) ~ exp(u); avoids overflow
    if (u < -30.0) return std::exp(u);  // log(1+e^u) ~ e^u
    return std::log1p(std::exp(u));
}
inline double sigmoid(double u) {
    if (u > 30.0) return 1.0;
    if (u < -30.0) return std::exp(u);
    const double e = std::exp(u);
    return e / (1.0 + e);
}

}  // namespace

// Koren plate current. E1 >= 0 for Va >= 0; the (1 + sign(E1)) factor rectifies
// to cutoff (Ip = 0) once E1 <= 0 (deep cutoff / negative plate).
double TriodeStage::korenPlateCurrent(double Va, double Vgk, const KorenParams& p) {
    const double S = std::sqrt(p.kvb + Va * Va);
    const double A = p.kp * (1.0 / p.mu + Vgk / S);
    const double L = softplus(A);            // log(1 + exp(A))
    const double E1 = (Va / p.kp) * L;
    if (E1 <= 0.0) return 0.0;
    return 2.0 * std::pow(E1, p.ex) / p.kg1;  // (1 + sign(E1)) = 2 for E1 > 0
}

namespace {

// Koren current AND its partials dIp/dVa, dIp/dVgk at (Va, Vgk). Derivatives of
// the expressions in korenPlateCurrent (see TriodeStage.h for the algebra).
struct KorenEval { double Ip, dVa, dVgk; };
KorenEval korenEval(double Va, double Vgk, const TriodeStage::KorenParams& p) {
    const double S = std::sqrt(p.kvb + Va * Va);
    const double A = p.kp * (1.0 / p.mu + Vgk / S);
    const double L = softplus(A);
    const double sig = sigmoid(A);
    const double E1 = (Va / p.kp) * L;
    if (E1 <= 0.0) return {0.0, 0.0, 0.0};
    const double C = 2.0 / p.kg1;
    const double Ip = C * std::pow(E1, p.ex);
    const double dIp_dE1 = C * p.ex * std::pow(E1, p.ex - 1.0);
    // dE1/dVgk = Va*sig/S ;  dE1/dVa = L/kp - Va^2*Vgk*sig/S^3
    const double dE1_dVgk = Va * sig / S;
    const double dE1_dVa = L / p.kp - (Va * Va * Vgk * sig) / (S * S * S);
    return {Ip, dIp_dE1 * dE1_dVa, dIp_dE1 * dE1_dVgk};
}

// Grid conduction: Igk = ig0 * softplus((Vgk - vgt)/vgn), ig0 = vgn/rgk.
struct GridEval { double Ig, dVgk; };
GridEval gridEval(double Vgk, const TriodeStage::Config& c) {
    const double ig0 = c.gridVgn / c.gridRgk;
    const double u = (Vgk - c.gridVgt) / c.gridVgn;
    const double Ig = ig0 * softplus(u);
    const double dIg = (ig0 / c.gridVgn) * sigmoid(u);  // -> 1/rgk above knee
    return {Ig, dIg};
}

// Solve the 3x3 system J*x = b in place (Cramer's rule). Returns false if J is
// singular (the caller then damps / bails).
bool solve3x3(const double J[3][3], const double b[3], double x[3]) {
    const double det =
        J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) -
        J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
        J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
    if (std::fabs(det) < 1e-30) return false;
    const double inv = 1.0 / det;
    // Cramer: replace column k with b.
    double c[3][3];
    for (int col = 0; col < 3; ++col) {
        for (int r = 0; r < 3; ++r) {
            c[r][0] = J[r][0]; c[r][1] = J[r][1]; c[r][2] = J[r][2];
        }
        for (int r = 0; r < 3; ++r) c[r][col] = b[r];
        const double d =
            c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
            c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
            c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
        x[col] = d * inv;
    }
    return true;
}

}  // namespace

TriodeStage::TriodeStage() = default;

void TriodeStage::configure(const Config& cfg) { cfg_ = cfg; }

void TriodeStage::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    os_.prepare(maxBlockSize_);
    reprepareReactive();
    solveOperatingPoint();
    // Park live node + companion states at the DC operating point.
    va_ = vaQuiescent_;
    vk_ = vkQuiescent_;
    vg_ = 0.0;
    vCc_ = 0.0;             // input coupling cap blocks DC -> grid DC = 0
    vCk_ = vkQuiescent_;    // cathode cap charged to the bias voltage
    vCo_ = vaQuiescent_;    // output coupling cap charged to the plate DC
    settleDC();
}

// Run silent samples until the discrete node/companion states reach the exact
// zero-input fixed point of processSampleOS. The 2D DC solve above ignores the
// (tiny) grid-leak DC current, leaving a ~40 mV plate offset that would otherwise
// high-pass through the output coupling as a ~40 ms turn-on thump. Settling here
// removes it (silence in -> silence out from sample 0) and refreshes the quiescent
// readouts to the true fixed point. One-time cost at prepare / OS change.
void TriodeStage::settleDC() {
    const int maxSettle = static_cast<int>(12.0 * cfg_.Rgl * cfg_.Cc * osRate_);
    for (int i = 0; i < maxSettle; ++i) {
        const float o = processSampleOS(0.0f);
        if (i > 256 && std::fabs(o) < 1e-9f) break;
    }
    vaQuiescent_ = va_;
    vkQuiescent_ = vk_;
    iqQuiescent_ = korenPlateCurrent(va_, vg_ - vk_, cfg_.tube);
    lastMaxIters_ = 0;
}

void TriodeStage::setOversampling(int factor) {
    os_.setFactor(factor);
    reprepareReactive();
    // Reset live states to the operating point after an OS change (clean restart).
    va_ = vaQuiescent_;
    vk_ = vkQuiescent_;
    vg_ = 0.0;
    vCc_ = 0.0;
    vCk_ = vkQuiescent_;
    vCo_ = vaQuiescent_;
    settleDC();
}

void TriodeStage::reprepareReactive() {
    osRate_ = sampleRate_ * os_.factor();
    const double T = 1.0 / osRate_;
    gCc_ = cfg_.Cc / T;
    gRgl_ = 1.0 / cfg_.Rgl;
    gCk_ = (cfg_.Ck > 0.0) ? cfg_.Ck / T : 0.0;
    Gk_ = 1.0 / cfg_.Rk + gCk_;
    // Output coupling (Cc in series with the next-stage grid leak Rgl to ground),
    // as a backward-Euler series-RC companion loading the plate: series
    // conductance gOut = gCc*gRgl/(gCc+gRgl) (-> ~1/Rgl at audio, i.e. the 1 M
    // grid leak loads the plate; the cap blocks DC).
    gOut_ = gCc_ * gRgl_ / (gCc_ + gRgl_);
}

// DC operating point: caps open (block DC). Grid tied to 0 through Rgl (no grid
// current at bias), so Vg = 0, Vgk = -Vk. Solve the plate/cathode pair:
//   (B+ - Va)/Ra = Ip(Va, -Vk),   Ip(Va, -Vk) = Vk/Rk
// with a 2D Newton from a sensible guess; robust because it runs once at prepare.
void TriodeStage::solveOperatingPoint() {
    double Va = 0.6 * cfg_.bPlus;  // guess: mid load line
    double Vk = 1.0;
    for (int it = 0; it < 200; ++it) {
        const double Vgk = -Vk;
        const KorenEval k = korenEval(Va, Vgk, cfg_.tube);
        // r1 = (B+-Va)/Ra - Ip ;  r2 = Ip - Vk/Rk   (Igk ~ 0 at bias)
        const double r1 = (cfg_.bPlus - Va) / cfg_.Ra - k.Ip;
        const double r2 = k.Ip - Vk / cfg_.Rk;
        // dVgk/dVk = -1.
        const double J00 = -1.0 / cfg_.Ra - k.dVa;   // dr1/dVa
        const double J01 = k.dVgk;                   // dr1/dVk (dIp/dVgk * -1, but -Ip so +)
        const double J10 = k.dVa;                    // dr2/dVa
        const double J11 = -k.dVgk - 1.0 / cfg_.Rk;  // dr2/dVk
        const double det = J00 * J11 - J01 * J10;
        if (std::fabs(det) < 1e-30) break;
        const double dVa = -(r1 * J11 - r2 * J01) / det;
        const double dVk = -(J00 * r2 - J10 * r1) / det;
        Va += std::clamp(dVa, -40.0, 40.0);
        Vk += std::clamp(dVk, -2.0, 2.0);
        if (std::fabs(dVa) < 1e-9 && std::fabs(dVk) < 1e-9) break;
    }
    vaQuiescent_ = Va;
    vkQuiescent_ = Vk;
    iqQuiescent_ = korenPlateCurrent(Va, -Vk, cfg_.tube);
}

inline float TriodeStage::processSampleOS(float xf) {
    const double x = static_cast<double>(xf);

    // Input coupling-network Thevenin into the grid node (see file header).
    const double vth = gCc_ * (x - vCc_) / (gCc_ + gRgl_);
    const double rth = 1.0 / (gCc_ + gRgl_) + cfg_.Rg;
    const double gThev = 1.0 / rth;

    double Va = va_, Vg = vg_, Vk = vk_;  // warm start
    int it = 0;
    for (; it < kMaxNewtonIter; ++it) {
        const double Vgk = Vg - Vk;
        const KorenEval k = korenEval(Va, Vgk, cfg_.tube);
        const GridEval g = gridEval(Vgk, cfg_);

        // Plate KCL includes the output coupling load gOut*(Va - vCo_).
        const double r1 = (cfg_.bPlus - Va) / cfg_.Ra - k.Ip - gOut_ * (Va - vCo_);
        const double r2 = k.Ip + g.Ig - Gk_ * Vk + gCk_ * vCk_;
        const double r3 = gThev * (vth - Vg) - g.Ig;

        const double J[3][3] = {
            {-1.0 / cfg_.Ra - k.dVa - gOut_, -k.dVgk,          k.dVgk},
            {k.dVa,                   k.dVgk + g.dVgk,   -k.dVgk - g.dVgk - Gk_},
            {0.0,                    -gThev - g.dVgk,     g.dVgk},
        };
        const double b[3] = {-r1, -r2, -r3};
        double d[3];
        if (!solve3x3(J, b, d)) break;  // singular -> keep current iterate
        // Damp to keep Newton out of exp-overflow territory on a hard slam.
        d[0] = std::clamp(d[0], -60.0, 60.0);   // Va
        d[1] = std::clamp(d[1], -20.0, 20.0);   // Vg
        d[2] = std::clamp(d[2], -20.0, 20.0);   // Vk
        Va += d[0]; Vg += d[1]; Vk += d[2];
        if (std::fabs(d[0]) < 1e-7 && std::fabs(d[1]) < 1e-7 &&
            std::fabs(d[2]) < 1e-7)
            break;
    }
    lastMaxIters_ = std::max(lastMaxIters_, it + 1);

    // Commit node state (warm start for the next sample).
    va_ = Va; vg_ = Vg; vk_ = Vk;

    // Update reactive companions from the resolved solution.
    const double Vgk = Vg - Vk;
    const GridEval g = gridEval(Vgk, cfg_);
    const double vGL = Vg + g.Ig * cfg_.Rg;  // grid-leak node (drop across stopper)
    vCc_ = x - vGL;                          // input coupling-cap V (blocking state)
    vCk_ = Vk;                               // cathode-cap history

    // Output = next-stage grid voltage: plate AC through the output coupling cap
    // into the next 1 M grid leak (DC-blocked, so no quiescent subtraction).
    const double Vo = gCc_ * (Va - vCo_) / (gCc_ + gRgl_);
    vCo_ = Va - Vo;                          // output coupling-cap history
    return static_cast<float>(Vo);
}

void TriodeStage::process(const float* in, float* out, int numFrames) {
    lastMaxIters_ = 0;
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
