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

#include <memory>
#include <vector>

#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/Jcm800PowerAmp.h"
#include "clipper/dsp/ReverbModel.h"

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
        // --- M10.1 additive: spring reverb as a USABILITY convenience -----------
        // A real 2204 has no reverb; the app adds one anyway (the user wants the
        // normal amp conveniences — authenticity yields to usability here, docs
        // §19 note). Mono ReverbModel placed AFTER the power amp, BEFORE the C
        // ABI's dual-mono split (same placement logic as the Twin). 0..1 wet mix;
        // 0 == bit-exact passthrough (so the M9.3 tests stay bit-exact).
        PARAM_REVERB = 6,
        PARAM_COUNT = 7,
    };

    Jcm800Amp();
    ~Jcm800Amp();
    Jcm800Amp(const Jcm800Amp&) = delete;
    Jcm800Amp& operator=(const Jcm800Amp&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);     // both preamp + power section
    int oversampling() const { return oversampling_; }
    void setParameter(int paramId, float value);

    // Process mono audio, in -> out (may alias). Input = guitar grid drive (V).
    // Output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state in the voice and
    // re-park at the already-solved DC operating point. Preamp stages, power section,
    // and the spring tank. Deliberately NOT prepare(): prepare() re-solves every
    // tube DC point and settles ~50 k silent samples per stage (measured ~69 ms for
    // this voice), which is not something a recovery path may cost. Knob positions
    // survive; only state is cleared. Allocation-free.
    void reset();

    // Total oversampling-filter group delay in base-rate samples (0 at 1x): the
    // preamp cascade's four serial triode stages + the power section's own OS
    // round trip. The interstage scale is a memoryless gain (no delay). Reported
    // by the C ABI so the worklet/plugin can delay-compensate the JCM path just
    // like the RAT/SD-1 oversamplers.
    // ONE shared oversampling domain (2026-08-26). Both halves are prepared AT the
    // oversampled rate with their own resamplers at 1x — Oversampler.h's documented
    // exact pass-through — so this is `os_` plus whatever the halves still report,
    // which at factor 1 is zero. It was 360 samples / 7.50 ms: FIVE independent 4x
    // domains (one per triode stage plus one round the power section), each paying
    // its own 72-sample halfband group delay. §63.14 made exactly this change on the
    // Rockerverb and its alias floor IMPROVED by 19.6 dB in the same edit.
    int latencySamples() const {
        return os_.latencySamples() + preamp_.latencySamples() + power_.latencySamples();
    }

    // Introspection / passthrough for tests + the render CLI.
    Jcm800Preamp& preamp() { return preamp_; }
    Jcm800PowerAmp& powerAmp() { return power_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // The preamp-volts → PI-grid-drive trim (documented in the .cpp).
    static constexpr double kInterstageScale = 0.16;

private:
    void rebuild();  // (re)prepare both halves at the current oversampled rate

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;
    bool prepared_ = false;

    Oversampler os_;
    Jcm800Preamp preamp_;
    Jcm800PowerAmp power_;
    // M10.1: mono spring reverb after the power amp (usability convenience). Held
    // by unique_ptr so this header needn't inline ReverbModel's Impl. Default mix
    // 0 → bit-exact passthrough, so the M9.3 tests stay bit-exact.
    std::unique_ptr<ReverbModel> reverb_;
    std::vector<float> buf_;  // interstage scratch (maxBlock * factor)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_JCM800_AMP_H
