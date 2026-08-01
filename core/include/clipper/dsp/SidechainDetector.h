// Clipper — portable DSP core.
//
// SidechainDetector — the SHARED full-wave peak detector + envelope integrator.
//
// ============================================================================
//  WHY THIS FILE EXISTS (docs §61.2, and a correction to ADR 019)
// ============================================================================
// M13.1 (docs §59) built this detector inside `CompressorEngine` and ADR 019
// wrote down a seam saying the noise gate (M13.6) would be "this same
// `Sidechain` feeding a different `ControlMap`". When M13.6 came to use it, the
// CLAIM was right and the SEAM was not: `Sidechain` was a config struct with no
// code attached to it, `detectorLeg()`/`advanceEnvelope()` were PRIVATE methods
// of `CompressorEngine`, and their state lived in that class's members. There
// was no object a second consumer could hold. The fix is this file — the
// detector extracted, verbatim and bit-identically, into a component both
// pedals own — NOT a widening of `CompressorEngine` until a gate fits inside a
// compressor. See docs §61.2 for the full finding and ADR 019's amendment.
//
// ============================================================================
//  THE CIRCUIT
// ============================================================================
// Per leg: a coupling cap into a node with a resistor to ground, a diode that
// clamps the NEGATIVE excursion (anode grounded), and the base-emitter junction
// of a grounded-emitter transistor whose collector discharges the envelope cap.
// The clamp makes it a DC RESTORER, so what the transistor sees is the swing
// referred to one diode drop below ground — which is why the detector has an
// intrinsic threshold even when nothing calls it one: the threshold IS a Vbe.
//
//   drive --[C]--+--[R]--gnd        envelope node: C_env with R_env to +V
//                |                       ^
//                +--|<|-- gnd            | collector current DISCHARGES it
//                |                       |
//                +--- base of Q ---------+
//
// BOTH legs are driven (one with +drive, one with -drive), which is what makes
// the rectifier full-wave. The two legs may sit behind DIFFERENT source
// impedances — the Dyna Comp's do (an emitter follower vs a 10 k collector
// load), a gate's op-amp rectifier's do not — so that is a config field.
//
// The envelope node RESTS HIGH (at the supply) and signal pulls it DOWN. Both
// consumers depend on that sign: the compressor turns a low node into a low
// bias current, the gate turns a low node into "loud enough, open".
//
// ============================================================================
//  THIS IS A *THRESHOLD* DETECTOR — and the wah is deliberately NOT a consumer
//  (docs §58.8, ADR 023)
// ============================================================================
// Say this before someone reaches for it as a general "envelope follower". The
// clamp is a DC restorer into a base-emitter junction: conduction is
// EXPONENTIAL, and a conducting 2N3904's collector current is orders of
// magnitude larger than the envelope resistor's pull-up. So the transfer from
// input level to envelope is STEEP. Measured open loop, as the input dB range
// over which the node travels from 10 % to 90 % of the rail:
//
//     M13.1 compressor (10 uF / 150 k) ......  1.950 dB
//     M13.6a gate      (47 nF / 220 k) ......  2.384 dB
//
// That is a switch, and BOTH consumers want one. The compressor's graded
// response is its FEEDBACK LOOP, not this block (docs §59: 216:1 feed-back vs
// 3.3:1 feed-forward); the gate feeds it to a comparator and §61.4 builds a
// 40 dB dB-linear THRESHOLD on top of exactly that steepness.
//
// §58's wah needs the opposite: a follower that is PROPORTIONAL to pick
// strength, open loop (a wah has no gain to reduce, so it cannot borrow the
// loop), with no threshold at all — its one-pole on |x| measures 19.085 dB of
// proportional range. The substitution WAS built and measured rather than
// argued: a substituted wah moves 0.000 octaves on a 0.10 V pick, its full-SENSE
// sweep drops 1.534 -> 0.958 octaves (failing §58.6's own bar), its
// time-to-peak stops being level-independent, and its `maxAbsRestingState()`
// goes from exactly 0.0 to 2.675e-13 — because this node rests at a NONZERO
// operating point by design, which is the opposite side of ADR 006's scope rule
// from a wah envelope that rests at zero. Full table: docs §58.8.
//
// **Do not add a `rectifierKind` / `idealRectifier` / `asymmetricAttack` field
// to make a wah fit.** That is the union-of-N-models ADR 021 forbids in terms.
//
// ============================================================================
//  M13.3 RAN THE SUBSTITUTION TOO, AND ALSO REFUSED (docs §64.3, ADR 025)
// ============================================================================
// The rule above said the next consumer's test was procedural, not a veto: build
// the substitution and measure it. M13.3's optical compressor did, and the answer
// is the same as the wah's, for a different reason.
//
// **That pedal has no envelope capacitor.** Its detector is an electroluminescent
// panel lighting a CdS photocell: the panel emits on BOTH polarities, so the
// PANEL is the rectifier and the CELL — with a two-stage, trap-retarded
// recovery — is the integrator. There is no component in the reference circuit
// for this block to be.
//
// Measured on a validated replica of that pedal's loop with one block swapped
// (agrees with the shipped model to 0.05 dB before the swap): proportional range
// 2.031 dB here against **14.323 dB** for the panel-and-cell; the ratio curve
// goes non-monotone and reaches **10.40:1**; gain reduction at −30 dBV collapses
// to **0.09 dB**; and that voice's whole acceptance property — a program-dependent
// release, 2.892x — goes to **1.000x**, which is M13.1's own figure, because an RC
// is an RC.
//
// So this component still has exactly TWO consumers, the compressor and the gate,
// and it was not widened for a third. The photocell lives in its own component
// (`OptoCell.h`).
//
// Platform-free (C++17, no OS/browser/Emscripten). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_SIDECHAIN_DETECTOR_H
#define CLIPPER_DSP_SIDECHAIN_DETECTOR_H

