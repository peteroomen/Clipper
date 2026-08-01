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
// Oversampling: the preamp oversamples per triode stage (4 stages), the power
// section oversamples once around the whole LTP->EL34->OT solve. setOversampling
// sets both. Convention: 1.0f == full scale.
// Platform-free C++17, zero web/server/electron deps.

#ifndef CLIPPER_DSP_ROCKERVERB_AMP_H
#define CLIPPER_DSP_ROCKERVERB_AMP_H

#include <memory>
#include <vector>

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

    int latencySamples() const {
        return preamp_.latencySamples() + power_.latencySamples();
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
    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    RockerverbPreamp preamp_;
    RockerverbPowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ROCKERVERB_AMP_H
