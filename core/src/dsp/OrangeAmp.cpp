// Clipper — OrangeAmp (M10.3, docs §57). Composition of the OR120 preamp and its
// power section. See OrangeAmp.h for the "no master volume" contract.

#include "clipper/dsp/OrangeAmp.h"

#include <algorithm>

namespace clipper::dsp {

OrangeAmp::OrangeAmp() : reverb_(std::make_unique<ReverbModel>()) {}
OrangeAmp::~OrangeAmp() = default;

void OrangeAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    // The transcribed dropper chain runs B+ -> 33K -> C+ -> 33K -> D+, so the
    // preamp's supply is downstream of the power section's. Prepare the power
    // section FIRST and hand it across, so the two halves cannot disagree about
    // a rail (the reconstruction had three unrelated hard-coded B+ values).
    power_.prepare(sampleRate_, maxBlockSize_);
    preamp_.setSupplyCPlus(power_.cPlusIdle());
    preamp_.prepare(sampleRate_, maxBlockSize_);
    reverb_->prepare(sampleRate_);
    setOversampling(oversampling_);
}

void OrangeAmp::setOversampling(int factor) {
    oversampling_ = factor;
    preamp_.setOversampling(factor);
    power_.setOversampling(factor);
}

void OrangeAmp::reset() {
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void OrangeAmp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_VOLUME:
            preamp_.setParameter(OrangePreamp::PARAM_VOLUME, value);
            break;
        case PARAM_BASS: preamp_.setParameter(OrangePreamp::PARAM_BASS, value); break;
        case PARAM_TREBLE:
            preamp_.setParameter(OrangePreamp::PARAM_TREBLE, value);
            break;
        case PARAM_FAC: preamp_.setParameter(OrangePreamp::PARAM_FAC, value); break;
        case PARAM_HF_DRIVE:
            power_.setParameter(OrangePowerAmp::PARAM_HF_DRIVE, value);
            break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void OrangeAmp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        preamp_.process(in + off, w, n);
        // Preamp volts → driver grid, and after the schematic correction there is
        // NOTHING LEFT FOR THIS CONSTANT TO DO. OrangePreamp's last element is the
        // F.A.C. network, which already ends at the driver's own grid node (68n ->
        // Cfac -> 100k send + 100k return -> 1M grid leak), so its output IS the
        // driver's grid voltage in volts. kInterstageScale is therefore 1.0 — an
        // UN-FITTING in the docs §53 sense, not a re-derivation.
        //
        // It was still swept, because "the constant is unity" is only defensible if
        // unity is also where the amp behaves. Measured on the composed amp,
        // 0.15 V / 220 Hz for the onset and VOLUME 1.0 with 0.15/0.30/0.50 V in for
        // the power, at the FINAL kFullScaleSecV so the column is comparable:
        //
        //   scale   onset   cranked power, W into 8 ohm (0.15 / 0.30 / 0.50 V in)
        //   0.02     ----      1.8 /   7.9 /  21.2
        //   0.06     1.00     18.0 /  61.8 /  70.8
        //   0.12     1.00     58.2 /  77.0 /  84.1
        //   0.20     0.90     72.8 /  86.8 /  87.7
        //   0.40     0.70     88.3 /  89.2 /  92.5
        //   0.70     0.60     90.8 /  90.2 /  93.3
        //   1.00     0.50     90.8 /  90.6 /  94.0     <- shipped (unity)
        //   1.50     0.40     90.1 /  91.3 /  94.5
        //   2.00     0.35     90.1 /  91.7 /  94.8
        //
        // TWO OF THE FIRST RELEASE'S FINDINGS ARE REFUTED BY THE CORRECTED CIRCUIT,
        // and both are reported rather than quietly dropped (docs §57.9):
        //
        //  1. "kInterstageScale does NOT set the breakup onset" is no longer true.
        //     On the reconstruction the onset sat at VOLUME 0.59 across a 5x sweep
        //     because the PREAMP clipped first. The real preamp loses ~35 dB in the
        //     James stack + GAIN pot + 330p before V1B, so it clips far later and
        //     the POWER section is the first thing to break up: the onset now moves
        //     monotonically with the scale, 1.00 -> 0.35 over the sweep. At unity it
        //     lands at VOLUME 0.50, i.e. the docs §46 no-master window is met BY THE
        //     CIRCUIT rather than by choosing a constant.
        //
        //  2. The amp does NOT reach its rated 120 W, at any scale. The power
        //     section's own ceiling, driven directly, measures 92.7 W with feedback
        //     and 93.6 W without — so the docs §42 "smallest value that reaches
        //     rated power" criterion is UNSATISFIABLE here and was not used. The
        //     shortfall is reported, not tuned away: it is dominated by the
        //     transcribed EL34 grid network (68n into 110k, against the
        //     reconstruction's 22n into 220k) driving the grids harder into
        //     conduction, plus the transcribed per-tube 1k screen resistors with no
        //     bypass cap (worth ~4 W on its own: at 1 ohm the ceiling measures
        //     97.7 W). See docs §57.3 for the open item.
        for (int i = 0; i < n; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, out + off, n);
        off += n;
    }
    // Spring reverb after the power amp (usability convenience — see the header).
    // Mix 0 == bit-exact passthrough.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