#include <cmath>

namespace clipper::dsp {

// --- Device models -----------------------------------------------------------
//
// One NPN card and one diode card, both quoted verbatim from a published SPICE
// model, so nothing here is fitted. The Gummel-Poon low-current term (ISE/NE) is
// modelled DELIBERATELY: the detector transistors' base current is what loads
// the clamp network, and their hFE at ~0.2 mA is what sets the attack. With the
// ideal `betaF` alone a 2N3904 would report hFE = 416; with the ISE term the
// same card yields hFE ≈ 106 at 0.2 mA — the datasheet's own figure, for free.
struct NpnDevice {
    double Is = 6.734e-15;    // ON Semi 2N3904 SPICE: IS
    double betaF = 416.4;     // ON Semi 2N3904 SPICE: BF (the IDEAL-region beta)
    double Ise = 6.734e-15;   // ON Semi 2N3904 SPICE: ISE (B-E leakage)
    double Ne = 1.259;        // ON Semi 2N3904 SPICE: NE
    double Vt = 0.02585;      // kT/q at 300 K
};

// 1N4148, the SAME card the RAT's clipper uses after docs §36 fixed the ideality
// factor. Do not drop n back to 1.0 here either.
struct DiodeDevice {
    double Is = 2.52e-9;      // 1N4148 SPICE: IS
    double nVt = 0.0452892;   // n·Vt with n = 1.752 (docs §36), Vt = 25.85 mV
};

// The detector's component values. Everything here is a part on a schematic.
struct Sidechain {
    double clampCapF;     // the per-leg coupling / DC-restorer cap
    double clampResOhms;  // the per-leg node resistor to ground
    double envCapF;       // the envelope cap — the "squish" cap on a compressor
    double envResOhms;    // pulls the envelope back up (this is the RELEASE)
    double supplyV;       // the rail the envelope resistor pulls to
    double vceSatV;       // where the discharge transistors bottom out
    // The two legs are not necessarily driven from equal impedances. The Dyna
    // Comp's are not (emitter follower ~1.4 kΩ vs a 10 kΩ collector load), which
    // limits how much base current the collector leg can deliver on a peak, so
    // the two halves of the "full-wave" rectifier do not contribute equally.
    // A gate's op-amp rectifier drives both legs from the same impedance.
    // Modelled rather than averaged away.
    double srcEmitterOhms;
    double srcCollectorOhms;
};

// --- Device helpers ----------------------------------------------------------
//
// Ebers-Moll forward collector current and the Gummel-Poon base current with the
// low-current (ISE/NE) term. The ISE term is why nothing here carries a
// hand-picked hFE: the published card reproduces the datasheet's own ~106 at
// 0.2 mA on its own.

inline double npnIc(double vbe, const NpnDevice& d) {
    return d.Is * (std::exp(vbe / d.Vt) - 1.0);
}

inline double npnIb(double vbe, const NpnDevice& d) {
    return (d.Is / d.betaF) * (std::exp(vbe / d.Vt) - 1.0) +
           d.Ise * (std::exp(vbe / (d.Ne * d.Vt)) - 1.0);
}

// d(Ib)/d(vbe). ONLY the base current appears in the clamp node's KCL — the
// collector current flows from the envelope node to the emitter and never
// touches the base. Getting that wrong makes the node ~beta times too stiff,
// which starves the discharge and turns a 5 ms attack into half a second
// (docs §59.8 — the one real bug M13.1 made).
inline double npnBaseCond(double vbe, const NpnDevice& d) {
    const double e1 = std::exp(vbe / d.Vt);
    const double e2 = std::exp(vbe / (d.Ne * d.Vt));
    return (d.Is / d.betaF) * e1 / d.Vt + d.Ise * e2 / (d.Ne * d.Vt);
}

// The clamp diode: anode at GROUND, cathode at the node. Positive return value
// means current flowing INTO the node (which is what pulls a negative excursion
// back up toward one diode drop below ground).
inline double clampDiodeIn(double vNode, const DiodeDevice& d) {
    return d.Is * (std::exp(-vNode / d.nVt) - 1.0);
}

inline double clampDiodeCond(double vNode, const DiodeDevice& d) {
    return d.Is * std::exp(-vNode / d.nVt) / d.nVt;
}

// --- The detector ------------------------------------------------------------

class SidechainDetector {
public:
    // Configure from component values. Call before setRate()/reset().
    void configure(const Sidechain& det, const NpnDevice& npn,
                   const DiodeDevice& diode) {
        det_ = det;
        npn_ = npn;
        diode_ = diode;
    }

