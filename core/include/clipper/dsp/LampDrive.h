// Clipper — portable DSP core.
//
// LampDrive — an INCANDESCENT FILAMENT LAMP as a light source: a driven tungsten
// filament with real thermal mass, so its brightness LAGS its drive and lags it
// ASYMMETRICALLY (heating is forced, cooling is not). M13.5's Uni-Vibe
// (`VibeModel`) is its first consumer.
//
// ============================================================================
//  WHY THIS EXISTS AT ALL — `OptoCell.h` asked for it by name
// ============================================================================
// `OptoCell` models an electro-optical attenuator cell and its header carries
// this instruction in terms:
//
//     "Do not grow this class a `lightSourceKind` axis to cover it: the EL law
//      lives in `elExponent`, and a lamp's drive law is a different function of
//      a different quantity. Extract THAT if and when it arrives."
//
// It has arrived. This file is that extraction, and `OptoCell` gets **zero** new
// parameters as a result (see the seam note below).
//
// ============================================================================
//  THE SEAM — and why it needs no change on the other side
// ============================================================================
// `OptoCell` maps a DRIVE quantity to light by a power law (`elExponent`) and
// light to excess conductance by the CdS gamma (`cellGamma`). The EL panel it was
// written for supplies the first law itself, so its drive quantity is a VOLTAGE.
// A lamp does not: its light is a function of FILAMENT TEMPERATURE, which is a
// state, not an instantaneous function of the drive at all.
//
// So the split is exactly where `OptoCell.h` predicted:
//
//     drive volts ──► [ LampDrive ] ──► normalized LIGHT ──► [ OptoCell ] ──► ohms
//                      (this file:            (0..1)          (elExponent = 1,
//                       thermal state,                         i.e. "the quantity
//                       T^m emission)                          I hand you IS the
//                                                              light")
//
// `elExponent = 1.0` is not a widening and not a trick: it is the existing,
// published-source field used with a DEGENERATE exponent, which is what "the
// source law lives in the source" means arithmetically. `panelMaxV` correspondingly
// reads as "the light level at which the cell reaches `rLightOhms`" and is set to
// 1.0. Both are ordinary `OptoCellSpec` values. Nothing on that side moved.
//
// ============================================================================
//  THE PHYSICS — textbook incandescence, and the honest source split
// ============================================================================
// SOURCED for the Uni-Vibe (docs §67.1, search-extract grade — no schematic was
// reachable): the light source is a MINIATURE INCANDESCENT LAMP sharing a
// reflective box with four LDRs. That is the whole of what is sourced about it.
//
// THIS MODEL'S OWN: everything below. The STRUCTURE is standard filament
// thermodynamics and is not invented for this pedal —
//
//   * a lumped heat capacity C holding the filament's excess energy;
//   * electrical heating P = v²/R(T) with tungsten's POSITIVE temperature
//     coefficient R(T) ∝ T^ρ, ρ ≈ 1.2 over the incandescent range — this is why
//     a cold filament draws an inrush and why turn-on is fast;
//   * radiative cooling P = k(T⁴ − T_amb⁴), Stefan–Boltzmann — this is why
//     turn-off is slow, and why it gets slower the cooler the filament already is;
//   * luminous output in the cell's own sensitivity band rising far faster than
//     the total radiated power, L ∝ T^m with m well above 4.
//
// — but every CONSTANT (`operatingK`, `thermalTauSeconds`, `brightnessExponent`,
// `resistanceExponent`) is a stated reconstruction. §57's rule applies verbatim:
// **do not re-tune any of them toward a sound; find the schematic.**
//
// THE ASYMMETRY IS DERIVED, NOT SET. There is no "rise time" and no "fall time"
// field here, deliberately: the two come out of the same two terms. Heating is
// whatever the drive can force through a resistance that FALLS as the filament
// cools; cooling is only what the T⁴ law removes, and it collapses as T drops. A
// model with a `riseTau`/`fallTau` pair would let a later slice tune the asymmetry
// directly, which is precisely the fitting this project forbids.
//
// `k` and `C` are both DERIVED rather than chosen:
//
//     k = nominalVolts² / (operatingK⁴ − ambientK⁴)      (steady state at nominal)
//     C = thermalTauSeconds · |d(net power)/dT| at that operating point
//
// so `thermalTauSeconds` names the SMALL-SIGNAL time constant at the operating
// point — one honest number — and the large-signal asymmetry falls out of the
// nonlinearities rather than being typed in beside it.
//
// ============================================================================
//  DENORMALS (ADR 006, scope by measurement)
// ============================================================================
// `tempK_` rests at a REAL OPERATING POINT — a Uni-Vibe's lamp is always lit, and
// even unpowered the filament rests at `ambientK` (300), never at zero. It is
// therefore deliberately NOT denormal-guarded and there is no
// `maxAbsRestingState()` on this class: an assertion here would have no teeth and
// a flush would be unreachable code. Note that this puts the LAMP on the opposite
// side of the scope rule from `OptoCell` — and, in this pedal, drags the CELL
// across with it (docs §67.8), because a continuously-lit cell does not rest at
// zero either.
//
// Platform-free (C++17, no OS/browser/Emscripten). Deterministic and
// allocation-free in `process()`.

