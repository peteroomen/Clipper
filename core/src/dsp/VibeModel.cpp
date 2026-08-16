// Clipper — VibeModel (M13.5). The Uni-Vibe: four staggered photocell phase
// stages, one incandescent lamp. See VibeModel.h for the research channel, the
// SOURCED/RECONSTRUCTED split and the two acceptance bars; docs §67 has the
// numbers.
//
// ---------------------------------------------------------------------------
// Things worth knowing before you touch a constant in this file:
//
//  * `kStageCapsFarads` is the ONE SOURCED set of component values on this whole
//    voice (S1). The sources also say the reason for these particular values is
//    unknown, so nothing here explains them and nothing here should. They ship
//    literally, and a test asserts them literally, so a later slice cannot
//    "tidy" them into a matched set or a geometric progression.
//
//  * The lamp's BIAS and SWING are DERIVED, not chosen: S6 publishes the cell's
//    illuminated range as 10 kΩ…100 kΩ, and `LampDrive::driveForSteadyBrightness`
//    inverts the filament law to say what drive holds each end. Change the cell's
//    resistance range and the lamp endpoints move with it, by construction.
//
//  * The four cells are held BY VALUE and configured IDENTICALLY (S2: one lamp,
//    one reflective box). They therefore return the same resistance, and a test
//    asserts that they do. They are four objects because they are four devices —
//    a sourced per-cell tolerance or a per-cell illumination figure has a place
//    to go, and inventing one today would be exactly the fitting §57 forbids.
//
//  * `OptoCell` was NOT WIDENED to get here. It gets `elExponent = 1.0` and
//    `panelMaxV = 1.0`, which is the existing field used with a degenerate
//    exponent so that the quantity handed to it IS the light — the source law
//    lives in `LampDrive`, exactly where `OptoCell.h` said to put it.
// ---------------------------------------------------------------------------

#include "clipper/dsp/VibeModel.h"

#include <algorithm>
#include <cmath>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/LampDrive.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/OptoCell.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ParamGuard.h"

