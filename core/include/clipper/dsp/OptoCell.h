// Clipper — portable DSP core.
//
// OptoCell — an ELECTRO-OPTICAL ATTENUATOR CELL: an electroluminescent panel
// lighting a cadmium-sulfide photoresistor, i.e. the active half of a
// Teletronix-T4B-style module. M13.3's optical compressor (`OptoModel`) is its
// first consumer.
//
// ============================================================================
//  WHY THIS IS A COMPONENT AND NOT A STRUCT OF NUMBERS (ADR 021, ADR 025)
// ============================================================================
// ADR 021's finding was "a config struct is not a seam — extract the block".
// This file is that lesson applied on the way in rather than after the fact: the
// cell owns its own state and behaviour and is held BY VALUE, so a second
// consumer can hold one without anybody widening anybody.
//
// NAMED FUTURE CONSUMER: ROADMAP **M13.5, the Uni-Vibe** — four MISMATCHED
// photocell-driven phase stages with lamp thermal lag. That is the same device
// with a different light source (an incandescent lamp, whose own thermal lag
// belongs to the LAMP and not here) and different component values. Do not grow
// this class a `lightSourceKind` axis to cover it: the EL law lives in
// `elExponent`, and a lamp's drive law is a different function of a different
// quantity. Extract THAT if and when it arrives.
//
// ============================================================================
//  THE DEVICE — and the honest split between what is sourced and what is not
// ============================================================================
// Full source table in docs §64.1. In brief:
//
//   SOURCED  the module is an EL panel plus two Clairex CL-505L CdS cells (one
//            for gain reduction, one for the meter — only the GR cell is
//            modelled); the panel is driven by the amplified audio, stepped up
//            to no more than ~90 V peak; the cell's resistance runs from under
//            1 kΩ lit to over 1 MΩ dark; CdS resistance follows illumination as
//            a LOG-LOG power law R ∝ E^−γ with γ ≈ 0.58 (bright) to 0.92 (dim);
//            EL brightness rises "in proportion to the second to the third
//            power of the voltage"; the unit's published attack is 10 ms and its
//            release is "0.06 s for 50 % release, 0.5 to 5 s for complete
//            release DEPENDING UPON THE AMOUNT OF PREVIOUS REDUCTION".
//
//   THIS     the internal split into a fast and a slow conductance branch, the
//   MODEL'S  trap-occupancy state that lengthens the slow branch's release, and
//   OWN      every time constant in `OptoCellSpec`. No published dynamic curve
//            for a T4B was reachable (docs §64.1 lists the hosts that 403'd), so
//            the STRUCTURE comes from photoconductor physics and the CONSTANTS
//            are pinned to the published behavioural spec above. They are not
//            fitted to a sound and must not be re-tuned toward one.
//
// ============================================================================
//  THE MODEL
// ============================================================================
// Steady state. Light output L ∝ |v_panel|^n (n = the EL exponent); excess cell
// conductance ΔG ∝ L^γ. Composing them, ΔG_ss = K·|v_panel|^(n·γ), and K is
// DERIVED (not chosen) from the two published endpoints — the cell reaches
// `rLightOhms` at `panelMaxV`:
//
//     p  = elExponent · cellGamma
//     K  = (1/rLight − 1/rDark) / panelMaxV^p
//     G  = 1/rDark + ΔG          R_cell = 1/G
//
// **The panel is the rectifier.** An EL panel emits on BOTH polarities of its
// drive, so |v| is the physics and not a modelling convenience — which is why
// this compressor has no rectifier and no envelope capacitor anywhere in its
// control path. The integrator is the CELL.
//
// Dynamics. The excess conductance is carried by TWO branches:
//
//     ΔG = g_fast + g_slow          g_fast → a·ΔG_ss, g_slow → (1−a)·ΔG_ss
//
// both rising with `tauAttackSeconds` (the published 10 ms attack is the whole
// unit's, so it is one constant), and falling with two very different time
// constants — which is the published "two-stage release" as a mechanism rather
// than as an envelope shape.
//
// PROGRAM DEPENDENCE, the property this whole pedal exists for, is a third
// state: a TRAP OCCUPANCY `m` that fills while the cell is lit and empties
// slowly afterwards, and whose level MULTIPLIES the slow branch's release time
// constant:
//
//     tau_slow_release = tauSlowReleaseSeconds · (1 + (slowReleaseMemMult − 1)·m)
//
// Its target is the standard trap-kinetics (Langmuir) form rather than a linear
// normalization, and that detail is load-bearing rather than cosmetic. Trap
// filling obeys `dn/dt = c·(N − n)·n_free − r·n` with the free-carrier
// population proportional to the light, so the steady occupancy SATURATES:
//
//     m_ss = ΔG_ss / (ΔG_ss + ΔG_half)
//
// where `trapHalfFraction` fixes ΔG_half as a fraction of the cell's own light
// limit. A linear `ΔG/ΔG_max` was the first thing written here and it does not
// work: the cell's limit corresponds to the full 90 V panel drive, so at any
// playable level the occupancy sits at ~1e-3 and the release is not program
// dependent at all — measured at **1.083×** where the shipped form measures the
// figure in docs §64.4. Reported rather than quietly fixed, because it is the
// difference between modelling the mechanism and decorating it.
//
// This gives BOTH dependences the reference is described by. The published spec
// is a DEPTH statement — "0.5 to 5 seconds for complete release depending upon
// the amount of previous reduction" — and that falls out of `m_ss` rising with
// the light. The DURATION dependence (a stab recovers faster than a held chord
// of the same depth) falls out of `tauMemChargeSeconds` being of the order of a
// second, which is the CdS "light history" effect. A single RC detector cannot
// have either at any coefficient, which is why this is the voice's acceptance
// bar (docs §64.4).
//
// DENORMALS (ADR 006, scope by measurement). All three states — `gFast_`,
// `gSlow_` and `mem_` — rest at EXACTLY ZERO in the dark, so all three are
// guarded and all three are in `maxAbsRestingState()`. Note that this puts the
// cell on the OPPOSITE side of the scope rule from `SidechainDetector`, whose
// envelope node rests at a supply rail; that is one of the several reasons the
// two are not the same block (docs §64.3). Each branch is FIRST ORDER, so the
// house one-liner is the correct form here — §56.4b's warning is about direct
// forms of order >= 2.
//
// Platform-free (C++17, no OS/browser/Emscripten).

