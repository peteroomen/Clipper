// Clipper — BjtStage (ROADMAP v1.1 item 4). See BjtStage.h for the model overview,
// the Ebers-Moll citation, and the collector-feedback circuit. This file is the
// numerics: the Ebers-Moll device law + its analytic derivatives, the per-sample
// 3×3 nodal Newton solve, the reactive companions, and the DC operating-point solve.
//
// ---------------------------------------------------------------------------
// Per-sample nodal system (all currents in A, voltages in V).
//
// Unknowns: Vb (base), Vc (collector), Ve (emitter). Vbe = Vb−Ve, Vbc = Vb−Vc.
//   Ic = ebersMollIc(Vbe, Vbc), Ib = ebersMollIb(Vbe, Vbc), Ie = Ic + Ib.
//   Id = 2·Isd·sinh((Vc−Vb)/nVt)   (clip diode pair, base↔collector; 0 if absent)
//
// Reactive elements as backward-Euler companions at the (oversampled) rate:
//   * Input coupling cap Cin between the driving node (vin) and the base:
//       current into base = gCin·(vin − Vb) − gCin·vCin_prev,  vCin = vin − Vb.
//   * Feedback cap Cf across Rf (base↔collector): current into base =
//       gCf·(Vc − Vb) − gCf·vCf_prev,  vCf = Vc − Vb.
//
// Residuals (KCL, current INTO each node = 0):
//   r1 (base)      = gCin·(vin−Vb) − gCin·vCin + (Vc−Vb)/Rf + gCf·(Vc−Vb) − gCf·vCf
//                    + Id(Vc−Vb) − Ib
//   r2 (collector) = (Vcc−Vc)/Rc + (Vb−Vc)/Rf + gCf·(Vb−Vc) + gCf·vCf − Ic − Id(Vc−Vb)
//   r3 (emitter)   = Ie − Ve/Re
//
// Jacobian columns (Vb, Vc, Ve): gf = dIf/dVbe, gr = dIr/dVbc, gd = dId/dv.
// Newton: solve J·Δ = −r, damp Δ, iterate to |Δ|<tol or the cap. A clamped exp +
// per-iteration step clamps keep it slam-proof (see kMaxNewtonIter, the ±10 V test).
// ---------------------------------------------------------------------------

#include "clipper/dsp/BjtStage.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {

// exp(x) with a TANGENT-LINE extension above xMax: for x > xMax the true exp is
// replaced by its tangent e0·(1 + (x−xMax)) — a straight line with slope e0 =
// exp(xMax). This is the standard junction-limiting trick: the value and its
// derivative stay finite, monotonic, and CONTINUOUS on a ±10 V slam, so Newton
// never hits a flat plateau (a hard clamp zeroes the derivative and stalls the
// solve — the failure mode this replaces). Returns {value, dvalue/dx}.
constexpr double kExpMax = 40.0;  // exp(40) ≈ 2.4e17; well past any real junction
struct Exp2 { double v, d; };
inline Exp2 limExp(double x) {
    if (x <= kExpMax) {
        const double e = std::exp(x);
        return {e, e};
    }
    const double e0 = std::exp(kExpMax);
    return {e0 * (1.0 + (x - kExpMax)), e0};  // tangent line, slope e0
}

// Solve the 3x3 system J*x = b in place (Cramer's rule). Returns false if J is
// singular (the caller then keeps the current iterate / bails). Mirrors the
// TriodeStage solver so the two device models share a numeric idiom.
bool solve3x3(const double J[3][3], const double b[3], double x[3]) {
    const double det =
        J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) -
        J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
        J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
    if (std::fabs(det) < 1e-30) return false;
    const double inv = 1.0 / det;
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

