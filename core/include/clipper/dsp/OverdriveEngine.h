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
// trait), followed by the pedal's own TONE network, a DC blocker, and LEVEL. The
// M2 antialiasing (oversampled feedback clip), the 4558-class op-amp band-limit +
// slew (LM308Stage), and the ADAA soft clip (AsymSoftClipper) are all here.
//
// The two family members differ ONLY in an OverdriveConfig:
//   - SD-1  : ASYMMETRIC diodes (Vp 0.95 / Vn 0.50 — 2-vs-1) => even-harmonic
//             warmth; DRIVE pot 1 MΩ => plateau +46.6 dB.
//   - TS808 : SYMMETRIC diodes (Vp == Vn ≈ 0.60 — 1-vs-1)   => odd harmonics
//             only; DRIVE pot 500 kΩ => plateau +40.6 dB.
//   - ...and, since docs §65, their TONE NETWORKS: the same topology
//     (OverdriveToneStack.h) with the two pedals' own component values. The
//     SD-1's input leg is 10 k / 18 n against a 10 k bias return, so its tone
//     stage sits 6.02 dB down behind an 884 Hz low-pass; the TS's is 1 k / 220 n
//     against the same 10 k, so 0.83 dB down behind a 723 Hz low-pass. That
//     low-pass is the half of the family's mid HUMP the model used not to have.
// Everything else (the mid-hump corner, the 4558 op-amp values, the level and
// output-coupling topology) is IDENTICAL — which is why they are one engine.
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
#include "clipper/dsp/OverdriveToneStack.h"
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

    // Stage 3 — the pedal's own TONE network (docs §65) + output coupling.
    // Until 2026-08-01 this was `tonePivotHz` + `toneMaxTiltDb`: a first-order
    // treble tilt, transparent at noon, explicitly documented in §11.6/§21 as an
    // approximation of "the published SD-1 tone response SHAPE". It had NO
    // low-pass in it, so the family's mid HUMP came out as a high SHELF. It is now
    // the netlist — see OverdriveToneStack.h for the topology and the derivation.
    OverdriveToneConfig tone;
    double dcBlockHz;  // output coupling-cap high-pass
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

    // Measurement hooks (NOT user knobs): the tone network's own state, so the
    // discretization bar and the denormal bar can address it directly.
    const OverdriveToneStack& toneStack() const { return tone_; }
    double maxAbsToneState() const { return tone_.maxAbsRestingState(); }

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
    OnePoleSmoother driveK_;     // feedback gain K (plateau - 1)
    OnePoleSmootherD tonePot_;   // the TONE pot's wiper fraction (docs §65)
    OnePoleSmoother level_;      // output gain

    // Stage 1 mid-hump high-pass (oversampled rate; hp = x - LP720).
    float midHumpCoef_ = 0.0f;
    float midLpState_ = 0.0f;

    // Stage 3 — the tone network (base rate) then the output coupling cap.
    OverdriveToneStack tone_;
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
