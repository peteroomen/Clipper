// Clipper — portable DSP core (M6.7-2).
//
// ReverbModel: a TRUE DISPERSIVE SPRING reverb standing in the JC-120's authentic
// spring-tank position — AFTER the preamp/tone/volume and BEFORE the stereo chorus
// split (see AmpModel), so the tail blooms in stereo through the chorus exactly like
// the real amp's spring tank feeding the stereo power section. It is MONO (the split
// comes after) and owned by AmpModel, which routes its one param here.
//
// SCOPE (M6.7-2): this replaces M6.7's Schroeder/Moorer "spring-flavored" comb bank
// with a Parker/Välimäki-style dispersive waveguide — a feedback loop whose delay is
// a long cascade of first-order allpass filters. That cascade IS the physics: a
// broadband transient exits smeared into a DOWNWARD-swept chirp ("boing"), and the
// frequency-dependent round-trip delay STRETCHES the resonant modes so they never
// stack into M6.7's metallic pitch. Same one-knob MIX interface (PARAM_REVERB=9),
// same position, same bit-exact-dry-at-0 guarantee. What makes it a real spring:
//   * two detuned dispersive springs in parallel (the dual-tank shimmer),
//   * gentle in-loop damping (a soft HF shelf + a low-cut) — NOT M6.7's steep 4.5 kHz
//     in-loop lid that made it sound "underwater",
//   * a transducer band-limit (~150 Hz .. 5.2 kHz) that keeps some air on top.
// See ReverbModel.cpp for the full topology and the metallic/underwater postmortem.
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
