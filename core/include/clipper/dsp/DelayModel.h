// Clipper — portable DSP core.
//
// M13.4: the "Echoman" — an EHX Deluxe Memory Man-class BBD ANALOG DELAY, and
// the lineup's first DELAY pedal (a whole new DSP family; see DelayLine.h for
// the primitive it leaves behind). Three knobs, because the three that matter on
// the real box are DELAY / FEEDBACK / BLEND.
//
// The full research record — what was sourced, what was NOT, and every constant's
// provenance — is docs/DEVELOPMENT.md §60. The short version of what makes this
// a bucket-brigade delay rather than a digital line with a lowpass on it:
//
//  1. THE BBD IS A SAMPLED DEVICE AND THE DELAY KNOB MOVES ITS CLOCK.
//     Two MN3005s in series is 8192 stages; a charge packet advances one stage
//     per clock HALF-cycle, so the device holds 4096 samples and the delay is
//     `4096 / f_clk` seconds. The DELAY pot varies the CD4047 astable, i.e. the
//     CLOCK — nothing else in the delay path moves. The anti-alias and
//     reconstruction filters around the device are FIXED. That asymmetry is the
//     whole reason a Memory Man's repeats get darker as you turn the time up,
//     and a fixed filter on a variable-length digital line cannot reproduce it.
//
//  2. THE COMPANDER IS PART OF THE SOUND, NOT A NOISE FIX.
//     An NE570/NE571 compresses 2:1 into the BBD and expands 1:2 out of it. Each
//     half has its OWN full-wave rectifier looking at its OWN input, so on the
//     way out the expander is tracking the DELAYED, band-limited, degraded
//     signal — the two envelopes cannot agree, and that disagreement is why the
//     repeats breathe. It is not omitted and compensated with a gain.
//
//  3. DEGRADATION IS CUMULATIVE THROUGH THE FEEDBACK LOOP.
//     FEEDBACK returns the expander's output to the compressor's input, so every
//     repeat is band-limited, companded and charge-smeared AGAIN. Repeat 5 has
//     been through the device five times. One filter in the loop of an otherwise
//     clean line does not converge to the same place, and the test suite pins the
//     monotone HF decay across repeats 1 / 2 / 5 as THE bucket-brigade property.
//
// Out of scope, deliberately: the DMM's chorus/vibrato section (a parallel slice
// ships the CE-1), tape echo, and any digital delay. Trademark-safe on every user
// surface — the wordmark is "Echoman", there is no EHX or Memory Man text.
//
// Platform-free (C++17). Convention: 1.0f == 1.0 V. Parameter ids mirror the
// shared positional triple so the chain ABI stays pedal-agnostic.

#ifndef CLIPPER_DSP_DELAY_MODEL_H
#define CLIPPER_DSP_DELAY_MODEL_H

#include <memory>

namespace clipper::dsp {

class DelayModel {
public:
    enum ParamId : int {
        PARAM_DELAY = 0,     // the BBD CLOCK. 30..550 ms, linear in rotation.
        PARAM_FEEDBACK = 1,  // repeats. The loop re-enters the compressor.
        PARAM_BLEND = 2,     // wet level. 0 is BIT-IDENTICAL to bypass.
        PARAM_COUNT
    };

    DelayModel();
    ~DelayModel();

    DelayModel(const DelayModel&) = delete;
    DelayModel& operator=(const DelayModel&) = delete;

    // Sizes the delay line for the LONGEST delay at the HIGHEST oversampling
    // factor this instance can ever be set to, so `setOversampling()` and every
    // DELAY move afterwards are allocation-free.
    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const;

    // ZERO. The dry path never enters the oversampled domain (that is what keeps
    // BLEND 0 bit-identical), so the pedal adds no latency to the dry signal. The
    // oversampler's own group delay lands on the WET path only and shows up as a
    // constant offset on the echo times — measured and reported in docs §60.4,
    // not hidden.
    int latencySamples() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clears the delay line, the filters, the
    // compander envelopes and the clock phase, and re-snaps the knob smoothers.
    // Never reallocates and never re-solves.
    void reset();

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // The BBD clock in Hz right now (smoothed), and the delay it implies. These
    // are what `clipper_delay_tests` uses to check the rendered echo time against
    // the device's own `stages / (2 * f_clk)` law rather than against itself.
    double clockHz() const;
    double delaySeconds() const;
    // Endpoints of the shipped DELAY travel, in seconds.
    static double minDelaySeconds();
    static double maxDelaySeconds();
    // BBD sample slots (stages / 2). 4096 for the two-MN3005 DMM.
    static int bbdSlots();

    // Compander probes: the compressor's gain and the expander's gain at this
    // instant, so a test can plot the real curve instead of inferring it.
    double compressorGain() const;
    double expanderGain() const;
    // The charge-transfer-inefficiency corner (Hz) implied by the current clock.
    double chargeSmearCornerHz() const;

    // Largest absolute value in any recursive state whose rest value is ZERO —
    // which here is ALL of them, including the delay line (ADR 006).
    double maxAbsRestingState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DELAY_MODEL_H
