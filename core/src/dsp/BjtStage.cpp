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
// Newton: solve J·Δ = −r, damp Δ, iterate until the RESIDUAL is at tolerance (see
// kNewtonResidualTolA), or |Δ| stops moving, or the cap. A clamped exp +
// per-iteration step clamps keep it slam-proof (see kMaxNewtonIter, the ±20 V test).
//
// ---------------------------------------------------------------------------
// The FOURTH node: the DC-blocked diode branch (cfg.Cdiode > 0 — docs §53).
//
// The real Big Muff clip stage does NOT wire its antiparallel diodes straight
// across the collector-base feedback: they sit in series with a 1 µF cap (C6/C7),
// so the branch is open at DC and only clips the AC swing. Modelling that needs
// one more unknown, Vd — the junction between the cap and the diode pair:
//
//        Vc ──┤ Cd ├── Vd ──(D± pair)── Vb
//
//   Icd = gCd·(Vc−Vd) − gCd·vCd     (backward-Euler companion, vCd = Vc − Vd)
//   Id  = 2·Isd·sinh((Vd−Vb)/nVt)   (the pair, now referenced to Vd, not Vc)
//
// so Id leaves the base row's Vc dependence and the collector row loses Id and
// gains −Icd, and a fourth residual closes the new node:
//
//   r1 (base)      = … + Id(Vd−Vb) − Ib
//   r2 (collector) = … − Ic − Icd
//   r4 (node d)    = Icd − Id(Vd−Vb)
//
// Jacobian columns become (Vb, Vc, Ve, Vd); the diode partials move to
// ∂/∂Vb = −gd and ∂/∂Vd = +gd, and gCd appears in the (collector, d) block. The
// 4×4 is solved by Gaussian elimination with partial pivoting (solve4x4) rather
// than Cramer — 4×4 Cramer is five 4×4 determinants, and the pivoting is worth
// having on a system whose diode row can be many decades stiffer than its
// resistive rows.
//
// At DC the cap is open (gCd = 0), so r4 degenerates to −Id(Vd−Vb) = 0, i.e.
// Vd = Vb with ZERO branch current — the diodes are off and the stage biases on
// Rf + Rbg alone, exactly as the real one does. That row is never singular: gd is
// Isd/nVt·(e^x + e^−x) > 0 everywhere.
//
// Cdiode == 0 keeps the 3-node path above, unchanged and bit-identical. The two
// paths are separate instantiations of ONE templated damped Newton, so the
// globalization, the step clamps, the residual early-out and the iteration
// accounting cannot drift apart between them.
// ---------------------------------------------------------------------------

#include "clipper/dsp/BjtStage.h"

#include "clipper/dsp/TubeSolverMode.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

// Solve the 3x3 system J*x = b (Cramer's rule). Returns false if J is
// singular (the caller then keeps the current iterate / bails). Mirrors the
// TriodeStage solver so the two device models share a numeric idiom.
//
// DO NOT "unify" this with solve4x4 below. This exact expression sequence is what
// makes a Cdiode == 0 stage bit-identical to the pre-§53 model (and to the pre-§49
// one at Rs == 0), which is the property that keeps Q1/Q4 — and every future
// non-Muff BjtStage user — out of this slice's blast radius.
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

