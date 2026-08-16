// Clipper — portable DSP core.
//
// VibeModel (M13.5) — the "Uni-Vibe": a Univox-style four-stage PHOTOCELL phase
// shifter, one incandescent lamp lighting four LDRs in a reflective box. Pedal
// type `vibe`, slot 12, and the lineup's SECOND modulation phaser after the
// script-era "Ninety" (`PhaserModel`).
//
// ============================================================================
//  THE RESEARCH CHANNEL — read docs §67.1 before touching any constant here
// ============================================================================
// NO SCHEMATIC AND NO NETLIST WAS READ. Re-probed at the top of this slice:
// `geofex.com` (R.G. Keen's "The Technology of the Univibe", the canonical
// article) and `montagar.com` (the parts list) both return **403 at the egress
// proxy**, and this session's GitHub access is repository-scoped, so even the
// code search the previous slices leaned on was unavailable. The channel was
// **search-result extracts only**.
//
// That makes this the §57 / §61 / §64 situation, not the §59 / §63 one, and
// §57's rule applies verbatim to every RECONSTRUCTED value below: **do not
// re-tune any of them toward a sound; find the schematic.**
//
// RULE ZERO (§63.14). Three Uni-Vibe implementations exist on GitHub. They are
// other people's models. Nothing in this file is tuned toward, compared against,
// or copied from any of them.
//
// SOURCED (search-extract grade — docs §67.1 tabulates these row by row):
//
//   S1  The four phase-shift stages use STAGGERED capacitors — 0.015 µF,
//       0.22 µF, 470 pF, 0.0047 µF — where a conventional phaser uses four
//       matched values. **The sources also state that the reason for these
//       particular values is not known.** So this model reproduces the STAGGER,
//       which is the structural fact, and invents no rationale for it.
//   S2  One miniature INCANDESCENT LAMP and FOUR LDRs inside a reflective box.
//   S3  The phase shifter is built from TRANSISTOR stages, not op-amps, and the
//       "imperfections" of those stages are part of the sound. NOT MODELLED —
//       the largest named gap on this voice.
//   S4  The LFO is a PHASE-SHIFT OSCILLATOR (hence approximately sinusoidal),
//       "implemented oddly".
//   S5  The mixer sums the buffered DRY through a 100 kΩ resistor and the WET
//       through another 100 kΩ — i.e. EQUAL weights — and the CHORUS/VIBRATO
//       switch selects either the purely wet phase-line output (VIBRATO) or that
//       roughly equal mix (CHORUS).
//   S6  The LDRs' useful range is 10 kΩ…100 kΩ UNDER ILLUMINATION, with a dark
//       resistance of 5 MΩ…20 MΩ.
//   S7  Speed spans roughly 0.1 Hz at the slow end up to 7.6 Hz.
//   S8  INTENSITY is an AMPLITUDE control: it selects a fraction 0…1 of the LFO
//       output and hands that to the LAMP DRIVER.
//
// ============================================================================
//  WHAT THAT BUYS, AND WHAT IS STILL THIS MODEL'S OWN
// ============================================================================
// S6 + S8 are worth more than they look: together they DERIVE the lamp's bias and
// swing rather than leaving them to be chosen. The cell must sit at 100 kΩ at the
// dim end of the sweep and 10 kΩ at the bright end, so `LampDrive`'s inverse
// (`driveForSteadyBrightness`) says exactly what the two drive endpoints are, and
// the LFO is a sine between them scaled by INTENSITY. Nothing is fitted there.
//
// RECONSTRUCTED, and named as such: the phase stage as an ideal first-order
// allpass (S3's transistor imperfections are the largest known gap on this
// voice); the whole lamp card (`LampSpec` — structure from filament
// thermodynamics, constants this model's own); and the photocell's DYNAMIC
// constants, which are discussed under `makeCellSpec()` in the .cpp because they
// are the one place where a published Uni-Vibe figure would have changed the
// answer and none was reachable.
//
// ============================================================================
//  THE ACCEPTANCE BAR — notch DISTRIBUTION, computed against the sibling
// ============================================================================
// The ROADMAP frames this voice as "four MISMATCHED stages (not the Ninety's
// matched four)". **That framing is wrong and this file corrects it.**
// `PhaserModel` already carries a deterministic ±1.5 % per-stage detune,
// specifically so its four corners do not stack into one infinitely-deep null. So
// "mismatched" is not the distinction. The MAGNITUDE is:
//
//     Ninety     ±1.5 % of corner spread  =  0.043 octaves
//     Uni-Vibe   0.22 µF vs 470 pF        =  468:1  =  8.87 octaves
//
// — a factor of about 204 in octaves, or 470:1 in capacitance. The player-facing
// consequence is the SPREAD OF THE NOTCH PAIR: a cascade of four first-order
// allpasses summed with dry has exactly two notches whatever the corners are, but
// where the Ninety's sit a fixed 5.83× apart (tan 22.5° and tan 67.5° — the
// matched-stage geometry), this one's sit far wider. **That ratio is SCALE-FREE:
// it does not move with the cell resistance, so it cannot be affected by any of
// the reconstructed constants above.** `clipper_vibe_tests` measures it on BOTH
// models from the same stimulus and asserts the contrast.
//
// Second bar, and it is the one a JFET phaser cannot fake: the LAMP has thermal
// mass, so the sweep is ASYMMETRIC IN TIME — the notch frequency rises faster
// than it falls at the same SPEED. `PhaserModel`'s rounded-triangle LFO is
// symmetric about its own peak by construction and measures 1.00 on the same
// metric.
//
// ============================================================================
//  MODES, AND THE §62 TRAP — avoided on the way in, not discovered again
// ============================================================================
// §62 forwarded a mode to an owned model as a HARD SWITCH and it clicked at
// 17.02× the signal's own slew. The fix was to never switch. So here, from the
// first line: CHORUS and VIBRATO are the SAME dry/wet pair with one smoothed
// weight `m`, per S5's equal-weight mixer:
//
//     dryWeight = 0.5·(1 − m)      wetWeight = 0.5·(1 + m)
//
// m = 0 is CHORUS (0.5 dry + 0.5 wet — S5's two 100 kΩ resistors) and m = 1 is
// VIBRATO (no dry at all). There is no second code path, so "vibrato is the wet
// path alone" is structurally true rather than something two branches have to
// agree about — and because `OnePoleSmoother` converges EXACTLY to a zero target
// (§62's numeric note), the dry weight really does reach 0.0 and the test asserts
// it with `==`.
//
// INTENSITY is S8's LFO amplitude control, NOT a wet/dry mix. Consequence,
// asserted rather than assumed: **INTENSITY 0 is NOT bypass.** The lamp still
// sits at its bias point, so the four stages sit at a FIXED corner set — a static
// comb in CHORUS, and in VIBRATO a static allpass, i.e. flat magnitude. Both are
// measured in `clipper_vibe_tests` instead of being asserted from the schematic
// nobody could read.
//
// ============================================================================
//  OVERSAMPLING — decided by measurement, and the measurement OVERTURNED the
//  precedent this slice expected to inherit (docs §67.7)
// ============================================================================
// The phaser precedent (§12) says a linear time-varying pedal needs no
// oversampling, and on ALIASING it is right about this one too: the cells here
// are lit by an LFO under 8 Hz and by nothing else, so the audio path never
// multiplies by an audio-rate signal. Measured with a HANN-WINDOWED analysis
// first (§60's trap: a rectangular window's own sidelobe read as a −55.8 dB floor
// and nearly sent that slice the wrong way) the non-harmonic floor is FLAT in the
// factor — §54's tell for "not aliasing".
//
// **It is oversampled anyway, for a different measured reason, and that reason is
// the 470 pF stage.** S1's stagger puts one allpass corner at 14.6 kHz at the
// pedal's own bias point and 33.9 kHz at the bright end of the sweep, i.e. right
// up against — and past — Nyquist at the shipped rates. A bilinear first-order
// allpass is exact only AT its corner, so a corner that close to Nyquist warps the
// phase curve, and the phase curve is where the notches are. Measured against the
// analytic ANALOG network the upper notch lands +18.90 % out at 44.1 kHz 1× and
// +0.89 % at 4×. The same defect exists in `PhaserModel`, where it is PRE-EXISTING
// and small (−3.15 % at 44.1 kHz) because its corners never leave a clean
// 200 Hz…2 kHz decade.
//
// So the factor is DERIVED, not defaulted: **4× is the smallest house factor at
// which this voice's discretization error is smaller than the sibling's own**
// (2× measures 3.61 %, above the Ninety's 3.15 %). Latency 72 samples. The WHOLE
// model — the dry sum included — sits inside the one domain, so the group delay
// is uniform and the dry/wet sum cannot comb; splitting it would manufacture
// exactly the artefact this pedal is made of.
//
// Platform-free (C++17, no OS/browser/Emscripten). Heavy state behind a pimpl.
// Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_VIBE_MODEL_H
#define CLIPPER_DSP_VIBE_MODEL_H