    const Sidechain& config() const { return det_; }

    // The rate the detector is integrated at (the OVERSAMPLED rate, because both
    // consumers run it inside the same oversampling domain as their nonlinearity).
    void setRate(double rate) { rate_ = rate > 0.0 ? rate : 44100.0; }
    double rate() const { return rate_; }

    // Clear the clamp network and re-park the envelope node at the CACHED
    // operating point. Never solves — prepare() is not a recovery path.
    void reset(double vEnvPark) {
        clampP_ = clampN_ = 0.0;
        vbP_ = vbN_ = 0.0;
        vEnv_ = vEnvPark;
    }

    // Clear only the rate-dependent state (used when the oversampling factor
    // changes and the stored history belonged to another rate).
    void clearRateState() {
        clampP_ = clampN_ = 0.0;
        vbP_ = vbN_ = 0.0;
    }

    // ONE detector step: both legs of the full-wave rectifier, driven anti-phase
    // by `drive`. Returns the TOTAL collector current available to discharge the
    // envelope cap (amps).
    double detect(double drive) {
        const double icP = leg(drive, clampP_, vbP_, det_.srcEmitterOhms);
        const double icN = leg(-drive, clampN_, vbN_, det_.srcCollectorOhms);
        return icP + icN;
    }

    // Advance the envelope cap by one sample. `extraLoadAmps` is any additional
    // steady load on the node that belongs to the CONSUMER rather than the
    // detector — the compressor's Q5 base current; zero for the gate, whose
    // comparator is an op-amp input.
    void advanceEnvelope(double dischargeAmps, double extraLoadAmps) {
        // Simplified saturation of the discharge transistors: their collector
        // current collapses as Vce approaches zero. The exact statement is
        // Ebers-Moll's reverse term; this smooth, monotone, bounded stand-in only
        // ever acts when the envelope node is already bottomed (docs §59.6).
        const double vce = vEnv_ > 0.0 ? vEnv_ : 0.0;
        const double sat = vce / (vce + det_.vceSatV);

        const double h = 1.0 / (rate_ * det_.envCapF);
        const double num = vEnv_ + h * (det_.supplyV / det_.envResOhms -
                                        dischargeAmps * sat - extraLoadAmps);
        const double den = 1.0 + h / det_.envResOhms;
        double v = num / den;
        if (v < 0.0) v = 0.0;
        if (v > det_.supplyV) v = det_.supplyV;
        // NO flushDenormal: this node rests at a nonzero DC operating point (the
        // supply rail, or one base current below it) and can never be subnormal.
        // ADR 006's scope rule — a guard here is unreachable code in a hot loop.
        vEnv_ = v;
    }

    double envelopeVolts() const { return vEnv_; }
    void setEnvelopeVolts(double v) { vEnv_ = v; }

