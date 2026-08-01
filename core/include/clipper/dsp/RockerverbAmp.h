// Clipper — portable DSP core (M10.7, docs §63).
//
// RockerverbAmp: the FULL Orange Rockerverb 100 dirty channel —
// RockerverbPreamp -> RockerverbPowerAmp -> spring reverb — as one
// guitar-in-to-normalized-out module. Amp voice 5.
//
// THIS AMP HAS A MASTER VOLUME, AND THAT IS THE POINT. The OR120 (docs §57) has
// exactly one knob and its power section IS the overdrive, so §46's no-master
// convention applies there and breakup tracks the knob. Here the dirty channel's
// VOLUME sits AFTER the tone stack, so GAIN and VOLUME are independent: the amp
// can be filthy and quiet. That decoupling is half of this slice's acceptance
// bar (docs §63.5) and it is asserted — do not "simplify" the VOLUME pot back
// into a pre-stack position.
//
// THE REVERB IS AUTHENTIC HERE. The JCM800's spring (docs §19) and the OR120's
// (§57) are usability adds to amps that have none. A Rockerverb has a real
// valve-driven spring tank — it is in the amp's name — shared by both channels.
// Mono, after the power amp, 0 == bit-exact passthrough (the shared ReverbModel;
// §48 set its wet trim and the §48 amendment coaches its usable range).
//
// ONLY THE DIRTY CHANNEL SHIPS. The netlist this voice is transcribed from
// (docs §63.1) is the dirty channel's; the clean channel would be invention, and
// a footswitch in front of an invented channel is worse than no footswitch.
// Named as the §63.11 follow-up.
//
// ===========================================================================
// OVERSAMPLING: ONE SHARED DOMAIN AROUND THE WHOLE CASCADE (docs §63.14)
// ===========================================================================
// This voice shipped (PR #52) with FIVE independent 4x domains — one per triode
// stage plus one around the power section — which cost 360 samples of latency
// (7.50 ms), made it the most expensive amp in the lineup, and left a -52.7 dB
// composed alias floor on a 44.1 kHz grid against a -56 dB bar (the XFAIL
// `rockerverb-alias-44k1`, docs §63.8). Every one of those three facts is the
// same fact: five cascaded band-limitings, and four hand-offs back to the base
// rate at which the NEXT stage's nonlinearity then generates products above
// Nyquist/2 all over again.
//
// So the amp now owns ONE Oversampler. The preamp and the power section are
// prepared AT THE OVERSAMPLED RATE with their own resamplers set to 1x (an exact
// pass-through — Oversampler.h's own documented contract), which is why this
// needed no change to TriodeStage, RockerverbPreamp or RockerverbPowerAmp: the
// existing classes are simply run at 192 kHz instead of 48 kHz. That also puts
// the three interstage MNAs and the FMV stack inside the domain, which is a
// strictly better discretization for them, not a worse one.
//
// This is a DELIBERATE TONE CHANGE and it is confined to this voice (and, in the
// same slice, the OR120's identical defect). See docs §63.14 for the measured
// before -> after: alias floor, latency, CPU.
//
// Convention: 1.0f == full scale.
// Platform-free C++17, zero web/server/electron deps.

#ifndef CLIPPER_DSP_ROCKERVERB_AMP_H
#define CLIPPER_DSP_ROCKERVERB_AMP_H

#include <memory>
#include <vector>

#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ReverbModel.h"
#include "clipper/dsp/RockerverbPowerAmp.h"
#include "clipper/dsp/RockerverbPreamp.h"

namespace clipper::dsp {

class RockerverbAmp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,    // the ganged dual 1M log GAIN pot (the drive knob)
        PARAM_VOLUME = 1,  // the post-stack 1M LINEAR master
        PARAM_BASS = 2,
        PARAM_MID = 3,
        PARAM_TREBLE = 4,
        PARAM_REVERB = 5,  // the shared valve-driven spring tank
        PARAM_COUNT = 6,
    };

    RockerverbAmp();
    ~RockerverbAmp();
    RockerverbAmp(const RockerverbAmp&) = delete;
    RockerverbAmp& operator=(const RockerverbAmp&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }
    void setParameter(int paramId, float value);

    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state and re-park at
    // the already-solved DC operating point. Never re-solves.
    void reset();

    // ONE domain (see the banner): the preamp and the power section run at 1x
    // INSIDE it and contribute nothing, so the whole amp's latency is this
    // oversampler's alone — 72 samples at 4x, against the 360 the five-domain
    // arrangement cost. Both terms are still summed rather than assumed, so a
    // future slice that re-arms an inner domain cannot silently under-report.
    int latencySamples() const {
        return os_.latencySamples() + preamp_.latencySamples() +
               power_.latencySamples();
    }

    RockerverbPreamp& preamp() { return preamp_; }
    RockerverbPowerAmp& powerAmp() { return power_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // Preamp volts -> PI grid. UN-FITTED to 1.0, for the same reason the OR120's
    // is (docs §57.9): RockerverbPreamp's last element is the FMV stack's own
    // VOLUME pot, and that pot is loaded IN THE MNA by the phase inverter's grid
    // leak — so the preamp's output already IS the PI's grid voltage, in volts,
    // and there is nothing left for a trim to represent. It was still swept
    // (table in the .cpp), because "the constant is unity" is only defensible if
    // unity is also where the amp behaves.
    static constexpr double kInterstageScale = 1.0;

private:
    void rebuild();  // (re)prepare both halves at the current oversampled rate

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;
    bool prepared_ = false;

    Oversampler os_;
    RockerverbPreamp preamp_;
    RockerverbPowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ROCKERVERB_AMP_H