// Ebers-Moll evaluation: currents Ic, Ib, Ie AND the two conductance partials
// gf = dIf/dVbe, gr = dIr/dVbc (the raw diode transconductances), from which every
// node partial is assembled by the chain rule in the solver.
struct EmEval {
    double Ic, Ib, Ie, gf, gr;
};
EmEval emEval(double Vbe, double Vbc, const BjtStage::EbersMoll& p) {
    const Exp2 ef = limExp(Vbe / p.Vt);
    const Exp2 er = limExp(Vbc / p.Vt);
    const double If = p.Is * (ef.v - 1.0);
    const double Ir = p.Is * (er.v - 1.0);
    const double rR = 1.0 / p.betaR;
    const double rF = 1.0 / p.betaF;
    const double Ic = If - Ir * (1.0 + rR);
    const double Ib = If * rF + Ir * rR;
    const double gf = (p.Is / p.Vt) * ef.d;  // dIf/dVbe
    const double gr = (p.Is / p.Vt) * er.d;  // dIr/dVbc
    return {Ic, Ib, Ic + Ib, gf, gr};
}

// Antiparallel diode pair Id(v) = 2·Isd·sinh(v/nVt) = Isd·(e^{v/nVt} − e^{−v/nVt}),
// each exponential tangent-limited (limExp) so the current + slope stay finite and
// monotonic past the ~0.5 V knee (a fuzz clip stage drives well past it). dId/dv too.
struct DiodeEval { double Id, gd; };
DiodeEval diodeEval(double v, const BjtStage::DiodePair& d) {
    if (!d.present) return {0.0, 0.0};
    const Exp2 ep = limExp(v / d.nVt);
    const Exp2 en = limExp(-v / d.nVt);
    const double Id = d.Isd * (ep.v - en.v);
    const double gd = (d.Isd / d.nVt) * (ep.d + en.d);
    return {Id, gd};
}

// The full nodal system (residual + analytic Jacobian) for one stage at one
// sample, packaged so the DC solve and the per-sample solve share EXACTLY the same
// KCL and derivatives (the DC solve just passes the reactive companions as zero).
// eval() fills the 3-residual r and, if J != nullptr, the 3×3 Jacobian.
struct Sys {
    const BjtStage::Config& cfg;
    double gRc, gRf, gRe;
    double gCin, gCf;       // 0/0 for the DC solve (caps open)
    double vin, histCin, histCf;

    void eval(double Vb, double Vc, double Ve, double r[3], double J[3][3]) const {
        const EmEval e = emEval(Vb - Ve, Vb - Vc, cfg.bjt);
        const DiodeEval d = diodeEval(Vc - Vb, cfg.diodes);
        r[0] = gCin * (vin - Vb) - histCin + (Vc - Vb) * gRf +
               gCf * (Vc - Vb) - histCf + d.Id - e.Ib;                // base KCL
        r[1] = (cfg.Vcc - Vc) * gRc + (Vb - Vc) * gRf +
               gCf * (Vb - Vc) + histCf - e.Ic - d.Id;                // collector KCL
        r[2] = e.Ie - Ve * gRe;                                       // emitter KCL
        if (!J) return;
        const double rF = 1.0 / cfg.bjt.betaF, rR = 1.0 / cfg.bjt.betaR;
        const double dIc_dVb = e.gf - (1.0 + rR) * e.gr;
        const double dIc_dVc = (1.0 + rR) * e.gr;
        const double dIc_dVe = -e.gf;
        const double dIb_dVb = e.gf * rF + e.gr * rR;
        const double dIb_dVc = -e.gr * rR;
        const double dIb_dVe = -e.gf * rF;
        const double dIe_dVb = e.gf * (1.0 + rF) - e.gr;
        const double dIe_dVc = e.gr;
        const double dIe_dVe = -e.gf * (1.0 + rF);
        J[0][0] = -gCin - gRf - gCf - d.gd - dIb_dVb;
        J[0][1] = gRf + gCf + d.gd - dIb_dVc;
        J[0][2] = -dIb_dVe;
        J[1][0] = gRf + gCf + d.gd - dIc_dVb;
        J[1][1] = -gRc - gRf - gCf - d.gd - dIc_dVc;
        J[1][2] = -dIc_dVe;
        J[2][0] = dIe_dVb;
        J[2][1] = dIe_dVc;
        J[2][2] = dIe_dVe - gRe;
    }
};

