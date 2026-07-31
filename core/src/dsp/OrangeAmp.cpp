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
    preamp_.prepare(sampleRate_, maxBlockSize_);
    power_.prepare(sampleRate_, maxBlockSize_);
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
        // Tone-stack volts → driver grid. There is no master volume to soak this
        // up, so this constant sets how hard the POWER SECTION runs.
        //
        // DERIVED against an absolute criterion, and the first thing the sweep did
        // was refute this slice's own premise. Measured on the composed amp,
        // 0.15 V / 220 Hz for the onset and VOLUME 1.0 with 0.15/0.30/0.50 V in for
        // the power (docs §57):
        //
        //   scale   onset   cranked power, W into 8 ohm (0.15 / 0.30 / 0.50 V in)
        //   0.010    0.59       5.4 /   6.7 /   7.5
        //   0.015    0.59      12.7 /  16.0 /  17.8
        //   0.020    0.59      23.4 /  29.3 /  32.6
        //   0.030    0.59      53.3 /  63.5 /  68.3
        //   0.050    0.59      86.2 /  84.3 /  88.5
        //   0.080    0.59      93.7 /  98.3 /  99.4
        //   0.120    0.49      90.1 /  93.2 / 130.2
        //   0.200    0.37     105.0 / 146.6 / 118.1
        //   0.400    0.25     149.4 / 105.8 / 104.4
        // (watts computed at the FINAL kFullScaleSecV = 50.7 throughout, so the
        //  column is comparable down the whole table)
        //
        // THE BREAKUP ONSET IS NOT SET BY THIS CONSTANT below ~0.08 — it sits at
        // VOLUME 0.59 across a 5x sweep, because down there the first thing to
        // clip is the PREAMP (two fully-bypassed 12AX7s with the volume pot
        // between them), not the power section. Choosing this number "so the
        // breakup lands at 0.5-0.6" would therefore have been choosing it for a
        // reason that is not true. It was chosen instead by the docs §42 criterion:
        // the SMALLEST value at which a cranked OR120 genuinely reaches its rated
        // 120 W. That is 0.12 (130 W at 0.50 V in; 0.08 tops out at 99 W).
        //
        // The non-monotonic peaks at 0.20 and 0.40 are not noise: harder drive
        // charges the EL34 coupling caps through grid conduction and shifts the
        // bias toward cutoff (the blocking mechanism), so past a point MORE input
        // gives LESS output. That is the real amp's behaviour and is why the
        // criterion is "reaches rated power", not "peaks highest".
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