    // The largest magnitude held by the clamp network. NOT part of any
    // `maxAbsRestingState()` — see the note in leg(): these float at the SOLVER's
    // residual tolerance (~1e-11 V), not at zero. Exposed so a test can assert
    // that measured floor rather than assume it.
    double maxAbsClampState() const {
        double m = std::fabs(clampP_);
        if (std::fabs(clampN_) > m) m = std::fabs(clampN_);
        if (std::fabs(vbP_) > m) m = std::fabs(vbP_);
        if (std::fabs(vbN_) > m) m = std::fabs(vbN_);
        return m;
    }

    // Newton budget for one leg. The solve is MONOTONE (see below), so this is a
    // guard rail, not a working iteration count: the shipped 4x / 48 kHz path
    // measures 2.0 evaluations per solve at idle and 4 at a hard transient.
    static constexpr int kMaxNewtonIter = 24;
    // Residual tolerance as a CURRENT (amps). The node's own resistive leg
    // carries ~2.5e-7 A at a typical excursion, so 1e-14 A is seven decades
    // inside it — and docs §34's finding is that a solver with NO residual exit
    // burns its whole budget at the parked operating point for nothing.
    static constexpr double kNewtonResidualTolA = 1e-14;
    // Largest Newton step (volts). One exponential overshoot on the first
    // iteration is otherwise enough to push exp() to infinity.
    static constexpr double kNewtonMaxStepV = 0.25;

private:
    // One sidechain leg: solves the clamp node, advances the coupling cap, and
    // returns the collector current that discharges the envelope cap.
    //
    // The node solve is EXACT, not a lookup. Eliminating the coupling cap's
    // charge leaves ONE scalar unknown per leg:
    //
    //     g(vb) = vb − drive + qPrev + (Rsrc + T/C)·f(vb) = 0
    //     f(vb) = vb/R + Ib(vb) + Ic(vb) − Idiode(vb)
    //
    // f' > 0 everywhere, so g' = 1 + (Rsrc + T/C)·f' > 0 and g is STRICTLY
    // MONOTONE: Newton cannot rotate off a descent direction the way §53 found
    // `BjtStage`'s clamped step doing. The step limiter is a guard against the
    // first exponential overshoot only.
    double leg(double drive, double& capState, double& vbWarm,
               double srcOhms) const {
        const double tOverC = 1.0 / (rate_ * det_.clampCapF);
        const double k = srcOhms + tOverC;
        const double invR = 1.0 / det_.clampResOhms;

        double vb = vbWarm;
        double f = 0.0;
        for (int it = 0; it < kMaxNewtonIter; ++it) {
            f = vb * invR + npnIb(vb, npn_) - clampDiodeIn(vb, diode_);
            const double g = vb - drive + capState + k * f;
            // Residual expressed as a CURRENT so the tolerance means something.
            if (std::fabs(g) / k < kNewtonResidualTolA) break;
            const double fp =
                invR + npnBaseCond(vb, npn_) + clampDiodeCond(vb, diode_);
            double step = g / (1.0 + k * fp);
            if (step > kNewtonMaxStepV) step = kNewtonMaxStepV;
            if (step < -kNewtonMaxStepV) step = -kNewtonMaxStepV;
            vb -= step;
        }
        // Recompute the branch current at the converged node so the cap advance
        // agrees with the same vb.
        f = vb * invR + npnIb(vb, npn_) - clampDiodeIn(vb, diode_);
        // NO flushDenormal on either of these, and that is measured rather than
        // assumed (ADR 006's "measure which, don't guess"). The node's physical
        // equilibrium against the clamp diode's reverse saturation current is
        // -4.90e-272 V, which IS below the flush floor — but the SOLVER cannot
        // resolve it: Newton exits at kNewtonResidualTolA, so vb floors at
        // kNewtonResidualTolA * (Rsrc + T/C) and the cap settles inside that.
        // That floor is ~300 decades above the subnormal range, so a guard here
        // can never fire — it would be unreachable code in the hottest loop in
        // the file, exactly what docs §33 found was the wrong thing to ship.
        capState = capState + tOverC * f;
        vbWarm = vb;

        const double ic = npnIc(vb, npn_);
        return ic > 0.0 ? ic : 0.0;
    }

    Sidechain det_{};
    NpnDevice npn_{};
    DiodeDevice diode_{};
    double rate_ = 176400.0;

    // Clamp caps and the Newton warm starts for the two clamp nodes.
    double clampP_ = 0.0, clampN_ = 0.0;
    double vbP_ = 0.0, vbN_ = 0.0;
    // The envelope node. Rests at a NONZERO operating point — no flush.
    double vEnv_ = 9.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_SIDECHAIN_DETECTOR_H
