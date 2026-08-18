// Clipper — portable DSP core.
//
// OverdriveEngine (v1.1): the shared Tube-Screamer-FAMILY overdrive engine
// extracted from the M8 SD-1 model so a second family member (the TS808-style
// "Screamer", v1.1) reuses ALL of its machinery instead of copy-pasting it. The
// engine IS the topology both pedals share:
//
//     V_out = V_in + f( K * HP720(V_in) )
//
// a non-inverting op-amp stage whose feedback diodes clip SOFTLY inside the loop
// (the "+V_in pedestal" — the clean signal always passes), whose gain RISES above
// a ~720 Hz "mid-hump" corner (the shared Zg = 4.7 k + 0.047 µF leg — the family
// trait), followed by a treble-tilt tone control, a DC blocker, and LEVEL. The
// M2 antialiasing (oversampled feedback clip), the 4558-class op-amp band-limit +
// slew (LM308Stage), and the ADAA soft clip (AsymSoftClipper) are all here.
//
// The two family members differ ONLY in an OverdriveConfig:
//   - SD-1  : ASYMMETRIC diodes (Vp 0.95 / Vn 0.50 — 2-vs-1) => even-harmonic
//             warmth; DRIVE pot 1 MΩ => plateau +46.6 dB.
//   - TS808 : SYMMETRIC diodes (Vp == Vn ≈ 0.60 — 1-vs-1)   => odd harmonics
//             only; DRIVE pot 500 kΩ => plateau +40.6 dB.
// Everything else (the mid-hump corner, the 4558 op-amp values, the tone/level
// topology) is IDENTICAL — which is exactly why the two pedals are one engine.
//
// ONE HONESTY NOTE ON THAT SENTENCE, added 2026-08-10 (docs §68). The shared
// "tone/level topology" is a MODELLING POSITION, not a sourced fact for both
// members: a TS808 netlist is reachable and annotates its level pot
// ("R15 is level pot (1-100k, log)"), and no SD-1 netlist or parts list could be
// reached from this container at all. So the SD-1's output-pot law is that
// documented inheritance — unchanged in KIND by the taper slice (it was the TS's
// identity map before and it is the TS's sourced law now), but it is a
// reconstruction and it is the largest gap that slice leaves. §57's rule applies
// verbatim: do not re-tune it toward a sound; find the schematic. The law is
// carried in OverdriveConfig so one pedal can move alone when it is found.
//
// SdModel and TsModel are thin wrappers that own an OverdriveEngine built from
// their config; the C ABI (sd_* / ts_*) and the worklet drive them identically.
// SD-1's numeric behaviour is BYTE-IDENTICAL to the pre-refactor SdModel (its
// config reproduces the former in-line constants exactly), so the M8 test suite
// passes unchanged.
//
// Platform-free (C++17, no OS/browser/Emscripten). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_OVERDRIVE_ENGINE_H
#define CLIPPER_DSP_OVERDRIVE_ENGINE_H

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/LM308Stage.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"

namespace clipper::dsp {

// The circuit-derived values that distinguish one family member from another.
// Everything a Tube-Screamer-family pedal needs to declare lives here; the engine
// holds no other per-pedal constants.
struct OverdriveConfig {
    // Stage 1 — mid-hump + DRIVE.
    double midHumpHz;    // 1/(2*pi*R_g*C_g); the shared 720.5 Hz (4.7k + 0.047uF)
    float driveMinDb;    // plateau gain (dB) at DRIVE knob 0 (NOT unity by design)
    float driveMaxDb;    // plateau gain (dB) at DRIVE knob 1 = 1 + Zf/R_g

    // Feedback diode knees (volts). Vp == Vn makes the clip SYMMETRIC (odd
    // harmonics only, the TS); Vp != Vn makes it ASYMMETRIC (even harmonics, the
    // SD-1). AsymSoftClipper's tanh asymptote sits at each knee.
    double diodeVp;      // positive-swing knee
    double diodeVn;      // negative-swing knee

    // 4558-class op-amp (shared: a FAST chip vs the RAT's slow LM308).
    double opAmpGbwHz;              // gain-bandwidth product
    double opAmpSlewVoltsPerSec;    // slew rate

