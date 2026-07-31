// Clipper — portable DSP core.
//
// M13.1: the "Squash" compressor — an MXR Dyna Comp / Ross-style CA3080 OTA
// compressor, and the first DYNAMICS processor in the lineup. Two knobs, because
// the real box has two: SUSTAIN and LEVEL. There is no attack knob and no
// release knob and no ratio knob, and that is not an omission — the detector's
// component values ARE the pedal, and they are fixed.
//
// This is a THIN WRAPPER, exactly like SdModel/TsModel over OverdriveEngine
// (docs §21): it owns a CompressorEngine built from the Dyna Comp config and
// forwards every call. The optical / LA-2A-style voice (M13.3) is a SECOND
// wrapper over the SAME engine with a different config — see the M13.3 seam
// banner at the top of CompressorEngine.h before starting that slice.
//
// Three things a reader should know before they touch the constants (docs §59):
//
//  1. It is FEED-BACK, not feed-forward. The detector taps the gain cell's
//     OUTPUT. Online writeups get this wrong often enough that the model carries
//     it as a config field with a measurement hook, and the test suite proves it
//     is load-bearing rather than asserting it in a comment.
//
//  2. SUSTAIN IS NOT A THRESHOLD. It is a rheostat in the control-current path,
//     so it sets the gain cell's IDLE transconductance — i.e. how much gain the
//     loop has to give away before it settles. The threshold is a fixed
//     base-emitter drop in the detector and does not move with any knob. Turning
//     SUSTAIN up therefore buys more compression AND more make-up gain AND more
//     noise, all from the same turn. That is the pedal, not a defect.
//
//  3. Honest expectations, measured and reported in docs §59, NOT designed out:
//     it squashes the pick attack, it thins the low end (there are three
//     high-passes between input and output, the smallest coupling cap being
//     10 nF), and at the top of the SUSTAIN travel it has ~40 dB of idle gain,
//     which is where a real one's famous hiss comes from. None of that is
//     "fixed" here — a better-behaved compressor would be a different pedal.
//
// Platform-free (C++17). Convention: 1.0f == 1.0 V. Parameter ids mirror the
// shared positional triple so the chain ABI stays pedal-agnostic; slot 1 is
// carried and unused (the phaser precedent).

#ifndef CLIPPER_DSP_COMP_MODEL_H
#define CLIPPER_DSP_COMP_MODEL_H

#include <memory>

namespace clipper::dsp {

class CompModel {
public:
    enum ParamId : int {
        PARAM_SUSTAIN = 0,  // the OTA's idle bias current (NOT a threshold)
        PARAM_UNUSED = 1,   // carried, unused — a compressor has two knobs
        PARAM_LEVEL = 2,    // output pot
        PARAM_COUNT
    };

    CompModel();
    ~CompModel();

    CompModel(const CompModel&) = delete;
    CompModel& operator=(const CompModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clears recursive state and re-parks at the
    // cached quiescent point. Never re-solves.
    void reset();

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // Flip the detector tap to feed-forward. This is how `clipper_comp_tests`
    // proves the feed-back claim by measurement.
    void setDetectorTap(bool fromOutput);
    bool detectorFromOutput() const;
    // The gain cell's control current (amps) and the envelope node (volts) right
    // now, so a test can read gain reduction directly instead of inferring it.
    double controlCurrent() const;
    double envelopeVolts() const;
    double cellGainAtDc() const;
    // Largest absolute value in any recursive state whose rest value is ZERO.
    // The envelope node is deliberately excluded — it rests at 8.5786 V (ADR 006).
    double maxAbsRestingState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_COMP_MODEL_H
