// Clipper — portable DSP core.
//
// M8: a Boss SD-1 Super Overdrive model — the SOFT, ASYMMETRIC contrast to the
// RAT (RatModel.h). Same three-stage skeleton, but a fundamentally different
// nonlinearity: the SD-1 clips inside the op-amp FEEDBACK loop (2 diodes one
// way, 1 the other), so a clean signal component always passes and the knee is
// soft — the Tube-Screamer family behaviour.
//
//   1. Gain / shaping  — a non-inverting op-amp stage whose gain RISES above a
//                        ~720 Hz corner (the "mid-hump": unity below, up to
//                        ~+47 dB above at max DRIVE) and whose feedback diodes
//                        clip softly. Modelled as V_out = V_in + f(K * HP720(V_in)),
//                        with f the asymmetric soft-clipper (AsymSoftClipper.h)
//                        and K the DRIVE-set feedback gain. The 4558 op-amp's
//                        finite bandwidth / slew are reused from LM308Stage.h with
//                        4558 values.
//   2. (folded into 1)   the clipper is IN the feedback loop, not a separate
//                        shunt stage — the defining SD-1-vs-RAT difference.
//   3. Tone / output   — the SD-1 tone control (a first-order treble tilt,
//                        transparent at noon) then LEVEL as clean output gain.
//
// M2 antialiasing reused directly: only the nonlinear feedback clip runs
// oversampled (1x/2x/4x/8x, default 4x); the linear shaping/tone stay conceptually
// base-rate. The op-amp model and the ADAA soft-clip both live at the oversampled
// rate. See SdModel.cpp for the analytic circuit values and every approximation.
//
// Platform-free (C++17, no OS/browser/Emscripten). Heavy state lives behind a
// pimpl. Parameter ids intentionally mirror RatModel's positional triple so the
// worklet/ABI treat both pedals identically (id 0/1/2).

#ifndef CLIPPER_DSP_SD_MODEL_H
#define CLIPPER_DSP_SD_MODEL_H

#include <memory>

#include "clipper/dsp/OnePoleSmoother.h"

namespace clipper::dsp {

class SdModel {
public:
    // Normalized knob positions in [0, 1]; mapped to physical units internally.
    // Ids match RatModel positionally (0/1/2) so the chain ABI is pedal-agnostic.
    enum ParamId : int {
        PARAM_DRIVE = 0,  // feedback gain into the soft clipper (0..1 knob)
        PARAM_TONE = 1,   // post treble tilt (0=dark .. 1=bright; 0.5 flat)
        PARAM_LEVEL = 2,  // output level (0..1 knob)
        PARAM_COUNT
    };

    // Stage-nonlinearity selection: ADAA is the SD-1 production path (soft clip),
    // NAIVE is the aliasing comparison path used by the measurement tests.
    enum ClipMode : int {
        CLIP_ADAA = 0,
        CLIP_NAIVE = 1,
    };

    SdModel();
    ~SdModel();

    SdModel(const SdModel&) = delete;
    SdModel& operator=(const SdModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    // Oversampling factor for the nonlinear feedback clip (1/2/4/8, default 4).
    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    // Select the clip evaluation (ADAA default; NAIVE for alias measurement).
    void setClipMode(int mode);
    int clipMode() const;

    // Measurement hook (NOT a user knob): true bypasses the 4558 op-amp model
    // (ideal op-amp) — used by the closed-loop-bandwidth / aliasing A/B, exactly
    // like RatModel::setIdealOpAmp. Default false (4558 modelled).
    void setIdealOpAmp(bool ideal);
    bool idealOpAmp() const;

    // Measurement hook (NOT a user knob): true forces SYMMETRIC diodes (Vp==Vn),
    // the even-harmonic reference the asymmetry test compares against. Default
    // false (true 2-vs-1 asymmetry).
    void setSymmetric(bool symmetric);
    bool symmetric() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1) — see OverdriveEngine::reset(). Clears every
    // recursive state without re-preparing; knobs and rate survive.
    void reset();

private:
    void processChunk(const float* in, float* out, int numFrames);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_SD_MODEL_H