// Solve the 4x4 system J*x = b by Gaussian elimination with PARTIAL PIVOTING
// (docs §53). Used only by the DC-blocked diode branch. Pivoting matters here in a
// way it does not for the 3x3: the diode row's conductance gd spans ~20 decades
// between "off" (Isd/nVt ~ 1e-7 S) and "slammed" (tangent-limited, ~1e10 S), so an
// unpivoted elimination can divide by a pivot many decades below the column's
// largest entry. Returns false on a singular/degenerate pivot, in which case the
// caller keeps the current iterate — the same contract as solve3x3.
bool solve4x4(const double J[4][4], const double b[4], double x[4]) {
    double a[4][5];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) a[i][j] = J[i][j];
        a[i][4] = b[i];
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        double best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            const double m = std::fabs(a[r][col]);
            if (m > best) { best = m; piv = r; }
        }
        if (best < 1e-30) return false;  // singular -> keep the iterate
        if (piv != col)
            for (int j = col; j < 5; ++j) std::swap(a[col][j], a[piv][j]);
        const double inv = 1.0 / a[col][col];
        for (int r = col + 1; r < 4; ++r) {
            const double f = a[r][col] * inv;
            if (f == 0.0) continue;
            for (int j = col; j < 5; ++j) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 3; i >= 0; --i) {
        double s = a[i][4];
        for (int j = i + 1; j < 4; ++j) s -= a[i][j] * x[j];
        x[i] = s / a[i][i];
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
    static constexpr int kNodes = 3;
    const BjtStage::Config& cfg;
    double gRc, gRf, gRe;
    double gCin, gCf;       // 0/0 for the DC solve (caps open)
    double vin, histCin, histCf;
    double kSer = 1.0;      // 1/(1+gCin*Rs): series base resistor divider (§49)
    double gRbg = 0.0;      // base-to-ground bias conductance (0 = absent)

    void eval(const double x[3], double r[3], double J[3][3]) const {
        const double Vb = x[0], Vc = x[1], Ve = x[2];
        const EmEval e = emEval(Vb - Ve, Vb - Vc, cfg.bjt);
        const DiodeEval d = diodeEval(Vc - Vb, cfg.diodes);
        r[0] = kSer * (gCin * (vin - Vb) - histCin) - Vb * gRbg + (Vc - Vb) * gRf +
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
        J[0][0] = -kSer * gCin - gRbg - gRf - gCf - d.gd - dIb_dVb;
        J[0][1] = gRf + gCf + d.gd - dIb_dVc;
        J[0][2] = -dIb_dVe;
        J[1][0] = gRf + gCf + d.gd - dIc_dVb;
        J[1][1] = -gRc - gRf - gCf - d.gd - dIc_dVc;
        J[1][2] = -dIc_dVe;
        J[2][0] = dIe_dVb;
        J[2][1] = dIe_dVc;
        J[2][2] = dIe_dVe - gRe;
    }
    // Gross safety clamp, COMPONENT-WISE (keeps the exp argument sane before the
    // line search). Frozen exactly as it was: this expression IS part of the
    // bit-identity contract for every stage without the DC-blocked branch.
    //
    // It is also, measurably, WRONG — see kMaxNewtonStepV below. Clipping the
    // components rotates the search direction off Newton's, and a rotated
    // direction is not guaranteed to be a descent direction for the residual
    // norm, so the backtracking line search can reject all 30 trials and the
    // solver spins at a standstill for the whole iteration cap. That is the
    // mechanism behind the pre-existing `muff-slam-exhausts-newton-cap` XFAIL
    // (docs §34): traced at 48 kHz x4, the failing samples show dx pinned at
    // exactly [-10, -10, …] with lam collapsed to 2^-30 and the residual frozen
    // at 2.487e-3 A for 60 iterations. Fixing it here would change the audio of
    // every existing BjtStage user, so it stays owned by that XFAIL's own slice.
    static void limitStep(double dx[3]) {
        for (int i = 0; i < 3; ++i) dx[i] = std::clamp(dx[i], -10.0, 10.0);
    }
};

// Step limit for the 4-node path, in volts, applied by SCALING the whole Newton
// direction rather than clipping its components (Sys4::limitStep). Same 10 V
// magnitude the 3-node path uses — the value is not the change, the direction
// preservation is. Traced, not guessed (docs §53): with the DC-blocked branch the
// component clamp saturates three of four components at once, and the resulting
// direction is not a descent direction, so every one of the 30 backtracks is
// rejected, |lam·dx| stays just above the 1e-9 step-size exit, and the solve
// stands still for all 60 iterations. Direction-preserving scaling keeps a true
// Newton direction, for which a small enough lam always decreases the residual,
// so the line search does its job again.
//
// ±20 V slam, worst Newton iterations of 60 over all 16 rate x oversampling
// combinations, measured on the clip-stage config:
//   component clamp (the 3-node rule)   60 at 16 of 16   NOT converged
//   direction-preserving scale          16 at 16 of 16   converged
// Amplitude sweep at 48 kHz x4 with scaling: 1 V=10, 3 V=12, 5 V=14, 10 V=14,
// 15 V=16, 20 V=16, 30 V=17 — it degrades gracefully instead of falling off a
// cliff, which is what a globalization is supposed to do.
constexpr double kMaxNewtonStepV = 10.0;

