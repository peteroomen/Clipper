// Clipper — portable DSP core.
//
// M1: a RAT-style diode-clipper distortion model. Three cascaded stages:
//
//   1. Gain stage      — variable pre-gain (DISTORTION knob) plus the RAT's
//                        characteristic pre-clip frequency shaping (rising gain
//                        toward mids/highs). Linear filter + gain; no WDF.
//   2. Clipper stage   — antiparallel silicon diode pair to ground, built with
//                        chowdsp_wdf (voltage source + series R || shunt C ->
//                        diode-pair root). 1N4148 SPICE device (Is = 2.52 nA,
//                        ideality n = 1.752): a soft silicon knee whose MEASURED
//                        settled ceiling runs +/-0.55 V at 1 V of drive to
//                        +/-0.74 V at 100 V. Until 2026-07-25 the ideality factor
//                        was 1.0 and the real ceiling was 0.32-0.43 V, 5-6 dB
//                        below what this comment claimed (audit finding 15; docs
//                        §36, ADR 008).
//   3. Tone / output   — one-pole passive low-pass whose cutoff the FILTER knob
//                        sweeps (clockwise = darker), then LEVEL as clean gain.
//
// M2 (antialiasing): only the nonlinear stage-2 clipper runs oversampled. The
// linear stages 1 and 3 stay at the base rate. setOversampling() selects a 1x /
// 2x / 4x / 8x polyphase-halfband cascade (default 4x); factor 1 reproduces the
// M1 signal path exactly. An experimental ADAA (antiderivative-antialiasing)
// memoryless clipper is available as an alternate stage 2 for measurement
// (setStage2Mode); the production default stays WDF + oversampling.
//
// This header is platform-free (C++17, no OS/browser/Emscripten includes). The
// heavy WDF members live behind a pimpl so this header does not drag the whole
// chowdsp_wdf template tree into every translation unit that merely wants the
// public API.

#ifndef CLIPPER_DSP_RAT_MODEL_H
#define CLIPPER_DSP_RAT_MODEL_H

#include <memory>

#include "clipper/dsp/OnePoleSmoother.h"

namespace clipper::dsp {

class RatModel {
public:
    // Parameter identifiers. All three params are normalized knob positions in
    // [0, 1]; the model maps each to physical units internally (see RatModel.cpp
    // for the exact mappings, mirrored in docs/DEVELOPMENT.md).
    enum ParamId : int {
        PARAM_DISTORTION = 0,  // pre-clip gain + shaping   (0..1 knob)
        PARAM_FILTER = 1,      // post-clip low-pass cutoff (0..1 knob; 1 = dark)
        PARAM_LEVEL = 2,       // output level              (0..1 knob)
        PARAM_COUNT
    };

    // Stage-2 nonlinearity selection. WDF is the production default; ADAA is an
    // experimental memoryless comparison path (see DiodeClipperADAA.h).
    enum Stage2Mode : int {
        STAGE2_WDF = 0,
        STAGE2_ADAA = 1,
    };

    RatModel();
    ~RatModel();

    // Non-copyable (owns WDF state via pimpl).
    RatModel(const RatModel&) = delete;
    RatModel& operator=(const RatModel&) = delete;

    // Configure for a sample rate and maximum block size; resets all state and
    // snaps smoothers to their current targets. maxBlockSize sizes the fixed
    // oversampling scratch buffers (worst case 8x); process() never exceeds it.
    void prepare(double sampleRate, int maxBlockSize);

    // Select the oversampling factor for the nonlinear stage: 1, 2, 4, or 8
    // (other values snap down to the nearest valid power of two). Default 4.
    // Takes effect immediately, resetting the oversampling filter state (a click
    // is possible on a live change; intended to be set before playing). Factor 1
    // reproduces the M1 (non-oversampled) path exactly. Must be called after
    // prepare() (or before — it is re-applied on the next prepare()).
    void setOversampling(int factor);
    int oversampling() const;

    // Round-trip latency introduced by the oversampling filters, in base-rate
    // samples (0 at factor 1). Useful for host plugin delay compensation later.
    int latencySamples() const;

    // Select the stage-2 nonlinearity (WDF default, or experimental ADAA).
    void setStage2Mode(int mode);
    int stage2Mode() const;

    // M6.5 measurement hook (NOT a user knob — the LM308 is the pedal's fixed
    // identity, like setStage2Mode is a measurement path). When true, the LM308
    // op-amp model (gain-tracking closed-loop bandwidth + slew limiting) is
    // BYPASSED, giving an ideal op-amp — used by the A/B render + aliasing
    // measurements to show the fizz the LM308 model removes. Default false
    // (LM308 modelled).
    void setIdealOpAmp(bool ideal);
    bool idealOpAmp() const;

    // Set a normalized parameter (id in ParamId, value in [0, 1]). Out-of-range
    // values are clamped. Applied with one-pole smoothing inside process().
    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out. in and out may alias.
    // Input is assumed guitar-level normalized float where 1.0f == 1.0 V for the
    // diode stage (documented reference; a hot humbucker peaks around ~0.3 V).
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Clear every recursive state — the three knob
    // smoothers (snapped onto their targets), the two stage-1 shaping one-poles, the
    // stage-3 low-pass, the LM308 model, the WDF cap and the oversampler histories.
    // Keeps the sample rate, the oversampling factor and the knob positions: this is
    // a state flush, not a re-prepare, and it allocates nothing.
    void reset();

private:
    // Process a single sub-block of at most maxBlockSize frames (the fixed
    // oversampling scratch size). process() chunks larger buffers into these.
    void processChunk(const float* in, float* out, int numFrames);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_RAT_MODEL_H