namespace clipper::dsp {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr int kNumStages = 4;

// ============================================================================
//  S1 — THE ONE SOURCED SET OF COMPONENT VALUES
// ============================================================================
// The four phase-shift stages' capacitors, verbatim: 0.015 µF, 0.22 µF, 470 pF,
// 0.0047 µF. Multiple independent extracts agree on the set, AND they agree that
// the reason for these particular values is not known. So: the STAGGER is
// modelled and no rationale is invented for it.
//
// The consequence the acceptance bar is built on: 0.22 µF / 470 pF = 468.1 : 1 of
// capacitance = 8.87 octaves of corner spread, against `PhaserModel`'s ±1.5 %
// detune = 0.043 octaves. The same cell resistance appears in every stage (one
// lamp, one box), so the corner RATIOS are fixed and scale-free.
constexpr double kStageCapsFarads[kNumStages] = {
    0.015e-6,   // 15 nF
    0.22e-6,    // 220 nF
    470.0e-12,  // 470 pF
    0.0047e-6,  // 4.7 nF
};

// ============================================================================
//  THE PHOTOCELL — S6 for the resistances, RECONSTRUCTED for the dynamics
// ============================================================================
// SOURCED (S6): 10 kΩ…100 kΩ under illumination, 5 MΩ…20 MΩ dark. `rLightOhms`
// is therefore the bright end of the published illuminated range and `rDarkOhms`
// the midpoint of the published dark range. `cellGamma` is the SAME published CdS
// figure `OptoCell` already carries (R ∝ E^−γ, γ ≈ 0.58…0.92), unchanged.
//
// `elExponent = 1.0` and `panelMaxV = 1.0`: the drive quantity handed to the cell
// is the LAMP's normalized light, not a panel voltage, so the source law is a
// degenerate identity here and lives in `LampDrive`. This is `OptoCell` used as
// documented, not widened.
//
// THE DYNAMIC CONSTANTS ARE RECONSTRUCTED, and this is the one place on this
// voice where a published Uni-Vibe figure would have changed the answer:
//
//   * `OptoCell`'s DEFAULT card is pinned to the LA-2A's published 10 ms attack
//     and 0.5–5 s complete release. Reusing it unchanged is REFUTED by S7: a
//     cell whose slow branch releases over 180 ms…2.7 s cannot deliver the
//     "intense vibrato" the sources describe at 7.6 Hz, whose half period is
//     65.8 ms. That is a published-behaviour refutation, not a taste call.
//
//   * So the release is pinned by the same published figure, using the standard
//     first-order criterion: the modulation must survive to at least half depth
//     (−6.02 dB) at S7's top speed, i.e. 2π·f·τ = √3 → τ = √3/(2π·7.6) = 36.3 ms.
//     `tauSlowReleaseSeconds` is set so the EFFECTIVE slow constant at this
//     pedal's measured trap occupancy (docs §67.8 — the cells here are lit
//     continuously, so it sits near 1) lands on that figure:
//     0.012·(1 + (3 − 1)·0.95) = 34.8 ms.
//
//   * The attack and the fast branch keep `OptoCell`'s own SHAPE (a cell rises
//     faster than it falls, and the branch split is the device's, not the
//     pedal's) scaled to the same span. `fastShare` and `trapHalfFraction` are
//     the device card's, unchanged.
//
// None of this is fitted to a sound and none of it may be. §57's rule.
OptoCellSpec makeCellSpec() {
    OptoCellSpec s{};
    s.rDarkOhms = 10.0e6;   // S6: 5 MΩ…20 MΩ dark — the midpoint
    s.rLightOhms = 10.0e3;  // S6: the bright end of the illuminated range
    s.panelMaxV = 1.0;      // the drive quantity is NORMALIZED LIGHT
    s.elExponent = 1.0;     // ...so the source law here is the identity
    s.cellGamma = 0.75;     // the published CdS γ midpoint, unchanged
    s.tauAttackSeconds = 0.004;
    s.fastShare = 0.70;  // the device card's branch split, unchanged
    s.tauFastReleaseSeconds = 0.008;
    s.tauSlowReleaseSeconds = 0.012;
    s.slowReleaseMemMult = 3.0;
    s.trapHalfFraction = 0.01;  // the device card's, unchanged
    s.tauMemChargeSeconds = 0.25;
    s.tauMemDischargeSeconds = 0.40;
    return s;
}

// ============================================================================
//  THE LAMP — every constant RECONSTRUCTED (see LampDrive.h)
// ============================================================================
// SOURCED only that it is a miniature incandescent lamp (S2). The operating
// temperature is an ordinary small-filament figure, ρ = 1.2 is tungsten's, and
// m = 7 is emission in a CdS cell's own sensitivity band rising far faster than
// the T⁴ total. `thermalTauSeconds` is the small-signal constant at the operating
// point and sits inside the 10–100 ms band a miniature filament occupies. The
// RISE/FALL ASYMMETRY is not a constant here at all — it falls out of the inrush
// and the T⁴ law (LampDrive.h's banner).
LampSpec makeLampSpec() {
    LampSpec s{};
    s.ambientK = 300.0;
    s.operatingK = 2400.0;
    s.nominalVolts = 1.0;
    s.resistanceExponent = 1.2;
    s.brightnessExponent = 7.0;
    s.thermalTauSeconds = 0.030;
    return s;
}

// S7: roughly 0.1 Hz at the slow end up to 7.6 Hz. Log mapped, as every other LFO
// in this repo is, so the slow end (where a Uni-Vibe lives most of its life)
// occupies most of the travel.
constexpr double kMinHz = 0.1;
constexpr double kMaxHz = 7.6;

// Knob smoothing: the house ~8 ms, applied per BASE sample.
constexpr double kSmoothSeconds = 0.008;
// The MODE weight is smoothed on the SAME law rather than switched. §62 measured
// a hard mode switch clicking at 17.02x the signal's own slew; this is that
// lesson applied on the way in.
constexpr double kModeSmoothSeconds = 0.008;

// The shipped oversampling factor. DERIVED, not defaulted — see VibeModel.h's
// banner and docs §67.7: it is the smallest house factor at which this voice's
// notch-position error against the analytic analog network (0.89 %) is smaller
// than `PhaserModel`'s own at 44.1 kHz (3.15 %). 2x measures 3.61 % and does not
// clear that. Pinned by `clipper_vibe_tests`.
constexpr int kDefaultOversampling = 4;

double clamp01d(double v) { return clampParam01(v); }
float clamp01f(float v) { return clampParam01(v); }

}  // namespace