#ifndef CLIPPER_DSP_OPTO_CELL_H
#define CLIPPER_DSP_OPTO_CELL_H

#include <cmath>

#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

// Every field is either a published device figure or a constant named in docs
// §64.1's RECONSTRUCTED column. Nothing here is fitted to a sound.
struct OptoCellSpec {
    // --- Steady state (all four pinned to published figures) ----------------
    double rDarkOhms = 10.0e6;    // unlit resistance
    double rLightOhms = 1.0e3;    // resistance at `panelMaxV`
    double panelMaxV = 90.0;      // the panel drive the light figure refers to
    double elExponent = 2.5;      // n: EL brightness ∝ |V|^n (published 2…3)
    double cellGamma = 0.75;      // γ: CdS R ∝ E^−γ (published 0.58…0.92)

    // --- Dynamics (structure from physics, constants pinned to the spec) ----
    double tauAttackSeconds = 0.010;      // the published 10 ms attack
    double fastShare = 0.70;              // a — the fast branch's share of ΔG
    double tauFastReleaseSeconds = 0.020; // gives the published ~60 ms to 50 %
    double tauSlowReleaseSeconds = 0.18;  // with no light history
    double slowReleaseMemMult = 15.0;     // × that, at full trap occupancy
    double trapHalfFraction = 0.01;       // ΔG_half, as a fraction of the limit
    double tauMemChargeSeconds = 1.2;     // how fast the traps fill
    double tauMemDischargeSeconds = 2.0;  // and empty
};

class OptoCell {
public:
    // Configure from device figures. Call before setRate()/reset().
    void configure(const OptoCellSpec& s) {
        spec_ = s;
        gDark_ = 1.0 / spec_.rDarkOhms;
        gExcessMax_ = 1.0 / spec_.rLightOhms - gDark_;
        if (gExcessMax_ < 0.0) gExcessMax_ = 0.0;
        exponent_ = spec_.elExponent * spec_.cellGamma;
        // K is DERIVED from the two published endpoints, never chosen.
        k_ = gExcessMax_ / std::pow(spec_.panelMaxV, exponent_);
        gHalf_ = spec_.trapHalfFraction * gExcessMax_;
        if (gHalf_ <= 0.0) gHalf_ = 1e-30;
        rebuild();
    }

    const OptoCellSpec& spec() const { return spec_; }

