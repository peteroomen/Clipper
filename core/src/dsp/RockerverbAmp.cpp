// Clipper — RockerverbAmp (M10.7, docs §63). Composition of the Rockerverb dirty
// channel's preamp and its power section. See RockerverbAmp.h for the master-
// volume contract and the dirty-channel-only scope.

#include "clipper/dsp/RockerverbAmp.h"

#include <algorithm>

namespace clipper::dsp {

RockerverbAmp::RockerverbAmp() : reverb_(std::make_unique<ReverbModel>()) {}
RockerverbAmp::~RockerverbAmp() = default;

// Prepare both halves AT THE OVERSAMPLED RATE with their own resamplers set to
// 1x. Oversampler.h documents factor() == 1 as an exact pass-through with no
// filtering and no delay, so this is genuinely "the same model, clocked faster"
// rather than a second resampling arrangement. See RockerverbAmp.h's banner and
// docs §63.14.
void RockerverbAmp::rebuild() {
    os_.prepare(maxBlockSize_);
    os_.setFactor(oversampling_);
    const int f = os_.factor();
    const double osRate = sampleRate_ * static_cast<double>(f);
    const int osBlock = maxBlockSize_ * f;
    power_.setOversampling(1);
    power_.prepare(osRate, osBlock);
    preamp_.setOversampling(1);
    preamp_.prepare(osRate, osBlock);
    buf_.assign(static_cast<size_t>(osBlock), 0.0f);
}

void RockerverbAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    reverb_->prepare(sampleRate_);
    rebuild();
    prepared_ = true;
}

void RockerverbAmp::setOversampling(int factor) {
    oversampling_ = factor;
    // Changing the factor changes the rate every stage is discretized at, so the
    // whole cascade has to be re-prepared (and its DC operating points re-settled)
    // — unlike the old arrangement, where each sub-unit re-prepared its own
    // companions. Before prepare() this only records the choice.
    if (prepared_) rebuild();
}

void RockerverbAmp::reset() {
    os_.reset();
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
    const int f = os_.factor();
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        const int m = n * f;
        // ONE band-limiting in, one out. Everything between runs at osRate.
        os_.upsample(in + off, n);
        float* w = os_.buffer();
        preamp_.process(w, buf_.data(), m);
        // kInterstageScale == 1.0 — an UN-FITTING, not a calibration. The preamp
        // ends at the VOLUME wiper with the PI's 1M grid leak already stamped into
        // the same matrix, so its output IS the phase inverter's grid voltage in
        // volts. See RockerverbAmp.h; the sweep that justifies unity is in
        // docs §63.5.
        if (kInterstageScale != 1.0) {
            for (int i = 0; i < m; ++i)
                buf_[static_cast<size_t>(i)] =
                    static_cast<float>(buf_[static_cast<size_t>(i)] * kInterstageScale);
        }
        // Writes back into the oversampler's own buffer, which downsample() reads.
        power_.process(buf_.data(), w, m);
        os_.downsample(out + off, n);
        off += n;
    }
    // The shared valve-driven spring tank, after the power amp. Unlike the JCM's
    // and the OR120's, this one is an authentic part of the amp. Mix 0 ==
    // bit-exact passthrough.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