// The 4-node system: same stage, but the diode pair sits behind its DC-blocking
// cap Cd, so its junction node Vd is a fourth unknown (docs §53 and the header
// block at the top of this file). Written out separately rather than folded into
// Sys with a flag, because Sys's expression sequence IS the bit-identity contract
// for every stage that does not have the branch.
struct Sys4 {
    static constexpr int kNodes = 4;
    const BjtStage::Config& cfg;
    double gRc, gRf, gRe;
    double gCin, gCf, gCd;              // gCd = Cd/T; 0/0/0 for the DC solve
    double vin, histCin, histCf, histCd;
    double kSer = 1.0;                  // 1/(1+gCin*Rs) — §49, as in Sys
    double gRbg = 0.0;                  // base-to-ground bias conductance

    void eval(const double x[4], double r[4], double J[4][4]) const {
        const double Vb = x[0], Vc = x[1], Ve = x[2], Vd = x[3];
        const EmEval e = emEval(Vb - Ve, Vb - Vc, cfg.bjt);
        // The pair now spans Vd -> Vb, not Vc -> Vb. Id is still "current flowing
        // INTO the base through the diodes", so the base row's sign is unchanged.
        const DiodeEval d = diodeEval(Vd - Vb, cfg.diodes);
        // Cap branch current, collector -> node d (backward Euler companion).
        const double Icd = gCd * (Vc - Vd) - histCd;
        r[0] = kSer * (gCin * (vin - Vb) - histCin) - Vb * gRbg + (Vc - Vb) * gRf +
               gCf * (Vc - Vb) - histCf + d.Id - e.Ib;                // base KCL
        r[1] = (cfg.Vcc - Vc) * gRc + (Vb - Vc) * gRf +
               gCf * (Vb - Vc) + histCf - e.Ic - Icd;                 // collector KCL
        r[2] = e.Ie - Ve * gRe;                                       // emitter KCL
        r[3] = Icd - d.Id;                                            // diode-node KCL
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
        J[0][0] = -kSer * gCin - gRbg - gRf - gCf - d.gd - dIb_dVb;
        J[0][1] = gRf + gCf - dIb_dVc;
        J[0][2] = -dIb_dVe;
        J[0][3] = d.gd;
        J[1][0] = gRf + gCf - dIc_dVb;
        J[1][1] = -gRc - gRf - gCf - gCd - dIc_dVc;
        J[1][2] = -dIc_dVe;
        J[1][3] = gCd;
        J[2][0] = dIe_dVb;
        J[2][1] = dIe_dVc;
        J[2][2] = dIe_dVe - gRe;
        J[2][3] = 0.0;
        J[3][0] = d.gd;
        J[3][1] = gCd;
        J[3][2] = 0.0;
        J[3][3] = -gCd - d.gd;
    }
    // Direction-PRESERVING step limit: scale the whole Newton direction so its
    // largest component is at most kMaxNewtonStepV, instead of clipping each
    // component to that bound. Same magnitude bound, but the direction survives,
    // which is what the backtracking line search needs to be able to succeed.
    static void limitStep(double dx[4]) {
        double m = 0.0;
        for (int i = 0; i < 4; ++i) m = std::max(m, std::fabs(dx[i]));
        if (m > kMaxNewtonStepV) {
            const double s = kMaxNewtonStepV / m;
            for (int i = 0; i < 4; ++i) dx[i] *= s;
        }
    }
};

