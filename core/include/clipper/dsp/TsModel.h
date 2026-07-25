// Clipper — portable DSP core.
//
// v1.1: a TS808-style overdrive — the "Screamer" — the SYMMETRIC-clipping sibling
// of the M8 SD-1 (SdModel.h). The two pedals ARE the same Tube-Screamer-family
// topology (a soft feedback clipper inside a non-inverting op-amp stage with the
// shared ~720 Hz mid-hump), so this model is DELIBERATELY THIN: it owns a shared
// OverdriveEngine (OverdriveEngine.h) built from the TS config and forwards every
// call. The two differences from the SD-1, both in that config:
//
//   1. SYMMETRIC diodes (1-vs-1, both Vf ~= 0.60 V) instead of the SD-1's 2-vs-1
//      (0.95 / 0.50). => ODD harmonics only, the even-harmonic warmth ABSENT;
//      a smoother, glassier grind (the mirror of the SD-1's asymmetry).
//   2. DRIVE pot 500 kΩ instead of 1 MΩ. => max plateau 1 + 500k/4.7k ~= 107.4x
//      = +40.6 dB (vs the SD-1's +46.6 dB) — less top-end gain, the classic
//      lower-gain, mid-forward "transparent boost" voicing.
//
// Everything else — the 4.7k + 0.047 µF Zg leg (720 Hz mid-hump, the shared
// family trait), the 4558-class op-amp values, the TS tone/level topology, and
// the M2 oversampled ADAA clip — is IDENTICAL to the SD-1. See TsModel.cpp for
// the analytic circuit values and every approximation.
//
// Platform-free (C++17). Parameter ids mirror SdModel/RatModel's positional
// triple (0=DRIVE, 1=TONE, 2=LEVEL) so the worklet/ABI treat every pedal
// identically. Heavy state lives behind a pimpl.

#ifndef CLIPPER_DSP_TS_MODEL_H
#define CLIPPER_DSP_TS_MODEL_H

#include <memory>

namespace clipper::dsp {

class TsModel {
public:
    // Ids match SdModel/RatModel positionally so the chain ABI is pedal-agnostic.
    enum ParamId : int {
        PARAM_DRIVE = 0,  // feedback gain into the soft clipper (0..1 knob)
        PARAM_TONE = 1,   // post treble tilt (0=dark .. 1=bright; 0.5 flat)
        PARAM_LEVEL = 2,  // output level (0..1 knob)
        PARAM_COUNT
    };

    // ADAA is the production path (soft clip); NAIVE is the aliasing A/B path.
    enum ClipMode : int {
        CLIP_ADAA = 0,
        CLIP_NAIVE = 1,
    };

    TsModel();
    ~TsModel();

    TsModel(const TsModel&) = delete;
    TsModel& operator=(const TsModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    void setClipMode(int mode);
    int clipMode() const;

    // Measurement hook: bypass the 4558 op-amp model (ideal op-amp), for the
    // closed-loop-bandwidth / aliasing A/B. Default false.
    void setIdealOpAmp(bool ideal);
    bool idealOpAmp() const;

    // Measurement hook: the TS ships SYMMETRIC (both knees ~0.60 V); passing
    // `false` here forces the SD-1's ASYMMETRIC 2-vs-1 knees, the even-harmonic
    // reference the symmetry test contrasts against. Default true (TS symmetric).
    void setSymmetric(bool symmetric);
    bool symmetric() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1) — see OverdriveEngine::reset(). Clears every
    // recursive state without re-preparing; knobs and rate survive.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TS_MODEL_H
