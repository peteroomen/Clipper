// Clipper — Ac30Amp (M10.2). Composition of the Ac30Preamp, the Ac30PowerAmp, and a
// mono spring ReverbModel (usability add). See Ac30Amp.h for the topology and the
// VOLUME-is-the-overdrive rationale.

#include "clipper/dsp/Ac30Amp.h"

#include <algorithm>

namespace clipper::dsp {

Ac30Amp::Ac30Amp() : reverb_(std::make_unique<ReverbModel>()) {}
Ac30Amp::~Ac30Amp() = default;

void Ac30Amp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    preamp_.prepare(sampleRate_, maxBlockSize_);
    power_.prepare(sampleRate_, maxBlockSize_);
    reverb_->prepare(sampleRate_);
    setOversampling(oversampling_);
}

void Ac30Amp::setOversampling(int factor) {
    oversampling_ = factor;
    preamp_.setOversampling(factor);
    power_.setOversampling(factor);
}

void Ac30Amp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_VOLUME: preamp_.setParameter(Ac30Preamp::PARAM_VOLUME, value); break;
        case PARAM_BASS: preamp_.setParameter(Ac30Preamp::PARAM_BASS, value); break;
        case PARAM_TREBLE: preamp_.setParameter(Ac30Preamp::PARAM_TREBLE, value); break;
        case PARAM_TOPCUT: power_.setParameter(Ac30PowerAmp::PARAM_TOPCUT, value); break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void Ac30Amp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // Preamp: V1 → top-boost stack → VOLUME (real volts at the volume node).
        preamp_.process(in + off, w, n);
        // Preamp volts → hot PI grid drive (memoryless trim, documented constant).
        for (int i = 0; i < n; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, out + off, n);
        // Spring reverb (usability add) AFTER the power amp, mono; mix 0 = passthrough.
        reverb_->process(out + off, out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