inline double infNorm3(const double r[3]) {
    return std::max(std::fabs(r[0]), std::max(std::fabs(r[1]), std::fabs(r[2])));
}
inline double infNorm4(const double r[4]) {
    return std::max(infNorm3(r), std::fabs(r[3]));
}
// Dispatch by node count so ONE damped Newton drives both systems. N == 3 keeps
// the exact pre-§53 primitives (Cramer + the 3-way max), which is what the
// bit-identity proof rests on.
template <int N> struct Numerics;
template <> struct Numerics<3> {
    static double infNorm(const double r[3]) { return infNorm3(r); }
    static bool solve(const double J[3][3], const double b[3], double x[3]) {
        return solve3x3(J, b, x);
    }
};
template <> struct Numerics<4> {
    static double infNorm(const double r[4]) { return infNorm4(r); }
    static bool solve(const double J[4][4], const double b[4], double x[4]) {
        return solve4x4(J, b, x);
    }
};

// Convergence tolerance for the KCL residual ∞-norm, in AMPS — the residuals are
// node currents, so this is a current, not a voltage and not a relative figure
// (audit finding 12, docs §34). Scaled by tubeSolverTolScale() so the existing
// test-only reference mode (TubeSolverMode.h) tightens it 1000x along with every
// valve solver's, rather than this file growing a second knob.
//
// THIS IS AN ACCURACY TRADE, NOT A FREE LUNCH — read before touching the value.
//
// The pre-fix solver kept iterating until the STEP fell below 1e-9 V, and its line
// search only accepts a strict residual decrease, so it drove the residual all the
// way down to the floating-point floor. Any early-out that actually FIRES therefore
// declines refinement the old solver performed. Bit-identity and the fix are
// mutually exclusive, and that was measured rather than assumed — a sweep of this
// constant against the pre-fix solver over 25 renders (5 sustain settings x 5 input
// levels, 2 s each at 48 kHz):
//
//   tol     idle cost     worst difference vs the pre-fix solver
//   1e-13   fixed         -81.8 dBFS abs / -89.0 dB rel
//   1e-14   fixed        -104.3 dBFS abs / -111.8 dB rel
//   1e-15   fixed        -108.5 dBFS abs / -116.0 dB rel
//   1e-16   fixed        -124.3 dBFS abs / -129.5 dB rel
//   1e-17   fixed        -127.4 dBFS abs / -134.1 dB rel   <-- chosen
//   1e-18   NOT fixed    -132.5 dBFS abs / -137.7 dB rel
//   1e-19   NOT fixed    bit-identical (never fires)
//
// The floor of the usable window is the IDLE RESIDUAL CEILING: the largest residual
// the parked stage ever presents, measured at 2.0600e-18 A. Below that the early-out
// stops firing and the pathology returns (the 1e-18 and 1e-19 rows). That ceiling is
// astonishingly stable — 2.0600e-18 to five figures at every one of 44.1/48/88.2/96/
// 192 kHz x 1/2/4/8 oversampling x sustain MIN/mid/MAX — because at the parked point
// the two large companion terms gCin·(vin−Vb) and histCin cancel exactly, leaving the
// residual set by the DC branch currents (~0.9 mA scale) and not by gCin = Cin/T. So
// the margin does not erode with rate or oversampling factor.
//
// 1e-17 is chosen as the tightest value that still fires: 4.9x above the idle
// ceiling, and 7.4 dB inside this project's established -120 dBFS solver-accuracy
// gate (docs §25) — the same bar every valve solver's early exit is held to.
// test_tube_solver.cpp measures exactly this number on every run, because reference
// mode scales this to 1e-20, below the idle ceiling, so the reference render IS the
// pre-fix solver.
//
// Per-sample, the error is far smaller than the output figure suggests: a residual
// |r| bounds the node-voltage error by |r|/g with g the limiting node conductance
// (the collector, gRc + gRf + gCf ~ 1.9e-4 S), so 1e-17 A is 53 femtovolts of node
// error per solve. The -127 dBFS output figure is that 53 fV amplified by the Muff's
// four cascaded high-gain stages and accumulated through their recursive state — it
// is a cascade-gain figure, not a per-solve one. In device terms 1e-17 A is 2.5e-14
// of the ~0.4 mA quiescent collector current.
//
// If you tighten this below ~2.1e-18 the Muff silently goes back to costing 3x more
// CPU idle than playing; test_muff_model.cpp's "idle solver cost" block fails if it
// stops firing, and test_tube_solver.cpp fails if it starts costing audible accuracy.
// Both directions are pinned. Do not change it without re-running both.
constexpr double kNewtonResidualTolA = 1e-17;

