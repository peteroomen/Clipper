// Clipper — portable DSP core (M6.7).
//
// ReverbModel: an ALGORITHMIC, spring-FLAVORED reverb standing in the JC-120's
// authentic spring-tank position — AFTER the preamp/tone/volume and BEFORE the
// stereo chorus split (see AmpModel), so the tail blooms in stereo through the
// chorus exactly like the real amp's spring tank feeding the stereo power section.
// It is MONO (the split comes after) and owned by AmpModel, which routes its one
// param here.
//
// SCOPE (M6.7): this is a compact Schroeder/Moorer network TUNED to read as
// "spring-ish" — NOT the full dispersive-waveguide spring (that is M6.7-2, which
// swaps THIS core out behind the same one-knob interface). What makes it spring-
// flavored rather than a bright plate:
//   * a band-limited voice (the loop + output are cut to roughly a real spring
//     transducer's ~150 Hz .. 4.5 kHz passband — thin, not full-range),
//   * a FIXED medium decay (~1.5 s; "dwell" is fixed, matching the real amp whose
//     single REVERB control is a MIX, not a time),
//   * a short all-pass "chirp" cascade that hints at the spring's dispersive boing
//     without modeling the waveguide (documented approximation).
//
// ONE parameter: reverb (0..1), an EQUAL-POWER dry/wet MIX (like the amp's single
// REVERB knob). reverb == 0 is a BIT-EXACT dry passthrough (asserted): the network
// is skipped entirely and the input is copied through, so adding this block leaves
// a reverb-off rig unchanged sample-for-sample.
//
// Deterministic and allocation-free in process() (all buffers sized in prepare()).
// Platform-free C++17 (no OS/browser/Emscripten includes); same call-shape family
// as ChorusModel, owned BY AmpModel.

#ifndef CLIPPER_DSP_REVERB_MODEL_H
#define CLIPPER_DSP_REVERB_MODEL_H

#include <memory>

namespace clipper::dsp {

class ReverbModel {
public:
    ReverbModel();
    ~ReverbModel();
    ReverbModel(const ReverbModel&) = delete;
    ReverbModel& operator=(const ReverbModel&) = delete;

    // Configure for a sample rate; allocates the delay lines, resets state, and
    // snaps the mix smoother to its current target.
    void prepare(double sampleRate);

    // Reset every delay line + filter to silence without reallocating.
    void reset();

    // Set the wet/dry MIX from a 0..1 knob (clamped). 0 = fully dry (bit-exact
    // passthrough), 1 = fully wet. Equal-power blend. Smoothed so a knob move never
    // clicks.
    void setMix(float knob01);

    // Process numFrames of MONO audio, in -> out. in and out MAY alias (the dry
    // sample is read before the wet is written). With the mix idle at 0 this is a
    // bit-exact copy of in into out (the network is skipped).
    void process(const float* in, float* out, int numFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_REVERB_MODEL_H
