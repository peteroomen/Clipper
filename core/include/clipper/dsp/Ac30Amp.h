// Clipper — portable DSP core (M10.2).
//
// Ac30Amp: the FULL Vox AC30 "top boost" — Ac30Preamp -> Ac30PowerAmp -> [spring
// REVERB] — as one guitar-in-to-normalized-out module. The preamp (one 12AX7 gain
// stage + the interactive top-boost tone stack + VOLUME) feeds the class-A power
// section (hot 12AX7 LTP phase inverter -> TOP CUT -> cathode-biased EL84 quad ->
// output transformer -> NO NFB -> deep GZ34 sag). The VOLUME knob IS the overdrive:
// it sets how hard the (early-clipping) phase inverter is driven.
//
// Reverb: the real AC30 top-boost has no reverb; the app adds a mono spring anyway,
// AFTER the power amp and BEFORE the C-ABI dual-mono split (the same usability call
// and placement as the JCM800's added reverb, docs §19/§23). 0..1 wet mix; 0 ==
// bit-exact passthrough so every power-section validation number is unchanged.
//
// Oversampling: the preamp oversamples its triode stage; the power section over-
// samples independently. setOversampling sets both (default 4×, docs §23). The
// reverb runs at the base rate. Convention: 1.0f == full scale. Platform-free C++17.

#ifndef CLIPPER_DSP_AC30_AMP_H
#define CLIPPER_DSP_AC30_AMP_H

#include <memory>
#include <vector>

#include "clipper/dsp/Ac30PowerAmp.h"
#include "clipper/dsp/Ac30Preamp.h"
#include "clipper/dsp/ReverbModel.h"

namespace clipper::dsp {

class Ac30Amp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,
        PARAM_BASS = 1,
        PARAM_TREBLE = 2,
        PARAM_TOPCUT = 3,     // TOP CUT — post-PI treble cut (inverted sense)
        PARAM_REVERB = 4,     // spring reverb wet mix (0 = dry, bit-exact)
        PARAM_COUNT = 5,
    };

    Ac30Amp();
    ~Ac30Amp();
    Ac30Amp(const Ac30Amp&) = delete;
    Ac30Amp& operator=(const Ac30Amp&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);     // both preamp + power section
    int oversampling() const { return oversampling_; }
    void setParameter(int paramId, float value);

    // Process mono audio, in -> out (may alias). Input = guitar grid drive (V).
    // Output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state (preamp, power
    // section incl. the sag envelope, spring tank) and re-park at the already-solved
    // DC point. NOT prepare() — see Jcm800Amp::reset(). Knob positions survive.
    void reset();

    int latencySamples() const {
        return preamp_.latencySamples() + power_.latencySamples();
    }

    // Introspection / passthrough for tests + the render CLI.
    Ac30Preamp& preamp() { return preamp_; }
    Ac30PowerAmp& powerAmp() { return power_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // The preamp-volts → PI-grid-drive handoff. This is a PASSIVE path in the real
    // amp — the channel volume wiper reaches the PI grid through a coupling cap, a
    // grid stopper and the channel MIXING resistors — so it can only ever be ≤ 1.
    // 0.67 == the two-channel mixing division (each channel's wiper through its own
    // 1 M mixing resistor into the shared PI grid node): the top-boost channel keeps
    // two thirds of its volts. It is much higher than the Twin's 0.16 / the JCM's
    // 0.25 for a structural reason — those preamps put TWO gain stages (and the JCM
    // four) ahead of their volume pot, so their wipers carry tens of volts and the
    // handoff has to trim them back, while the AC30 top boost has a SINGLE stage into
    // a stack that loses ~13 dB. Raised from 0.30 in the §23 second amendment (the
    // "not enough gain/breakup" report): 0.30 was an arbitrary trim, not a divider.
    static constexpr double kInterstageScale = 0.67;

private:
    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    Ac30Preamp preamp_;
    Ac30PowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;  // mono spring (usability add; mix 0 = passthrough)
    std::vector<float> buf_;  // interstage scratch (maxBlock)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_AC30_AMP_H