    void setRate(double rate) {
        rate_ = rate > 0.0 ? rate : 44100.0;
        rebuild();
    }
    double rate() const { return rate_; }

    // Park the cell DARK. Never solves — there is nothing to solve; a cell with
    // no light on it is at its dark resistance by definition.
    void reset() {
        gFast_ = 0.0;
        gSlow_ = 0.0;
        mem_ = 0.0;
    }

    // One sample. `vPanel` is the instantaneous panel drive in volts (either
    // polarity — the panel emits on both). Returns the cell's resistance in ohms.
    double process(double vPanel) {
        const double av = vPanel < 0.0 ? -vPanel : vPanel;
        // The steady-state excess conductance this much light would reach.
        double gT = av > 0.0 ? k_ * std::pow(av, exponent_) : 0.0;
        if (gT > gExcessMax_) gT = gExcessMax_;

        const double tFast = spec_.fastShare * gT;
        const double tSlow = (1.0 - spec_.fastShare) * gT;

        gFast_ += (tFast > gFast_ ? attackCoef_ : fastRelCoef_) * (tFast - gFast_);
        // The slow branch's RELEASE is the program-dependent one: its time
        // constant is stretched by the trap occupancy. Computed per sample
        // because `mem_` moves per sample; the exp is the price of the property.
        double slowCoef;
        if (tSlow > gSlow_) {
            slowCoef = attackCoef_;
        } else {
            const double tau = spec_.tauSlowReleaseSeconds *
                               (1.0 + (spec_.slowReleaseMemMult - 1.0) * mem_);
            slowCoef = 1.0 - std::exp(-1.0 / (rate_ * tau));
        }
        gSlow_ += slowCoef * (tSlow - gSlow_);

        // Trap occupancy: the SATURATING trap-kinetics target, not a linear
        // normalization (see the banner — the linear form measures 1.083x of
        // program dependence, i.e. none).
        const double u = gT / (gT + gHalf_);
        mem_ += (u > mem_ ? memChargeCoef_ : memDischargeCoef_) * (u - mem_);

        gFast_ = flushDenormal(gFast_);
        gSlow_ = flushDenormal(gSlow_);
        mem_ = flushDenormal(mem_);

        return 1.0 / (gDark_ + gFast_ + gSlow_);
    }

    // --- Measurement hooks (not user-facing) --------------------------------
    double resistanceOhms() const { return 1.0 / (gDark_ + gFast_ + gSlow_); }
    double excessConductance() const { return gFast_ + gSlow_; }
    double memory() const { return mem_; }
    // The composite exponent p = n·γ. `OptoModel`'s ratio prediction is built on
    // this, so a test can quote the device rather than re-typing the arithmetic.
    double compositeExponent() const { return exponent_; }
    // Every state in this class rests at EXACTLY zero (a dark cell), so this
    // covers all of them and the denormal suite asserts it is 0.0.
    double maxAbsRestingState() const {
        double m = std::fabs(gFast_);
        if (std::fabs(gSlow_) > m) m = std::fabs(gSlow_);
        if (std::fabs(mem_) > m) m = std::fabs(mem_);
        return m;
    }

private:
    void rebuild() {
        attackCoef_ = coefFor(spec_.tauAttackSeconds);
        fastRelCoef_ = coefFor(spec_.tauFastReleaseSeconds);
        memChargeCoef_ = coefFor(spec_.tauMemChargeSeconds);
        memDischargeCoef_ = coefFor(spec_.tauMemDischargeSeconds);
    }
    double coefFor(double tau) const {
        if (tau <= 0.0) return 1.0;
        return 1.0 - std::exp(-1.0 / (rate_ * tau));
    }

    OptoCellSpec spec_{};
    double rate_ = 48000.0;
    double gDark_ = 1.0e-7;
    double gExcessMax_ = 1.0e-3;
    double exponent_ = 1.875;
    double k_ = 0.0;
    double gHalf_ = 1e-5;  // the trap-kinetics half-occupancy conductance
    double attackCoef_ = 0.0, fastRelCoef_ = 0.0;
    double memChargeCoef_ = 0.0, memDischargeCoef_ = 0.0;

    // All three rest at EXACTLY zero (a dark cell) — guarded, and covered by
    // maxAbsRestingState(). ADR 006's scope rule, on the opposite side from
    // SidechainDetector's rail-resting envelope node.
    double gFast_ = 0.0, gSlow_ = 0.0, mem_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OPTO_CELL_H
