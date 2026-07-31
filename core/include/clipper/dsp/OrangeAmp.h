// Clipper — portable DSP core (M10.3, docs §57).
//
// OrangeAmp: the FULL early-70s Orange OR120 "Overdrive" head — OrangePreamp →
// OrangePowerAmp — as one guitar-in-to-normalized-out module.
//
// THERE IS NO MASTER VOLUME. The single VOLUME pot sets how hard the whole amp is
// driven, so the power section IS the overdrive and breakup onset must TRACK THE
// KNOB — the same design convention (and the same test) as the AC30, docs §46.
// Do not add a master to "make it usable": that is a different amplifier.
//
// Oversampling: the preamp oversamples per triode stage (2 stages), the power
// section oversamples once around the whole driver→cathodyne→EL34→OT solve.
// setOversampling sets both. Convention: 1.0f == full scale.
// Platform-free C++17, zero web/server/electron deps.

#ifndef CLIPPER_DSP_ORANGE_AMP_H
#define CLIPPER_DSP_ORANGE_AMP_H

#include <memory>
#include <vector>

#include "clipper/dsp/OrangePowerAmp.h"
#include "clipper/dsp/OrangePreamp.h"
#include "clipper/dsp/ReverbModel.h"

namespace clipper::dsp {

class OrangeAmp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,    // the ONE gain control
        PARAM_BASS = 1,
        PARAM_TREBLE = 2,
        PARAM_FAC = 3,       // F.A.C. — six-position rotary (0..1 -> 6 detents)
        PARAM_HF_DRIVE = 4,  // presence, inside the feedback loop
        // A real OR120 has no reverb; the app adds one anyway, exactly as the JCM
        // does (docs §19 note: authenticity yields to usability for the normal amp
        // conveniences). Mono, after the power amp, 0 == bit-exact passthrough.
        PARAM_REVERB = 5,
        PARAM_COUNT = 6,
    };

    OrangeAmp();
    ~OrangeAmp();
    OrangeAmp(const OrangeAmp&) = delete;
    OrangeAmp& operator=(const OrangeAmp&) = delete;

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

    OrangePreamp& preamp() { return preamp_; }
    OrangePowerAmp& powerAmp() { return power_; }
    double lastOutputPeak() const { return power_.lastOutputPeak(); }

    // The preamp-volts → driver-grid trim. UN-FITTED to 1.0 by the schematic
    // correction: the preamp now emits V1B's plate AC through the real 68n, the
    // real F.A.C. cap and the real 100k+100k loop feed into the driver's 1M grid
    // leak, so the driver grid voltage IS the preamp's output and there is nothing
    // left for a trim to represent. Sweep table and the two premises it refuted
    // are in the .cpp.
    static constexpr double kInterstageScale = 1.0;

private:
    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    OrangePreamp preamp_;
    OrangePowerAmp power_;
    std::unique_ptr<ReverbModel> reverb_;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ORANGE_AMP_H