struct VibeModel::Impl {
    double sampleRate = 44100.0;
    double osRate = 176400.0;
    int maxBlockSize = 128;
    int osFactor = kDefaultOversampling;
    int osPhase = 0;
    Oversampler os;

    LampDrive lamp;
    OptoCell cell[kNumStages];

    OnePoleSmoother rateHz;      // SPEED  -> knob position (mapped per sample)
    OnePoleSmoother intensity;   // S8's amplitude control
    OnePoleSmoother modeWeight;  // 0 = CHORUS, 1 = VIBRATO (never a switch)
    bool snapPending = true;

    double lfoPhase = 0.0;
    bool frozen = false;
    double frozenPhase = 0.0;

    // The lamp drive endpoints DERIVED from the cell's published illuminated
    // range (S6). Computed in configure/prepare; see deriveLampDrive().
    double vDim = 0.0;
    double vBright = 1.0;

    // Per-stage allpass state (previous input/output) and the cell it divides with.
    double x1[kNumStages] = {0, 0, 0, 0};
    double y1[kNumStages] = {0, 0, 0, 0};
    double rCell[kNumStages] = {1.0e5, 1.0e5, 1.0e5, 1.0e5};

    bool cellDynamics = true;
    double dryW = 0.5, wetW = 0.5;

    // The drive that holds the cell at `ohms` in steady state. Inverts the CdS
    // law for the light, then the filament law for the volts — both closed form.
    double driveForCellOhms(double ohms) const {
        const OptoCellSpec& s = cell[0].spec();
        const double gDark = 1.0 / s.rDarkOhms;
        const double gExcessMax = 1.0 / s.rLightOhms - gDark;
        double dG = 1.0 / ohms - gDark;
        if (dG < 0.0) dG = 0.0;
        if (dG > gExcessMax) dG = gExcessMax;
        // ΔG = gExcessMax · L^(elExponent·γ); elExponent is 1 here by construction.
        const double p = s.elExponent * s.cellGamma;
        const double light = std::pow(dG / gExcessMax, 1.0 / p);
        return lamp.driveForSteadyBrightness(light);
    }

    void deriveLampDrive() {
        // S6's published illuminated range IS the sweep, so the lamp's endpoints
        // are whatever holds it. Nothing is chosen here.
        vDim = driveForCellOhms(100.0e3);
        vBright = driveForCellOhms(10.0e3);
    }

    void rebuildRates() {
        osRate = sampleRate * static_cast<double>(os.factor());
        // The lamp, the cells and the LFO all live INSIDE the oversampled domain,
        // because the allpass network they drive does; splitting them would put a
        // control staircase on the corner for no reason.
        lamp.setRate(osRate);
        for (int s = 0; s < kNumStages; ++s) cell[s].setRate(osRate);
        deriveLampDrive();
    }

    double lampVoltsFor(double intensity01, double lfo) const {
        const double bias = 0.5 * (vBright + vDim);
        const double swing = 0.5 * (vBright - vDim);
        return bias + swing * intensity01 * lfo;
    }

    // First-order allpass coefficient for a corner fc, clamped safely below
    // Nyquist. a = (t-1)/(t+1), t = tan(pi*fc/fs) — the same prewarped form
    // `PhaserModel` uses, so the two models' notch geometry is compared on equal
    // terms. Evaluated at the OVERSAMPLED rate, which is the whole point of the
    // domain (docs §67.7): the 470 pF stage's corner reaches 33.9 kHz at the
    // bright end of the sweep, so at a 44.1 kHz base rate it would sit at 0.77 of
    // Nyquist and the bilinear phase curve would warp under it.
    double coeffForCorner(double fc) const {
        double f = fc;
        const double fmax = 0.45 * osRate;
        if (f < 1.0) f = 1.0;
        if (f > fmax) f = fmax;
        const double t = std::tan(kTwoPi * 0.5 * f / osRate);
        return (t - 1.0) / (t + 1.0);
    }
};

