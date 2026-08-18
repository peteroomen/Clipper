// Clipper — portable DSP core (M10.4, docs §69).
//
// MesaAmp: the composed Mesa/Boogie Dual Rectifier Solo Head — MesaPreamp into
// MesaPowerAmp, in ONE shared oversampling domain.
//
// ===========================================================================
// ONE OVERSAMPLING DOMAIN, FROM THE START (docs §63.14's lesson)
// ===========================================================================
// The OR120 and the Rockerverb both shipped with per-triode oversampling domains
// plus a separate one round the power section, and BOTH opened the same 44.1 kHz
// alias XFAIL. §63.14 fixed the Rockerverb by preparing each half AT THE
// OVERSAMPLED RATE with its own resampler at 1x — Oversampler.h documents
// factor() == 1 as an exact pass-through with no filtering and no delay, so this
// is genuinely "the same model clocked faster", not a second resampling
// arrangement. This voice is built that way from its first commit: one
// band-limiting in, one out, and a five-triode cascade plus a power section in
// between. Latency is therefore ONE Oversampler's, not seven.
//
// (§63.14 also found that the same change measured WORSE on the OR120, and
// re-owned that amp's XFAIL to its cathodyne. That is a reason to measure this
// one, not a reason to go back to per-stage domains — this amp has an LTP.)
//
// ===========================================================================
// WHAT THIS VOICE SHIPS, AND WHAT IT DOES NOT
// ===========================================================================
// SHIPS: all five modes off the drawing's truth table, both channels' complete
// FMV stacks, the switchable global feedback, the rectifier select and the
// SPONGY/BOLD switch.
//
// DOES NOT SHIP, named rather than hidden:
//   * THE EFFECTS LOOP. V4A/V4B are on sheet `mbdr3` and sit in the signal path
//     when the loop is engaged. This model runs the LOOP BYPASS path (LDR13),
//     which is what the sheet's own rotary calls BYPASS, so the two loop triodes
//     are absent. Their standing draw IS accounted for — it is part of
//     MesaPreamp::kIdownstreamC, derived from their marked node voltages.
//   * THE EL34 BIAS OPTION. The sheet supports it ("EL34 -39V", a 1/2 DPDT bias
//     select and an LDR pair). This voice ships 6L6 at the transcribed -51 V.
//   * THE 4-OHM TAP and the slave out.
//
// REVERB: a Dual Rectifier SOLO HEAD HAS NO REVERB — the reverb-equipped sibling
// is the Trem-O-Verb, a different amp. The shared ReverbModel is carried anyway
// for lineup parity (mix 0 is a bit-exact pass-through, so the default costs
// nothing audible), exactly as the JCM800 and the OR120 carry one they do not
// have. Stated here so nobody later cites this model as evidence a Recto has
// a reverb tank.
//
// Convention: 1.0f == 1.0 V in; output normalized (1.0 == full scale).

#ifndef CLIPPER_DSP_MESA_AMP_H
#define CLIPPER_DSP_MESA_AMP_H

#include <memory>
#include <vector>

#include "clipper/dsp/MesaPowerAmp.h"
#include "clipper/dsp/MesaPreamp.h"
#include "clipper/dsp/Oversampler.h"

namespace clipper::dsp {

class ReverbModel;

class MesaAmp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,
        PARAM_MASTER = 1,
        PARAM_BASS = 2,
        PARAM_MID = 3,
        PARAM_TREBLE = 4,
        PARAM_PRESENCE = 5,
        PARAM_REVERB = 6,
        PARAM_COUNT = 7,
    };

    MesaAmp();
    ~MesaAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);
    void reset();
    void process(const float* in, float* out, int numFrames);

    // The three switched controls. Each is a genuinely DISCRETE front-panel
    // control on the real amp, so none of them is smuggled into a continuous
    // knob (docs §62's ADR 020 records why that distinction matters).
    void setMode(MesaMode m);
    MesaMode mode() const { return mode_; }
    void setRectifier(MesaRectifier r);
    MesaRectifier rectifier() const { return power_.rectifier(); }
    void setPowerMode(MesaPowerMode p);
    MesaPowerMode powerMode() const { return power_.powerMode(); }

    MesaPreamp& preamp() { return preamp_; }
    MesaPowerAmp& powerAmp() { return power_; }

    // The preamp ends at the MASTER wiper with the PI's 1M grid leak already
    // stamped into the same matrix, so its output IS the phase inverter's grid
    // voltage in volts — the Rockerverb's situation (§63.5), where the answer was
    // an UN-FITTING to 1.0. It is NOT unity here, and the reason is structural
    // rather than a calibration: this preamp has FIVE gain stages where the
    // Rockerverb has four, and its cathode follower drives the stack from ~375
    // ohm instead of a plate, so far more signal survives to the master. The
    // value is DERIVED by §42's criterion — the smallest scale at which a cranked
    // amp reaches its rated power — and the sweep is in docs §69.7.
    static constexpr double kInterstageScale = 0.10;

private:
    void rebuild();

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;
    bool prepared_ = false;

    Oversampler os_;
    MesaPreamp preamp_;
    MesaPowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;
    MesaMode mode_ = MesaMode::RedModern;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_MESA_AMP_H