#include <memory>

namespace clipper::dsp {

class VibeModel {
public:
    // Slots mirror every other pedal's positional triple so the chain ABI stays
    // pedal-agnostic. All THREE are real controls here.
    enum ParamId : int {
        PARAM_SPEED = 0,      // LFO rate, log mapped over S7's published span
        PARAM_INTENSITY = 1,  // S8: the fraction of the LFO reaching the lamp
        PARAM_MODE = 2,       // < 0.5 CHORUS, >= 0.5 VIBRATO (S5's switch)
        PARAM_COUNT
    };

    VibeModel();
    ~VibeModel();
    VibeModel(const VibeModel&) = delete;
    VibeModel& operator=(const VibeModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    // Clear the allpass memories and re-park the lamp and the four cells at the
    // operating point the current INTENSITY holds. Never solves an audio-path
    // network — there is none — and allocation-free.
    void reset();

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias).
    void process(const float* in, float* out, int numFrames);

    // OVERSAMPLED at 4×, and NOT for the usual reason — there is no aliasing here
    // to remove (the floor is flat in the factor). The 470 pF stage's corner sits
    // against Nyquist and the bilinear allpass's phase warps there, which moves a
    // notch by 18.90 % at 44.1 kHz; 4× takes that to 0.89 %. Full derivation and
    // the sibling comparison are in the banner and docs §67.7. The shipped default
    // is PINNED by the test (§58's could-not-fail finding).
    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // The photocell resistance the stages are dividing with right now (all four
    // cells share one lamp and one spec, so they agree — asserted).
    double cellResistanceOhms(int stage = 0) const;
    // The allpass corner of one stage, in Hz: 1/(2π·R_cell·C_stage).
    double stageCornerHz(int stage) const;
    // The lamp, so a test can attribute a measured asymmetry to it.
    double lampTemperatureK() const;
    double lampBrightness() const;
    // The composite exponent p = elExponent * cellGamma of the cell THIS MODEL
    // actually holds. Read from the shipped object, deliberately: a bar that
    // builds its own `OptoCell` to check the seam is a bar that cannot fail
    // (§66's lesson, found again here by perturbation P7).
    double cellCompositeExponent(int stage = 0) const;
    // The trap occupancy of cell `stage` — reported, because in THIS pedal the
    // cells are continuously lit and it does not behave the way it does in the
    // compressor (docs §67.8).
    double cellMemory(int stage = 0) const;
    // The smoothed mixer weights. `dryWeight()` reaches EXACTLY 0.0 in VIBRATO.
    double dryWeight() const;
    double wetWeight() const;
    // The lamp drive endpoints this model DERIVED from S6 (the published
    // illuminated resistance range) — the dim end and the bright end, in the
    // lamp's own normalized volts. Exposed so a test can quote the derivation
    // rather than re-typing it.
    double lampDriveDimVolts() const;
    double lampDriveBrightVolts() const;