VibeModel::VibeModel() : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.lamp.configure(makeLampSpec());
    const OptoCellSpec cs = makeCellSpec();
    for (int s = 0; s < kNumStages; ++s) d.cell[s].configure(cs);
    // Shipped knob defaults live in the CONSTRUCTOR, not prepare(), so a caller
    // that writes parameters BEFORE prepare() keeps them — which is exactly what
    // `native/tests/identical_core_test.cpp`'s reference chain does, and what §64
    // found the hard way (a 2.4e-03 plugin-vs-reference difference).
    // `OnePoleSmootherT::prepare` snaps value to TARGET, so these survive.
    d.rateHz.setImmediate(0.35f);
    d.intensity.setImmediate(0.70f);
    d.modeWeight.setImmediate(0.0f);  // CHORUS
    d.deriveLampDrive();
}

VibeModel::~VibeModel() = default;

void VibeModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 128;

    d.os.prepare(d.maxBlockSize);
    d.os.setFactor(d.osFactor);

    // Knob smoothing at the BASE rate, advanced once per base sample and HELD
    // across the oversampled sub-samples: advancing per oversampled sample would
    // land an 8 ms ramp in 2 ms at 4x (the CompressorEngine / OptoModel precedent).
    d.rateHz.prepare(kSmoothSeconds, d.sampleRate);
    d.intensity.prepare(kSmoothSeconds, d.sampleRate);
    d.modeWeight.prepare(kModeSmoothSeconds, d.sampleRate);

    d.rebuildRates();
    reset();
    d.snapPending = true;
}

void VibeModel::reset() {
    Impl& d = *impl_;
    d.rateHz.reset();
    d.intensity.reset();
    d.modeWeight.reset();
    d.lfoPhase = 0.0;
    d.os.reset();
    d.osPhase = 0;

    // Park the lamp and the cells where a steady LFO trough would hold them,
    // rather than cold: a Uni-Vibe's lamp has been lit since the amp was switched
    // on, and a warm-up transient on every reset() would be a fiction. The LFO
    // starts at phase 0 (sin = 0), so the bias point is the park point.
    const double v = d.lampVoltsFor(static_cast<double>(d.intensity.value()), 0.0);
    d.lamp.parkAt(v);
    const double light = d.lamp.brightness();
    const int n = static_cast<int>(0.200 * d.osRate);
    for (int s = 0; s < kNumStages; ++s) {
        d.cell[s].reset();
        // Settle each cell onto that light so the first block is not a ramp.
        // Deterministic and bounded: 200 ms of the cell's own dynamics, several
        // times its longest constant. Runs only from reset()/prepare().
        double r = 0.0;
        for (int i = 0; i < n; ++i) r = d.cell[s].process(light);
        d.rCell[s] = r;
        d.x1[s] = 0.0;
        d.y1[s] = 0.0;
    }

    d.dryW = 0.5 * (1.0 - static_cast<double>(d.modeWeight.value()));
    d.wetW = 0.5 * (1.0 + static_cast<double>(d.modeWeight.value()));
}

void VibeModel::setParameter(int paramId, float value) {
    // ParamGuard, never a hand-rolled clamp: std::clamp and the ternary both pass
    // NaN through, and a NaN corner coefficient latches in the allpass memories
    // permanently (audit finding 1).
    const float v = clamp01f(value);
    Impl& d = *impl_;
    switch (paramId) {
        case PARAM_SPEED:
            d.rateHz.setTarget(v);
            break;
        case PARAM_INTENSITY:
            d.intensity.setTarget(v);
            break;
        case PARAM_MODE:
            // DISCRETE control, CONTINUOUS implementation. The real switch has
            // two positions, so the target is 0 or 1 — but it is a smoothed
            // weight, never a branch. §62's 17.02x click came from doing the
            // other thing; do not "simplify" this back into an if.
            d.modeWeight.setTarget(v >= 0.5f ? 1.0f : 0.0f);
            break;
        default:
            break;
    }
}

void VibeModel::setOversampling(int factor) {
    Impl& d = *impl_;
    d.os.setFactor(factor);
    d.osFactor = d.os.factor();
    d.rebuildRates();
    // Everything in the domain is rate-dependent; re-park rather than carry
    // history that belonged to another rate.
    reset();
}

