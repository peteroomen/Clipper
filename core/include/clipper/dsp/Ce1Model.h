// Clipper — portable DSP core.
//
// M13.7: the Boss CE-1 Chorus Ensemble (1976) — Boss's first pedal, the world's
// first chorus pedal, and pedal type `chorus`.
//
// ===========================================================================
// READ THIS FIRST: THIS IS THE JC-120 CHORUS RE-VOICED, AND THAT IS THE POINT
// ===========================================================================
//
// The CE-1's literal design origin is the Roland JC-120 Jazz Chorus amplifier's
// chorus/vibrato circuit, lifted into a floor box the year after the amp shipped.
// This project ALREADY models that circuit — `ChorusModel`, docs §11.2 — and this
// pedal OWNS one. The swept BBD delay line, the 4-point Lagrange interpolator,
// the LFO, the ~8 ms control smoothing: all of it is the amp's validated code,
// unmodified, driven through a voicing seam.
//
// So the honest description of this model is "the JC-120 chorus with the CE-1's
// own knob ranges, its own LFO waveform, and its own MONO output". There are
// exactly THREE differences from the amp's voicing, each of them sourced, and
// they are enumerated in docs §62.2. Nothing else was invented to make it look
// like more work.
//
//   1. THE KNOB RANGES ARE NARROWER AND SPLIT BY MODE (docs §62.3).
//      Chorus mode runs 1.0 .. 3.0 Hz; vibrato mode runs 3.2 .. 11.6 Hz. Those
//      are measured figures from a teardown of a real unit, and they are
//      internally consistent in a way that is worth noticing: the FASTEST chorus
//      rate (3.0) is slower than the SLOWEST vibrato rate (3.2), which is why
//      players describe the two modes as different effects rather than the same
//      effect at two speeds. The amp's single knob spans 0.15 .. 8 Hz across
//      both. The delay window is 3 .. 5 ms at minimum DEPTH and 3 .. 7 ms at
//      maximum — a sweep amplitude of 1 .. 2 ms against the amp's 0 .. 3.5 ms.
//      Note the floor: the CE-1's DEPTH knob at ZERO still modulates. That is
//      the circuit, not an oversight.
//
//   2. THE LFO WAVEFORM IS PER MODE (docs §62.4). Triangle in chorus, sine in
//      vibrato. The amp's model uses sine for both, deliberately and with its
//      reasoning written down (a triangle DELAY sweep makes the PITCH deviation
//      a SQUARE wave — two fixed detunings with an abrupt reversal). The CE-1
//      does the triangle anyway, and it gets away with it in chorus mode because
//      what you hear there is the COMB, not the pitch. In vibrato mode, where
//      you do hear the pitch, the real circuit switches to a sine.
//
//   3. IT IS MONO, AND THAT IS NOT A COMPROMISE (docs §62.5). This is the
//      difference that matters most, and it is the opposite of what you would
//      expect from a mono host. The CE-1 has BOTH a stereo pair (dry on one
//      jack, wet on the other) AND a MONO jack that mixes wet and dry
//      internally. The pedal chain here is mono, so we ship the factory mono
//      output — a real output on a real pedal, not a sum we invented.
//      The consequence is audible and is the whole reason this pedal is not the
//      amp's chorus: mixing dry and wet ELECTRICALLY produces COMB FILTERING in
//      the output. The amp's chorus never combs, because `ChorusModel` in
//      CHORUS mode is a SPLIT — L is the bit-exact dry signal, R is the wet one,
//      and the comb only ever happens acoustically in the room. Here it happens
//      in the box.
//      WHAT IS LOST: the stereo image. A real CE-1 into two amps is wide; this
//      is not, and no mono model can be. Documented, not papered over.
//
// ---------------------------------------------------------------------------
// THE ONE DELIBERATE DEPARTURE FROM THE REAL CONTROL LAYOUT (ADR: number needed)
// ---------------------------------------------------------------------------
// The real CE-1's front panel is asymmetric: CHORUS mode has ONE knob
// (INTENSITY, which is ganged — it moves rate and depth together, and the rate
// is the part you notice), while VIBRATO mode has TWO (RATE and DEPTH). Ganging
// them here would mean that in chorus mode one of our three slots does NOTHING,
// and this codebase forbids dead UI in terms. So RATE and DEPTH are independent
// in BOTH modes; the mode selects which RATE RANGE and which WAVEFORM apply.
// A player who wants the factory chorus feel moves RATE alone. This is recorded
// as a departure rather than smuggled in — see docs §62.6.
//
// ---------------------------------------------------------------------------
// WHAT IS NOT MODELLED, NAMED RATHER THAN GUESSED (docs §62.7)
// ---------------------------------------------------------------------------
// The CE-1 is a BUCKET-BRIGADE circuit: an MN3002 512-stage BBD behind a
// compander, an anti-alias filter in front of it and a reconstruction filter
// after it. Sources describe the result as "dark, full and throaty". NONE of
// that is modelled here — the wet path is a clean Lagrange-interpolated delay,
// exactly as the amp's chorus is, and it inherits that approximation rather than
// adding a new one. No corner frequency, compander ratio or BBD bandwidth figure
// could be sourced, and inventing one to chase an adjective is precisely the
// fitting this project forbids. It is the largest known gap on this voice.
//
// Platform-free (C++17). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_CE1_MODEL_H
#define CLIPPER_DSP_CE1_MODEL_H

#include <memory>

namespace clipper::dsp {

class Ce1Model {
public:
    enum ParamId : int {
        PARAM_RATE = 0,   // LFO speed; the RANGE depends on PARAM_MODE
        PARAM_DEPTH = 1,  // sweep amplitude 1..2 ms (never zero — see above)
        PARAM_MODE = 2,   // < 0.5 = CHORUS, >= 0.5 = VIBRATO
        PARAM_COUNT
    };

    // The mode switch is genuinely DISCRETE on the real pedal (a footswitch
    // between two circuits), so unlike §58's wah SENSE it is not smuggled into a
    // continuous law. The float slot carries a threshold and both ends are
    // asserted by the test suite.
    enum Mode : int {
        MODE_CHORUS = 0,
        MODE_VIBRATO = 1,
    };

    Ce1Model();
    ~Ce1Model();

    Ce1Model(const Ce1Model&) = delete;
    Ce1Model& operator=(const Ce1Model&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    // Accepted and ignored: this is a LINEAR time-varying effect. There is no
    // nonlinearity anywhere in it, so oversampling buys nothing and costs
    // latency. Same call shape and same answer as the phaser (docs §12).
    void setOversampling(int factor);
    int oversampling() const;
    // Always 0. The modulated delay IS the effect, not a processing latency, and
    // it is not a constant that could be compensated anyway.
    int latencySamples() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias).
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clears the delay line, LFO phase and every
    // smoother. Never reallocates and never re-solves.
    void reset();

    // --- Measurement hooks (NOT user controls) ------------------------------
    int mode() const;
    // Settled LFO rate in Hz for the current knob + mode.
    double rateHz() const;
    // Settled base delay and sweep amplitude, milliseconds.
    double baseDelayMs() const;
    double sweepMs() const;
    // Settled dry and wet mix weights. In VIBRATO the dry weight is EXACTLY 0.0
    // — that is the property the null test pins.
    double dryGain() const;
    double wetGain() const;
    // Largest absolute value in any recursive state whose rest value is ZERO
    // (ADR 006). Covers the delay line; the mix-weight smoothers rest at their
    // targets and snap exactly (OnePoleSmoother's own guard).
    double maxAbsRestingState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CE1_MODEL_H
