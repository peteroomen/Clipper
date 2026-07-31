// Clipper — portable DSP core (post-v1.1: the lineup's first FILTER pedal).
//
// WahModel: a Dunlop GCB-95-class "Cry Baby" wah — a swept resonant bandpass in
// front of a real common-emitter transistor stage — whose POSITION is an ordinary
// automatable parameter AND which can be driven by its own envelope follower, so
// ONE resonant primitive covers both treadle-wah and Mu-Tron-style
// envelope-filter territory. Full research, sources and derivations: docs §58.
//
//   in ─► [buffer] ─► [ RESONANT TANK: f0(pos), Q(pos, voice) ] ─► ×kTankDivider
//                                                                      │
//                     ┌────────────────────────────────────────────────┘
//                     └─► 4× oversampled ─► [ BjtStage: common emitter ] ─► out
//                             │
//        [ envelope follower ] ┘  (SENSITIVITY > 0 moves the tank's position)
//
// ---------------------------------------------------------------------------
// THE SWEEP LAW IS DERIVED, NOT TASTED (docs §58.2)
// ---------------------------------------------------------------------------
// A wah's character is its sweep law, and it is neither linear nor a plain log.
// ElectroSmash names the mechanism verbatim: the pot "tunes the APPARENT
// CAPACITANCE" of the tank cap. So
//
//     f0(p) = f_LC / sqrt(M(p)),   M(p) = Ceff/C = 1 + A·u(p)
//     f_LC  = 1/(2π·sqrt(L·C))
//
// with L = 500 mH and C = 0.01 µF straight off the published component list:
//
//     f_LC = 2250.7908 Hz
//
// At full toe the bootstrap feeds back nothing (M = 1) so the peak sits at the
// BARE LC resonance — and 2250.79 Hz derived from two published component values
// lands +1.57 % from the CCRMA/Julius-Smith MEASURED toe of 2216.06 Hz (the
// digitised CryBaby in Faust's vaeffects.lib, fitted to three measured GCB-95
// responses). Two independent routes agreeing to 1.6 % is what says the mechanism
// is right.
//
// Pinning the published heel (450 Hz) fixes the span: A = (f_LC/450)² − 1 =
// 24.0175762. That leaves exactly ONE free parameter — the pot taper — and its
// honesty check is that the fitted value must look like a pot taper. It does:
// kTaperBeta = 23.537247 puts the wiper at 17.09 % of full resistance at half
// rotation, inside the documented audio/log-taper spec (an A-taper reads ~10 %
// at 50 % rotation; the GCB-95's "Hot Potz" is a log/custom-audio part).
//
// Result vs the measured reference: rms 0.46 %, worst +1.57 % (at the toe), over
// 2.322 octaves of travel, 450.0 → 2250.8 Hz.
//
// WHY THIS MATTERS: the circuit's own law is f ∝ 1/sqrt(1 + A·u). With a LINEAR
// control law it would spend 0.472 octaves in the heel half and 1.851 in the toe
// half — "all the wah in the last inch", the single most common way a modelled
// wah feels wrong even when the filter is right. The audio taper's compression
// and the circuit's square root very nearly cancel: shipped, the two halves are
// 1.147 and 1.176 octaves.
//
// ---------------------------------------------------------------------------
// Q ACROSS THE SWEEP — the topology decides, and the measurement agrees (§58.3)
// ---------------------------------------------------------------------------
// Fixed damping resistance Rp + fixed L + VARIABLE C is a parallel RLC:
//
//     BW = 1/(2π·Rp·Ceff) ∝ f0²      Q = Rp/(2π·f0·L) ∝ 1/f0
//
// so the resonance is SHARPEST at the heel and BROADEST at the toe. That is
// counter-intuitive and it is what the measurement says: CCRMA's fitted Q runs
// 8 → 4 → 2 heel → mid → toe, bandwidth exponent 1.870 against this topology's
// exact 2.000. The alternative (a series LCR in the degeneration path) predicts
// CONSTANT absolute bandwidth and is refuted by that measurement.
//
// Scale: fitting Q·f0 to the reference gives Rp = 12558.32 Ω. Derived vs measured
// Q: 8.883/8.000 (heel), 4.013/4.000 (mid), 1.776/2.000 (toe) — ±11 % at the ends,
// +0.3 % at the middle. The end error is the exponent difference (−1.000 derived
// vs −0.870 measured) and is REPORTED, not fitted away.
//
// VOICE is ElectroSmash's documented "Vocal Mod" as a knob: R7 (33 kΩ stock,
// modded to 39 k/68 k/100 k) sweeps log-centred on the stock value, changing the
// bell's WIDTH only — the centre frequency and the peak height are untouched by
// construction. Peak boost is the published +18 dB, constant across the sweep
// (the two published measurements quote ~18 dB at BOTH ends).
//
// ---------------------------------------------------------------------------
// Implementation notes that are load-bearing
// ---------------------------------------------------------------------------
// * The resonator is a TPT (Zavalishin) STATE-VARIABLE filter, not a direct-form
//   biquad: its two integrator states ARE the inductor current and the cap
//   voltage, and it is unconditionally stable under PER-SAMPLE coefficient
//   modulation — which is the whole point of a wah (the phaser's precedent,
//   docs §22). Its BP output peaks at exactly Q, so 2R·bp is a unity-peak
//   bandpass and the peak height is decoupled from Q by construction.
// * Denormals: the SVF is a SECOND-ORDER recursion, so the flush uses the
//   WHOLE-STATE form (docs §56.4b — the house one-liner does not converge above
//   first order).
// * The output stage is a real BjtStage (docs §24/§53) configured as the
//   GCB-95's MPSA18-class common emitter, inside a 4× oversampled domain like
//   every other nonlinear stage here. The staging between filter and transistor
//   has NO fitted constant: the stage's own small-signal gain G0 is MEASURED
//   from the model in prepare(), and kTankDivider = 10^(18/20)/G0 forces the
//   pedal's small-signal resonant boost to the published +18 dB. Where it starts
//   to bark is then a prediction, not a knob.
// * DEPARTURE, recorded rather than fitted around (ADR number needed): in the
//   real pedal the tank sits INSIDE the transistor's feedback path — filter and
//   gain are one stage. This model splits them, so the tank does not see the
//   clipped output and the resonance does not detune when the stage is slammed.
//
// ---------------------------------------------------------------------------
// AUTO (docs §58.5)
// ---------------------------------------------------------------------------
// SENSITIVITY hands the SAME f0(p) law an envelope-driven position:
//
//     posEff = pos + (1 − pos)·sens·env
//
// so SENS = 0 is EXACTLY the manual pedal (the envelope term is multiplied by
// zero — a test bar, not a claim), and with SENS > 0 POSITION becomes the resting
// frequency the note falls back to. There is deliberately no discrete mode hidden
// in a float slot: a control whose whole travel does nothing is dead UI.
// Follower constants come from Geofex's published figures: attack τ = 10 ms,
// release τ = 166.7 ms (3τ = the stated 500 ms drift-back).
//
// Trademark-safe (docs §17): wordmark "Weeper", model line FILTER Nº7 · TREADLE.
//
// Portable C++17, zero platform/OS/browser deps; heavy state behind a pimpl.
// Parameter ids mirror the shared 3-knob pedal shape so the chain/worklet/ABI
// drive it identically to every other pedal.

