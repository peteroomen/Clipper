// Clipper — portable DSP core (M10.10, docs §72).
//
// ChampAmp: the Fender tweed Champ 5F1 — two 12AX7 stages, a 1 MΩ log VOLUME pot,
// and ONE cathode-biased 6V6GT into a small single-ended output transformer.
// Composed in ONE shared oversampling domain. Platform-free C++17.
//
// THE WHOLE AMP IS: input -> V1A -> VOLUME -> V1B -> 6V6 -> OT. No tone stack, no
// phase inverter, no negative feedback, no master volume, no channel switching, no
// effects loop. It is the smallest signal path in this project by a wide margin and
// that is the point of building it — see ChampPowerAmp.h for the three properties
// that fall out of having ONE output tube instead of a push-pull pair, each of which
// is a hard assertion in clipper_champ_tests.
//
// ONE OVERSAMPLING DOMAIN FROM THE FIRST COMMIT (§63.14's lesson, applied up front
// exactly as §69's Mesa did rather than retrofitted). Both halves are prepared AT
// THE OVERSAMPLED RATE with their own resamplers at 1x — Oversampler's documented
// exact pass-through — so there is one band-limiting in and one out for the entire
// cascade. Latency is therefore 72 samples / 1.50 ms, not the 216 that per-stage
// domains would cost (measured: two TriodeStages at 72 each plus the power section's
// 72). Do not "simplify" this by letting each stage oversample itself.
//
// REVERB IS A DELIBERATE DEPARTURE FROM THE CIRCUIT, and it is the §19 JCM800
// precedent verbatim: a real 5F1 has no reverb tank (Fender's first reverb amps came
// years later), the app adds one anyway because that is how people actually record
// this amp, and mix 0 is a bit-exact pass-through so the default costs nothing. It
// is placed AFTER the power section. Owner decision, 2026-08-25.
//
#pragma once

#include "clipper/dsp/ChampPowerAmp.h"
#include "clipper/dsp/ChampPreamp.h"
#include "clipper/dsp/Oversampler.h"

#include <memory>
#include <vector>

namespace clipper::dsp {

class ReverbModel;

class ChampAmp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,   // the amp's ONLY real knob
        PARAM_REVERB = 1,   // usability convenience (§19 precedent), default 0
        PARAM_COUNT = 2,
    };

    ChampAmp();
    ~ChampAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);
    void reset();

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    const ChampPreamp& preamp() const { return preamp_; }
    const ChampPowerAmp& power() const { return power_; }
    ChampPowerAmp& powerMutable() { return power_; }

    // AN UN-FITTING TO 1.0 — the §57 OR120 / §63 Rockerverb precedent, and here it
    // is simply the circuit: in a real 5F1 there IS no interstage divider. V1B's
    // plate goes through the transcribed 20 nF coupling cap straight to the 6V6
    // grid, and that cap is already modelled inside TriodeStage/ChampPowerAmp. So
    // there is nothing for a trim to represent and it is not fitted to anything.
    //
    // §42's "smallest scale that reaches rated power" criterion is UNSATISFIABLE
    // here and the sweep says so rather than being quietly snugged: composed
    // cranked power PLATEAUS at 3.90 W (scale 0.80 → 3.896, 1.00 → 3.890) against
    // the power section's OWN sine ceiling of 5.29 W and a rated ~5 W, and above
    // scale 1.0 it DECLINES (2.00 → 3.788, 3.00 → 3.772). That non-monotonicity is
    // §57.9's exact signature: more input means LESS output because grid conduction
    // charges the coupling cap and shifts the bias toward cutoff (blocking). The
    // shortfall is REPORTED, not closed — do NOT raise kVsupply or soften the grid
    // coupling to chase the last 1.4 W (§57.3's standing instruction).
    static constexpr double kInterstageScale = 1.0;

private:
    void rebuild();

    Oversampler os_;
    ChampPreamp preamp_;
    ChampPowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;
    bool prepared_ = false;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp
