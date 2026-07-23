// Clipper — portable DSP core (M5).
//
// AmpModel: a JC-120-inspired CLEAN amp, modeled as a strictly LINEAR block
// (volume + a bass/mid/treble tone stack + a bright switch). This is a
// deliberate scope choice from the roadmap: a solid-state clean platform can be
// honestly modeled linearly, and all the drive character comes from the pedal in
// front of it. No nonlinearity here, so no oversampling and no aliasing.
//
// The cab impulse response lives OUTSIDE this class (CabConvolver); the C ABI
// composes amp -> cab. AmpModel is mono, same call shape as RatModel.
//
// Tone stack (linear RBJ biquads; exact centers/ranges documented in
// AmpModel.cpp): bass low-shelf ~100 Hz, mid peak ~650 Hz, treble high-shelf
// ~3.5 kHz, each ranging around flat at knob = 0.5. Bright is a fixed +5 dB high
// shelf above ~3 kHz.
//
// Parameter smoothing: the tone gains (in dB) and the volume (linear) are
// one-pole smoothed; the biquad coefficients are recomputed from the smoothed dB
// values at a control rate (every 32 samples). Small, gradual coefficient steps
// => no zipper noise on a knob sweep (verified by test).
//
// Platform-free C++17 (no OS/browser/Emscripten includes).

#ifndef CLIPPER_DSP_AMP_MODEL_H
#define CLIPPER_DSP_AMP_MODEL_H

#include <memory>

namespace clipper::dsp {

class AmpModel {
public:
    // Normalized knob positions in [0, 1]. flat (tone-neutral) is 0.5 for the
    // three tone controls; BRIGHT is a 0/1 toggle.
    enum ParamId : int {
        PARAM_VOLUME = 0,   // audio-taper master, +6 dB max, 0.4=unity (0=silent)
        PARAM_BASS = 1,     // low-shelf,  +/-12 dB around 0.5
        PARAM_MIDDLE = 2,   // mid peak,   +/- 9 dB around 0.5
        PARAM_TREBLE = 3,   // high-shelf, +/-12 dB around 0.5
        PARAM_BRIGHT = 4,   // 0/1: fixed +5 dB bright high-shelf
        PARAM_COUNT
        // NB: the cab on/off toggle (AMP_PARAM_CAB) is a CHAIN-level id handled by
        // the C ABI wrapper (it enables/bypasses CabConvolver), not by AmpModel.
    };

    AmpModel();
    ~AmpModel();
    AmpModel(const AmpModel&) = delete;
    AmpModel& operator=(const AmpModel&) = delete;

    // Configure for a sample rate and maximum block size; resets state and snaps
    // smoothers to their current targets.
    void prepare(double sampleRate, int maxBlockSize);

    // Set a normalized parameter (id in ParamId, value in [0, 1]). Clamped.
    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out. in and out may alias.
    void process(const float* in, float* out, int numFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_AMP_MODEL_H