// Damped Newton with backtracking line search on the residual ∞-norm — the
// globalization that makes the STIFF diode-in-feedback system converge under a
// hard slam (a bare Newton oscillates and stalls at the iteration cap, which is
// what produced the pre-fix cascade blow-up). Warm-started from x[] = (Vb,Vc,Ve[,Vd]);
// writes the solution back into it. Returns the number of iterations that actually took
// a step (0 when the warm start was already the solution), never more than maxIter.
//
// `tol` is the residual early-out, in amps. The PER-SAMPLE solve passes
// kNewtonResidualTolA; the DC operating-point solve passes 0.0, i.e. opts out.
// That asymmetry is the point, not an oversight (docs §34):
//
//   * The early-out exists to remove per-sample work in the audio thread. The DC
//     solve runs ONCE inside prepare(), so early-outing it buys nothing measurable.
//   * It would not be free, either. The DC solve's final iterate seeds vbQ_/vcQ_/veQ_
//     and settleDC(), so it sets the quiescent point every render is referenced to.
//     Stopping it earlier moves that operating point in its last bits, and through
//     the Muff's four cascaded high-gain stages the shift showed up as a MEASURED
//     3x more output difference than the per-sample early-out alone contributes.
//     Opting the DC solve out costs nothing and removes that term entirely.
//
// tol = 0.0 makes `cur <= tol` fire only on an exactly-zero residual, where the
// Newton step is exactly zero and the iterate cannot move — so opting out preserves
// the old behaviour exactly rather than approximately.
//
// Templated on the system so the 3-node stage and the 4-node DC-blocked-branch
// stage share ONE globalization, one early-out and one iteration accounting. The
// N == 3 instantiation performs the same operations in the same order as the
// pre-§53 hand-written function — that is deliberate and load-bearing: it is what
// makes a stage without the branch bit-identical, and it is why the array/dispatch
// shape below must not be "simplified" into anything that reassociates.
template <class SysT>
int dampedNewton(const SysT& sys, double x[SysT::kNodes], int maxIter, double tol) {
    constexpr int N = SysT::kNodes;
    using Num = Numerics<N>;
    double r[N], J[N][N];
    sys.eval(x, r, J);
    int taken = 0;
    for (int it = 0; it < maxIter; ++it) {
        const double cur = Num::infNorm(r);
        // ALREADY SOLVED -> return. Without this the line search below is
        // unsatisfiable by construction at the quiescent point: the residual is at
        // the floating-point floor, no trial step can make it STRICTLY smaller, so
        // all 30 backtracks burn — 31 full Ebers-Moll system evaluations (4 exp
        // each) per sample to reproduce the answer we already had. That is why the
        // Muff cost 3x more CPU idle than playing (audit finding 12). Note this is
        // `<=`, so tol = 0.0 (the DC solve) still short-circuits an exactly-zero
        // residual, where the step is exactly zero and nothing could move anyway.
        if (cur <= tol) break;
        double b[N];
        for (int i = 0; i < N; ++i) b[i] = -r[i];
        double dx[N];
        if (!Num::solve(J, b, dx)) break;  // singular -> keep the iterate
        // Gross safety limit (keeps the exp argument sane before the line search).
        // Per-system: the 3-node path clamps components, frozen for bit-identity;
        // the 4-node path scales the direction. See both limitStep bodies.
        SysT::limitStep(dx);
        // Backtrack: halve the step until the residual norm drops (Armijo-ish).
        // Each trial evaluates the JACOBIAN alongside the residual — given the
        // Ebers-Moll/diode exponentials the J entries are a handful of extra adds
        // and multiplies, and carrying them out of the accepted trial removes the
        // separate refresh eval that used to re-run the exponentials at the new
        // point every iteration (~2x fewer evals; §25 solver-perf pass; the Newton
        // path — every iterate, every accepted step — is unchanged bit for bit).
        //
        // Deliberately NOT also short-circuiting this loop on `cur <= tol`: `cur` is
        // infNorm3(r) with r unchanged since the early-out above, so that branch is
        // unreachable. The top-of-loop check subsumes it. Measured: after the
        // early-out, ZERO iterations exhaust the 30 backtracks on any material —
        // including a ±20 V slam at max sustain, which burned 10.9 % of iterations
        // before. Every remaining backtrack is accepted well inside the 30, so the
        // globalization that makes a slam converge is intact and untouched.
        double lam = 1.0, rt[N], Jt[N][N];
        double xt[N];
        for (int i = 0; i < N; ++i) xt[i] = x[i];
        for (int bt = 0; bt < 30; ++bt) {
            for (int i = 0; i < N; ++i) xt[i] = x[i] + lam * dx[i];
            sys.eval(xt, rt, Jt);
            if (Num::infNorm(rt) < cur * (1.0 - 1e-4 * lam)) break;
            lam *= 0.5;
        }
        for (int i = 0; i < N; ++i) x[i] = xt[i];
        // Adopt the accepted trial's residual + Jacobian for the next iteration.
        for (int i = 0; i < N; ++i) r[i] = rt[i];
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) J[i][j] = Jt[i][j];
        taken = it + 1;
        bool settled = true;
        for (int i = 0; i < N; ++i)
            if (std::fabs(lam * dx[i]) >= 1e-9) { settled = false; break; }
        if (settled) break;
    }
    // `taken` counts iterations that moved the iterate, so exhausting the loop
    // reports exactly maxIter. The old `return it + 1` over-reported by one there,
    // which let lastMaxNewtonIterations() read kMaxNewtonIter + 1 (audit finding 12,
    // secondary note) — an iteration count above the cap it is compared against.
    return taken;
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
    // 0 when the diode branch is DC-coupled — the 4-node path is not taken at all
    // in that case, so this stays exactly 0 and costs nothing.
    gCd_ = cfg_.Cdiode > 0.0 ? cfg_.Cdiode / T : 0.0;
}