#ifndef CLIPPER_DSP_LAMP_DRIVE_H
#define CLIPPER_DSP_LAMP_DRIVE_H

#include <cmath>

namespace clipper::dsp {

// Every field is a stated reconstruction (docs §67.1). Nothing here is fitted to
// a sound, and there is deliberately no rise/fall time pair — see the banner.
struct LampSpec {
    double ambientK = 300.0;      // the box the filament sits in
    double operatingK = 2400.0;   // filament temperature at `nominalVolts`
    double nominalVolts = 1.0;    // the drive that holds `operatingK` in steady state
    double resistanceExponent = 1.2;   // ρ: tungsten R ∝ T^ρ over the hot range
    double brightnessExponent = 7.0;   // m: emission in the cell's band ∝ T^m
    double thermalTauSeconds = 0.030;  // SMALL-SIGNAL τ at the operating point
};

class LampDrive {
public:
    // Configure from the spec. Call before setRate()/reset().
    void configure(const LampSpec& s) {
        spec_ = s;
        if (spec_.operatingK <= spec_.ambientK) spec_.operatingK = spec_.ambientK + 1.0;
        const double top = spec_.operatingK;
        const double ta = spec_.ambientK;
        const double vn2 = spec_.nominalVolts * spec_.nominalVolts;
        // Steady state at (operatingK, nominalVolts): heating == cooling. R is
        // normalized to 1 there, so this is the whole of k.
        kRad_ = vn2 / (pow4(top) - pow4(ta));
        // C from the small-signal time constant at that same point.
        const double dNet =
            spec_.resistanceExponent * vn2 / top + 4.0 * kRad_ * top * top * top;
        heatCap_ = spec_.thermalTauSeconds * (dNet > 0.0 ? dNet : 1e-12);
        if (heatCap_ <= 0.0) heatCap_ = 1e-12;
        rebuild();
    }

    const LampSpec& spec() const { return spec_; }

    void setRate(double rate) {
        rate_ = rate > 0.0 ? rate : 44100.0;
        rebuild();
    }
    double rate() const { return rate_; }

    // Park the filament COLD (at ambient). Never solves.
    void reset() { tempK_ = spec_.ambientK; }

    // Park the filament where a steady `volts` would hold it. `VibeModel` uses
    // this in prepare()/reset() so the pedal does not open with a warm-up
    // transient that no player would ever hear on a pedal that has been on for
    // more than a second.
    void parkAt(double volts) { tempK_ = steadyTempK(volts); }