#ifndef CLIPPER_DSP_WAH_MODEL_H
#define CLIPPER_DSP_WAH_MODEL_H

#include <memory>

namespace clipper::dsp {

class WahModel {
public:
    // Normalized knob positions in [0, 1]. Positionally identical to
    // RatModel/SdModel so the chain ABI stays pedal-agnostic.
    enum ParamId : int {
        PARAM_POSITION = 0,     // treadle position: heel (0) → toe (1)
        PARAM_SENSITIVITY = 1,  // 0 = manual pedal; > 0 = the envelope sweeps it
        PARAM_VOICE = 2,        // the "Vocal Mod": R7 10.89 k…100 k, 0.5 = stock 33 k
        PARAM_COUNT
    };

    WahModel();
    ~WahModel();
    WahModel(const WahModel&) = delete;
    WahModel& operator=(const WahModel&) = delete;

    // Configure for a sample rate / max block. Solves the transistor stage's DC
    // operating point, MEASURES its small-signal gain (which sets kTankDivider),
    // and resets. Allocates; process() does not.
    void prepare(double sampleRate, int maxBlockSize);

    // Recovery seam (audit finding 1): clear the resonator + follower state and
    // re-park the transistor stage at its cached operating point. Never re-solves.
    void reset();

    void setPosition(float knob01);
    void setSensitivity(float knob01);
    void setVoice(float knob01);
    void setParameter(int paramId, float value);

    // 1 / 2 / 4 / 8. Only the transistor stage is oversampled; the resonator is
    // linear and runs at base rate.
    void setOversampling(int factor);
    int latencySamples() const;

    // Process numFrames of mono audio, in -> out (may alias).
    void process(const float* in, float* out, int numFrames);

    // --- Derived-law accessors (the tests compare RENDERS against these) -------
    // The bare LC resonance, 1/(2π√(LC)) from the published L and C (Hz).
    static double bareTankHz();
    // The shipped sweep law: POSITION (0..1) -> peak frequency (Hz).
    static double positionToHz(double position01);
    // The pot's normalised wiper law at a position (1 at heel, 0 at toe).
    static double wiperLaw(double position01);
    // The shipped Q law: (POSITION, VOICE) -> resonance Q.
    static double resonanceQ(double position01, double voice01);
    // VOICE (0..1) -> the modelled R7 in ohms (0.5 = the stock 33 kΩ).
    static double voiceToR7Ohms(double voice01);
    // The published resonant boost, in dB (constant across the sweep).
    static double peakBoostDb();
    // The CCRMA/Julius-Smith MEASURED reference the law is validated against —
    // present so a test can compare against it WITHOUT restating it, and so a
    // future re-derivation cannot silently drift from what it was checked on.
    static double referenceHz(double position01);
    static double referenceQ(double position01);

    // --- Measurement hooks (NOT user knobs) -----------------------------------
    // The tank position the model is using right now, after smoothing and the
    // envelope (0..1). Reading it is how the auto-mode test watches the sweep.
    double currentPosition() const;
    // The tank centre frequency the model is using right now (Hz).
    double currentCentreHz() const;
    // The envelope follower's current value (0..1).
    double currentEnvelope() const;
    // The measured small-signal gain of the transistor output stage (×), and the
    // divider derived from it. Both settle in prepare().
    double outputStageGain() const;
    double tankDivider() const;
    // Largest |value| across EVERY recursive state whose rest value is zero.
    // Must be EXACTLY 0.0 after a silent tail (docs §33 / ADR 006).
    double maxAbsRestingState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_WAH_MODEL_H