// True when this stage runs the 4-node system: the diode pair exists AND it sits
// behind its DC-blocking cap. Both conditions matter — Cdiode on a stage with no
// diodes would be a capacitor in series with an open circuit.
bool BjtStage::usesDcBlockedDiodes() const {
    return cfg_.diodes.present && cfg_.Cdiode > 0.0;
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
    // The DC-block cap sits charged to the whole collector-base bias, with its
    // diode side at the base (the diodes carry no DC — that is what C6/C7 do).
    // vd_ tracks vb_ on a stage without the branch so the state is never garbage.
    vd_ = vdQ_;
    vCd_ = vcQ_ - vdQ_;
    settleDC();
}

// DC operating point: caps OPEN (block DC). With vin = 0 the input coupling cap
// carries no DC current, so the only base path is Rf (+ the diodes). Solve the
// full 3-node system with gCin_ = gCf_ = 0 but the DIODES LIVE (a clip stage
// self-biases with the diodes conducting — see BjtStage.h). Damped Newton from a
// sensible guess; robust because it runs once at prepare.
void BjtStage::solveOperatingPoint() {
    const double gRbg = cfg_.Rbg > 0.0 ? 1.0 / cfg_.Rbg : 0.0;
    // tol = 0.0: the DC solve opts OUT of the residual early-out. It runs once at
    // prepare(), so there is no per-sample cost to remove, and its final iterate
    // seeds the quiescent point every render is referenced to — see dampedNewton.
    if (usesDcBlockedDiodes()) {
        // 4-node DC solve. gCd = 0 (the cap is open at DC), so the diode-node row
        // reduces to Id(Vd − Vb) = 0 and the solve drives Vd onto Vb — i.e. the
        // clip stage biases with its diodes OFF, on Rf + Rbg alone. That is the
        // whole point of C6/C7 and it is why this stage now has real headroom
        // (measured: Vc 1.213 V DC-coupled -> 5.5 V here; docs §53).
        double x[4] = {0.85, 1.3, 0.28, 0.85};  // guess near the cold Muff bias
        const Sys4 sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                       0.0,  0.0, 0.0,  // caps OPEN at DC
                       0.0,  0.0, 0.0, 0.0,
                       1.0,  gRbg};
        dampedNewton(sys, x, 400, 0.0);
        vbQ_ = x[0]; vcQ_ = x[1]; veQ_ = x[2]; vdQ_ = x[3];
    } else {
        double x[3] = {0.85, 1.3, 0.28};  // guess near the cold Muff bias
        const Sys sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                      0.0,  0.0,  // caps OPEN at DC
                      0.0,  0.0, 0.0,
                      1.0,  gRbg};
        dampedNewton(sys, x, 400, 0.0);
        vbQ_ = x[0]; vcQ_ = x[1]; veQ_ = x[2];
        vdQ_ = x[0];  // inert; parked on the base so it is never garbage
    }
    icQ_ = ebersMollIc(vbQ_ - veQ_, vbQ_ - vcQ_, cfg_.bjt);
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
    vcQ_ = vc_; vbQ_ = vb_; veQ_ = ve_; vdQ_ = vd_;
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
    vdPark_ = vd_;
    vCinPark_ = vCin_;
    vCfPark_ = vCf_;
    vCdPark_ = vCd_;
}