int VibeModel::oversampling() const { return impl_->os.factor(); }
int VibeModel::latencySamples() const { return impl_->os.latencySamples(); }

int VibeModel::numStages() { return kNumStages; }

double VibeModel::stageCapFarads(int stage) {
    if (stage < 0 || stage >= kNumStages) return 0.0;
    return kStageCapsFarads[stage];
}

double VibeModel::speedKnobToHz(double knob01) {
    const double k = clamp01d(knob01);
    return kMinHz * std::pow(kMaxHz / kMinHz, k);
}

double VibeModel::cellResistanceOhms(int stage) const {
    if (stage < 0 || stage >= kNumStages) return 0.0;
    return impl_->rCell[stage];
}

double VibeModel::stageCornerHz(int stage) const {
    if (stage < 0 || stage >= kNumStages) return 0.0;
    return 1.0 / (kTwoPi * impl_->rCell[stage] * kStageCapsFarads[stage]);
}

double VibeModel::lampTemperatureK() const { return impl_->lamp.temperatureK(); }
double VibeModel::lampBrightness() const { return impl_->lamp.brightness(); }

double VibeModel::cellCompositeExponent(int stage) const {
    if (stage < 0 || stage >= kNumStages) return 0.0;
    return impl_->cell[stage].compositeExponent();
}

double VibeModel::cellMemory(int stage) const {
    if (stage < 0 || stage >= kNumStages) return 0.0;
    return impl_->cell[stage].memory();
}

double VibeModel::dryWeight() const { return impl_->dryW; }
double VibeModel::wetWeight() const { return impl_->wetW; }
double VibeModel::lampDriveDimVolts() const { return impl_->vDim; }
double VibeModel::lampDriveBrightVolts() const { return impl_->vBright; }

void VibeModel::setFrozen(bool frozen, double phase01) {
    Impl& d = *impl_;
    d.frozen = frozen;
    d.frozenPhase = clamp01d(phase01);
    if (frozen) {
        // Park the lamp and the cells AT that phase, so a measurement taken
        // immediately afterwards sees the settled network rather than a filament
        // warming up into it. Same shape as reset(), with the frozen drive.
        const double v = d.lampVoltsFor(static_cast<double>(d.intensity.value()),
                                        std::sin(kTwoPi * d.frozenPhase));
        d.lamp.parkAt(v);
        const double light = d.lamp.brightness();
        const int n = static_cast<int>(0.400 * d.osRate);
        for (int s = 0; s < kNumStages; ++s) {
            d.cell[s].reset();
            double r = 0.0;
            for (int i = 0; i < n; ++i) r = d.cell[s].process(light);
            d.rCell[s] = r;
        }
    }
}

void VibeModel::setLampThermalMass(bool on) { impl_->lamp.setThermalMass(on); }
void VibeModel::setCellDynamics(bool on) { impl_->cellDynamics = on; }

double VibeModel::maxAbsRestingState() const {
    // The four allpass memories and NOTHING ELSE. Deliberately EXCLUDED, measured
    // rather than assumed (ADR 006's scope rule, docs §67.8):
    //  * the LAMP's filament temperature — rests at its bias point (a Uni-Vibe's
    //    lamp is always lit), so it can never be subnormal;
    //  * the four CELLS' three states each — for the same reason, and this is the
    //    interesting one: inside `OptoModel` the identical class rests at exactly
    //    zero and IS asserted to, because there the light source is the audio.
    //    Same component, opposite sides of the scope rule, decided by what is
    //    shining on it;
    //  * `rCell[]` — rests at the illuminated operating resistance;
    //  * the LFO phase — a bounded cycle position, not an accumulator decaying
    //    toward zero.
    const Impl& d = *impl_;
    return maxAbsState(d.x1[0], d.y1[0], d.x1[1], d.y1[1], d.x1[2], d.y1[2], d.x1[3],
                       d.y1[3]);
}

void VibeModel::process(const float* in, float* out, int numFrames) {
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, impl_->maxBlockSize);
        processChunk(in + done, out + done, n);
        done += n;
    }
}