inline double infNorm3(const double r[3]) {
    return std::max(std::fabs(r[0]), std::max(std::fabs(r[1]), std::fabs(r[2])));
}

// Damped Newton with backtracking line search on the residual ∞-norm — the
// globalization that makes the STIFF diode-in-feedback system converge under a
// hard slam (a bare Newton oscillates and stalls at the iteration cap, which is
// what produced the pre-fix cascade blow-up). Returns iterations used. Warm-started
// from (Vb,Vc,Ve); writes the solution back into them.
int dampedNewton(const Sys& sys, double& Vb, double& Vc, double& Ve, int maxIter) {
    double r[3], J[3][3];
    sys.eval(Vb, Vc, Ve, r, J);
    int it = 0;
    for (; it < maxIter; ++it) {
        const double b[3] = {-r[0], -r[1], -r[2]};
        double dx[3];
        if (!solve3x3(J, b, dx)) break;  // singular -> keep the iterate
        // Gross safety clamp (keeps the exp argument sane before the line search).
        for (double& v : dx) v = std::clamp(v, -10.0, 10.0);
        const double cur = infNorm3(r);
        // Backtrack: halve the step until the residual norm drops (Armijo-ish).
        // Each trial evaluates the JACOBIAN alongside the residual — given the
        // Ebers-Moll/diode exponentials the J entries are a handful of extra adds
        // and multiplies, and carrying them out of the accepted trial removes the
        // separate refresh eval that used to re-run the exponentials at the new
        // point every iteration (~2x fewer evals; §25 solver-perf pass; the Newton
        // path — every iterate, every accepted step — is unchanged bit for bit).
        double lam = 1.0, rt[3], Jt[3][3];
        double Vb2 = Vb, Vc2 = Vc, Ve2 = Ve;
        for (int bt = 0; bt < 30; ++bt) {
            Vb2 = Vb + lam * dx[0];
            Vc2 = Vc + lam * dx[1];
            Ve2 = Ve + lam * dx[2];
            sys.eval(Vb2, Vc2, Ve2, rt, Jt);
            if (infNorm3(rt) < cur * (1.0 - 1e-4 * lam)) break;
            lam *= 0.5;
        }
        Vb = Vb2; Vc = Vc2; Ve = Ve2;
        // Adopt the accepted trial's residual + Jacobian for the next iteration.
        r[0] = rt[0]; r[1] = rt[1]; r[2] = rt[2];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) J[i][j] = Jt[i][j];
        if (std::fabs(lam * dx[0]) < 1e-9 && std::fabs(lam * dx[1]) < 1e-9 &&
            std::fabs(lam * dx[2]) < 1e-9)
            break;
    }
    return it + 1;
}

}  // namespace

double BjtStage::ebersMollIc(double Vbe, double Vbc, const EbersMoll& p) {
    const double If = p.Is * (limExp(Vbe / p.Vt).v - 1.0);
    const double Ir = p.Is * (limExp(Vbc / p.Vt).v - 1.0);
    return If - Ir * (1.0 + 1.0 / p.betaR);
}
double BjtStage::ebersMollIb(double Vbe, double Vbc, const EbersMoll& p) {
    const double If = p.Is * (limExp(Vbe / p.Vt).v - 1.0);
    const double Ir = p.Is * (limExp(Vbc / p.Vt).v - 1.0);
    return If / p.betaF + Ir / p.betaR;
}

BjtStage::BjtStage() = default;

void BjtStage::configure(const Config& cfg) { cfg_ = cfg; }

void BjtStage::reprepareReactive() {
    const double T = 1.0 / sampleRate_;
    gCin_ = cfg_.Cin / T;
    gCf_ = cfg_.Cf / T;
}