// EVERY recursive state in this class belongs here (audit finding 1's rule), which
// as of §53 includes the DC-blocked branch's node warm start and its cap history —
// clipper_nan_guard_tests block C is what fails if one is forgotten.
void BjtStage::reset() {
    vb_ = vbPark_;
    vc_ = vcPark_;
    ve_ = vePark_;
    vd_ = vdPark_;
    vCin_ = vCinPark_;
    vCf_ = vCfPark_;
    vCd_ = vCdPark_;
    lastMaxIters_ = 0;
}

float BjtStage::processSample(float vinF) {
    const double vin = static_cast<double>(vinF);
    const double kSer = 1.0 / (1.0 + gCin_ * cfg_.Rs);  // §49; Rs = 0 -> 1.0 exactly
    const double gRbg = cfg_.Rbg > 0.0 ? 1.0 / cfg_.Rbg : 0.0;
    const double tol = kNewtonResidualTolA * tubeSolverTolScale();

    double Vb, Vc;
    if (usesDcBlockedDiodes()) {
        // 4-node solve (docs §53). Same companion discipline as the 3-node path;
        // the extra history is the DC-block cap's, vCd = Vc − Vd.
        const Sys4 sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                       gCin_, gCf_, gCd_,
                       vin, gCin_ * vCin_, gCf_ * vCf_, gCd_ * vCd_,
                       kSer, gRbg};
        double x[4] = {vb_, vc_, ve_, vd_};  // warm start
        const int iters = dampedNewton(sys, x, kMaxNewtonIter, tol);
        lastMaxIters_ = std::max(lastMaxIters_, iters);
        Vb = x[0]; Vc = x[1];
        vb_ = x[0]; vc_ = x[1]; ve_ = x[2]; vd_ = x[3];
        // vCd_ rests at the FULL collector-base bias (measured 4.56 V on the Muff's
        // clip stages at the shipped 4x rate) — it can never be subnormal, so per
        // the ADR 006 scope rule it gets this comment rather than a flushDenormal.
        vCd_ = Vc - x[3];
    } else {
        // Companion history currents (constant across this sample's iterations).
        const Sys sys{cfg_, 1.0 / cfg_.Rc, 1.0 / cfg_.Rf, 1.0 / cfg_.Re,
                      gCin_, gCf_, vin, gCin_ * vCin_, gCf_ * vCf_,
                      kSer, gRbg};
        double x[3] = {vb_, vc_, ve_};  // warm start
        const int iters = dampedNewton(sys, x, kMaxNewtonIter, tol);
        lastMaxIters_ = std::max(lastMaxIters_, iters);
        Vb = x[0]; Vc = x[1];
        vb_ = x[0]; vc_ = x[1]; ve_ = x[2];  // warm start for the next sample
    }
    // Series-Rs backward-Euler companion: vC += kSer*(vin - Vb - vC). Rs = 0 gives
    // exactly the stock update vCin_ = vin - Vb (§49; verified bit-identical).
    vCin_ = vCin_ + kSer * (vin - Vb - vCin_);  // update companion histories
    vCf_ = Vc - Vb;

    return static_cast<float>(Vc - vcQ_);  // collector AC (inverting output)
}

}  // namespace clipper::dsp
