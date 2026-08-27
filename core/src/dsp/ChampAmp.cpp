// Clipper — ChampAmp (M10.10, docs §73). See ChampAmp.h.

#include "clipper/dsp/ChampAmp.h"

#include "clipper/dsp/ReverbModel.h"

#include <algorithm>

namespace clipper::dsp {

ChampAmp::ChampAmp() : reverb_(std::make_unique<ReverbModel>()) {}
ChampAmp::~ChampAmp() = default;

// Both halves prepared AT THE OVERSAMPLED RATE with their own resamplers at 1x
// (an exact pass-through — Oversampler.h). One band-limiting in, one out.
void ChampAmp::rebuild() {
    os_.prepare(maxBlockSize_);
    os_.setFactor(oversampling_);
    const int f = os_.factor();
    const double osRate = sampleRate_ * static_cast<double>(f);
    const int osBlock = maxBlockSize_ * f;
    power_.setOversampling(1);
    power_.prepare(osRate, osBlock);
    // The preamp's rails hang off the rail the POWER section actually solved, so
    // the two halves cannot disagree about B1.
    preamp_.setMainRail(power_.railIdle());
    preamp_.setOversampling(1);
    preamp_.prepare(osRate, osBlock);
    buf_.assign(static_cast<size_t>(osBlock), 0.0f);
}

void ChampAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    reverb_->prepare(sampleRate_);
    rebuild();
    prepared_ = true;
}

void ChampAmp::setOversampling(int factor) {
    oversampling_ = factor;
    if (prepared_) rebuild();
}

void ChampAmp::reset() {
    os_.reset();
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void ChampAmp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_VOLUME: preamp_.setParameter(ChampPreamp::PARAM_VOLUME, value); break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void ChampAmp::process(const float* in, float* out, int numFrames) {
    const int f = os_.factor();
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        const int m = n * f;
        os_.upsample(in + off, n);
        float* w = os_.buffer();
        preamp_.process(w, buf_.data(), m);
        if (kInterstageScale != 1.0) {
            for (int i = 0; i < m; ++i)
                buf_[static_cast<size_t>(i)] =
                    static_cast<float>(buf_[static_cast<size_t>(i)] * kInterstageScale);
        }
        power_.process(buf_.data(), w, m);
        os_.downsample(out + off, n);
        off += n;
    }
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