void BjtStage::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 176400.0;
    reprepareReactive();
    solveOperatingPoint();
    // Park live node + companion states at the DC operating point (Cin blocks DC,
    // so the base-side of Cin is at Vb with the input side at 0 -> vCin = -Vb).
    vb_ = vbQ_;
    vc_ = vcQ_;
    ve_ = veQ_;
    vCin_ = 0.0 - vbQ_;   // vin = 0 at DC; cap holds the base bias
    vCf_ = vcQ_ - vbQ_;   // feedback cap charged to the base-collector bias
    settleDC();
}

// DC operating point: caps OPEN (block DC). With vin = 0 the input coupling cap
// carries no DC current, so the only base path is Rf (+ the diodes). Solve the
// full 3-node system with gCin_ = gCf_ = 0 but the DIODES LIVE (a clip stage
// self-biases with the diodes conducting — see BjtStage.h). Damped Newton from a
// sensible guess; robust because it runs once at prepare.
void BjtStage::solveOperatingPoint() {
    double Vb = 0.85, Vc = 1.3, Ve = 0.28;  // guess near the cold Muff bias
    const Sys sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                  0.0,  0.0,  // caps OPEN at DC
                  0.0,  0.0, 0.0};
    dampedNewton(sys, Vb, Vc, Ve, 400);
    vbQ_ = Vb; vcQ_ = Vc; veQ_ = Ve;
    icQ_ = ebersMollIc(Vb - Ve, Vb - Vc, cfg_.bjt);
}

// Run silent samples until the discrete node/companion states reach the exact
// zero-input fixed point of processSample. The DC solve above ignores the (tiny)
// cap leakage; settling removes any residual turn-on offset so silence in ->
// silence out from sample 0. One-time cost at prepare.
void BjtStage::settleDC() {
    // ~12 input-coupling RC constants is plenty (tau = Rf*Cin dominates recovery).
    const double tau = cfg_.Rf * cfg_.Cin;
    const int maxSettle = std::max(512, static_cast<int>(12.0 * tau * sampleRate_));
    for (int i = 0; i < maxSettle; ++i) {
        const float o = processSample(0.0f);
        if (i > 256 && std::fabs(o) < 1e-9f) break;
    }
    vcQ_ = vc_; vbQ_ = vb_; veQ_ = ve_;
    icQ_ = ebersMollIc(vb_ - ve_, vb_ - vc_, cfg_.bjt);
    cachePark();
    lastMaxIters_ = 0;
}

// Snapshot the settled zero-input fixed point so reset() can restore it without
// re-running solveOperatingPoint() + settleDC().
void BjtStage::cachePark() {
    vbPark_ = vb_;
    vcPark_ = vc_;
    vePark_ = ve_;
    vCinPark_ = vCin_;
    vCfPark_ = vCf_;
}

void BjtStage::reset() {
    vb_ = vbPark_;
    vc_ = vcPark_;
    ve_ = vePark_;
    vCin_ = vCinPark_;
    vCf_ = vCfPark_;
    lastMaxIters_ = 0;
}

float BjtStage::processSample(float vinF) {
    const double vin = static_cast<double>(vinF);
    // Companion history currents (constant across this sample's iterations).
    const Sys sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                  gCin_, gCf_, vin, gCin_ * vCin_, gCf_ * vCf_};

    double Vb = vb_, Vc = vc_, Ve = ve_;  // warm start
    const int iters = dampedNewton(sys, Vb, Vc, Ve, kMaxNewtonIter);
    lastMaxIters_ = std::max(lastMaxIters_, iters);

    vb_ = Vb; vc_ = Vc; ve_ = Ve;      // warm start for the next sample
    vCin_ = vin - Vb;                   // update companion histories
    vCf_ = Vc - Vb;

    return static_cast<float>(Vc - vcQ_);  // collector AC (inverting output)
}

}  // namespace clipper::dsp
