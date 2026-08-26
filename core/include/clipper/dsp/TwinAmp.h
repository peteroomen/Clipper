// Clipper — portable DSP core (M10.1).
//
// TwinAmp: the FULL Fender blackface AB763 "Twin-style" combo — the clean
// benchmark — as one guitar-in-to-normalized-out module. It composes the M10.1
// TwinPreamp (2× 12AX7 around the pre-gain Fender TMB stack + volume/bright), a
// mono spring ReverbModel, a reusable OptoTremolo, and the TwinPowerAmp (12AT7
// LTP PI → 6L6GC quad → OT → flat NFB → light sag), in the REAL AB763 SIGNAL
// ORDER:
//
//   guitar → TwinPreamp → [REVERB] → [TREMOLO] → (interstage trim) → TwinPowerAmp
//
// Reverb sits AFTER the recovery stage / volume and BEFORE the tremolo and the
// phase inverter (the authentic AB763 order: the reverb-return is blended into
// the channel, then the whole thing is amplitude-modulated by the optical trem,
// then it drives the PI). The reverb is MONO here. The tremolo ("vibrato" on the
// panel — a misnomer, it is amplitude tremolo) modulates the blended signal.
//
// Headroom is the product: at typical settings this amp stays CLEAN — the two
// warm 12AX7 stages plus the lossy Fender stack keep the preamp well inside its
// linear window at moderate volume, and the huge 6L6 quad + light sag mean the
// breakup, when it comes, arrives late and mostly from the PI/power stage.
//
// Oversampling: the preamp oversamples per triode stage; the power section
// oversamples independently. setOversampling sets both (default 4×, the measured
// requirement — docs §20). The reverb + tremolo run at the base rate (linear /
// slowly-time-varying). Convention: 1.0f == full scale. Platform-free C++17.

#ifndef CLIPPER_DSP_TWIN_AMP_H
#define CLIPPER_DSP_TWIN_AMP_H

#include <memory>
#include <vector>

#include "clipper/dsp/OptoTremolo.h"
#include "clipper/dsp/ReverbModel.h"
#include "clipper/dsp/TwinPowerAmp.h"
#include "clipper/dsp/TwinPreamp.h"
#include "clipper/dsp/Oversampler.h"

namespace clipper::dsp {

class TwinAmp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,
        PARAM_BASS = 1,
        PARAM_MID = 2,
        PARAM_TREBLE = 3,
        PARAM_BRIGHT = 4,
        PARAM_REVERB = 5,     // spring reverb wet mix (0 = dry, bit-exact)
        PARAM_SPEED = 6,      // tremolo SPEED (LFO rate, log map)
        PARAM_INTENSITY = 7,  // tremolo INTENSITY (modulation depth)
        PARAM_TREMOLO_ENABLE = 8,  // tremolo ON/OFF (>=0.5 on). Default OFF; off is a
                                   // bit-exact bypass, click-free toggle (docs §20).
        PARAM_COUNT = 9,
    };

    TwinAmp();
    ~TwinAmp();
    TwinAmp(const TwinAmp&) = delete;
    TwinAmp& operator=(const TwinAmp&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);     // both preamp + power section
    int oversampling() const { return oversampling_; }
    void setParameter(int paramId, float value);

    // Process mono audio, in -> out (may alias). Input = guitar grid drive (V).
    // Output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state (preamp, spring
    // tank, opto tremolo, power section) and re-park at the already-solved DC point.
    // NOT prepare() — see Jcm800Amp::reset(). Knob positions survive.
    void reset();

    // Total oversampling-filter group delay in base-rate samples: the preamp's two
    // serial triode stages + the power section's own OS round trip. Reverb/tremolo
    // add no counted latency (base-rate, phase-linear at DC).
    // ONE shared oversampling domain (2026-08-26), §63.14's change. It was 216
    // samples / 4.50 ms: THREE independent 4x domains (V1, V2, power section).
    //
    // The Twin is the awkward one of the three amps this was applied to, because
    // the AB763 puts the spring tank and the optical tremolo BETWEEN the preamp and
    // the phase inverter — so consolidating means those two run at the oversampled
    // rate as well. Their ORDER in the chain is unchanged, which is what §20
    // documents; only the rate they are discretized at moves.
    int latencySamples() const {
        return os_.latencySamples() + preamp_.latencySamples() + power_.latencySamples();
    }

    // Introspection / passthrough for tests + the render CLI.
    TwinPreamp& preamp() { return preamp_; }
    TwinPowerAmp& powerAmp() { return power_; }
    OptoTremolo& tremolo() { return *tremolo_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // The preamp-volts → PI-grid-drive trim (documented in the .cpp).
    static constexpr double kInterstageScale = 0.107;

private:
    void rebuild();  // (re)prepare the whole chain at the current oversampled rate
    Oversampler os_;
    bool prepared_ = false;
    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    TwinPreamp preamp_;
    std::unique_ptr<ReverbModel> reverb_;
    std::unique_ptr<OptoTremolo> tremolo_;
    TwinPowerAmp power_;
    std::vector<float> buf_;  // interstage scratch (maxBlock)
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TWIN_AMP_H
