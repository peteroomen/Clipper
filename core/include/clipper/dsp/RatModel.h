// Clipper — portable DSP core.
//
// M1: a RAT-style diode-clipper distortion model. Three cascaded stages:
//
//   1. Gain stage      — variable pre-gain (DISTORTION knob) plus the RAT's
//                        characteristic pre-clip frequency shaping (rising gain
//                        toward mids/highs). Linear filter + gain; no WDF.
//   2. Clipper stage   — antiparallel silicon diode pair to ground, built with
//                        chowdsp_wdf (voltage source + series R || shunt C ->
//                        diode-pair root). Hard clip at ~+/-0.6 V.
//   3. Tone / output   — one-pole passive low-pass whose cutoff the FILTER knob
//                        sweeps (clockwise = darker), then LEVEL as clean gain.
//
// NO oversampling / antialiasing here — that is M2. High-gain settings will
// alias ("fizz"); that is expected and accepted for this milestone.
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

    RatModel();
    ~RatModel();

    // Non-copyable (owns WDF state via pimpl).
    RatModel(const RatModel&) = delete;
    RatModel& operator=(const RatModel&) = delete;

    // Configure for a sample rate and maximum block size; resets all state and
    // snaps smoothers to their current targets. maxBlockSize is accepted for API
    // symmetry (no scratch buffers are needed yet).
    void prepare(double sampleRate, int maxBlockSize);

    // Set a normalized parameter (id in ParamId, value in [0, 1]). Out-of-range
    // values are clamped. Applied with one-pole smoothing inside process().
    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out. in and out may alias.
    // Input is assumed guitar-level normalized float where 1.0f == 1.0 V for the
    // diode stage (documented reference; a hot humbucker peaks around ~0.3 V).
    void process(const float* in, float* out, int numFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_RAT_MODEL_H