    // One sample. `volts` is the lamp driver's output (a driver cannot pull the
    // filament colder than off, so negative drive is zero drive). Returns the
    // NORMALIZED light output, 1.0 at `operatingK` — the quantity `OptoCell` is
    // handed when its `elExponent` is 1.0.
    double process(double volts) {
        const double v = volts > 0.0 ? volts : 0.0;
        if (thermalMass_) {
            const double heat = v * v / resistanceAt(tempK_);
            const double cool = kRad_ * (pow4(tempK_) - ambient4_);
            double dT = dt_ * (heat - cool) / heatCap_;
            // Deterministic guard, never a tuning knob: one sample may not move
            // the filament more than a quarter of its own operating span. At the
            // shipped rates the real step is ~1 K, well inside this.
            if (dT > stepClamp_) dT = stepClamp_;
            if (dT < -stepClamp_) dT = -stepClamp_;
            tempK_ += dT;
            if (tempK_ < spec_.ambientK) tempK_ = spec_.ambientK;
            if (tempK_ > tempCeiling_) tempK_ = tempCeiling_;
        } else {
            // Measurement hook: an INSTANTANEOUS filament (no thermal mass). Used
            // to attribute the sweep asymmetry to this class rather than assume
            // it — docs §67.6. Not reachable from any parameter.
            tempK_ = steadyTempK(v);
        }
        return brightness();
    }

    // --- Measurement hooks (not user-facing) --------------------------------
    double temperatureK() const { return tempK_; }
    // Normalized light output, 1.0 at the operating temperature.
    double brightness() const {
        return std::pow(tempK_ / spec_.operatingK, spec_.brightnessExponent);
    }
    // The filament temperature a steady `volts` holds. The net-power residual is
    // monotone decreasing in T, so a plain bisection is exact to the bracket. Not
    // called from process() unless the thermal mass is disabled.
    double steadyTempK(double volts) const {
        const double v = volts > 0.0 ? volts : 0.0;
        if (v <= 0.0) return spec_.ambientK;
        double lo = spec_.ambientK, hi = tempCeiling_;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            const double net = v * v / resistanceAt(mid) - kRad_ * (pow4(mid) - ambient4_);
            if (net > 0.0)
                lo = mid;
            else
                hi = mid;
        }
        return 0.5 * (lo + hi);
    }
    // The steady drive that produces a given NORMALIZED brightness. Closed form —
    // this is the inverse of the pair above, and it is what lets `VibeModel`
    // DERIVE its lamp bias and swing from the cell's own published resistance
    // range instead of choosing them (docs §67.2).
    double driveForSteadyBrightness(double light) const {
        if (light <= 0.0) return 0.0;
        const double t =
            spec_.operatingK * std::pow(light, 1.0 / spec_.brightnessExponent);
        const double tc = t < spec_.ambientK ? spec_.ambientK : t;
        const double p = kRad_ * (pow4(tc) - ambient4_) * resistanceAt(tc);
        return p > 0.0 ? std::sqrt(p) : 0.0;
    }
    // Perturbation/attribution hook: false runs the filament with NO thermal mass
    // (brightness follows the drive instantly). Never wired to a parameter.
    void setThermalMass(bool on) { thermalMass_ = on; }
    bool thermalMass() const { return thermalMass_; }

    // NOTE, deliberately: there is no `maxAbsRestingState()` here. `tempK_` rests
    // at `ambientK` (300 K) unpowered and at the lamp's bias point in this pedal —
    // a real operating point either way, so ADR 006's scope rule says comment,
    // not guard.

private:
    static double pow4(double x) {
        const double x2 = x * x;
        return x2 * x2;
    }
    // Tungsten's positive temperature coefficient, normalized to 1 at operatingK.
    double resistanceAt(double t) const {
        const double r = (t < spec_.ambientK ? spec_.ambientK : t) / spec_.operatingK;
        const double rr = std::pow(r, spec_.resistanceExponent);
        return rr > 1e-12 ? rr : 1e-12;
    }
    void rebuild() {
        dt_ = 1.0 / rate_;
        ambient4_ = pow4(spec_.ambientK);
        tempCeiling_ = 8.0 * spec_.operatingK;
        stepClamp_ = 0.25 * (spec_.operatingK - spec_.ambientK);
    }

    LampSpec spec_{};
    double rate_ = 48000.0;
    double dt_ = 1.0 / 48000.0;
    double kRad_ = 0.0;
    double heatCap_ = 1.0;
    double ambient4_ = 8.1e9;
    double tempCeiling_ = 19200.0;
    double stepClamp_ = 525.0;
    bool thermalMass_ = true;

    // Rests at a REAL operating point (ambientK unpowered, the bias point in
    // service). NOT denormal-guarded — ADR 006's scope rule.
    double tempK_ = 300.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_LAMP_DRIVE_H
