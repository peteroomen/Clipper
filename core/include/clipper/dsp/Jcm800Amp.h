// Clipper — portable DSP core (M9.3).
//
// Jcm800Amp: the FULL Marshall JCM800 2204 — Jcm800Preamp → Jcm800PowerAmp — as one
// guitar-in-to-normalized-out module. The preamp (M9.2: 4× 12AX7 + Marshall TMB +
// GAIN/MASTER) feeds the power section (M9.3: 12AX7 LTP phase inverter → push-pull
// EL34 → output transformer → global NFB + presence + B+ sag). The MASTER volume
// (inside the preamp) sets how hard the phase inverter is driven — so master IS the
// power-amp drive control, exactly as on the real amp.
//
// Interstage level: the preamp emits real volts at the tone-stack/master node; those
// volts ARE the PI grid drive. A documented `kInterstageScale` trims the handoff so
// the PI sees a realistic drive across the master range (the preamp's tens-of-volts
// swing at high gain would otherwise slam the PI far past any real amp). See the .cpp.
//
// Oversampling: the preamp oversamples per triode stage (M9.2 measured 4×); the power
// section oversamples independently (M9.3 measured — see docs §18). setOversampling
// sets both. Convention: 1.0f == full scale (the power amp's normalized output).
// Platform-free C++17, zero web/server/electron deps.

#ifndef CLIPPER_DSP_JCM800_AMP_H
#define CLIPPER_DSP_JCM800_AMP_H

#include <vector>

#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/Jcm800PowerAmp.h"

namespace clipper::dsp {

class Jcm800Amp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,      // preamp GAIN (drive)
        PARAM_MASTER = 1,    // MASTER volume — drives the phase inverter
        PARAM_BASS = 2,
        PARAM_MID = 3,
        PARAM_TREBLE = 4,
        PARAM_PRESENCE = 5,  // power-amp presence (HF feedback lift)
        PARAM_COUNT = 6,
    };

    Jcm800Amp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);     // both preamp + power section
    int oversampling() const { return oversampling_; }
    void setParameter(int paramId, float value);

    // Process mono audio, in -> out (may alias). Input = guitar grid drive (V).
    // Output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // Introspection / passthrough for tests + the render CLI.
    Jcm800Preamp& preamp() { return preamp_; }
    Jcm800PowerAmp& powerAmp() { return power_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // The preamp-volts → PI-grid-drive trim (documented in the .cpp).
    static constexpr double kInterstageScale = 0.25;

private:
    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    Jcm800Preamp preamp_;
    Jcm800PowerAmp power_;
    std::vector<float> buf_;  // interstage scratch (maxBlock)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_JCM800_AMP_H
