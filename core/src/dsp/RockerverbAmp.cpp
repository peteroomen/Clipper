// Clipper — RockerverbAmp (M10.7, docs §63). Composition of the Rockerverb dirty
// channel's preamp and its power section. See RockerverbAmp.h for the master-
// volume contract and the dirty-channel-only scope.

#include "clipper/dsp/RockerverbAmp.h"

#include <algorithm>

namespace clipper::dsp {

RockerverbAmp::RockerverbAmp() : reverb_(std::make_unique<ReverbModel>()) {}
RockerverbAmp::~RockerverbAmp() = default;

void RockerverbAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    power_.prepare(sampleRate_, maxBlockSize_);
    preamp_.prepare(sampleRate_, maxBlockSize_);
    reverb_->prepare(sampleRate_);
    setOversampling(oversampling_);
}

void RockerverbAmp::setOversampling(int factor) {
    oversampling_ = factor;
    preamp_.setOversampling(factor);
    power_.setOversampling(factor);
}

void RockerverbAmp::reset() {
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void RockerverbAmp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_GAIN: preamp_.setParameter(RockerverbPreamp::PARAM_GAIN, value); break;
        case PARAM_VOLUME:
            preamp_.setParameter(RockerverbPreamp::PARAM_VOLUME, value);
            break;
        case PARAM_BASS: preamp_.setParameter(RockerverbPreamp::PARAM_BASS, value); break;
        case PARAM_MID: preamp_.setParameter(RockerverbPreamp::PARAM_MID, value); break;
        case PARAM_TREBLE:
            preamp_.setParameter(RockerverbPreamp::PARAM_TREBLE, value);
            break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void RockerverbAmp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        preamp_.process(in + off, w, n);
        // kInterstageScale == 1.0 — an UN-FITTING, not a calibration. The preamp
        // ends at the VOLUME wiper with the PI's 1M grid leak already stamped into
        // the same matrix, so its output IS the phase inverter's grid voltage in
        // volts. See RockerverbAmp.h; the sweep that justifies unity is in
        // docs §63.5.
        if (kInterstageScale != 1.0) {
            for (int i = 0; i < n; ++i)
                w[i] = static_cast<float>(w[i] * kInterstageScale);
        }
        power_.process(w, out + off, n);
        off += n;
    }
    // The shared valve-driven spring tank, after the power amp. Unlike the JCM's
    // and the OR120's, this one is an authentic part of the amp. Mix 0 ==
    // bit-exact passthrough.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
