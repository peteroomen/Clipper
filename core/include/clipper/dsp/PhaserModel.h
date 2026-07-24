// Clipper — portable DSP core (v1.1 item 3 — the script-era 4-stage phaser).
//
// PhaserModel: the "Ninety" — a homage to the script-logo MXR Phase 90. FOUR
// cascaded FIRST-ORDER allpass sections whose corner frequency sweeps together
// under one LFO; the dry signal is summed 50/50 with the 4-allpass ("wet")
// output. Because each first-order allpass rotates phase 0 → −180° as frequency
// rises, four in series rotate 0 → −720°, passing through −180° and −540°: the
// dry+wet sum therefore has EXACTLY TWO moving notches (4 stages → 2 notches),
// the signature Phase-90 comb that whooshes as the corner sweeps.
//
//   in ─┬────────────────────────────────────────────► 0.5·dry ─┐
//       └─► [AP₁]─►[AP₂]─►[AP₃]─►[AP₄] ──────────────► 0.5·wet ─┴─► out
//
// Circuit lineage (circuit-informed, not SPICE): the original sweeps four JFETs
// used as voltage-variable resistors; their drain-source resistance rides over
// roughly a decade, moving each allpass corner across ~200 Hz…2 kHz, which puts
// the two notches in the ~80 Hz…4.8 kHz region over a cycle. ONE knob: SPEED
// (0.06…8 Hz, log — the original's famously wide range). Depth is FIXED
// (script-authentic — the script Phase 90 has no depth control). There is NO
// feedback/regeneration path: that is the block-logo ("Script switch"/reissue)
// addition; the SCRIPT voicing modelled here omits it (documented in the ledger,
// docs §20).
//
// LFO SHAPE — a slightly-shark-toothed (rounded) TRIANGLE, not a sine. The
// corner is modulated in LOG-frequency; a triangle in log-fc sweeps the notches
// at a CONSTANT musical rate (equal time per octave rising and falling — the
// even, "searching" Phase-90 movement), whereas a sine dwells at the turnarounds
// and reads as a phaser that "sits" at the extremes. A pure triangle, though,
// reverses instantaneously at its peaks — a corner in d(fc)/dt that ticks. The
// real pedal's op-amp LFO integrator + the JFET's soft R(Vgs) law round those
// peaks, so we blend a little aligned cosine into the triangle (≈15%): the
// "shark tooth" is the constant-slope body with rounded reversals. Justified by
// (1) the constant-per-octave sweep vs a sine, and (2) the no-zipper spectral-
// floor test (the rounded reversal leaves no turnaround tick). See docs §20.
//
// LINEAR time-varying: no nonlinearity, so no clipping and NO oversampling. The
// only aliasing risk is coefficient stepping (zipper) as the corner sweeps; the
// allpass coefficients are recomputed PER SAMPLE from the continuous LFO, so a
// fast sweep leaves a clean spectral floor (asserted by the tests).
//
// Per-stage corner DETUNE (±1.5%): a fixed, deterministic component-tolerance
// spread so the four notches of the real (never perfectly matched) JFET stages
// do not stack into a single infinitely-deep null — a touch of realism that
// keeps the notches finite but still >20 dB deep. A measurement hook disables it
// for the clean analytic comparison.
//
// Platform-free C++17 (no OS/browser/Emscripten includes); heavy state behind a
// pimpl. Parameter ids mirror the shared 3-knob pedal shape (RatModel/SdModel)
// so the chain/worklet/ABI drive it identically: slot 0 = SPEED, slots 1/2 are
// carried but UNUSED (the pedal face shows ONE big knob).

#ifndef CLIPPER_DSP_PHASER_MODEL_H
#define CLIPPER_DSP_PHASER_MODEL_H

#include <memory>

namespace clipper::dsp {

class PhaserModel {
public:
    // Normalized knob positions in [0, 1]. Ids match RatModel/SdModel positionally
    // so the chain ABI is pedal-agnostic; only slot 0 does anything here.
    enum ParamId : int {
        PARAM_SPEED = 0,     // LFO rate (0..1 knob -> 0.06..8 Hz, log)
        PARAM_UNUSED_1 = 1,  // carried for the shared 3-param shape; ignored
        PARAM_UNUSED_2 = 2,  // carried for the shared 3-param shape; ignored
        PARAM_COUNT
    };

    PhaserModel();
    ~PhaserModel();
    PhaserModel(const PhaserModel&) = delete;
    PhaserModel& operator=(const PhaserModel&) = delete;

    // Configure for a sample rate; resets all state and snaps the rate smoother.
    void prepare(double sampleRate);

    // Reset the allpass state + LFO phase without reallocating.
    void reset();

    // speed is a 0..1 knob position (clamped). setParameter routes slot 0 here.
    void setSpeed(float knob01);
    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias).
    void process(const float* in, float* out, int numFrames);

    // --- Measurement hooks (NOT user knobs) -----------------------------------
    // Freeze the LFO at a fixed phase (0..1 of a cycle) so a static notch/response
    // can be measured; frozen=false resumes normal sweeping. When frozen, the
    // corner is pinned to the value the LFO would hold at that phase.
    void setFrozen(bool frozen, double phase01 = 0.0);
    // Enable/disable the per-stage component-tolerance detune (default ON). Off
    // gives four identical stages — the clean reference for the analytic notches.
    void setDetune(bool on);
    // The shared allpass corner (Hz) the model is using right now (before detune).
    double currentCornerHz() const;
    // The fixed per-stage detune factor applied to the shared corner (kNumStages).
    static double stageDetune(int stage);
    static int numStages();
    // SPEED knob (0..1) -> LFO rate in Hz (the log map endpoints the tests pin).
    static double speedKnobToHz(double knob01);
    // Corner (Hz) the (rounded-triangle) LFO holds at a given cycle phase (0..1).
    static double cornerHzAtPhase(double phase01);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_PHASER_MODEL_H
