// Clipper — portable DSP core.
//
// M0 scope: a trivial one-parameter gain stage with click-free one-pole
// smoothing. NO real DSP yet (that starts in M1). This file must stay
// zero-dependency: no platform, OS, browser, or Emscripten includes.
// It must compile natively with a plain C++17 compiler.

#ifndef CLIPPER_PROCESSOR_H
#define CLIPPER_PROCESSOR_H

namespace clipper {

// Parameter identifiers. Kept as a plain enum so the C ABI can pass ints.
enum ParamId : int {
    PARAM_GAIN = 0,  // linear gain, valid range [0, 2]
    PARAM_COUNT
};

class Processor {
public:
    Processor() = default;

    // Configure for a given sample rate and maximum block size. Resets state.
    // maxBlockSize is accepted for API symmetry with later milestones (which
    // will allocate scratch buffers); M0 does not need it.
    void prepare(double sampleRate, int maxBlockSize);

    // Set a parameter target. Smoothing (if any) is applied inside process().
    void setParameter(int paramId, float value);

    // Process numFrames of mono audio from in -> out. in and out may alias.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): snap the smoothed gain onto its target,
    // discarding the recursive state. Keeps the prepared rate/coefficient, so this
    // is not a re-prepare. Allocation-free.
    void reset();

    // Exposed for testing/introspection: the current (smoothed) gain value.
    float currentGain() const { return gain_; }

private:
    double sampleRate_ = 44100.0;

    // Gain smoothing (one-pole toward the target).
    float gainTarget_ = 1.0f;  // where the parameter wants to be
    float gain_ = 1.0f;        // current smoothed value
    float smoothingCoeff_ = 0.0f;  // per-sample one-pole coefficient
};

}  // namespace clipper

#endif  // CLIPPER_PROCESSOR_H