void VibeModel::processChunk(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;

    if (d.snapPending) {
        // Deferred snap (docs §35): the C ABI's prepare-then-set order would
        // otherwise ramp every knob from its construction default on the first
        // block.
        d.rateHz.setImmediate(d.rateHz.target());
        d.intensity.setImmediate(d.intensity.target());
        d.modeWeight.setImmediate(d.modeWeight.target());
        d.snapPending = false;
    }

    // The WHOLE model runs inside ONE oversampling domain — the dry sum included.
    // That is deliberate: the group delay is then uniform across dry and wet, so
    // the mixer cannot comb. Putting only the allpass chain inside would delay the
    // wet path relative to the dry by the halfbands' 72 samples and manufacture a
    // 6.7 ms comb that is not in the circuit.
    d.os.upsample(in, numFrames);
    float* w = d.os.buffer();
    const int f = d.os.factor();
    const int n = numFrames * f;
    d.osPhase = 0;
    double speedKnob = static_cast<double>(d.rateHz.value());
    double intensity = static_cast<double>(d.intensity.value());
    double m = static_cast<double>(d.modeWeight.value());

    for (int i = 0; i < n; ++i) {
        // One knob advance per BASE sample — the smoothers were prepared for the
        // base rate (see prepare()).
        if (d.osPhase == 0) {
            speedKnob = static_cast<double>(d.rateHz.next());
            intensity = static_cast<double>(d.intensity.next());
            m = static_cast<double>(d.modeWeight.next());
        }
        if (++d.osPhase >= f) d.osPhase = 0;

        // --- The LFO: a sine (S4 — the oscillator is a phase-shift type) ------
        const double lfo = std::sin(kTwoPi * (d.frozen ? d.frozenPhase : d.lfoPhase));

        // --- S8: INTENSITY is the fraction of the LFO reaching the lamp -------
        const double vLamp = d.lampVoltsFor(intensity, lfo);

        // --- The lamp. Its thermal mass is where the sweep asymmetry lives. ---
        const double light = d.lamp.process(vLamp);

        // --- Four cells, one box, one light (S2) ------------------------------
        const double xin = static_cast<double>(w[i]);
        double y = xin;
        for (int s = 0; s < kNumStages; ++s) {
            if (d.cellDynamics) {
                d.rCell[s] = d.cell[s].process(light);
            } else {
                // Attribution hook: a MEMORYLESS cell (its steady-state law with
                // no dynamics at all), so the asymmetry can be attributed to the
                // lamp and the cell independently. Never wired to a parameter.
                const OptoCellSpec& cs = d.cell[s].spec();
                const double gDark = 1.0 / cs.rDarkOhms;
                const double gMax = 1.0 / cs.rLightOhms - gDark;
                const double p = cs.elExponent * cs.cellGamma;
                double dG = gMax * std::pow(light > 0.0 ? light : 0.0, p);
                if (dG > gMax) dG = gMax;
                d.rCell[s] = 1.0 / (gDark + dG);
            }

            const double fc = 1.0 / (kTwoPi * d.rCell[s] * kStageCapsFarads[s]);
            const double a = d.coeffForCorner(fc);
            const double x = y;
            // Anti-denormal (Denormal.h): the allpass memory rings down through
            // the double subnormal range on a silent tail. Each section is FIRST
            // ORDER, so the house one-liner is the correct form (§56.4b's warning
            // is about direct forms of order >= 2).
            const double yn = flushDenormal(a * x + d.x1[s] - a * d.y1[s]);
            d.x1[s] = x;
            d.y1[s] = yn;
            y = yn;
        }

        // --- S5's mixer: two 100 kΩ resistors, i.e. EQUAL weights -------------
        // CHORUS is 0.5·dry + 0.5·wet; VIBRATO is the wet path alone. One smoothed
        // weight, never a switch (§62).
        d.dryW = 0.5 * (1.0 - m);
        d.wetW = 0.5 * (1.0 + m);
        w[i] = static_cast<float>(d.dryW * xin + d.wetW * y);

        if (!d.frozen) {
            d.lfoPhase += VibeModel::speedKnobToHz(speedKnob) / d.osRate;
            if (d.lfoPhase >= 1.0) d.lfoPhase -= std::floor(d.lfoPhase);
        }
    }
    d.os.downsample(out, numFrames);
}

}  // namespace clipper::dsp