    // Freeze the LFO at a fixed phase (0..1 of a cycle) so a STATIC network can
    // be measured, and let the lamp settle there; frozen=false resumes sweeping.
    // The direct counterpart of `PhaserModel::setFrozen`, so the two models can be
    // measured on exactly equal terms rather than through two different hooks.
    void setFrozen(bool frozen, double phase01 = 0.0);

    // Attribution / perturbation hooks. Neither is reachable from a parameter.
    // `setLampThermalMass(false)` gives an INSTANTANEOUS filament;
    // `setCellDynamics(false)` gives a memoryless photocell. They are separate
    // because §66's `setIdealOpAmp` removed two behaviours at once and made a bar
    // that could not fail.
    void setLampThermalMass(bool on);
    void setCellDynamics(bool on);

    // The largest absolute value held by any recursive state whose rest value is
    // ZERO. Covers the four allpass memories and NOTHING ELSE — see docs §67.8:
    // in this pedal the lamp and therefore the cells rest at a REAL OPERATING
    // POINT, which is the opposite side of ADR 006's scope rule from where the
    // same `OptoCell` sits inside the compressor.
    double maxAbsRestingState() const;

    // --- Statics the tests and the UI quote ---------------------------------
    static int numStages();
    // SOURCED (S1). Index order does not matter: a cascade of allpasses commutes,
    // so the magnitude response is independent of it — which removes a degree of
    // freedom that would otherwise have had to be guessed.
    static double stageCapFarads(int stage);
    // SPEED knob (0..1) -> LFO rate in Hz, log mapped over S7's published span.
    static double speedKnobToHz(double knob01);

private:
    void processChunk(const float* in, float* out, int numFrames);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_VIBE_MODEL_H
