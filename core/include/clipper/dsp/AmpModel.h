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
        PARAM_COUNT = 5,    // number of contiguous tone params; ALSO the id the C
                            // ABI reserves for the CHAIN-level cab toggle
                            // (AMP_PARAM_CAB == 5), which is NOT handled here.
        // --- M6.3 chorus/vibrato (routed to the owned ChorusModel) ------------
        // Placed ABOVE the reserved cab id (5) so the cab id stays 5 forever and
        // the ABI is purely additive. These ARE handled by AmpModel::setParameter.
        PARAM_CHORUS_SPEED = 6,  // 0..1 knob, log map ~0.15..8 Hz
        PARAM_CHORUS_DEPTH = 7,  // 0..1 knob, sweep amount (0..~1.5 ms peak)
        PARAM_CHORUS_MODE = 8,   // 0=off, 1=chorus, 2=vibrato (NOT clamped to 0..1)
        // --- M6.7 spring-flavored reverb (routed to the owned ReverbModel) ------
        // Appended additively above the chorus ids. Sits in the JC-120 spring-tank
        // position (preamp -> REVERB -> chorus split), so it is applied inside
        // processStereo() BEFORE the chorus split. 0..1 equal-power wet MIX; 0 is a
        // bit-exact dry passthrough. Handled by AmpModel::setParameter.
        PARAM_REVERB = 9,        // 0..1 knob, reverb wet/dry mix (0 = dry)
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

    // Process numFrames of mono audio, in -> out. in and out may alias. This is
    // the tone stack + volume ONLY (no chorus) — the mono voice of the amp, kept
    // for the pre-M6.3 mono path and the offline tone/volume tests.
    void process(const float* in, float* out, int numFrames);

    // M6.3 STEREO entry point: tone stack + volume, then (M6.7) the spring-flavored
    // REVERB in its authentic JC-120 spring-tank position, then the chorus/vibrato
    // split into a stereo pair (outL/outR must be distinct buffers; in may alias
    // neither) — so the reverb tail blooms in stereo THROUGH the chorus, like the
    // hardware tank feeding the stereo section. With reverb == 0 (default) the
    // reverb is a bit-exact passthrough; with the chorus mode OFF too, outL == outR
    // == the mono voice, so this degrades to two copies of process().
    void processStereo(const float* in, float* outL, float* outR, int numFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_AMP_MODEL_H