    // Stage 3 — tone tilt + output coupling.
    double tonePivotHz;   // treble-tilt split frequency
    float toneMaxTiltDb;  // +/- tilt at the TONE extremes
    double dcBlockHz;     // output coupling-cap high-pass

    // Stage 3 — the LEVEL pot's own law (docs §68). The wiper law is the house
    // audio taper; the NETWORK then bends it, because this family's pot drives
    // an emitter-follower OUTPUT BUFFER and a loaded wiper does not deliver the
    // bare taper. Both numbers live in the config rather than in the engine for
    // one reason: **the SD-1's own schematic was not reachable** and it inherits
    // the TS's values, so a slice that DOES source the SD-1 must be able to move
    // one pedal without touching the other. Derivation in OutputPotTaper.h.
    double levelPotOhms;        // the pot's track resistance
    double levelWiperLoadOhms;  // load on the wiper (<= 0 means unloaded)
};

// The shared overdrive engine. Param ids and clip modes mirror SdModel's
// (0=DRIVE, 1=TONE, 2=LEVEL; clip 0=ADAA, 1=NAIVE) so the wrappers pass them
// straight through.
class OverdriveEngine {
public:
    enum ParamId : int { PARAM_DRIVE = 0, PARAM_TONE = 1, PARAM_LEVEL = 2 };
    enum ClipMode : int { CLIP_ADAA = 0, CLIP_NAIVE = 1 };

    explicit OverdriveEngine(const OverdriveConfig& cfg) : cfg_(cfg) {}

    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setClipMode(int mode);
    int clipMode() const { return clipMode_; }

    // Measurement hooks (NOT user knobs), identical to SdModel's.
    void setIdealOpAmp(bool ideal);
    bool idealOpAmp() const { return idealOpAmp_; }
    // symmetric=true collapses the diodes to the symmetric average of the config
    // knees; false restores the config (production) knees. Clears any override.
    void setSymmetric(bool symmetric);
    bool symmetric() const { return symmetric_; }
    // Directly force the diode knees (measurement hook), overriding config +
    // symmetric until the next setSymmetric. Lets the symmetric TS install the
    // SD-1's ASYMMETRIC reference knees (and vice versa) for the harmonic A/B.
    void setDiodeKnees(double vp, double vn);

    void setParameter(int paramId, float value);

    // Process numFrames mono, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Clear every recursive state — the three knob
    // smoothers (snapped onto their targets), the mid-hump high-pass, the tone-tilt
    // low-pass, the DC blocker, the op-amp model, the soft clipper and the
    // oversampler histories. Keeps rate / factor / knobs / diode knees: a state
    // flush, not a re-prepare. Allocates nothing.
    void reset();

private:
    void processChunk(const float* in, float* out, int numFrames);
    void applyKnees();
    void reprepareStage2();

    OverdriveConfig cfg_;

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int osFactor_ = 4;
    int clipMode_ = CLIP_ADAA;
    bool idealOpAmp_ = false;
    bool symmetric_ = false;
    bool kneeOverride_ = false;  // measurement: explicit knees win over config
    double ovVp_ = 0.0, ovVn_ = 0.0;

    // Smoothed physical params.
    OnePoleSmoother driveK_;    // feedback gain K (plateau - 1)
    OnePoleSmoother toneTilt_;  // treble tilt linear gain
    OnePoleSmoother level_;     // output gain

    // Stage 1 mid-hump high-pass (oversampled rate; hp = x - LP720).
    float midHumpCoef_ = 0.0f;
    float midLpState_ = 0.0f;

    // Stage 3 tone tilt low-pass (base rate) + DC blocker state.
    float toneCoef_ = 0.0f;
    float toneLpState_ = 0.0f;
    double dcR_ = 0.0;
    float dcX1_ = 0.0f, dcY1_ = 0.0f;

    // M2 oversampling for the feedback clip.
    Oversampler os_;

    // 4558 op-amp (bandwidth + slew) + the ADAA soft clipper, both oversampled.
    LM308Stage opAmp_;
    AsymSoftClipper clip_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OVERDRIVE_ENGINE_H
